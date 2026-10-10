#include "app/palette_import.hpp"

#include <algorithm>
#include <cmath>

namespace cappedweather {
namespace {

bool parseDoubleStrict(const QString& text, double& out) {
    if (text.isEmpty()) return false;
    bool ok = false;
    const double value = text.toDouble(&ok);
    if (!ok || !std::isfinite(value)) return false;
    out = value;
    return true;
}

bool parseByte(const QString& text, int& out) {
    bool ok = false;
    const int value = text.toInt(&ok);
    if (!ok || value < 0 || value > 255) return false;
    out = value;
    return true;
}

}  // namespace

bool looksLikeGrColorTable(const QString& text) {
    static const char* const kMarkers[] = {"product:",  "units:",     "step:",
                                           "decimals:", "scale:",     "color:",
                                           "color4:",   "solidcolor:", "solidcolor4:"};
    for (const QString& line : text.split(QLatin1Char('\n'))) {
        const QString stripped = line.trimmed();
        if (stripped.isEmpty() || stripped.startsWith(QLatin1Char(';'))) continue;
        const QString lower = stripped.toLower();
        for (const char* marker : kMarkers) {
            if (lower.startsWith(QLatin1String(marker))) return true;
        }
        return false;  // first content line decides, like the importer
    }
    return false;
}

std::optional<GrColorTable> parseGrColorTable(const QString& text, QString* error) {
    const auto fail = [&](const QString& message) {
        if (error) *error = message;
        return std::optional<GrColorTable>{};
    };
    GrColorTable table;
    int lineNumber = 0;
    for (const QString& rawLine : text.split(QLatin1Char('\n'))) {
        ++lineNumber;
        // Strip inline `;` comments (full-line and trailing, GR-style).
        QString line = rawLine;
        const int comment = line.indexOf(QLatin1Char(';'));
        if (comment >= 0) line = line.left(comment);
        line = line.trimmed();
        if (line.isEmpty()) continue;
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon < 0) {
            return fail(QStringLiteral("Line %1 is not a statement").arg(lineNumber));
        }
        const QString keyword = line.left(colon).trimmed().toLower();
        const QStringList fields =
            line.mid(colon + 1).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (keyword == QStringLiteral("product")) {
            if (fields.isEmpty()) {
                return fail(QStringLiteral("Line %1 has an empty Product").arg(lineNumber));
            }
            table.product = fields.join(QLatin1Char(' '));
        } else if (keyword == QStringLiteral("units") || keyword == QStringLiteral("step") ||
                   keyword == QStringLiteral("decimals") || keyword == QStringLiteral("scale")) {
            if (fields.isEmpty()) {
                return fail(QStringLiteral("Line %1 has an empty %2").arg(lineNumber).arg(keyword));
            }
            if (keyword != QStringLiteral("units")) {
                double ignored = 0.0;
                if (!parseDoubleStrict(fields.first(), ignored)) {
                    return fail(QStringLiteral("Line %1 has a bad numeric %2")
                                    .arg(lineNumber)
                                    .arg(keyword));
                }
            } else {
                table.units = fields.join(QLatin1Char(' '));
            }
            // Scale converts the app's internal units, not the authored
            // values: values import as written (same trust as CSV import).
        } else if (keyword == QStringLiteral("color") || keyword == QStringLiteral("color4") ||
                   keyword == QStringLiteral("solidcolor") ||
                   keyword == QStringLiteral("solidcolor4")) {
            // Color: v r g b [r2 g2 b2]; Color4 adds alpha after each triple.
            const bool hasAlpha =
                keyword == QStringLiteral("color4") || keyword == QStringLiteral("solidcolor4");
            const int stride = hasAlpha ? 4 : 3;
            if (fields.size() != 1 + stride && fields.size() != 1 + 2 * stride) {
                return fail(QStringLiteral("Line %1 needs value plus %2 or %3 color numbers")
                                .arg(lineNumber)
                                .arg(stride)
                                .arg(2 * stride));
            }
            double value = 0.0;
            if (!parseDoubleStrict(fields.first(), value)) {
                return fail(QStringLiteral("Line %1 has a bad value").arg(lineNumber));
            }
            int rgb[3] = {0, 0, 0};
            for (int i = 0; i < 3; ++i) {
                if (!parseByte(fields.at(1 + i), rgb[i])) {
                    return fail(QStringLiteral("Line %1 has a bad color").arg(lineNumber));
                }
            }
            // RGB2 sharp-break ends collapse to the next stop (documented);
            // alpha drops (stops carry none).
            table.stops.push_back({value, QColor(rgb[0], rgb[1], rgb[2])});
        } else {
            return fail(
                QStringLiteral("Line %1 has an unknown statement").arg(lineNumber));
        }
    }
    if (table.stops.size() < 2) {
        return fail(QStringLiteral("A palette needs at least two stops"));
    }
    std::sort(table.stops.begin(), table.stops.end(),
              [](const RadarMapCanvas::PaletteStop& a, const RadarMapCanvas::PaletteStop& b) {
                  return a.value < b.value;
              });
    return table;
}

}  // namespace cappedweather
