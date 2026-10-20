#include "app/archive_level2.hpp"
#include "app/s3_listing.hpp"

#include "core/radar_catalog.hpp"

#include <QFileInfo>
#include <QRegularExpression>
#include <QTimeZone>
#include <QUrlQuery>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <climits>
#include <span>

#ifdef CAPPEDWEATHER_HAS_BZIP2
#include <bzlib.h>
#endif

namespace cappedweather {
namespace {

constexpr qsizetype kArchiveHeaderSize = 24;
constexpr qsizetype kMaximumArchiveBytes = 512 * 1024 * 1024;
constexpr qsizetype kMaximumRecordBytes = 64 * 1024 * 1024;
constexpr int kMaximumRecords = 8192;
constexpr auto kArchiveBucket = "https://unidata-nexrad-level2.s3.amazonaws.com/";

bool validRadarId(const QString& radarId) {
    return RadarCatalog::isValidRadarId(radarId.toUpper().toStdString());
}

QString headerRadarId(const QByteArray& bytes, const QString& hint) {
    if (bytes.size() >= kArchiveHeaderSize) {
        const auto candidate = QString::fromLatin1(bytes.constData() + 20, 4).trimmed().toUpper();
        if (validRadarId(candidate)) return candidate;
    }
    const auto normalizedHint = hint.trimmed().toUpper();
    return validRadarId(normalizedHint) ? normalizedHint : QString{};
}

void mergeDecoded(DecodedChunk& destination, DecodedChunk source) {
    // One Archive-II volume holds one format, but never let a mixed merge claim
    // a convention it cannot guarantee: Unknown falls back to the historical
    // "<= 1 is no data" reading downstream.
    if (destination.messageCount == 0 && destination.rays.empty()) {
        destination.momentEncoding = source.momentEncoding;
    } else if (source.momentEncoding != destination.momentEncoding) {
        destination.momentEncoding = MomentEncoding::Unknown;
    }
    destination.messageCount += source.messageCount;
    destination.rejectedMessages += source.rejectedMessages;
    destination.rejectedBlocks += source.rejectedBlocks;
    // A volume is decoded record by record (KPAH 2021 is 94 bzip records), so
    // the pattern has to survive the merge. It is kept only while every record
    // agrees: disagreement means the volume spans a pattern change, and then
    // no single number describes the volume.
    mergeVolumeCoveragePattern(destination, source);
    destination.rays.insert(destination.rays.end(),
                            std::make_move_iterator(source.rays.begin()),
                            std::make_move_iterator(source.rays.end()));
}

std::optional<AwsArchiveObject> objectFromKey(const QString& key, const qint64 size,
                                              const QString& expectedRadar) {
    static const QRegularExpression filePattern(
        QStringLiteral(R"(^([A-Z0-9]{4})(\d{8})_(\d{6})_V\d{2}$)"));
    const auto fileName = QFileInfo(key).fileName().toUpper();
    const auto match = filePattern.match(fileName);
    if (!match.hasMatch()) return std::nullopt;
    const auto radarId = match.captured(1);
    if (radarId.compare(expectedRadar, Qt::CaseInsensitive) != 0 || !validRadarId(radarId) || size <= 0)
        return std::nullopt;
    const auto observedAt = QDateTime::fromString(
        match.captured(2) + match.captured(3), QStringLiteral("yyyyMMddHHmmss"));
    if (!observedAt.isValid()) return std::nullopt;
    auto utc = observedAt;
    utc.setTimeZone(QTimeZone::UTC);
    return AwsArchiveObject{key, radarId, utc, size};
}

} // namespace

std::optional<QByteArray> decompressLevel2BzipBlock(const QByteArray& input, QString* error) {
    if (error) error->clear();
#ifndef CAPPEDWEATHER_HAS_BZIP2
    if (error) *error = QStringLiteral("bzip2 support is unavailable in this build");
    return std::nullopt;
#else
    if (input.size() < 3 || input[0] != 'B' || input[1] != 'Z' || input[2] != 'h') {
        if (error) *error = QStringLiteral("record is not a bzip2 stream");
        return std::nullopt;
    }
    bz_stream stream{};
    stream.next_in = const_cast<char*>(input.constData());
    stream.avail_in = static_cast<unsigned int>(input.size());
    const auto initialized = BZ2_bzDecompressInit(&stream, 0, 0);
    if (initialized != BZ_OK) {
        if (error) *error = QStringLiteral("bzip2 initialization failed (%1)").arg(initialized);
        return std::nullopt;
    }
    constexpr qsizetype kOutputBlockSize = 256 * 1024;
    std::array<char, kOutputBlockSize> block{};
    QByteArray output;
    output.reserve(std::min<qsizetype>(kMaximumRecordBytes,
                                       std::max<qsizetype>(1 << 20, input.size() * 6)));
    int result = BZ_OK;
    while (result == BZ_OK) {
        stream.next_out = block.data();
        stream.avail_out = static_cast<unsigned int>(block.size());
        result = BZ2_bzDecompress(&stream);
        const auto produced = static_cast<qsizetype>(block.size() - stream.avail_out);
        if (produced > 0) output.append(block.data(), produced);
        if (output.size() > kMaximumRecordBytes) {
            BZ2_bzDecompressEnd(&stream);
            if (error) *error = QStringLiteral("bzip2 output exceeded the 64 MB safety limit");
            return std::nullopt;
        }
    }
    BZ2_bzDecompressEnd(&stream);
    if (result != BZ_STREAM_END) {
        if (error) *error = QStringLiteral("bzip2 decompression failed (%1)").arg(result);
        return std::nullopt;
    }
    return output;
#endif
}

std::optional<ArchiveLevel2Result> decodeLevel2Archive(const QByteArray& bytes,
                                                       QString* error,
                                                       const QString& radarIdHint) {
    if (error) error->clear();
    if (bytes.size() < kArchiveHeaderSize) {
        if (error) *error = QStringLiteral("Archive-II file is shorter than its 24-byte header");
        return std::nullopt;
    }
    if (bytes.size() > kMaximumArchiveBytes) {
        if (error) *error = QStringLiteral("Archive-II file exceeds the 512 MB safety limit");
        return std::nullopt;
    }
    if (!bytes.startsWith("AR2V") && !bytes.startsWith("ARCHIVE2")) {
        if (error) *error = QStringLiteral("file does not contain an Archive-II volume header");
        return std::nullopt;
    }
    const auto radarId = headerRadarId(bytes, radarIdHint);
    if (radarId.isEmpty()) {
        if (error) *error = QStringLiteral("Archive-II radar identifier is missing or invalid");
        return std::nullopt;
    }

    ArchiveLevel2Result result;
    result.radarId = radarId;
    result.decoded.radarId = radarId.toStdString();
    qsizetype cursor = kArchiveHeaderSize;
    bool foundCompressedFraming = false;
    while (cursor + 4 <= bytes.size() && result.compressedRecords < kMaximumRecords) {
        const auto signedLength = qFromBigEndian<qint32>(
            reinterpret_cast<const uchar*>(bytes.constData() + cursor));
        if (signedLength == 0 || signedLength == INT_MIN) break;
        const auto length = static_cast<qsizetype>(signedLength < 0 ? -static_cast<qint64>(signedLength)
                                                                   : signedLength);
        if (length < 3 || length > kMaximumRecordBytes) break;
        if (cursor + 4 + length > bytes.size()) {
            if (error) *error = QStringLiteral("Archive-II compressed record is truncated");
            return std::nullopt;
        }
        const auto compressed = bytes.mid(cursor + 4, length);
        if (!compressed.startsWith("BZh")) break;
        foundCompressedFraming = true;
        ++result.compressedRecords;
        QString blockError;
        const auto decompressed = decompressLevel2BzipBlock(compressed, &blockError);
        if (!decompressed) {
            ++result.failedRecords;
            result.warnings.push_back(QStringLiteral("record %1: %2")
                                          .arg(result.compressedRecords).arg(blockError));
        } else {
            auto decoded = decodeMessage31Chunk(
                std::span<const std::byte>(reinterpret_cast<const std::byte*>(decompressed->constData()),
                                           static_cast<std::size_t>(decompressed->size())),
                radarId.toStdString());
            mergeDecoded(result.decoded, std::move(decoded));
        }
        cursor += 4 + length;
        if (signedLength < 0) break;
    }

    if (!foundCompressedFraming) {
        auto decoded = decodeMessage31Chunk(
            std::span<const std::byte>(reinterpret_cast<const std::byte*>(bytes.constData() + kArchiveHeaderSize),
                                       static_cast<std::size_t>(bytes.size() - kArchiveHeaderSize)),
            radarId.toStdString());
        mergeDecoded(result.decoded, std::move(decoded));
    }
    if (result.decoded.rays.empty()) {
        if (error) {
            *error = result.failedRecords > 0
                         ? QStringLiteral("Archive-II records decompressed with errors and contained no decodable Message 31 radials")
                         : QStringLiteral("Archive-II file contains no decodable Message 31 radials");
        }
        return std::nullopt;
    }
    return result;
}

QVector<AwsArchiveObject> parseAwsArchiveListing(const QByteArray& xml,
                                                 const QString& radarId,
                                                 QString* error) {
    if (error) error->clear();
    QString parseError;
    const auto parsed = parseS3Objects(xml, &parseError);
    if (!parseError.isEmpty()) {
        if (error) *error = QStringLiteral("AWS archive listing XML: %1").arg(parseError);
        return {};
    }
    QVector<AwsArchiveObject> result;
    result.reserve(parsed.objects.size());
    for (const auto& entry : parsed.objects) {
        if (const auto object =
                objectFromKey(entry.key, static_cast<qint64>(entry.size), radarId.trimmed().toUpper()))
            result.push_back(*object);
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.observedAt < right.observedAt;
    });
    return result;
}

QVector<AwsArchiveObject> selectArchiveObjects(QVector<AwsArchiveObject> objects,
                                               const QDateTime& startUtc,
                                               const QDateTime& endUtc,
                                               const int maximumVolumes) {
    objects.erase(std::remove_if(objects.begin(), objects.end(), [&](const auto& object) {
        return !object.observedAt.isValid() || object.observedAt < startUtc.toUTC() ||
               object.observedAt > endUtc.toUTC();
    }), objects.end());
    std::sort(objects.begin(), objects.end(), [](const auto& left, const auto& right) {
        return left.observedAt < right.observedAt;
    });
    if (maximumVolumes > 0 && objects.size() > maximumVolumes) {
        if (maximumVolumes == 1) return {objects.constLast()};
        QVector<AwsArchiveObject> sampled;
        sampled.reserve(maximumVolumes);
        const auto lastIndex = objects.size() - 1;
        for (int index = 0; index < maximumVolumes; ++index) {
            const auto sampledIndex = static_cast<qsizetype>(
                (static_cast<qint64>(lastIndex) * index) / (maximumVolumes - 1));
            sampled.push_back(objects.at(sampledIndex));
        }
        return sampled;
    }
    return objects;
}

QUrl awsArchiveListingUrl(const QString& radarId, const QDate& utcDate) {
    QUrl url(QString::fromUtf8(kArchiveBucket));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("list-type"), QStringLiteral("2"));
    query.addQueryItem(QStringLiteral("prefix"),
                       QStringLiteral("%1/%2/%3/%4/")
                           .arg(utcDate.year(), 4, 10, QLatin1Char('0'))
                           .arg(utcDate.month(), 2, 10, QLatin1Char('0'))
                           .arg(utcDate.day(), 2, 10, QLatin1Char('0'))
                           .arg(radarId.trimmed().toUpper()));
    query.addQueryItem(QStringLiteral("max-keys"), QStringLiteral("1000"));
    url.setQuery(query);
    return url;
}

QUrl awsArchiveObjectUrl(const QString& key) {
    QUrl url(QString::fromUtf8(kArchiveBucket));
    url.setPath(QLatin1Char('/') + key);
    return url;
}

QDateTime archiveTimeFromFileName(const QString& fileName) {
    static const QRegularExpression pattern(
        QStringLiteral(R"(^[A-Z0-9]{4}(\d{8})_(\d{6})_V\d{2}$)"));
    const auto match = pattern.match(QFileInfo(fileName).fileName().toUpper());
    if (!match.hasMatch()) return {};
    auto result = QDateTime::fromString(match.captured(1) + match.captured(2),
                                        QStringLiteral("yyyyMMddHHmmss"));
    result.setTimeZone(QTimeZone::UTC);
    return result;
}

} // namespace cappedweather
