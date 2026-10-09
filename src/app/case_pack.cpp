#include "app/case_pack.hpp"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#ifdef CAPPEDWEATHER_HAS_ZLIB
#include <zlib.h>
#endif

namespace cappedweather {

std::optional<CaseManifest> parseCaseManifest(const QByteArray& data, QString* error) {
    if (error) error->clear();
    const auto fail = [error](const QString& message) -> std::optional<CaseManifest> {
        if (error) *error = message;
        return std::nullopt;
    };
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return fail(QStringLiteral("Case manifest is not a JSON object"));
    const auto root = document.object();
    if (root.value(QStringLiteral("app")).toString() != QStringLiteral("CappedWeather") ||
        root.value(QStringLiteral("caseFormat")).toInt(-1) != kCasePackFormat)
        return fail(QStringLiteral("Not a CappedWeather case-pack (format %1 expected)")
                        .arg(kCasePackFormat));
    CaseManifest manifest;
    manifest.radarId = root.value(QStringLiteral("radarId")).toString();
    if (manifest.radarId.isEmpty()) return fail(QStringLiteral("Case manifest has no radar"));
    manifest.frameIndex = root.value(QStringLiteral("frameIndex")).toInt(-1);
    for (const auto& entry : root.value(QStringLiteral("frames")).toArray()) {
        if (!entry.isObject()) return fail(QStringLiteral("Case frame is not an object"));
        const auto object = entry.toObject();
        CaseFrame frame;
        frame.volumeId = object.value(QStringLiteral("volumeId")).toString();
        frame.file = object.value(QStringLiteral("file")).toString();
        if (frame.volumeId.isEmpty() || frame.file.isEmpty())
            return fail(QStringLiteral("Case frame needs a volume id and file"));
        if (frame.file.startsWith(QLatin1Char('/')) || frame.file.contains(QStringLiteral("..")))
            return fail(QStringLiteral("Case frame path escapes the pack"));
        frame.dataTime = QDateTime::fromString(object.value(QStringLiteral("dataTime")).toString(),
                                               Qt::ISODateWithMs);
        frame.chunkCount = object.value(QStringLiteral("chunkCount")).toInt(0);
        frame.failedChunks = object.value(QStringLiteral("failedChunks")).toInt(0);
        manifest.frames.push_back(frame);
    }
    if (manifest.frames.isEmpty()) return fail(QStringLiteral("Case pack has no frames"));
    if (manifest.frameIndex < 0 || manifest.frameIndex >= manifest.frames.size())
        return fail(QStringLiteral("Case frame index is out of range"));
    for (const auto& entry : root.value(QStringLiteral("panels")).toArray()) {
        if (!entry.isObject()) continue;
        const auto object = entry.toObject();
        CasePanelState panel;
        panel.radarId = object.value(QStringLiteral("radar")).toString();
        panel.product = object.value(QStringLiteral("product")).toInt(0);
        panel.tilt = object.value(QStringLiteral("tilt")).toInt(-1);
        manifest.panels.push_back(panel);
    }
    for (const auto& entry : root.value(QStringLiteral("placefileUrls")).toArray())
        if (entry.isString()) manifest.placefileUrls.push_back(entry.toString());
    for (const auto& entry : root.value(QStringLiteral("placefileFiles")).toArray()) {
        if (!entry.isString()) continue;
        const auto file = entry.toString();
        if (file.startsWith(QLatin1Char('/')) || file.contains(QStringLiteral("..")))
            return fail(QStringLiteral("Case placefile path escapes the pack"));
        manifest.placefileFiles.push_back(file);
    }
    manifest.exportedAt = QDateTime::fromString(root.value(QStringLiteral("exportedAt")).toString(),
                                                Qt::ISODateWithMs);
    return manifest;
}

QByteArray writeCaseManifest(const CaseManifest& manifest) {
    QJsonObject root;
    root.insert(QStringLiteral("app"), QStringLiteral("CappedWeather"));
    root.insert(QStringLiteral("caseFormat"), kCasePackFormat);
    root.insert(QStringLiteral("radarId"), manifest.radarId);
    root.insert(QStringLiteral("frameIndex"), manifest.frameIndex);
    QJsonArray frames;
    for (const auto& frame : manifest.frames) {
        QJsonObject object;
        object.insert(QStringLiteral("volumeId"), frame.volumeId);
        object.insert(QStringLiteral("dataTime"), frame.dataTime.toString(Qt::ISODateWithMs));
        object.insert(QStringLiteral("chunkCount"), frame.chunkCount);
        object.insert(QStringLiteral("failedChunks"), frame.failedChunks);
        object.insert(QStringLiteral("file"), frame.file);
        frames.push_back(object);
    }
    root.insert(QStringLiteral("frames"), frames);
    QJsonArray panels;
    for (const auto& panel : manifest.panels) {
        QJsonObject object;
        object.insert(QStringLiteral("radar"), panel.radarId);
        object.insert(QStringLiteral("product"), panel.product);
        object.insert(QStringLiteral("tilt"), panel.tilt);
        panels.push_back(object);
    }
    root.insert(QStringLiteral("panels"), panels);
    QJsonArray urls;
    for (const auto& url : manifest.placefileUrls) urls.push_back(url);
    root.insert(QStringLiteral("placefileUrls"), urls);
    QJsonArray files;
    for (const auto& file : manifest.placefileFiles) files.push_back(file);
    root.insert(QStringLiteral("placefileFiles"), files);
    root.insert(QStringLiteral("exportedAt"),
                manifest.exportedAt.toString(Qt::ISODateWithMs));
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool zipAvailable() {
#ifdef CAPPEDWEATHER_HAS_ZLIB
    return true;
#else
    return false;
#endif
}

#ifdef CAPPEDWEATHER_HAS_ZLIB
namespace {

// Zip I/O is little-endian; QDataStream defaults big-endian, so every
// helper sets LittleEndian explicitly. Writer buffers one entry's deflated
// bytes in memory (volumes are tens of MB: fine transiently, and it lets
// the local header carry exact CRC/sizes instead of a data descriptor).

constexpr quint32 kLocalHeaderSignature = 0x04034B50;
constexpr quint32 kCentralHeaderSignature = 0x02014B50;
constexpr quint32 kEndSignature = 0x06054B50;

QByteArray deflateRaw(const QByteArray& raw, QString* error) {
    z_stream stream{};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        if (error) *error = QStringLiteral("deflate init failed");
        return {};
    }
    QByteArray out;
    out.resize(static_cast<qsizetype>(deflateBound(&stream, static_cast<uLong>(raw.size()))));
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(raw.constData()));
    stream.avail_in = static_cast<uInt>(raw.size());
    stream.next_out = reinterpret_cast<Bytef*>(out.data());
    stream.avail_out = static_cast<uInt>(out.size());
    const int result = deflate(&stream, Z_FINISH);
    deflateEnd(&stream);
    if (result != Z_STREAM_END) {
        if (error) *error = QStringLiteral("deflate failed");
        return {};
    }
    out.resize(static_cast<qsizetype>(stream.total_out));
    return out;
}

QByteArray inflateRaw(const QByteArray& packed, quint32 uncompressedSize, QString* error) {
    z_stream stream{};
    if (inflateInit2(&stream, -15) != Z_OK) {
        if (error) *error = QStringLiteral("inflate init failed");
        return {};
    }
    QByteArray out;
    out.resize(static_cast<qsizetype>(uncompressedSize));
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(packed.constData()));
    stream.avail_in = static_cast<uInt>(packed.size());
    stream.next_out = reinterpret_cast<Bytef*>(out.data());
    stream.avail_out = static_cast<uInt>(out.size());
    // A single Z_FINISH call suffices: entries are complete streams and the
    // output buffer is exactly the declared size (a bomb-sized lie is caught
    // by the total-size cap before this runs).
    const int result = inflate(&stream, Z_FINISH);
    const auto totalOut = stream.total_out;
    inflateEnd(&stream);
    if (result != Z_STREAM_END || totalOut != uncompressedSize) {
        if (error) *error = QStringLiteral("entry data does not inflate to its declared size");
        return {};
    }
    return out;
}

struct ZipEntry {
    QString name;
    quint32 crc = 0;
    quint32 compressedSize = 0;
    quint32 uncompressedSize = 0;
    quint32 localOffset = 0;
    quint16 method = 8;
};

void writeLocalHeader(QDataStream& stream, const QByteArray& nameBytes, quint32 crc,
                      quint32 compressedSize, quint32 uncompressedSize, quint16 method) {
    stream << kLocalHeaderSignature << quint16(20) << quint16(1 << 11)  // UTF-8 names
           << method << quint16(0) << quint16(0) << crc << compressedSize << uncompressedSize
           << quint16(nameBytes.size()) << quint16(0);
    stream.writeRawData(nameBytes.constData(), nameBytes.size());
}

bool readU16(const QByteArray& data, qsizetype& pos, quint16& value) {
    if (pos + 2 > data.size()) return false;
    value = static_cast<quint16>(static_cast<quint8>(data.at(pos)) |
                                 (static_cast<quint16>(static_cast<quint8>(data.at(pos + 1))) << 8));
    pos += 2;
    return true;
}

bool readU32(const QByteArray& data, qsizetype& pos, quint32& value) {
    if (pos + 4 > data.size()) return false;
    value = static_cast<quint32>(static_cast<quint8>(data.at(pos))) |
            (static_cast<quint32>(static_cast<quint8>(data.at(pos + 1))) << 8) |
            (static_cast<quint32>(static_cast<quint8>(data.at(pos + 2))) << 16) |
            (static_cast<quint32>(static_cast<quint8>(data.at(pos + 3))) << 24);
    pos += 4;
    return true;
}

bool cleanEntryName(const QString& name, QString* error) {
    if (name.isEmpty() || name.startsWith(QLatin1Char('/')) || name.contains(QLatin1Char('\\')))
        return false;
    for (const auto& part : name.split(QLatin1Char('/'))) {
        if (part.isEmpty() || part == QStringLiteral(".") || part == QStringLiteral(".."))
            return false;
    }
    if (error) error->clear();
    return true;
}

}  // namespace
#endif

bool zipPackFiles(const QVector<QPair<QString, QString>>& localFiles, const QString& zipPath,
                  QString* error) {
    if (error) error->clear();
#ifndef CAPPEDWEATHER_HAS_ZLIB
    if (error) *error = QStringLiteral("Case-packs need a CappedWeather build with zlib");
    return false;
#else
    if (localFiles.isEmpty() || localFiles.size() > kCasePackMaxEntries) {
        if (error) *error = QStringLiteral("Nothing to pack, or too many entries");
        return false;
    }
    QFile out(zipPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = out.errorString();
        return false;
    }
    QDataStream stream(&out);
    stream.setByteOrder(QDataStream::LittleEndian);
    QVector<ZipEntry> entries;
    for (const auto& [zipName, diskPath] : localFiles) {
        QString nameError;
        if (!cleanEntryName(zipName, &nameError)) {
            if (error) *error = QStringLiteral("Unsafe entry name: %1").arg(zipName);
            out.close();
            QFile::remove(zipPath);
            return false;
        }
        QFile in(diskPath);
        if (!in.open(QIODevice::ReadOnly)) {
            if (error) *error = QStringLiteral("%1: %2").arg(zipName, in.errorString());
            out.close();
            QFile::remove(zipPath);
            return false;
        }
        const QByteArray raw = in.readAll();
        const quint32 crc =
            static_cast<quint32>(crc32(0L, reinterpret_cast<const Bytef*>(raw.constData()),
                                       static_cast<uInt>(raw.size())));
        QString deflateError;
        const QByteArray packed = deflateRaw(raw, &deflateError);
        if (packed.isEmpty() && !raw.isEmpty()) {
            if (error) *error = deflateError;
            out.close();
            QFile::remove(zipPath);
            return false;
        }
        const QByteArray nameBytes = zipName.toUtf8();
        ZipEntry entry;
        entry.name = zipName;
        entry.crc = crc;
        entry.compressedSize = static_cast<quint32>(packed.size());
        entry.uncompressedSize = static_cast<quint32>(raw.size());
        entry.localOffset = static_cast<quint32>(out.pos());
        writeLocalHeader(stream, nameBytes, crc, entry.compressedSize, entry.uncompressedSize, 8);
        out.write(packed);
        entries.push_back(entry);
    }
    const quint32 centralOffset = static_cast<quint32>(out.pos());
    for (const auto& entry : entries) {
        const QByteArray nameBytes = entry.name.toUtf8();
        stream << kCentralHeaderSignature << quint16(20) << quint16(20) << quint16(1 << 11)
               << entry.method << quint16(0) << quint16(0) << entry.crc << entry.compressedSize
               << entry.uncompressedSize << quint16(nameBytes.size()) << quint16(0) << quint16(0)
               << quint16(0) << quint16(0) << quint32(0) << entry.localOffset;
        stream.writeRawData(nameBytes.constData(), nameBytes.size());
    }
    const quint32 centralSize = static_cast<quint32>(out.pos() - centralOffset);
    stream << kEndSignature << quint16(0) << quint16(0) << quint16(entries.size())
           << quint16(entries.size()) << centralSize << centralOffset << quint16(0);
    out.close();
    return true;
#endif
}

QStringList zipExtractFiles(const QString& zipPath, const QString& destDir, QString* error) {
    if (error) error->clear();
#ifndef CAPPEDWEATHER_HAS_ZLIB
    if (error) *error = QStringLiteral("Case-packs need a CappedWeather build with zlib");
    return {};
#else
    QStringList extracted;
    QFile in(zipPath);
    if (!in.open(QIODevice::ReadOnly)) {
        if (error) *error = in.errorString();
        return {};
    }
    const QByteArray data = in.readAll();
    in.close();
    const auto fail = [&](const QString& message) {
        // Never leave a half-extracted pack behind: nothing extracted is
        // trusted, so remove what landed before reporting.
        for (const auto& name : extracted) QFile::remove(destDir + QLatin1Char('/') + name);
        extracted.clear();
        if (error) *error = message;
        return extracted;
    };
    // Locate the end record by scanning the tail (no archive comment is
    // written, but foreign tools may add one on re-save).
    qsizetype endPos = -1;
    const qsizetype scanStart = std::max<qsizetype>(0, data.size() - 65557);
    for (qsizetype pos = data.size() - 22; pos >= scanStart; --pos) {
        quint32 signature = 0;
        qsizetype probe = pos;
        if (readU32(data, probe, signature) && signature == kEndSignature) {
            endPos = pos;
            break;
        }
    }
    if (endPos < 0) return fail(QStringLiteral("Not a zip file (no end record)"));
    qsizetype pos = endPos + 4;
    quint16 diskNumber = 0, centralDisk = 0, diskEntries = 0, totalEntries = 0;
    quint32 centralSize = 0, centralOffset = 0;
    quint16 commentLength = 0;
    if (!readU16(data, pos, diskNumber) || !readU16(data, pos, centralDisk) ||
        !readU16(data, pos, diskEntries) || !readU16(data, pos, totalEntries) ||
        !readU32(data, pos, centralSize) || !readU32(data, pos, centralOffset) ||
        !readU16(data, pos, commentLength))
        return fail(QStringLiteral("Truncated zip end record"));
    if (diskNumber != 0 || centralDisk != 0 || diskEntries != totalEntries)
        return fail(QStringLiteral("Multi-disk zips are not supported"));
    if (totalEntries == 0 || totalEntries > kCasePackMaxEntries)
        return fail(QStringLiteral("Zip entry count is out of bounds"));
    if (static_cast<qsizetype>(centralOffset) + centralSize > data.size())
        return fail(QStringLiteral("Zip central directory runs past the file"));
    struct CentralEntry {
        QString name;
        quint32 crc = 0;
        quint32 compressedSize = 0;
        quint32 uncompressedSize = 0;
        quint32 localOffset = 0;
        quint16 method = 0;
    };
    QVector<CentralEntry> central;
    pos = centralOffset;
    qint64 totalUncompressed = 0;
    for (int i = 0; i < totalEntries; ++i) {
        quint32 signature = 0;
        if (!readU32(data, pos, signature) || signature != kCentralHeaderSignature)
            return fail(QStringLiteral("Central directory entry is corrupt"));
        CentralEntry entry;
        quint16 versionMade = 0, versionNeeded = 0, flags = 0, modTime = 0, modDate = 0;
        quint16 nameLength = 0, extraLength = 0, fileCommentLength = 0;
        quint16 diskStart = 0, intAttrs = 0;
        quint32 extAttrs = 0;
        if (!readU16(data, pos, versionMade) || !readU16(data, pos, versionNeeded) ||
            !readU16(data, pos, flags) || !readU16(data, pos, entry.method) ||
            !readU16(data, pos, modTime) || !readU16(data, pos, modDate) ||
            !readU32(data, pos, entry.crc) || !readU32(data, pos, entry.compressedSize) ||
            !readU32(data, pos, entry.uncompressedSize) || !readU16(data, pos, nameLength) ||
            !readU16(data, pos, extraLength) || !readU16(data, pos, fileCommentLength) ||
            !readU16(data, pos, diskStart) || !readU16(data, pos, intAttrs) ||
            !readU32(data, pos, extAttrs) || !readU32(data, pos, entry.localOffset))
            return fail(QStringLiteral("Central directory entry is truncated"));
        if (pos + nameLength + extraLength + fileCommentLength > data.size())
            return fail(QStringLiteral("Central directory name runs past the file"));
        entry.name = QString::fromUtf8(data.constData() + pos, nameLength);
        pos += nameLength + extraLength + fileCommentLength;
        if (entry.method != 0 && entry.method != 8)
            return fail(QStringLiteral("Unsupported compression in %1").arg(entry.name));
        QString nameError;
        if (!cleanEntryName(entry.name, &nameError))
            return fail(QStringLiteral("Unsafe entry name: %1").arg(entry.name));
        if (entry.uncompressedSize > kCasePackMaxEntryBytes)
            return fail(QStringLiteral("Entry too large: %1").arg(entry.name));
        totalUncompressed += entry.uncompressedSize;
        if (totalUncompressed > kCasePackMaxUncompressedBytes)
            return fail(QStringLiteral("Zip contents exceed the 2 GiB safety bound"));
        central.push_back(entry);
    }
    QDir().mkpath(destDir);
    for (const auto& entry : central) {
        // Local header: skip to the data via its name/extra lengths (never
        // trust them blindly: bounds-checked against the file size).
        qsizetype local = entry.localOffset;
        quint32 localSignature = 0;
        if (!readU32(data, local, localSignature) || localSignature != kLocalHeaderSignature)
            return fail(QStringLiteral("Local header missing for %1").arg(entry.name));
        local += 22;  // version, flags, method, time, date, crc, sizes
        quint16 localNameLength = 0, localExtraLength = 0;
        if (!readU16(data, local, localNameLength) || !readU16(data, local, localExtraLength))
            return fail(QStringLiteral("Local header truncated for %1").arg(entry.name));
        local += localNameLength + localExtraLength;
        if (local < 0 || static_cast<qint64>(local) + entry.compressedSize > data.size())
            return fail(QStringLiteral("Entry data runs past the file: %1").arg(entry.name));
        const QByteArray packed(data.constData() + local,
                                static_cast<qsizetype>(entry.compressedSize));
        QByteArray raw;
        if (entry.method == 0) {
            raw = packed;
        } else {
            QString inflateError;
            raw = inflateRaw(packed, entry.uncompressedSize, &inflateError);
            if (raw.isEmpty() && entry.uncompressedSize > 0)
                return fail(QStringLiteral("%1: %2").arg(entry.name, inflateError));
        }
        if (static_cast<quint32>(raw.size()) != entry.uncompressedSize)
            return fail(QStringLiteral("Size mismatch in %1").arg(entry.name));
        const quint32 crc = static_cast<quint32>(
            crc32(0L, reinterpret_cast<const Bytef*>(raw.constData()), static_cast<uInt>(raw.size())));
        if (crc != entry.crc) return fail(QStringLiteral("CRC mismatch in %1").arg(entry.name));
        const QString outPath = destDir + QLatin1Char('/') + entry.name;
        QDir().mkpath(QFileInfo(outPath).absolutePath());
        QFile out(outPath);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate) || out.write(raw) != raw.size()) {
            out.close();
            return fail(QStringLiteral("Could not write %1").arg(entry.name));
        }
        out.close();
        extracted.push_back(entry.name);
    }
    return extracted;
#endif
}

}  // namespace cappedweather
