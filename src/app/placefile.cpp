#include "app/placefile.hpp"

#include <algorithm>
#include <cmath>

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

} // namespace cappedweather
