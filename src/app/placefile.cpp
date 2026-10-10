#include "app/placefile.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cappedweather {
namespace {

constexpr qsizetype kMaximumPlacefileBytes = 2 * 1024 * 1024;
constexpr qsizetype kMaximumElements = 50'000;
constexpr qsizetype kMaximumPointsPerElement = 10'000;

QString stripComment(const QString& input) {
    bool quoted = false;
    for (qsizetype index = 0; index < input.size(); ++index) {
        const auto character = input.at(index);
        if (character == QLatin1Char('"')) {
            if (quoted && index + 1 < input.size() && input.at(index + 1) == QLatin1Char('"')) {
                ++index;
            } else {
                quoted = !quoted;
            }
        } else if (character == QLatin1Char(';') && !quoted) {
            return input.left(index).trimmed();
        }
    }
    return input.trimmed();
}

QStringList splitCsv(const QString& input) {
    QStringList fields;
    QString field;
    bool quoted = false;
    for (qsizetype index = 0; index < input.size(); ++index) {
        const auto character = input.at(index);
        if (character == QLatin1Char('"')) {
            if (quoted && index + 1 < input.size() && input.at(index + 1) == QLatin1Char('"')) {
                field += QLatin1Char('"');
                ++index;
            } else {
                quoted = !quoted;
            }
        } else if (character == QLatin1Char(',') && !quoted) {
            fields.push_back(field.trimmed());
            field.clear();
        } else {
            field += character;
        }
    }
    fields.push_back(field.trimmed());
    return fields;
}

bool coordinate(const QStringList& fields, const qsizetype offset, QPointF& point) {
    if (offset < 0 || fields.size() < offset + 2) return false;
    bool latitudeOk = false;
    bool longitudeOk = false;
    const auto latitude = fields.at(offset).toDouble(&latitudeOk);
    const auto longitude = fields.at(offset + 1).toDouble(&longitudeOk);
    if (!latitudeOk || !longitudeOk || !std::isfinite(latitude) || !std::isfinite(longitude) ||
        latitude < -90.0 || latitude > 90.0 || longitude < -180.0 || longitude > 180.0) {
        return false;
    }
    point = {longitude, latitude};
    return true;
}

QString remainingText(const QStringList& fields, const qsizetype offset) {
    if (offset >= fields.size()) return {};
    return fields.mid(offset).join(QStringLiteral(", ")).trimmed();
}

void warn(PlacefileDocument& document, const int lineNumber, const QString& message) {
    document.warnings.push_back(QStringLiteral("line %1: %2").arg(lineNumber).arg(message));
}

} // namespace

std::optional<PlacefileDocument> parsePlacefile(const QByteArray& bytes, QString* error) {
    if (error) error->clear();
    if (bytes.size() > kMaximumPlacefileBytes) {
        if (error) *error = QStringLiteral("placefile exceeds the 2 MB safety limit");
        return std::nullopt;
    }

    PlacefileDocument document;
    std::optional<qsizetype> blockIndex;
    const auto lines = QString::fromUtf8(bytes).split(QLatin1Char('\n'));
    QColor currentColor = Qt::white;
    double currentThreshold = 999.0;

    for (qsizetype rawIndex = 0; rawIndex < lines.size(); ++rawIndex) {
        const auto lineNumber = static_cast<int>(rawIndex + 1);
        const auto line = stripComment(lines.at(rawIndex));
        if (line.isEmpty()) continue;

        if (blockIndex.has_value()) {
            if (line.compare(QStringLiteral("End:"), Qt::CaseInsensitive) == 0 ||
                line.compare(QStringLiteral("End"), Qt::CaseInsensitive) == 0) {
                const auto points = document.elements.at(*blockIndex).points.size();
                const auto kind = document.elements.at(*blockIndex).kind;
                const auto minimum = kind == PlacefileElementKind::Polygon ? 3 : 2;
                if (points < minimum) {
                    warn(document, lineNumber, QStringLiteral("discarded block with too few points"));
                    document.elements.removeAt(*blockIndex);
                }
                blockIndex.reset();
                continue;
            }
            QPointF point;
            const auto fields = splitCsv(line);
            if (!coordinate(fields, 0, point)) {
                warn(document, lineNumber, QStringLiteral("invalid block coordinate"));
                continue;
            }
            auto& element = document.elements[*blockIndex];
            if (element.points.size() >= kMaximumPointsPerElement) {
                warn(document, lineNumber, QStringLiteral("point limit reached"));
                continue;
            }
            element.points.push_back(point);
            continue;
        }

        const auto colon = line.indexOf(QLatin1Char(':'));
        if (colon < 0) {
            warn(document, lineNumber, QStringLiteral("ignored statement without ':'"));
            continue;
        }
        const auto directive = line.left(colon).trimmed().toLower();
        const auto value = line.mid(colon + 1).trimmed();
        const auto fields = splitCsv(value);

        if (directive == QStringLiteral("title")) {
            document.title = value;
        } else if (directive == QStringLiteral("refreshseconds") || directive == QStringLiteral("refresh")) {
            bool ok = false;
            auto refresh = value.toDouble(&ok);
            if (directive == QStringLiteral("refresh")) refresh *= 60.0;
            if (!ok || !std::isfinite(refresh) || refresh < 0.0) {
                warn(document, lineNumber, QStringLiteral("invalid refresh interval"));
            } else {
                document.refreshSeconds = std::clamp(static_cast<int>(std::lround(refresh)), 0, 86'400);
            }
        } else if (directive == QStringLiteral("threshold")) {
            bool ok = false;
            const auto threshold = value.toDouble(&ok);
            if (!ok || !std::isfinite(threshold) || threshold < 0.0) {
                warn(document, lineNumber, QStringLiteral("invalid threshold"));
            } else {
                currentThreshold = threshold;
            }
        } else if (directive == QStringLiteral("color")) {
            auto colorFields = fields;
            if (colorFields.size() < 3) colorFields = value.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
            if (colorFields.size() < 3) {
                warn(document, lineNumber, QStringLiteral("invalid color"));
                continue;
            }
            bool redOk = false;
            bool greenOk = false;
            bool blueOk = false;
            const auto red = colorFields.at(0).toInt(&redOk);
            const auto green = colorFields.at(1).toInt(&greenOk);
            const auto blue = colorFields.at(2).toInt(&blueOk);
            if (!redOk || !greenOk || !blueOk || red < 0 || red > 255 || green < 0 || green > 255 ||
                blue < 0 || blue > 255) {
                warn(document, lineNumber, QStringLiteral("invalid color"));
            } else {
                currentColor = QColor(red, green, blue);
            }
        } else if (directive == QStringLiteral("place") || directive == QStringLiteral("text") ||
                   directive == QStringLiteral("icon")) {
            QPointF point;
            if (!coordinate(fields, 0, point)) {
                warn(document, lineNumber, QStringLiteral("invalid coordinate"));
                continue;
            }
            PlacefileElement element;
            element.color = currentColor;
            element.thresholdNauticalMiles = currentThreshold;
            element.points.push_back(point);
            if (directive == QStringLiteral("place")) {
                element.kind = PlacefileElementKind::Marker;
                element.text = remainingText(fields, 2);
                element.hoverText = element.text;
            } else if (directive == QStringLiteral("text")) {
                if (fields.size() < 4) {
                    warn(document, lineNumber, QStringLiteral("Text requires font number and text"));
                    continue;
                }
                element.kind = PlacefileElementKind::Text;
                element.text = fields.at(3);
                if (fields.size() > 4) element.hoverText = remainingText(fields, 4);
            } else {
                if (fields.size() < 5) {
                    warn(document, lineNumber, QStringLiteral("Icon requires angle, file and icon numbers"));
                    continue;
                }
                bool angleOk = false;
                bool fileOk = false;
                bool iconOk = false;
                element.rotationDegrees = fields.at(2).toDouble(&angleOk);
                element.iconFileNumber = fields.at(3).toInt(&fileOk);
                element.iconNumber = fields.at(4).toInt(&iconOk);
                if (!angleOk || !fileOk || !iconOk) {
                    warn(document, lineNumber, QStringLiteral("invalid Icon fields"));
                    continue;
                }
                element.kind = PlacefileElementKind::Icon;
                if (fields.size() > 5) element.hoverText = remainingText(fields, 5);
            }
            document.elements.push_back(std::move(element));
        } else if (directive == QStringLiteral("line") || directive == QStringLiteral("polygon")) {
            PlacefileElement element;
            element.kind = directive == QStringLiteral("line") ? PlacefileElementKind::Line
                                                                : PlacefileElementKind::Polygon;
            element.color = currentColor;
            element.thresholdNauticalMiles = currentThreshold;
            if (element.kind == PlacefileElementKind::Line && !fields.isEmpty()) {
                bool widthOk = false;
                const auto width = fields.at(0).toInt(&widthOk);
                if (widthOk) element.lineWidth = std::clamp(width, 1, 20);
                if (fields.size() > 2) element.hoverText = remainingText(fields, 2);
            }
            document.elements.push_back(std::move(element));
            blockIndex = document.elements.size() - 1;
        } else if (directive == QStringLiteral("font") || directive == QStringLiteral("iconfile")) {
            // Accepted metadata. This first bounded renderer uses native labels
            // and marker glyphs rather than downloading arbitrary font/icon assets.
        } else {
            warn(document, lineNumber, QStringLiteral("unsupported directive '%1'").arg(directive));
        }

        if (document.elements.size() > kMaximumElements) {
            if (error) *error = QStringLiteral("placefile exceeds the 50000 element safety limit");
            return std::nullopt;
        }
    }

    if (blockIndex.has_value()) {
        warn(document, static_cast<int>(lines.size()), QStringLiteral("unterminated drawing block"));
        document.elements.removeAt(*blockIndex);
    }
    if (document.elements.isEmpty()) {
        if (error) *error = QStringLiteral("placefile contains no drawable supported elements");
        return std::nullopt;
    }
    return document;
}

namespace {

// Big-endian shapefile header integers vs little-endian payload: every
// read below is bounds-checked, positions are byte offsets.
bool readBe32(const QByteArray& bytes, qsizetype pos, qint32& out) {
    if (pos < 0 || pos + 4 > bytes.size()) return false;
    const auto* data = reinterpret_cast<const uchar*>(bytes.constData() + pos);
    out = static_cast<qint32>((static_cast<quint32>(data[0]) << 24) |
                              (static_cast<quint32>(data[1]) << 16) |
                              (static_cast<quint32>(data[2]) << 8) |
                              static_cast<quint32>(data[3]));
    return true;
}

bool readLe32(const QByteArray& bytes, qsizetype pos, qint32& out) {
    if (pos < 0 || pos + 4 > bytes.size()) return false;
    const auto* data = reinterpret_cast<const uchar*>(bytes.constData() + pos);
    out = static_cast<qint32>(static_cast<quint32>(data[0]) |
                              (static_cast<quint32>(data[1]) << 8) |
                              (static_cast<quint32>(data[2]) << 16) |
                              (static_cast<quint32>(data[3]) << 24));
    return true;
}

bool readLeDouble(const QByteArray& bytes, qsizetype pos, double& out) {
    if (pos < 0 || pos + 8 > bytes.size()) return false;
    std::uint64_t bits = 0;
    const auto* data = reinterpret_cast<const uchar*>(bytes.constData() + pos);
    for (int i = 7; i >= 0; --i) bits = (bits << 8) | data[i];
    std::memcpy(&out, &bits, sizeof(out));
    return std::isfinite(out);
}

} // namespace

bool looksLikeShapefile(const QByteArray& bytes) {
    if (bytes.size() < 100) return false;
    qint32 fileCode = 0;
    return readBe32(bytes, 0, fileCode) && fileCode == 9994;
}

std::optional<PlacefileDocument> parseShapefile(const QByteArray& bytes, QString* error) {
    if (error) error->clear();
    if (!looksLikeShapefile(bytes)) {
        if (error) *error = QStringLiteral("not an ESRI shapefile (file code 9994 missing)");
        return std::nullopt;
    }
    qint32 declaredWords = 0;
    qint32 version = 0;
    qint32 headerType = 0;
    if (!readBe32(bytes, 24, declaredWords) || !readLe32(bytes, 28, version) ||
        !readLe32(bytes, 32, headerType)) {
        if (error) *error = QStringLiteral("shapefile header is truncated");
        return std::nullopt;
    }
    if (version != 1000) {
        if (error) *error = QStringLiteral("unsupported shapefile version");
        return std::nullopt;
    }
    if (headerType != 3 && headerType != 5 && headerType != 13 && headerType != 15 &&
        headerType != 23 && headerType != 25) {
        if (error) *error = QStringLiteral("shapefile holds no polygon/polyline geometry");
        return std::nullopt;
    }
    if (static_cast<qint64>(declaredWords) * 2 > bytes.size()) {
        if (error) *error = QStringLiteral("shapefile is truncated");
        return std::nullopt;
    }
    const qsizetype declaredEnd = static_cast<qsizetype>(declaredWords) * 2;
    if (declaredEnd < 100) {
        if (error) *error = QStringLiteral("shapefile header is truncated");
        return std::nullopt;
    }
    PlacefileDocument document;
    qsizetype pos = 100;
    qsizetype outOfRangeRings = 0;
    while (pos + 8 <= declaredEnd) {
        qint32 contentWords = 0;
        qint32 recordType = 0;
        if (!readBe32(bytes, pos + 4, contentWords) || contentWords < 0) {
            if (error) *error = QStringLiteral("shapefile record header is truncated");
            return std::nullopt;
        }
        const qsizetype recordEnd = pos + 8 + static_cast<qsizetype>(contentWords) * 2;
        if (recordEnd > declaredEnd) {
            if (error) *error = QStringLiteral("shapefile record overruns the file");
            return std::nullopt;
        }
        if (!readLe32(bytes, pos + 8, recordType)) {
            if (error) *error = QStringLiteral("shapefile record is truncated");
            return std::nullopt;
        }
        pos += 8;
        if (recordType == 0) {
            pos = recordEnd;  // Null shape: filler, skip silently.
            continue;
        }
        if (recordType != 3 && recordType != 5) {
            document.warnings.push_back(
                QStringLiteral("unsupported shape type %1").arg(recordType));
            pos = recordEnd;
            continue;
        }
        qint32 partCount = 0;
        qint32 pointCount = 0;
        // Content layout past the 4-byte type: 32-byte box, NumParts,
        // NumPoints, part table, points.
        if (!readLe32(bytes, pos + 36, partCount) || !readLe32(bytes, pos + 40, pointCount) ||
            partCount < 0 || pointCount < 0 || partCount > 10'000 || pointCount > 256'000) {
            if (error) *error = QStringLiteral("shapefile ring counts are malformed");
            return std::nullopt;
        }
        if (pos + 44 + static_cast<qsizetype>(partCount) * 4 +
                static_cast<qsizetype>(pointCount) * 16 >
            recordEnd) {
            if (error) *error = QStringLiteral("shapefile ring overruns its record");
            return std::nullopt;
        }
        // Part table first (readable: bounds-checked above, so a short
        // read here is unreachable, but never trust it blindly).
        QVector<qint32> partStarts;
        partStarts.reserve(partCount);
        for (qint32 part = 0; part < partCount; ++part) {
            qint32 start = 0;
            if (!readLe32(bytes, pos + 44 + part * 4, start)) {
                if (error) *error = QStringLiteral("shapefile part table is truncated");
                return std::nullopt;
            }
            partStarts.push_back(start);
        }
        for (qint32 part = 0; part < partCount; ++part) {
            const qint32 first = partStarts.at(part);
            const qint32 last =
                part + 1 < partCount ? partStarts.at(part + 1) : pointCount;
            if (first < 0 || last < first || last > pointCount) {
                if (error) *error = QStringLiteral("shapefile part index is malformed");
                return std::nullopt;
            }
            if (static_cast<qsizetype>(last - first) > kMaximumPointsPerElement) {
                document.warnings.push_back(QStringLiteral("ring exceeds the 10000 point limit"));
                continue;
            }
            PlacefileElement element;
            element.kind = recordType == 5 ? PlacefileElementKind::Polygon
                                           : PlacefileElementKind::Line;
            bool ringOk = true;
            for (qint32 point = first; point < last; ++point) {
                double x = 0.0;
                double y = 0.0;
                const qsizetype pointPos =
                    pos + 44 + static_cast<qsizetype>(partCount) * 4 + static_cast<qsizetype>(point) * 16;
                if (!readLeDouble(bytes, pointPos, x) || !readLeDouble(bytes, pointPos + 8, y) ||
                    x < -180.0 || x > 180.0 || y < -90.0 || y > 90.0) {
                    ringOk = false;
                    break;
                }
                element.points.push_back({x, y});
            }
            if (!ringOk) {
                // Projected (meter) shapefiles land here on every ring.
                ++outOfRangeRings;
                continue;
            }
            if (element.points.size() < (element.kind == PlacefileElementKind::Polygon ? 3 : 2)) {
                continue;  // Degenerate ring: nothing to draw, not an error.
            }
            document.elements.push_back(std::move(element));
            if (document.elements.size() > kMaximumElements) {
                if (error) *error = QStringLiteral("shapefile exceeds the 50000 element safety limit");
                return std::nullopt;
            }
        }
        pos = recordEnd;
    }
    if (pos != declaredEnd) {
        if (error) {
            *error = pos > declaredEnd ? QStringLiteral("shapefile header is truncated")
                                       : QStringLiteral("shapefile has trailing bytes");
        }
        return std::nullopt;
    }
    if (document.elements.isEmpty()) {
        if (error) {
            *error = outOfRangeRings > 0
                         ? QStringLiteral("shapefile coordinates are not longitude/latitude "
                                          "(projected shapefiles are unsupported)")
                         : QStringLiteral("shapefile contains no drawable supported elements");
        }
        return std::nullopt;
    }
    return document;
}

} // namespace cappedweather
