#include "app/radar_map_canvas.hpp"

#include "app/http_client.hpp"
#include "app/volume_camera.hpp"
#include "core/radar_geometry.hpp"
#include "core/scan_diagnostics.hpp"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QNetworkDiskCache>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QAccessible>
#include <QAction>
#include <QApplication>
#include <QCache>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QPointer>
#include <QLineF>
#include <QMenu>
#include <QPainter>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QWheelEvent>
#include <QGestureEvent>
#include <QPinchGesture>
#include <QNativeGestureEvent>
#include <QTimer>
#include <QElapsedTimer>
#include <cstdio>
#include <QThreadPool>
#include <QToolTip>
#include <QStandardPaths>
#include <QSettings>
#include <QTimeZone>
#include <QSet>
#include <QShowEvent>
#include <QResizeEvent>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <future>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <mutex>

namespace cappedweather {

namespace {
constexpr double kPi = 3.14159265358979323846;

// Interface text scale (Settings → Display). Multiplies every HUD painter
// size; the app font is scaled separately in MainWindow::applyFontScale.
double g_hudScale = 1.0;

QFont hudFont(const int pointSize, const QFont::Weight weight = QFont::Normal) {
    QFont font;
    font.setFamilies({QStringLiteral("IBM Plex Sans"), QStringLiteral("Segoe UI"),
                      QStringLiteral("Noto Sans"), QStringLiteral("DejaVu Sans")});
    font.setPointSizeF(pointSize * g_hudScale);
    font.setWeight(weight);
    return font;
}

QColor lightningCellColor(const LightningCellSnapshot& cell) {
    if (!cell.active()) return QColor(90, 215, 235);
    if (cell.sigmaLevel >= 5.0) return QColor(238, 70, 255);
    if (cell.sigmaLevel >= 4.0) return QColor(255, 74, 74);
    if (cell.sigmaLevel >= 3.0) return QColor(255, 145, 40);
    return QColor(255, 222, 64);
}

QString elapsedText(const std::int64_t epochSec, const std::int64_t nowEpochSec) {
    if (epochSec <= 0) return QStringLiteral("unknown");
    const auto seconds = std::max<std::int64_t>(0, nowEpochSec - epochSec);
    if (seconds < 90) return QStringLiteral("%1s").arg(seconds);
    if (seconds < 90 * 60) return QStringLiteral("%1m").arg(seconds / 60);
    return QStringLiteral("%1h %2m").arg(seconds / 3600).arg((seconds % 3600) / 60);
}

double distanceToSegment(const QPointF& point, const QPointF& start, const QPointF& end) {
    const auto segment = end - start;
    const auto lengthSquared = QPointF::dotProduct(segment, segment);
    if (lengthSquared <= 0.0) return QLineF(point, start).length();
    const auto amount = std::clamp(QPointF::dotProduct(point - start, segment) / lengthSquared, 0.0, 1.0);
    return QLineF(point, start + segment * amount).length();
}

struct ColorStop {
    double position;
    QColor color;
};

struct ValueColorStop {
    double minimum;
    QColor color;
};

QColor discretePalette(const std::initializer_list<ValueColorStop> stops, const double value,
                       const int opacity) {
    auto selected = stops.begin()->color;
    for (const auto& stop : stops) {
        if (value < stop.minimum) break;
        selected = stop.color;
    }
    selected.setAlpha(opacity);
    return selected;
}

QColor interpolatePalette(const std::initializer_list<ColorStop> stops, const double position,
                          const int opacity) {
    const auto value = std::clamp(position, 0.0, 1.0);
    auto previous = stops.begin();
    for (auto current = std::next(previous); current != stops.end(); ++current) {
        if (value > current->position) {
            previous = current;
            continue;
        }
        const auto span = current->position - previous->position;
        const auto amount = span > 0.0 ? (value - previous->position) / span : 0.0;
        const auto channel = [amount](const int from, const int to) {
            return static_cast<int>(std::round(from + (to - from) * amount));
        };
        return QColor(channel(previous->color.red(), current->color.red()),
                      channel(previous->color.green(), current->color.green()),
                      channel(previous->color.blue(), current->color.blue()), opacity);
    }
    auto result = std::prev(stops.end())->color;
    result.setAlpha(opacity);
    return result;
}

// Probe readout decimals. KDP values live at 0.01-7 deg/km, so a single
// decimal collapses 0.30 and 0.34 into the same string; correlation
// coefficient has always carried three. Everything else keeps one.
int probeDecimals(const ProductKind product) {
    switch (product) {
    case ProductKind::CorrelationCoefficient:
        return 3;
    case ProductKind::SpecificDifferentialPhase:
        return 2;
    default:
        return 1;
    }
}

QColor productColor(const ProductKind product, const double normalized,
                    const RadarMapCanvas::PaletteKind palette, const int opacityPercent,
                    const QVector<RadarMapCanvas::PaletteStop>& custom = {}) {
    const auto alpha = std::clamp(static_cast<int>(std::round(opacityPercent * 2.55)), 0, 255);
    if (palette == RadarMapCanvas::PaletteKind::Custom && !custom.isEmpty()) {
        const auto& descriptor = productDescriptor(product);
        const auto value = descriptor.minimum + normalized * (descriptor.maximum - descriptor.minimum);
        auto color = custom.first().color;
        for (const auto& stop : custom) {
            if (value < stop.value) break;
            color = stop.color;
        }
        color.setAlpha(alpha);
        return color;
    }
    if (palette == RadarMapCanvas::PaletteKind::ColorSafe) {
        // Signed products share one diverging ramp. KDP belongs here: hail and
        // melting-layer gates go negative, so a sequential ramp would read as
        // "more rain" where the physics says less phase shift.
        if (product == ProductKind::Velocity || product == ProductKind::DifferentialReflectivity ||
            product == ProductKind::DifferentialPhase ||
            product == ProductKind::SpecificDifferentialPhase) {
            return interpolatePalette({
                {0.00, QColor("#3b4cc0")}, {0.25, QColor("#78a5d5")},
                {0.50, QColor("#f1f1f1")}, {0.75, QColor("#e89c75")},
                {1.00, QColor("#b40426")},
            }, normalized, alpha);
        }
        return interpolatePalette({
            {0.00, QColor("#440154")}, {0.25, QColor("#3b528b")},
            {0.50, QColor("#21918c")}, {0.75, QColor("#5ec962")},
            {1.00, QColor("#fde725")},
        }, normalized, alpha);
    }
    if (palette == RadarMapCanvas::PaletteKind::Operational) {
        const auto& descriptor = productDescriptor(product);
        const auto value = descriptor.minimum + normalized * (descriptor.maximum - descriptor.minimum);
        switch (product) {
        case ProductKind::Reflectivity:
            // NWS-style ramp: visual weight grows with dBZ. Bright cyan lives
            // at the noise floor (-30s) and fades through blues to green at
            // 20+. (A previous revision painted the whole -10..+5 band full
            // cyan, so drizzle-level KNMI/OPERA echo glowed like convection.)
            return discretePalette({
                {-32, QColor("#646464")}, {-30, QColor("#04e9e7")}, {-25, QColor("#00b4d8")},
                {-20, QColor("#019ff4")}, {-10, QColor("#0245f0")}, {0, QColor("#022cd8")},
                {5, QColor("#019ff4")}, {15, QColor("#0300f4")}, {20, QColor("#02fd02")},
                {30, QColor("#008e00")}, {40, QColor("#fdf802")}, {50, QColor("#fd9500")},
                {55, QColor("#fd0000")}, {65, QColor("#bc00bc")}, {75, QColor("#f8f8f8")},
            }, value, alpha);
        case ProductKind::Velocity:
            return discretePalette({
                {-80, QColor("#640064")}, {-50, QColor("#9b009b")}, {-35, QColor("#003cff")},
                {-20, QColor("#00a0ff")}, {-10, QColor("#00e6c8")}, {-2, QColor("#b8fff0")},
                {2, QColor("#f2f2f2")}, {10, QColor("#ffe000")}, {20, QColor("#ff7800")},
                {35, QColor("#e60000")}, {50, QColor("#960000")},
            }, value, alpha);
        case ProductKind::SpectrumWidth:
            return discretePalette({
                {0, QColor("#202a30")}, {2, QColor("#0060a8")}, {5, QColor("#00a9c7")},
                {8, QColor("#36c56a")}, {12, QColor("#d8d33c")}, {18, QColor("#f28b32")},
                {25, QColor("#db3657")}, {32, QColor("#f0e9ff")},
            }, value, alpha);
        case ProductKind::DifferentialReflectivity:
            return discretePalette({
                {-8, QColor("#4b2678")}, {-3, QColor("#2456b8")}, {-1, QColor("#3fb5ca")},
                {0, QColor("#d9e1df")}, {1, QColor("#f0c74d")}, {2, QColor("#ef8c3d")},
                {4, QColor("#dc3e70")}, {6, QColor("#8b2d91")},
            }, value, alpha);
        case ProductKind::CorrelationCoefficient:
            return discretePalette({
                {0.00, QColor("#4c1647")}, {0.70, QColor("#a72f4f")}, {0.85, QColor("#e06b4f")},
                {0.92, QColor("#d6b74a")}, {0.95, QColor("#7374c8")}, {0.97, QColor("#27a7b6")},
                {0.985, QColor("#91ddd0")}, {0.995, QColor("#f3fbf7")},
            }, value, alpha);
        case ProductKind::DifferentialPhase:
            return discretePalette({
                {-180, QColor("#403078")}, {-120, QColor("#2858a0")}, {-60, QColor("#20a7b0")},
                {0, QColor("#73c95b")}, {45, QColor("#e0cf48")}, {90, QColor("#ed7b45")},
                {135, QColor("#b94399")},
            }, value, alpha);
        case ProductKind::SpecificDifferentialPhase:
            // KDP is signed: a neutral band at 0 keeps light-rain KDP quiet,
            // green/yellow walks the operational heavy-rain range (3-7 deg/km),
            // and magenta/white is reserved for hail-bearing cores. A sequential
            // ramp would paint the negative (hail / melting-layer) gates as if
            // they were weak rain, which is the wrong physical story.
            return discretePalette({
                {-10, QColor("#2b2550")}, {-3, QColor("#33589e")},
                {-0.5, QColor("#7fa7bb")}, {0, QColor("#e2e8ea")},
                {0.5, QColor("#7fd48a")}, {1.5, QColor("#f0d95a")},
                {3, QColor("#ef8a3c")}, {5, QColor("#d94a6a")},
                {8, QColor("#b64aa8")}, {12, QColor("#f0d9ff")},
                {20, QColor("#ffffff")},
            }, value, alpha);
        case ProductKind::EchoTop:
            return discretePalette({
                {0, QColor("#16232b")}, {10, QColor("#1b6a83")}, {20, QColor("#2ebc9a")},
                {30, QColor("#c7dd52")}, {40, QColor("#ffb13b")}, {50, QColor("#ef4b4f")},
                {60, QColor("#c63eb5")}, {70, QColor("#f4f1ff")},
            }, value, alpha);
        case ProductKind::VerticallyIntegratedLiquid:
            return discretePalette({
                {0, QColor("#16232b")}, {5, QColor("#225ea8")}, {10, QColor("#22a6a1")},
                {20, QColor("#67c95c")}, {30, QColor("#e6d64a")}, {40, QColor("#f28b35")},
                {60, QColor("#df365f")}, {80, QColor("#e9e4ff")},
            }, value, alpha);
        case ProductKind::CompositeReflectivity:
        case ProductKind::Cappi:
            // dBZ-derived fields share REF packing by construction, so they
            // share the REF stops gate for gate.
            return discretePalette({
                {-32, QColor("#646464")}, {-30, QColor("#04e9e7")}, {-25, QColor("#00b4d8")},
                {-20, QColor("#019ff4")}, {-10, QColor("#0245f0")}, {0, QColor("#022cd8")},
                {5, QColor("#019ff4")}, {15, QColor("#0300f4")}, {20, QColor("#02fd02")},
                {30, QColor("#008e00")}, {40, QColor("#fdf802")}, {50, QColor("#fd9500")},
                {55, QColor("#fd0000")}, {65, QColor("#bc00bc")}, {75, QColor("#f8f8f8")},
            }, value, alpha);
        }
    }
    switch (product) {
    case ProductKind::Reflectivity:
    case ProductKind::CompositeReflectivity:
    case ProductKind::Cappi:
        return interpolatePalette({
            {0.00, QColor("#102b3a")}, {0.16, QColor("#146b78")},
            {0.30, QColor("#35c7b4")}, {0.44, QColor("#b7e75c")},
            {0.58, QColor("#ffd166")}, {0.72, QColor("#ff704d")},
            {0.86, QColor("#d946a8")}, {1.00, QColor("#f2e9ff")},
        }, normalized, alpha);
    case ProductKind::Velocity:
        return interpolatePalette({
            {0.00, QColor("#38156b")}, {0.18, QColor("#304fc4")},
            {0.36, QColor("#16a6d8")}, {0.48, QColor("#63eee0")},
            {0.495, QColor("#24343b")}, {0.505, QColor("#24343b")},
            {0.52, QColor("#ffe07a")}, {0.66, QColor("#ff9b42")},
            {0.84, QColor("#ef3f55")}, {1.00, QColor("#9e185f")},
        }, normalized, alpha);
    case ProductKind::SpectrumWidth:
        return interpolatePalette({
            {0.00, QColor("#17252d")}, {0.16, QColor("#14637c")},
            {0.32, QColor("#16b6c5")}, {0.48, QColor("#66d36e")},
            {0.64, QColor("#e7d44f")}, {0.78, QColor("#f58a3c")},
            {0.90, QColor("#d9365e")}, {1.00, QColor("#f4eaff")},
        }, normalized, alpha);
    case ProductKind::DifferentialReflectivity:
        return interpolatePalette({
            {0.00, QColor("#392568")}, {0.22, QColor("#3978b8")},
            {0.42, QColor("#54c8bd")}, {0.50, QColor("#dbe4df")},
            {0.66, QColor("#efb45d")}, {0.84, QColor("#e45a78")},
            {1.00, QColor("#9f3a96")},
        }, normalized, alpha);
    case ProductKind::CorrelationCoefficient:
        return interpolatePalette({
            {0.00, QColor("#76264c")}, {0.35, QColor("#d5525c")},
            {0.62, QColor("#e8a957")}, {0.78, QColor("#7779c9")},
            {0.90, QColor("#2eb7bd")}, {0.97, QColor("#a8e4d7")},
            {1.00, QColor("#edf7f2")},
        }, normalized, alpha);
    case ProductKind::DifferentialPhase:
        return interpolatePalette({
            {0.00, QColor("#234c89")}, {0.18, QColor("#2cb5b4")},
            {0.36, QColor("#9cd65f")}, {0.52, QColor("#f2cf58")},
            {0.68, QColor("#ee7454")}, {0.84, QColor("#bd4ca5")},
            {1.00, QColor("#51418d")},
        }, normalized, alpha);
    case ProductKind::SpecificDifferentialPhase:
        // Same KDP story as the Operational stops, interpolated: the fraction
        // is (value + 10) / 30, so 0.333 (neutral) is value 0 and the narrow
        // 0.317..0.367 band keeps the light-rain transition tight.
        return interpolatePalette({
            {0.000, QColor("#2b2550")}, {0.233, QColor("#33589e")},
            {0.317, QColor("#7fa7bb")}, {0.333, QColor("#e2e8ea")},
            {0.367, QColor("#7fd48a")}, {0.433, QColor("#f0d95a")},
            {0.500, QColor("#ef8a3c")}, {0.600, QColor("#d94a6a")},
            {0.733, QColor("#b64aa8")}, {1.000, QColor("#f6ecff")},
        }, normalized, alpha);
    case ProductKind::EchoTop:
        return interpolatePalette({
            {0.00, QColor("#16232b")}, {0.18, QColor("#1f708d")},
            {0.36, QColor("#31bc94")}, {0.54, QColor("#d7dc52")},
            {0.70, QColor("#ff9a3d")}, {0.84, QColor("#e54562")},
            {1.00, QColor("#eee9ff")},
        }, normalized, alpha);
    case ProductKind::VerticallyIntegratedLiquid:
        return interpolatePalette({
            {0.00, QColor("#16232b")}, {0.15, QColor("#2450a4")},
            {0.30, QColor("#20a7ad")}, {0.45, QColor("#69c75c")},
            {0.60, QColor("#e5d24b")}, {0.75, QColor("#ef8538")},
            {0.88, QColor("#d93467")}, {1.00, QColor("#eee9ff")},
        }, normalized, alpha);
    }
    return QColor("#d7e2e6");
}

int wrappedTileX(const int x, const int count) {
    return ((x % count) + count) % count;
}

struct SharedTileStore {
    QNetworkAccessManager* network = nullptr;
    QNetworkDiskCache* diskCache = nullptr;
    QCache<QString, QImage> tiles;
    QSet<QString> pendingKeys;
    QHash<QNetworkReply*, QString> replies;
    QHash<QString, QVector<QPointer<RadarMapCanvas>>> watchers;
    // In-flight base/reference composites, keyed by tile key ("z/x/y").
    struct TileComposite {
        QImage base;
        bool hasBase = false;
        QImage reference;
        bool referenceDone = false;
    };
    QHash<QString, TileComposite> staging;
};

// Close the anonymous namespace: basemapTileUrls is declared in the header
// (external linkage for tests) and must be defined in cappedweather scope.
}  // namespace

BasemapProvider basemapProviderFromInt(const int value) {
    switch (value) {
    case 1: return BasemapProvider::CartoLight;
    case 2: return BasemapProvider::EsriDark;
    case 3: return BasemapProvider::Custom;
    default: return BasemapProvider::CartoDark;
    }
}

QString basemapProviderName(const BasemapProvider provider) {
    switch (provider) {
    case BasemapProvider::CartoLight: return QStringLiteral("CARTO Light");
    case BasemapProvider::EsriDark: return QStringLiteral("Esri Dark Gray (legacy)");
    case BasemapProvider::Custom: return QStringLiteral("Custom template");
    case BasemapProvider::CartoDark:
    default: return QStringLiteral("CARTO Dark");
    }
}

BasemapProvider basemapProviderFromSettings() {
    return basemapProviderFromInt(
        QSettings().value(QStringLiteral("basemap/provider"), 0).toInt());
}

QString basemapApiKeyFromSettings() {
    return QSettings().value(QStringLiteral("basemap/apiKey")).toString().trimmed();
}

namespace {
QString basemapCustomTemplateFromSettings() {
    return QSettings().value(QStringLiteral("basemap/customTemplate")).toString().trimmed();
}

QString basemapCustomAttributionFromSettings() {
    const auto text =
        QSettings().value(QStringLiteral("basemap/customAttribution")).toString().trimmed();
    return text.isEmpty() ? QStringLiteral("© OpenStreetMap contributors") : text;
}
}  // namespace

BasemapTileUrls basemapTileUrls(const int zoom, const int x, const int y,
                                const BasemapProvider provider, const QString& apiKey,
                                const QString& customTemplate) {
    if (provider == BasemapProvider::EsriDark) {
        // Esri tile order is z/y/x — keep the flip in exactly one place.
        return {
            QUrl(QStringLiteral("https://server.arcgisonline.com/ArcGIS/rest/services/Canvas/"
                                "World_Dark_Gray_Base/MapServer/tile/%1/%2/%3")
                     .arg(zoom)
                     .arg(y)
                     .arg(x)),
            QUrl(QStringLiteral("https://server.arcgisonline.com/ArcGIS/rest/services/Canvas/"
                                "World_Dark_Gray_Reference/MapServer/tile/%1/%2/%3")
                     .arg(zoom)
                     .arg(y)
                     .arg(x)),
        };
    }
    if (provider == BasemapProvider::Custom && !customTemplate.isEmpty()) {
        // Deterministic subdomain per tile spreads load without breaking
        // the disk cache (same tile always maps to the same host).
        const auto sub = QStringLiteral("abcd").at((x + y) & 3);
        auto url = customTemplate;
        url.replace(QStringLiteral("{z}"), QString::number(zoom));
        url.replace(QStringLiteral("{x}"), QString::number(x));
        url.replace(QStringLiteral("{y}"), QString::number(y));
        url.replace(QStringLiteral("{s}"), QString(sub));
        url.replace(QStringLiteral("{key}"), apiKey);
        return {QUrl(url), QUrl()};
    }
    // CARTO raster: single layer, standard z/x/y order, key appended so
    // keyless requests do not come back watermarked. An empty key still
    // serves tiles (with CARTO's "API key required" watermark) rather than
    // failing, so a fresh install renders before the user pastes a key.
    const auto style = provider == BasemapProvider::CartoLight ? QStringLiteral("light_all")
                                                               : QStringLiteral("dark_all");
    const auto sub = QStringLiteral("abcd").at((x + y) & 3);
    return {QUrl(QStringLiteral("https://%1.basemaps.cartocdn.com/rastertiles/%2/%3/%4/%5.png?key=%6")
                     .arg(QString(sub), style)
                     .arg(zoom)
                     .arg(x)
                     .arg(y)
                     .arg(apiKey)),
            QUrl()};
}

namespace {
BasemapProvider effectiveProvider(const BasemapProvider selected, const QString& apiKey,
                                  const QString& customTemplate) {
    // No key, no CARTO: keyless CARTO tiles come back watermarked, so fall
    // back to Esri instead of showing branded tiles. Same when the custom
    // template is empty (nothing to substitute). A filled custom template
    // serves without any CARTO key: radar-mosaic presets (IEM, RainViewer)
    // are keyless by design and must not fall back.
    if (selected == BasemapProvider::Custom && customTemplate.isEmpty())
        return BasemapProvider::EsriDark;
    if (apiKey.isEmpty() && (selected == BasemapProvider::CartoDark ||
                             selected == BasemapProvider::CartoLight))
        return BasemapProvider::EsriDark;
    return selected;
}
}  // namespace

BasemapTileUrls basemapTileUrls(const int zoom, const int x, const int y) {
    const auto provider = basemapProviderFromSettings();
    const auto apiKey = basemapApiKeyFromSettings();
    const auto customTemplate = basemapCustomTemplateFromSettings();
    return basemapTileUrls(zoom, x, y, effectiveProvider(provider, apiKey, customTemplate), apiKey,
                           customTemplate);
}

QString basemapAttribution(const BasemapProvider provider, const QString& customAttribution) {
    switch (provider) {
    case BasemapProvider::EsriDark:
        return QStringLiteral("© OpenStreetMap contributors · Esri, HERE, Garmin");
    case BasemapProvider::Custom:
        return customAttribution.isEmpty() ? QStringLiteral("© OpenStreetMap contributors")
                                           : customAttribution;
    case BasemapProvider::CartoLight:
    case BasemapProvider::CartoDark:
    default:
        return QStringLiteral("© OpenStreetMap contributors · © CARTO");
    }
}

QString basemapAttribution() {
    const auto provider = basemapProviderFromSettings();
    const auto apiKey = basemapApiKeyFromSettings();
    const auto customTemplate = basemapCustomTemplateFromSettings();
    if (provider == BasemapProvider::Custom)
        return basemapAttribution(effectiveProvider(provider, apiKey, customTemplate),
                                  basemapCustomAttributionFromSettings());
    return basemapAttribution(effectiveProvider(provider, apiKey, customTemplate), QString());
}

QVector<BasemapPreset> basemapPresets() {
    // Static radar-mosaic templates for the Custom slot. Both verified live
    // (HTTP 200 PNG tiles, Oct 2026): IEM is a fixed XYZ path, RainViewer
    // needs its rotating frame path resolved first via
    // rainviewerTileTemplate(). Neither needs a key.
    return {
        {QStringLiteral("IEM US mosaic"),
         QStringLiteral("https://mesonet.agron.iastate.edu/cache/tile.py/1.0.0/"
                        "nexrad-n0q-900913/{z}/{x}/{y}.png"),
         QStringLiteral("Radar mosaic (c) Iowa Environmental Mesonet")},
    };
}

QString rainviewerTileTemplate(const QByteArray& weatherMapsJson, QString* error) {
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(weatherMapsJson, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error)
            *error = QStringLiteral("Invalid RainViewer metadata: %1").arg(parseError.errorString());
        return {};
    }
    const auto past =
        document.object().value(QStringLiteral("radar")).toObject().value(QStringLiteral("past")).toArray();
    if (past.isEmpty()) {
        if (error) *error = QStringLiteral("RainViewer metadata has no past frames");
        return {};
    }
    // Latest frame is last; paths are hash-based (timestamps went 410 Gone
    // in 2026), so the host + path must come from the payload, never be
    // reconstructed.
    const auto frame = past.at(past.size() - 1).toObject();
    const auto host = document.object().value(QStringLiteral("host")).toString();
    const auto path = frame.value(QStringLiteral("path")).toString();
    if (host.isEmpty() || path.isEmpty() || !path.startsWith(QStringLiteral("/"))) {
        if (error) *error = QStringLiteral("RainViewer frame is missing host or path");
        return {};
    }
    if (error) error->clear();
    return host + path + QStringLiteral("/256/{z}/{x}/{y}/2/1_1.png");
}

// Same linkage treatment as basemapTileUrls above: declared in the header,
// so the definition must live in cappedweather scope, outside the anonymous
// namespace that owns g_hudScale's helpers.
void setHudFontScale(const double scale) {
    g_hudScale = std::clamp(scale, 0.8, 1.5);
}

namespace {

void watchTile(SharedTileStore& store, const QString& key, RadarMapCanvas* canvas) {
    auto& watchers = store.watchers[key];
    const auto alreadyWatching = std::any_of(watchers.cbegin(), watchers.cend(),
                                              [canvas](const auto& watcher) {
                                                  return watcher == canvas;
                                              });
    if (!alreadyWatching) watchers.push_back(canvas);
}

SharedTileStore& tileStore() {
    static auto* store = [] {
        auto* result = new SharedTileStore;
        const QSettings settings;
        result->tiles.setMaxCost(std::clamp(
            settings.value(QStringLiteral("cache/memoryMbPerPanel"), 32).toInt(), 8, 256) * 1024 * 1024);
        result->network = new QNetworkAccessManager(qApp);
        result->diskCache = new QNetworkDiskCache(result->network);
        result->diskCache->setCacheDirectory(
            QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/map-tiles/shared"));
        result->diskCache->setMaximumCacheSize(std::clamp<qint64>(
            settings.value(QStringLiteral("cache/diskMbPerPanel"), 64).toLongLong(), 16, 1024) * 1024LL * 1024LL);
        result->network->setCache(result->diskCache);
        QObject::connect(result->network, &QNetworkAccessManager::finished,
                         result->network, [result](QNetworkReply* reply) {
            const auto subKey = result->replies.take(reply);
            if (subKey.isEmpty()) {
                reply->deleteLater();
                return;
            }
            const bool isReference = subKey.endsWith(QStringLiteral("@ref"));
            const auto key =
                isReference ? subKey.left(subKey.size() - 4) : subKey;
            result->pendingKeys.remove(subKey);
            // Auto-detect the image format: the base layer serves JPEG, the
            // reference layer transparent PNG. Forcing "PNG" breaks JPEG.
            const auto image = reply->error() == QNetworkReply::NoError
                                   ? QImage::fromData(reply->readAll())
                                   : QImage();
            reply->deleteLater();
            auto& composite = result->staging[key];
            if (isReference) {
                if (!image.isNull()) composite.reference = image;
                composite.referenceDone = true;
                if (!composite.hasBase && !result->watchers.contains(key)) {
                    // Base already failed and watchers were released; this
                    // late reference has nothing to composite onto.
                    result->staging.remove(key);
                    return;
                }
            } else {
                if (!image.isNull()) {
                    composite.base = image;
                    composite.hasBase = true;
                } else {
                    // Base failed: nothing to composite onto, drop the staging
                    // entry (the reference outcome is irrelevant) and wake the
                    // watchers so they repaint without this tile.
                    result->staging.remove(key);
                    const auto watchers = result->watchers.take(key);
                    for (const auto& canvas : watchers)
                        if (canvas && canvas->isVisible()) canvas->update();
                    return;
                }
            }
            // Publish once the base is in and the reference has resolved
            // (image or failure — a failed reference must not block the base).
            if (!composite.hasBase || !composite.referenceDone) return;
            auto merged = composite.base;
            if (!composite.reference.isNull()) {
                QPainter painter(&merged);
                painter.drawImage(0, 0, composite.reference);
            }
            const auto cost = static_cast<int>(std::min<qsizetype>(
                merged.sizeInBytes(), std::numeric_limits<int>::max()));
            result->tiles.insert(key, new QImage(merged), std::max(1, cost));
            result->staging.remove(key);
            const auto watchers = result->watchers.take(key);
            for (const auto& canvas : watchers)
                if (canvas && canvas->isVisible()) canvas->update();
        });
        return result;
    }();
    return *store;
}

struct VelocityCacheEntry {
    std::weak_ptr<const DecodedChunk> chunk;
    std::array<int, 4> parameters{};
    std::weak_ptr<const ProcessedVelocity> processed;
    std::shared_future<std::shared_ptr<const ProcessedVelocity>> pending;
};

std::mutex velocityCacheMutex;
std::vector<VelocityCacheEntry> velocityCache;

std::array<int, 4> velocityCacheParameters(const VelocityProcessingOptions& options) {
    return {static_cast<int>(options.mode),
            static_cast<int>(std::lround(options.nyquistVelocityMetersPerSecond * 10.0)),
            static_cast<int>(std::lround(options.stormMotionDirectionDegrees * 10.0)),
            static_cast<int>(std::lround(options.stormMotionSpeedMetersPerSecond * 10.0))};
}

bool sameOwner(const std::weak_ptr<const DecodedChunk>& cached,
               const std::shared_ptr<const DecodedChunk>& chunk) {
    const std::owner_less<void> less;
    return !less(cached, chunk) && !less(chunk, cached);
}

std::shared_ptr<const ProcessedVelocity> sharedProcessedVelocity(
    const std::shared_ptr<const DecodedChunk>& chunk, const VelocityProcessingOptions& options) {
    const auto parameters = velocityCacheParameters(options);
    std::shared_future<std::shared_ptr<const ProcessedVelocity>> pending;
    std::shared_ptr<std::promise<std::shared_ptr<const ProcessedVelocity>>> producer;
    {
        const std::scoped_lock lock(velocityCacheMutex);
        std::erase_if(velocityCache, [](const auto& entry) {
            return entry.chunk.expired() && !entry.pending.valid();
        });
        const auto existing = std::find_if(velocityCache.begin(), velocityCache.end(),
                                           [&chunk, &parameters](const auto& entry) {
                                               return entry.parameters == parameters &&
                                                      sameOwner(entry.chunk, chunk);
                                           });
        if (existing != velocityCache.end()) {
            if (const auto processed = existing->processed.lock()) return processed;
            if (existing->pending.valid()) pending = existing->pending;
        }
        if (!pending.valid()) {
            producer = std::make_shared<std::promise<std::shared_ptr<const ProcessedVelocity>>>();
            pending = producer->get_future().share();
            if (existing != velocityCache.end()) existing->pending = pending;
            else velocityCache.push_back({chunk, parameters, {}, pending});
        }
    }
    if (!producer) return pending.get();
    auto processed = std::make_shared<const ProcessedVelocity>(processVelocity(*chunk, options));
    {
        const std::scoped_lock lock(velocityCacheMutex);
        const auto existing = std::find_if(velocityCache.begin(), velocityCache.end(),
                                           [&chunk, &parameters](const auto& entry) {
                                               return entry.parameters == parameters &&
                                                      sameOwner(entry.chunk, chunk);
                                           });
        if (existing != velocityCache.end()) {
            existing->processed = processed;
            existing->pending = {};
        }
        if (velocityCache.size() > 64) {
            std::erase_if(velocityCache, [](const auto& entry) {
                return entry.chunk.expired() && !entry.pending.valid();
            });
        }
    }
    producer->set_value(processed);
    return processed;
}
}  // namespace

void RadarMapCanvas::clearSharedTileCache() {
    auto& store = tileStore();
    store.tiles.clear();
    store.staging.clear();
    store.pendingKeys.clear();
    // In-flight replies carry the old endpoint's bytes: abort them and drop
    // the routing so their finish handlers release watchers instead of
    // publishing stale tiles under the new provider.
    const auto inFlight = store.replies.keys();
    store.replies.clear();
    store.watchers.clear();
    for (auto* reply : inFlight) {
        if (reply) {
            reply->abort();
            reply->deleteLater();
        }
    }
}

RadarMapCanvas::RadarMapCanvas(QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("radarMap"));
    setMinimumSize(300, 220);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    // Touchscreens pan via synthesized mouse events already; pinch needs an
    // explicit opt-in or Qt never delivers gesture events to this widget.
    setAttribute(Qt::WA_AcceptTouchEvents);
    grabGesture(Qt::PinchGesture);
    setAccessibleName(QStringLiteral("Radar map panel"));
    setAccessibleDescription(
        QStringLiteral("Live Level II radar. Arrow keys step frames, plus and minus zoom, "
                       "Escape closes cards and cancels tools."));
    loadingTimer_ = new QTimer(this);
    loadingTimer_->setInterval(80);
    connect(loadingTimer_, &QTimer::timeout, this, [this] {
        loadingAngle_ = (loadingAngle_ + 18) % 360;
        // Repaint only the chip. The old 300 px-wide rect sat on top of the
        // legend's report and analysis rows, so the spinner forced a full
        // legend repaint 12.5x a second for the whole download.
        const auto chipWidth = std::min(282.0, std::max(120.0, width() - 24.0));
        const double chipX = std::max(12.0, width() - chipWidth - 12.0);
        update(QRectF(chipX - 2.0, 62.0, chipWidth + 4.0, 44.0).toAlignedRect());
    });
    probeTimer_ = new QTimer(this);
    probeTimer_->setSingleShot(true);
    probeTimer_->setInterval(16);
    connect(probeTimer_, &QTimer::timeout, this, [this] {
        if (!cursorInside_) {
            // The cursor left before the debounce elapsed (fast transit
            // across panels): never resurrect a probe nobody is hovering.
            if (pinnedInfo_.isEmpty()) probeText_.clear();
            update();
            return;
        }
        hoverRadarId_ = radarAt(pendingProbePosition_);
        updateProbe(pendingProbePosition_);
        // Linked cross-panel readout: broadcast the sampled geography (not
        // the value — each panel samples its own product/volume). Emitting
        // the raw position keeps products honest across REF/VEL panels.
        if (pinnedInfo_.isEmpty()) {
            const auto geographic = geographicPoint(pendingProbePosition_);
            emit probeMoved(QPointF(geographic.x(), geographic.y()));
        }
    });
    pinnedInfoScrollTimer_ = new QTimer(this);
    pinnedInfoScrollTimer_->setTimerType(Qt::PreciseTimer);
    pinnedInfoScrollTimer_->setInterval(16);
    connect(pinnedInfoScrollTimer_, &QTimer::timeout, this, [this] {
        const auto distance = pinnedInfoScrollTarget_ - pinnedInfoScrollOffset_;
        if (std::abs(distance) < 0.2) {
            pinnedInfoScrollOffset_ = pinnedInfoScrollTarget_;
            pinnedInfoScrollTimer_->stop();
        } else {
            pinnedInfoScrollOffset_ += distance * 0.32;
        }
        update(pinnedInfoCardRect_.adjusted(-2.0, -2.0, 2.0, 2.0).toAlignedRect());
    });
}

RadarMapCanvas::~RadarMapCanvas() = default;

void RadarMapCanvas::setSites(std::vector<RadarSite> sites) {
    sites_ = std::move(sites);
    siteIndexes_.clear();
    for (std::size_t index = 0; index < sites_.size(); ++index)
        siteIndexes_.insert(QString::fromStdString(sites_[index].id), index);
    siteWorldZoom_ = -1;
    invalidateOverlay(true);
    invalidateLightningFusion();
    update();
}

void RadarMapCanvas::setSelectedRadar(const QString& radarId) {
    const auto changed = selectedRadar_ != radarId;
    selectedRadar_ = radarId;
    if (changed) {
        chunk_.reset();
        invalidateOverlay();
        invalidateLightningFusion();
        stormTracker_.clear();
        // Fresh radar, fresh storms: persistence must not carry markers over.
        circulationTracks_.clear();
        circulations_.clear();
        tdsTracks_.clear();
        tdsDetections_.clear();
        hailCores_.clear();
        hailSwaths_.clear();
        brightBand_ = BrightBandEstimate{};
        dataTime_ = {};
        probeText_.clear();
        const auto* site = selectedSite();
        if (!site || !site->latitude || !site->longitude) {
            update();
            return;
        }
        center_ = {site->longitude.value(), site->latitude.value()};
        zoom_ = 6;
        invalidateWorldCaches(true);
    }
    update();
}

void RadarMapCanvas::setProduct(const ProductKind product) {
    product_ = product;
    if (isDerivedProduct(product_)) {
        elevationFilter_.reset();
        setCrossSectionMode(false);
    }
    refreshVelocityProcessing();
    rebuildProbeIndex();
    invalidateOverlay(true);
    refreshScanDiagnostics();
    if (volume3d_) rebuildVolumeRender();
    update();
}

void RadarMapCanvas::setElevationFilter(const std::optional<int> elevationNumber) {
    elevationFilter_ = elevationNumber;
    rebuildProbeIndex();
    invalidateOverlay();
    refreshScanDiagnostics();
    update();
}

void RadarMapCanvas::setDecodedChunk(DecodedChunk chunk, const QDateTime& dataTime) {
    setDecodedChunk(std::make_shared<DecodedChunk>(std::move(chunk)), dataTime);
}

void RadarMapCanvas::setDecodedChunk(std::shared_ptr<const DecodedChunk> chunk, const QDateTime& dataTime) {
    chunk_ = std::move(chunk);
    dataTime_ = dataTime;
    invalidateStatusText();
    invalidateOverlay();
    invalidateLightningFusion();
    refreshVelocityProcessing();
    rebuildProbeIndex();
    refreshScanDiagnostics();
    setLoading(false);
    if (volume3d_) rebuildVolumeRender();
    update();
}

void RadarMapCanvas::setWeatherOverlays(QVector<WeatherWarning> warnings,
                                        QVector<StormReport> reports) {
    warnings_ = std::move(warnings);
    stormReports_ = std::move(reports);
    ++weatherGeneration_;
    update();
}

void RadarMapCanvas::setPlacefiles(QVector<PlacefileDocument> documents) {
    placefiles_ = std::move(documents);
    if (pinnedInfoKind_ == PinnedInfoKind::Placefile) {
        pinnedInfoKind_ = PinnedInfoKind::None;
        pinnedInfo_.clear();
    }
    update();
}

void RadarMapCanvas::setPlacefilesVisible(const bool visible) {
    if (placefilesVisible_ == visible) return;
    placefilesVisible_ = visible;
    if (!visible && pinnedInfoKind_ == PinnedInfoKind::Placefile)
        setPinnedOverlay({}, probePosition_);
    update();
}

void RadarMapCanvas::setLightningCells(std::vector<LightningCellSnapshot> cells) {
    lightningCells_ = std::move(cells);
    invalidateLightningFusion();
    refreshPinnedLightningInfo();
    update();
}

void RadarMapCanvas::setLightningVisible(const bool visible) {
    if (lightningVisible_ == visible) return;
    lightningVisible_ = visible;
    if (!visible && pinnedInfoKind_ == PinnedInfoKind::Lightning) {
        setPinnedOverlay({}, probePosition_);
    }
    update();
}

void RadarMapCanvas::setLightningFusionVisible(const bool visible) {
    if (lightningFusionVisible_ == visible) return;
    lightningFusionVisible_ = visible;
    update();
}

void RadarMapCanvas::setShearVisible(const bool visible) {
    if (shearVisible_ == visible) return;
    shearVisible_ = visible;
    update();
}

void RadarMapCanvas::setTvsVisible(const bool visible) {
    if (tvsVisible_ == visible) return;
    tvsVisible_ = visible;
    update();
}

int RadarMapCanvas::circulationCount() const {
    return static_cast<int>(circulations_.size());
}

void RadarMapCanvas::setHailVisible(const bool visible) {
    if (hailVisible_ == visible) return;
    hailVisible_ = visible;
    update();
}

int RadarMapCanvas::hailCoreCount() const {
    return static_cast<int>(hailCores_.size());
}

std::vector<HailSwath> RadarMapCanvas::hailSwaths() const {
    return hailSwaths_.swaths();
}

void RadarMapCanvas::setZdrVisible(const bool visible) {
    if (zdrVisible_ == visible) return;
    zdrVisible_ = visible;
    update();
}

int RadarMapCanvas::zdrColumnCount() const {
    return static_cast<int>(zdrColumns_.size());
}

void RadarMapCanvas::setTbssVisible(const bool visible) {
    if (tbssVisible_ == visible) return;
    tbssVisible_ = visible;
    update();
}

int RadarMapCanvas::tbssCount() const {
    return static_cast<int>(tbssDetections_.size());
}

void RadarMapCanvas::setTdsVisible(const bool visible) {
    if (tdsVisible_ == visible) return;
    tdsVisible_ = visible;
    update();
}

int RadarMapCanvas::tdsCount() const {
    return static_cast<int>(tdsDetections_.size());
}

void RadarMapCanvas::setStormTrailVisible(const bool visible) {
    if (stormTrailVisible_ == visible) return;
    stormTrailVisible_ = visible;
    update();
}

bool RadarMapCanvas::hasStormTracks() const {
    for (const auto& track : stormTracker_.tracks()) {
        if (track.fixes.size() >= 2) return true;
    }
    return false;
}

void RadarMapCanvas::setDisplaySmoothing(const bool enabled) {
    if (displaySmoothing_ == enabled) return;
    displaySmoothing_ = enabled;
    invalidateOverlay();
    update();
}

QString RadarMapCanvas::scanDiagnosticsText() const {
    if (qualityText_.isEmpty() && scanQualityText_.isEmpty()) return {};
    if (qualityText_.isEmpty()) return scanQualityText_;
    if (scanQualityText_.isEmpty()) return qualityText_;
    return qualityText_ + QStringLiteral(" | ") + scanQualityText_;
}

int RadarMapCanvas::lightningCellCount() const {
    return static_cast<int>(lightningCells_.size());
}

int RadarMapCanvas::radarFeatureCount() const {
    return static_cast<int>(radarFeatures_.size());
}

int RadarMapCanvas::lightningFusionCount() const {
    return static_cast<int>(lightningAssociations_.size());
}

void RadarMapCanvas::invalidateLightningFusion() {
    ++lightningFusionGeneration_;
    radarFeatures_.clear();
    lightningAssociations_.clear();
    hailCores_.clear();
    zdrColumns_.clear();
    tbssDetections_.clear();
    brightBand_ = BrightBandEstimate{};
    tdsDetections_.clear();
    lightningFusionComputedGeneration_ = 0;
}

void RadarMapCanvas::rebuildLightningFusion() {
    if (lightningFusionBuildPending_ ||
        lightningFusionComputedGeneration_ == lightningFusionGeneration_) return;
    const auto* site = selectedSite();
    if (!site || !site->latitude || !site->longitude || !chunk_) {
        lightningFusionComputedGeneration_ = lightningFusionGeneration_;
        return;
    }
    lightningFusionBuildPending_ = true;
    const auto generation = lightningFusionGeneration_;
    const auto chunk = chunk_;
    const auto cells = lightningCells_;
    const auto latitude = *site->latitude;
    const auto longitude = *site->longitude;
    const auto antennaElevM = site->antennaElevationMeters;
    const auto nyquist = velocityOptions_.nyquistVelocityMetersPerSecond;
    const QPointer<RadarMapCanvas> guard(this);
    QThreadPool::globalInstance()->start(
        [guard, generation, chunk, cells, latitude, longitude, antennaElevM, nyquist] {
            // TVS / meso detection on dealiased velocity at the panel nyquist.
            // Independent of the display product: markers must survive while
            // the user looks at reflectivity. ROT shear objects stay on native
            // (folded) velocities: dealiased spans hit 60-200 m/s on real
            // volumes (adjacent rays unfold to different intervals).
            VelocityProcessingOptions detectOptions;
            detectOptions.mode = VelocityProcessingMode::Dealiased;
            detectOptions.nyquistVelocityMetersPerSecond = nyquist;
            auto dealiased = sharedProcessedVelocity(chunk, detectOptions);
            auto features = extractRadarStormFeatures(*chunk, latitude, longitude);
            auto associations = associateLightningWithRadar(cells, features);
            std::vector<ShearWatchCandidate> watch;
            auto circulations = dealiased ? detectCirculations(*chunk, *dealiased, latitude,
                                                               longitude, antennaElevM,
                                                               CirculationConfig{}, nullptr, &watch)
                                          : std::vector<CirculationDetection>{};
            // Hail + debris ride the same worker: VIL/ET18 arrive on the
            // published chunk from the derived-products pass, and TDS hangs
            // off the raw (unpersisted) couplets. Display persistence for
            // TDS happens on the GUI thread below, next to circulations.
            auto hailCores = detectHailCores(*chunk, latitude, longitude, antennaElevM);
            // ZDR columns ride free: same worker, same site fix, dual-pol
            // volumes only (single-pol rays never enter the candidate set).
            auto zdrColumns = detectZdrColumns(*chunk, latitude, longitude, antennaElevM);
            // TBSS rides free too: same worker, reflectivity-only geometry
            // (works on single-pol OPERA volumes unlike ZDR columns).
            auto tbss = detectTbss(*chunk, latitude, longitude, antennaElevM);
            // Melting level for the precip-type rings: same worker, same fix.
            auto brightBand = estimateBrightBand(*chunk, antennaElevM);
            auto tds = dealiased ? detectDebrisSignatures(*chunk, circulations, latitude,
                                                           longitude, antennaElevM)
                                 : std::vector<TdsDetection>{};
            if (!guard) return;
            QMetaObject::invokeMethod(
                guard,
                [guard, generation, features = std::move(features),
                 associations = std::move(associations),
                 circulations = std::move(circulations),
                 watch = std::move(watch), hailCores = std::move(hailCores),
                 zdrColumns = std::move(zdrColumns), tbss = std::move(tbss),
                 brightBand = std::move(brightBand),
                 tds = std::move(tds)]() mutable {
                    if (!guard) return;
                    guard->lightningFusionBuildPending_ = false;
                    if (generation == guard->lightningFusionGeneration_) {
                        guard->radarFeatures_ = std::move(features);
                        guard->lightningAssociations_ = std::move(associations);
                        guard->shearWatch_ = std::move(watch);
                        guard->persistCirculations(std::move(circulations));
                        guard->hailCores_ = std::move(hailCores);
                        guard->zdrColumns_ = std::move(zdrColumns);
                        guard->tbssDetections_ = std::move(tbss);
                        guard->brightBand_ = std::move(brightBand);
                        guard->persistTds(std::move(tds));
                        guard->lightningFusionComputedGeneration_ = generation;
                        guard->refreshPinnedLightningInfo();
                // Per-storm tracks: fold the whole feature set into the
                // tracker (newest volume only — the tracker itself ignores
                // older-or-equal updates, so timeline scrubbing can never
                // corrupt the tracks).
                if (guard->dataTime_.isValid()) {
                    guard->stormTracker_.update(
                        guard->radarFeatures_,
                        static_cast<double>(guard->dataTime_.toSecsSinceEpoch()));
                    // Hail swaths accumulate here: same tracks, same cores,
                    // same epoch, so the footprint follows the storm.
                    guard->hailSwaths_.update(
                        guard->stormTracker_.tracks(), guard->hailCores_,
                        static_cast<double>(guard->dataTime_.toSecsSinceEpoch()));
                }
            }
            guard->update();
        }, Qt::QueuedConnection);
    });
}

void RadarMapCanvas::persistCirculations(std::vector<CirculationDetection> fresh) {
    // Time continuity: a detection must repeat across volumes before it draws
    // (kMinHitsToShow), so single-volume dealiasing glitches never become a
    // TVS marker. Matches are nearest-within-gate; kinds may evolve MESO->TVS
    // as the couplet intensifies.
    constexpr double kMatchKm = 8.0;
    constexpr int kMinHitsToShow = 2;
    constexpr int kMaxMisses = 2;
    constexpr std::size_t kMaxTracks = 24;
    std::vector<char> used(fresh.size(), false);
    for (auto& track : circulationTracks_) {
        double best = -1.0;
        double bestDist = 0.0;
        for (std::size_t i = 0; i < fresh.size(); ++i) {
            if (used[i]) continue;
            const double dist =
                haversineMeters(track.detection.latitudeDeg, track.detection.longitudeDeg,
                                fresh[i].latitudeDeg, fresh[i].longitudeDeg);
            if (dist / 1000.0 <= kMatchKm && (best < 0.0 || dist < bestDist)) {
                best = static_cast<double>(i);
                bestDist = dist;
            }
        }
        if (best >= 0.0) {
            track.detection = std::move(fresh[static_cast<std::size_t>(best)]);
            track.hits += 1;
            track.misses = 0;
            used[static_cast<std::size_t>(best)] = true;
        } else {
            track.misses += 1;
        }
    }
    std::erase_if(circulationTracks_,
                  [](const PersistedCirculation& track) { return track.misses > kMaxMisses; });
    for (std::size_t i = 0; i < fresh.size(); ++i) {
        if (!used[i]) circulationTracks_.push_back({std::move(fresh[i]), 1, 0});
    }
    while (circulationTracks_.size() > kMaxTracks) {
        const auto victim =
            std::min_element(circulationTracks_.begin(), circulationTracks_.end(),
                             [](const PersistedCirculation& a, const PersistedCirculation& b) {
                                 if (a.hits != b.hits) return a.hits < b.hits;
                                 return a.misses > b.misses;
                             });
        circulationTracks_.erase(victim);
    }
    circulations_.clear();
    for (const auto& track : circulationTracks_) {
        if (track.hits >= kMinHitsToShow) circulations_.push_back(track.detection);
    }
    std::sort(circulations_.begin(), circulations_.end(), [](const auto& a, const auto& b) {
        return a.maxDeltaVelocityMetersPerSecond > b.maxDeltaVelocityMetersPerSecond;
    });
    emit circulationsUpdated(circulations_, selectedRadar_);
}

void RadarMapCanvas::persistTds(std::vector<TdsDetection> fresh) {
    // Same 2-hit rule as circulations: a one-volume CC dip is hail or
    // clutter until it repeats at the same place.
    constexpr double kMatchKm = 8.0;
    constexpr int kMinHitsToShow = 2;
    constexpr int kMaxMisses = 2;
    constexpr std::size_t kMaxTracks = 12;
    std::vector<char> used(fresh.size(), false);
    for (auto& track : tdsTracks_) {
        double best = -1.0;
        double bestDist = 0.0;
        for (std::size_t i = 0; i < fresh.size(); ++i) {
            if (used[i]) continue;
            const double dist =
                haversineMeters(track.detection.latitudeDeg, track.detection.longitudeDeg,
                                fresh[i].latitudeDeg, fresh[i].longitudeDeg);
            if (dist / 1000.0 <= kMatchKm && (best < 0.0 || dist < bestDist)) {
                best = static_cast<double>(i);
                bestDist = dist;
            }
        }
        if (best >= 0.0) {
            track.detection = std::move(fresh[static_cast<std::size_t>(best)]);
            track.hits += 1;
            track.misses = 0;
            used[static_cast<std::size_t>(best)] = true;
        } else {
            track.misses += 1;
        }
    }
    std::erase_if(tdsTracks_,
                  [](const PersistedTds& track) { return track.misses > kMaxMisses; });
    for (std::size_t i = 0; i < fresh.size(); ++i) {
        if (!used[i]) tdsTracks_.push_back({std::move(fresh[i]), 1, 0});
    }
    while (tdsTracks_.size() > kMaxTracks) {
        const auto victim =
            std::min_element(tdsTracks_.begin(), tdsTracks_.end(),
                             [](const PersistedTds& a, const PersistedTds& b) {
                                 if (a.hits != b.hits) return a.hits < b.hits;
                                 return a.misses > b.misses;
                             });
        tdsTracks_.erase(victim);
    }
    tdsDetections_.clear();
    for (const auto& track : tdsTracks_) {
        if (track.hits >= kMinHitsToShow) tdsDetections_.push_back(track.detection);
    }
    std::sort(tdsDetections_.begin(), tdsDetections_.end(), [](const auto& a, const auto& b) {
        return a.minCorrelationCoefficient < b.minCorrelationCoefficient;
    });
}

void RadarMapCanvas::setWarningsVisible(const bool visible) {
    if (warningsVisible_ == visible) return;
    warningsVisible_ = visible;
    ++weatherGeneration_;
    weatherOverlayImage_ = {};
    weatherOverlayZoom_ = -1;
    update();
}

void RadarMapCanvas::setStormReportsVisible(const bool visible) {
    if (stormReportsVisible_ == visible) return;
    stormReportsVisible_ = visible;
    ++weatherGeneration_;
    weatherOverlayImage_ = {};
    weatherOverlayZoom_ = -1;
    update();
}

void RadarMapCanvas::setSpcOutlooks(QVector<SpcOutlookFeature> outlooks) {
    spcOutlooks_ = std::move(outlooks);
    update();
}

void RadarMapCanvas::setSpcOutlookVisible(const bool visible) {
    if (spcOutlookVisible_ == visible) return;
    spcOutlookVisible_ = visible;
    update();
}

int RadarMapCanvas::spcOutlookCount() const {
    return static_cast<int>(spcOutlooks_.size());
}

void RadarMapCanvas::setSpcFireOutlooks(QVector<SpcOutlookFeature> outlooks) {
    spcFireOutlooks_ = std::move(outlooks);
    update();
}

void RadarMapCanvas::setSpcFireVisible(const bool visible) {
    if (spcFireVisible_ == visible) return;
    spcFireVisible_ = visible;
    update();
}

int RadarMapCanvas::spcFireCount() const {
    return static_cast<int>(spcFireOutlooks_.size());
}

void RadarMapCanvas::setNhcStorms(QVector<NhcStorm> storms) {
    nhcStorms_ = std::move(storms);
    update();
}

void RadarMapCanvas::setNhcVisible(const bool visible) {
    if (nhcVisible_ == visible) return;
    nhcVisible_ = visible;
    update();
}

int RadarMapCanvas::nhcStormCount() const {
    return static_cast<int>(nhcStorms_.size());
}

void RadarMapCanvas::setWpcOutlooks(QVector<SpcOutlookFeature> outlooks) {
    wpcOutlooks_ = std::move(outlooks);
    update();
}

void RadarMapCanvas::setWpcVisible(const bool visible) {
    if (wpcVisible_ == visible) return;
    wpcVisible_ = visible;
    update();
}

int RadarMapCanvas::wpcOutlookCount() const {
    return static_cast<int>(wpcOutlooks_.size());
}

void RadarMapCanvas::setHmsSmoke(QVector<HmsSmokePlume> plumes) {
    hmsPlumes_ = std::move(plumes);
    update();
}

void RadarMapCanvas::setHmsVisible(const bool visible) {
    if (hmsVisible_ == visible) return;
    hmsVisible_ = visible;
    update();
}

int RadarMapCanvas::hmsSmokeCount() const {
    return static_cast<int>(hmsPlumes_.size());
}

void RadarMapCanvas::setHmsFires(QVector<HmsFirePoint> fires) {
    hmsFires_ = std::move(fires);
    update();
}

int RadarMapCanvas::hmsFireCount() const {
    return static_cast<int>(hmsFires_.size());
}

void RadarMapCanvas::setDatPaths(QVector<DatPath> paths) {
    datPaths_ = std::move(paths);
    update();
}

void RadarMapCanvas::setDatVisible(const bool visible) {
    if (datVisible_ == visible) return;
    datVisible_ = visible;
    update();
}

int RadarMapCanvas::datPathCount() const {
    return static_cast<int>(datPaths_.size());
}

void RadarMapCanvas::setHrrrFrame(std::optional<HrrrFrame> frame) {
    hrrrFrame_ = std::move(frame);
    update();
}

void RadarMapCanvas::setHrrrVisible(const bool visible) {
    if (hrrrVisible_ == visible) return;
    hrrrVisible_ = visible;
    update();
}

void RadarMapCanvas::setGoesFrame(std::optional<GoesFrame> frame) {
    goesFrame_ = std::move(frame);
    goesGeo_.reset();
    if (goesFrame_.has_value()) requestGoesGeo();
    update();
}

void RadarMapCanvas::setGoesVisible(const bool visible) {
    if (goesVisible_ == visible) return;
    goesVisible_ = visible;
    // Opt-out guard: enabling with a frame but no grid kicks the shared
    // build; a hidden layer never starts projection work.
    if (visible && goesFrame_.has_value() && !goesGeo_) requestGoesGeo();
    update();
}

bool RadarMapCanvas::goesGeoReady() const {
    return !goesFrame_.has_value() || goesGeo_ != nullptr;
}

void RadarMapCanvas::requestGoesGeo() {
    if (!goesFrame_.has_value() || goesGeo_) return;
    const GoesGeoIdentity identity = GoesGeoIdentity::from(*goesFrame_);
    if (auto hit = GoesGeoCache::find(identity)) {
        attachGoesGeo(std::move(hit), identity);
        return;
    }
    // Disabled layers never queue projection work; enabling later hits the
    // shared entry (or queues then) through setGoesVisible.
    if (!goesVisible_) return;
    // Coalesced off-thread build: one worker per identity no matter how
    // many panels request it; every waiter attaches only if its canvas
    // still shows that identity (rapid swaps, disable, close and nullopt
    // all fail safe). QPointer is touched on the GUI thread only: the
    // worker sees just the identity, delivery runs as an app-thread
    // functor where posted events die with their receiver.
    {
        QMutexLocker lock(&goesGeoPendingMutex());
        auto& waiters = goesGeoPending()[identity];
        waiters.push_back(QPointer<RadarMapCanvas>(this));
        if (waiters.size() > 1) return;
    }
    QThreadPool::globalInstance()->start([identity] {
        auto data = GoesGeoCache::build(identity);
        GoesGeoCache::insert(identity, data);
        QObject* app = QCoreApplication::instance();
        if (!app) return;
        QMetaObject::invokeMethod(
            app,
            [identity, data = std::move(data)] {
                QMutexLocker lock(&goesGeoPendingMutex());
                // NOTE: operate on the live map, never a copy: erasing a
                // copy leaves a stale entry that dead-ends every later
                // same-identity request (no worker ever spawns again).
                auto pendingIt = goesGeoPending().find(identity);
                if (pendingIt == goesGeoPending().end()) return;
                const auto waiters = *pendingIt;
                goesGeoPending().erase(pendingIt);
                lock.unlock();
                for (QPointer<RadarMapCanvas> target : waiters) {
                    if (target) target->attachGoesGeo(data, identity);
                }
            },
            Qt::QueuedConnection);
    });
}

void RadarMapCanvas::attachGoesGeo(std::shared_ptr<const GoesGeoData> data,
                                   GoesGeoIdentity identity) {    // Identity, not generation: a stale task for a re-selected frame still
    // matches (same grid), while a swapped frame/channel/grid never does.
    if (!goesFrame_.has_value() || !(GoesGeoIdentity::from(*goesFrame_) == identity)) return;
    goesGeo_ = std::move(data);
    update();
}

QMutex& RadarMapCanvas::goesGeoPendingMutex() {
    static QMutex mutex;
    return mutex;
}

QHash<GoesGeoIdentity, RadarMapCanvas::GoesGeoWaiters>& RadarMapCanvas::goesGeoPending() {
    static QHash<GoesGeoIdentity, GoesGeoWaiters> pending;
    return pending;
}

void RadarMapCanvas::setMosaicFrame(std::optional<MosaicFrame> frame) {
    mosaicFrame_ = std::move(frame);
    update();
}

void RadarMapCanvas::setMosaicVisible(const bool visible) {
    if (mosaicVisible_ == visible) return;
    mosaicVisible_ = visible;
    update();
}

void RadarMapCanvas::setMcds(QVector<SpcMcd> mcds) {
    mcds_ = std::move(mcds);
    update();
}

void RadarMapCanvas::setMcdVisible(const bool visible) {
    if (mcdVisible_ == visible) return;
    mcdVisible_ = visible;
    update();
}

int RadarMapCanvas::mcdCount() const {
    return static_cast<int>(mcds_.size());
}

void RadarMapCanvas::setMetars(QVector<MetarObs> metars) {
    metars_ = std::move(metars);
    update();
}

void RadarMapCanvas::setMetarVisible(const bool visible) {
    if (metarVisible_ == visible) return;
    metarVisible_ = visible;
    update();
}

int RadarMapCanvas::metarCount() const {
    return static_cast<int>(metars_.size());
}

void RadarMapCanvas::setStormReportKindVisible(const StormReportKind kind, const bool visible) {
    if (stormReportKindsVisible_.at(static_cast<std::size_t>(kind)) == visible) return;
    stormReportKindsVisible_.at(static_cast<std::size_t>(kind)) = visible;
    ++weatherGeneration_;
    weatherOverlayImage_ = {};
    weatherOverlayZoom_ = -1;
    update();
}

void RadarMapCanvas::setWarningFilter(const QString& filter) {    if (warningFilter_ == filter) return;
    warningFilter_ = filter;
    ++weatherGeneration_;
    weatherOverlayImage_ = {};
    weatherOverlayZoom_ = -1;
    update();
}

void RadarMapCanvas::setOverlayOverrides(std::optional<OverlayVisibility> overrides) {
    overlayOverrides_ = std::move(overrides);
    if (overlayOverrides_.has_value()) {
        setWarningsVisible(overlayOverrides_->warnings);
        setStormReportsVisible(overlayOverrides_->stormReports);
        setLightningVisible(overlayOverrides_->lightning);
        setLightningFusionVisible(overlayOverrides_->lightningFusion);
    } else {
        // Drop the override: the next global toggle will repaint the flags.
        update();
    }
}

void RadarMapCanvas::setProximityFilter(std::optional<QPointF> anchor, double radiusKm) {
    proximityAnchor_ = std::move(anchor);
    proximityRadiusKm_ = std::max(1.0, radiusKm);
    ++weatherGeneration_;
    weatherOverlayImage_ = {};
    weatherOverlayZoom_ = -1;
    update();
}

void RadarMapCanvas::setPalette(const PaletteKind palette) {
    palette_ = palette;
    invalidateOverlay();
    if (volume3d_) rebuildVolumeRender();
    update();
}

void RadarMapCanvas::setCustomPalette(QVector<PaletteStop> stops) {
    std::sort(stops.begin(), stops.end(), [](const auto& left, const auto& right) {
        return left.value < right.value;
    });
    customPalette_ = std::move(stops);
    invalidateOverlay();
    if (volume3d_) rebuildVolumeRender();
    update();
}

void RadarMapCanvas::setRadarOpacity(const int percent) {
    radarOpacity_ = std::clamp(percent, 10, 100);
    invalidateOverlay();
    if (volume3d_) rebuildVolumeRender();
    update();
}

void RadarMapCanvas::setWarningOpacity(const int percent) {
    warningOpacity_ = std::clamp(percent, 10, 100);
    ++weatherGeneration_;
    weatherOverlayImage_ = {};
    weatherOverlayZoom_ = -1;
    update();
}

void RadarMapCanvas::setWarningsOnTop(const bool onTop) {
    if (warningsOnTop_ == onTop) return;
    warningsOnTop_ = onTop;
    update();
}

void RadarMapCanvas::clearPinnedOverlay() {
    if (pinnedInfo_.isEmpty()) return;
    pinnedInfo_.clear();
    pinnedInfoKind_ = PinnedInfoKind::None;
    pinnedLightningCell_.reset();
    pinnedInfoCardRect_ = {};
    pinnedInfoBodyRect_ = {};
    pinnedInfoContentHeight_ = 0;
    pinnedInfoScrollOffset_ = 0.0;
    pinnedInfoScrollTarget_ = 0.0;
    pinnedInfoScrollTimer_->stop();
    pinnedInfoLocked_ = false;
    pinnedInfoExpanded_ = false;
    probeText_.clear();
    update();
}

void RadarMapCanvas::focusWarning(const int warningIndex) {
    if (warningIndex < 0 || warningIndex >= warnings_.size()) return;
    const auto& warning = warnings_.at(warningIndex);
    // Centroid of every exterior ring: cheap, and good enough to land the
    // polygon on screen for polygons of any shape.
    double longitude = 0.0, latitude = 0.0;
    int count = 0;
    for (const auto& polygon : warning.polygons) {
        for (const auto& point : polygon.exterior) {
            longitude += point.x();
            latitude += point.y();
            ++count;
        }
    }
    if (count == 0) return;
    const QPointF center(longitude / count, latitude / count);
    setHighlightedWarning(-1);
    if (viewZoom() < 5) setView(center, 6);
    else setView(center, viewZoom());
    const QPointF screen = screenPoint(center.y(), center.x());
    const auto hit = overlayAt(screen);
    if (!hit.text.isEmpty()) setPinnedOverlay(hit, screen);
}

void RadarMapCanvas::setHighlightedWarning(const int warningIndex) {
    if (highlightedWarning_ == warningIndex) return;
    highlightedWarning_ = warningIndex;
    update();
}

bool RadarMapCanvas::pinnedCardContains(const QPointF& screenPos) const {
    return !pinnedInfo_.isEmpty() && pinnedInfoCardRect_.contains(screenPos);
}

void RadarMapCanvas::togglePinnedCardExpanded() {
    if (pinnedInfo_.isEmpty()) return;
    pinnedInfoExpanded_ = !pinnedInfoExpanded_;
    update();
}

bool RadarMapCanvas::hasScrollablePinnedCard() const {
    return !pinnedInfo_.isEmpty() && pinnedCardMaxScrollForTest() > 0.0;
}

void RadarMapCanvas::scrollPinnedCard(const int pixels) {
    if (!hasScrollablePinnedCard()) return;
    if (!pinnedInfoScrollTimer_->isActive())
        pinnedInfoScrollTarget_ = pinnedInfoScrollOffset_;
    pinnedInfoScrollTarget_ =
        std::clamp(pinnedInfoScrollTarget_ - pixels, 0.0, pinnedCardMaxScrollForTest());
    pinnedInfoScrollTimer_->start();
}

void RadarMapCanvas::setLinkedProbe(const QPointF& lonLat, const QString& text,
                                    const QString& sourceLabel) {
    linkedProbeGeo_ = lonLat;
    linkedProbeText_ = text;
    linkedProbeSource_ = sourceLabel;
    linkedProbeVisible_ = true;
    update();
}

void RadarMapCanvas::clearLinkedProbe() {
    if (!linkedProbeVisible_) return;
    linkedProbeVisible_ = false;
    linkedProbeText_.clear();
    update();
}

void RadarMapCanvas::paintWarningHighlight(QPainter& painter) {
    if (highlightedWarning_ < 0 || !warningsVisible_) return;
    ensureWeatherPaths();
    bool found = false;
    for (const auto& cached : warningPaths_) {
        if (cached.warningIndex == highlightedWarning_) {
            found = true;
            break;
        }
    }
    if (!found) return;
    // Foreground pass over the cached image: theme accent core over a dark
    // halo, same two-pass treatment as the rasterized warnings themselves.
    // Paths are cached in current-zoom world coordinates, so world pixels
    // map 1:1 to screen pixels after the center translation.
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const auto world = static_cast<double>(kTileSize * (1 << zoom_));
    const auto center = mapCenterWorld();
    painter.translate(width() / 2.0 - center.x(), height() / 2.0 - center.y());
    painter.setBrush(Qt::NoBrush);
    for (const auto& cached : warningPaths_) {
        if (cached.warningIndex != highlightedWarning_) continue;
        for (const auto xOffset : {0.0, -world, world}) {
            painter.setPen(QPen(QColor(5, 8, 10, 210), 4.5));
            painter.drawPath(cached.path.translated(xOffset, 0.0));
            painter.setPen(QPen(activeBorderColor_, 2.0));
            painter.drawPath(cached.path.translated(xOffset, 0.0));
        }
    }
    painter.restore();
}

void RadarMapCanvas::paintLinkedProbe(QPainter& painter) {
    if (!linkedProbeVisible_ || linkedProbeText_.isEmpty()) return;
    const QPointF screen = screenPoint(linkedProbeGeo_.y(), linkedProbeGeo_.x());
    if (!rect().contains(screen.toPoint())) return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setFont(hudFont(8));
    const auto text = QStringLiteral("%1: %2").arg(linkedProbeSource_, linkedProbeText_);
    const auto metrics = painter.fontMetrics();
    const auto textWidth = static_cast<double>(metrics.horizontalAdvance(text));
    const bool flipX = screen.x() + 16.0 + textWidth + 16.0 > width();
    const bool flipY = screen.y() + 16.0 + 26.0 > height();
    const QPointF anchor(screen.x() + (flipX ? -16.0 : 16.0), screen.y() + (flipY ? -16.0 : 16.0));
    const QRectF box(flipX ? anchor.x() - textWidth - 16.0 : anchor.x(),
                     flipY ? anchor.y() - 22.0 : anchor.y(), textWidth + 16.0, 22.0);
    auto accent = activeBorderColor_;
    painter.setPen(QPen(accent, 1.5, Qt::DashLine));
    painter.setBrush(QColor(8, 15, 19, 215));
    painter.drawRoundedRect(box, 4, 4);
    painter.setPen(QColor(225, 235, 240));
    painter.drawText(box.adjusted(8.0, 0.0, -8.0, 0.0), Qt::AlignLeft | Qt::AlignVCenter, text);
    painter.setPen(QPen(accent, 1.5));
    painter.setBrush(accent);
    painter.drawEllipse(screen, 3.0, 3.0);
    painter.restore();
}

void RadarMapCanvas::setBelowThresholdVisible(const bool visible) {
    belowThresholdVisible_ = visible;
    invalidateOverlay();
    update();
}

void RadarMapCanvas::setVelocityProcessing(VelocityProcessingOptions options) {
    options.nyquistVelocityMetersPerSecond = std::clamp(options.nyquistVelocityMetersPerSecond, 1.0, 80.0);
    options.stormMotionDirectionDegrees = std::fmod(options.stormMotionDirectionDegrees + 360.0, 360.0);
    options.stormMotionSpeedMetersPerSecond = std::clamp(options.stormMotionSpeedMetersPerSecond, 0.0, 80.0);
    velocityOptions_ = options;
    refreshVelocityProcessing();
    // Circulation detection keys off dealiased velocity at the panel nyquist,
    // so a nyquist change must recompute markers too.
    invalidateLightningFusion();
    invalidateOverlay();
    update();
}

void RadarMapCanvas::refreshVelocityProcessing() {
    processedVelocity_.reset();
    const auto generation = ++velocityGeneration_;
    velocityBuildPending_ = false;
    if (!isVisible()) return;
    if (!chunk_ || product_ != ProductKind::Velocity ||
        velocityOptions_.mode == VelocityProcessingMode::Native) return;
    velocityBuildPending_ = true;
    const auto chunk = chunk_;
    const auto options = velocityOptions_;
    const QPointer<RadarMapCanvas> guard(this);
    QThreadPool::globalInstance()->start([guard, generation, chunk, options] {
        const auto processed = sharedProcessedVelocity(chunk, options);
        if (!guard) return;
        QMetaObject::invokeMethod(guard, [guard, generation, processed] {
            if (!guard || generation != guard->velocityGeneration_) return;
            guard->processedVelocity_ = processed;
            guard->velocityBuildPending_ = false;
            guard->invalidateOverlay();
            guard->update();
        }, Qt::QueuedConnection);
    });
}

void RadarMapCanvas::rebuildProbeIndex() {
    probeRayIndex_.clear();
    if (!chunk_) return;
    probeRayIndex_.reserve(static_cast<qsizetype>(chunk_->rays.size()));
    for (std::size_t rayIndex = 0; rayIndex < chunk_->rays.size(); ++rayIndex) {
        const auto& ray = chunk_->rays[rayIndex];
        if (elevationFilter_ && ray.elevationNumber != *elevationFilter_) continue;
        if (!ray.has(product_)) continue;
        auto azimuth = std::fmod(static_cast<double>(ray.azimuthDegrees), 360.0);
        if (azimuth < 0.0) azimuth += 360.0;
        probeRayIndex_.push_back({azimuth, rayIndex});
    }
    std::sort(probeRayIndex_.begin(), probeRayIndex_.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });
}

void RadarMapCanvas::setMeasureMode(const bool enabled) {
    measureMode_ = enabled;
    if (enabled) crossSectionMode_ = false;
    if (!enabled) {
        measureStart_.reset();
        measureEnd_.reset();
    }
    setCursor(enabled ? Qt::CrossCursor : Qt::ArrowCursor);
    update();
}

void RadarMapCanvas::setCrossSectionMode(const bool enabled) {
    crossSectionMode_ = enabled && !isDerivedProduct(product_);
    if (crossSectionMode_) measureMode_ = false;
    if (!crossSectionMode_) {
        measureStart_.reset();
        measureEnd_.reset();
    }
    setCursor(crossSectionMode_ ? Qt::CrossCursor : Qt::ArrowCursor);
    update();
}

void RadarMapCanvas::setVolumeQuality(const int decodedChunks, const int totalChunks,
                                      const int failedChunks, const std::size_t rejectedMessages,
                                      const std::size_t rejectedBlocks) {
    // failedChunks < 0 is the progressive path: chunks are still landing,
    // so "N/M chunks | loading…" instead of a premature "complete".
    qualityText_ = totalChunks > 0
                       ? QStringLiteral("%1/%2 chunks%3")
                             .arg(decodedChunks).arg(totalChunks)
                             .arg(failedChunks > 0 ? QStringLiteral(" | %1 missing").arg(failedChunks)
                                 : failedChunks < 0 ? QStringLiteral(" | loading…")
                                                    : QStringLiteral(" | complete"))
                       : QString{};
    if (rejectedMessages > 0 || rejectedBlocks > 0)
        qualityText_ += QStringLiteral(" | parser rejected %1 message(s), %2 block(s)")
                            .arg(rejectedMessages).arg(rejectedBlocks);
    invalidateStatusText();
    update();
}

std::vector<TiltDiagnostics> RadarMapCanvas::tiltDiagnostics() const {
    if (!chunk_) return {};
    std::vector<TiltDiagnostics> perTilt;
    analyzeScan(*chunk_, product_, perTilt, std::nullopt);
    return perTilt;
}

void RadarMapCanvas::refreshScanDiagnostics() {
    scanQualityText_.clear();
    invalidateStatusText();
    if (!chunk_) return;
    const auto diagnostics = analyzeScan(*chunk_, product_, elevationFilter_);
    if (diagnostics.rayCount == 0) return;
    scanQualityText_ = QStringLiteral("%1 rays | %2%3 coverage | max gap %4%3")
                           .arg(diagnostics.rayCount)
                           .arg(diagnostics.azimuthCoverageDegrees, 0, 'f', 0)
                           .arg(QChar(0x00b0))
                           .arg(diagnostics.largestAzimuthGapDegrees, 0, 'f', 1);
}

void RadarMapCanvas::setLoading(const bool loading, const QString& message) {
    loading_ = loading;
    loadingMessage_ = message;
    if (loading) loadingAngle_ = 0;
    if (loading && !loadingTimer_->isActive()) loadingTimer_->start();
    else if (!loading) loadingTimer_->stop();
    update();
}

void RadarMapCanvas::showNationalView() {
    center_ = {-96.5, 38.5};
    zoom_ = 4;
    invalidateWorldCaches(true);
    invalidateOverlay();
    update();
    emit viewChanged(center_, zoom_);
}

void RadarMapCanvas::recenterSelected() {
    const auto* site = selectedSite();
    if (!site || !site->latitude || !site->longitude) return;
    center_ = {*site->longitude, *site->latitude};
    zoom_ = 6;
    invalidateWorldCaches(true);
    invalidateOverlay();
    update();
    emit viewChanged(center_, zoom_);
}

void RadarMapCanvas::zoomIn() {
    // No-op at the limit pushes nothing (pushViewHistory dedupes only an
    // identical top; a clamped press would bank a useless self-entry).
    if (zoom_ < kMaxZoom) pushViewHistory();
    setZoomAt(zoom_ + 1, QPointF(width() / 2.0, height() / 2.0));
}

void RadarMapCanvas::zoomOut() {
    if (zoom_ > kMinZoom) pushViewHistory();
    setZoomAt(zoom_ - 1, QPointF(width() / 2.0, height() / 2.0));
}

void RadarMapCanvas::setView(const QPointF& center, const int zoom) {
    const auto previousZoom = zoom_;
    center_ = {std::clamp(center.x(), -180.0, 180.0), std::clamp(center.y(), -85.0, 85.0)};
    zoom_ = std::clamp(zoom, kMinZoom, kMaxZoom);
    wheelZoomRemainder_ = 0.0;
    invalidateWorldCaches(zoom_ != previousZoom);
    if (zoom_ != previousZoom) invalidateOverlay(true);
    update();
}

void RadarMapCanvas::setActivePanel(const bool active) {
    activePanel_ = active;
    update();
}

void RadarMapCanvas::setActiveBorderColor(const QColor& color) {
    activeBorderColor_ = color;
    if (activePanel_) update();
}

void RadarMapCanvas::setSupplementalAttribution(const QString& text) {
    if (supplementalAttribution_ == text) return;
    supplementalAttribution_ = text;
    update();
}

QPointF RadarMapCanvas::viewCenter() const {
    return center_;
}

int RadarMapCanvas::viewZoom() const {
    return zoom_;
}

QString RadarMapCanvas::radarId() const {
    return selectedRadar_;
}

ProductKind RadarMapCanvas::product() const {
    return product_;
}

std::optional<int> RadarMapCanvas::elevationFilter() const {
    return elevationFilter_;
}

VelocityProcessingOptions RadarMapCanvas::velocityProcessing() const {
    return velocityOptions_;
}

RadarMapCanvas::PaletteKind RadarMapCanvas::palette() const {
    return palette_;
}

QVector<RadarMapCanvas::PaletteStop> RadarMapCanvas::customPalette() const {
    return customPalette_;
}

QColor RadarMapCanvas::colorForValue(const ProductKind product, const double value,
                                     const PaletteKind palette, const int opacityPercent,
                                     const QVector<PaletteStop>& custom) {
    const auto& descriptor = productDescriptor(product);
    const auto normalized = std::clamp((value - descriptor.minimum) /
                                           (descriptor.maximum - descriptor.minimum), 0.0, 1.0);
    return productColor(product, normalized, palette, opacityPercent, custom);
}

std::vector<RgbaStop> RadarMapCanvas::volumeTransferStops(const ProductKind product,
                                                          const PaletteKind palette,
                                                          const QVector<PaletteStop>& custom) {
    // 48 samples of the shipped palette colour plus the 3D alpha ramp. The CPU
    // march samples this vector; the RHI backend bakes the same vector into its
    // 256-entry LUT, so the 2D map, CPU 3D and GPU 3D cannot drift in colour.
    std::vector<RgbaStop> stops;
    stops.reserve(48);
    const auto rampAlpha = [](double v01, bool folds) {
        const double t = folds ? std::fabs(v01 - 0.5) * 2.0 : v01;
        const double edge0 = folds ? 0.12 : 0.24;
        const double edge1 = folds ? 0.50 : 0.60;
        const double s = std::clamp((t - edge0) / (edge1 - edge0), 0.0, 1.0);
        return (s * s * (3.0 - 2.0 * s)) * (folds ? 0.85 : 0.92);
    };
    const bool folds = product == ProductKind::Velocity;
    for (int stop = 0; stop < 48; ++stop) {
        const double normalized = static_cast<double>(stop) / 47.0;
        const QColor color = productColor(product, normalized, palette, 100, custom);
        stops.push_back({static_cast<float>(normalized), static_cast<float>(color.redF()),
                         static_cast<float>(color.greenF()),
                         static_cast<float>(color.blueF()),
                         static_cast<float>(rampAlpha(normalized, folds))});
    }
    return stops;
}

QString RadarMapCanvas::probeText() const {
    return probeText_;
}

bool RadarMapCanvas::infoCardScrollable() const {
    return !pinnedInfo_.isEmpty() &&
           pinnedInfoContentHeight_ > static_cast<int>(std::floor(pinnedInfoBodyRect_.height()));
}

QString RadarMapCanvas::elidedStatusText() const {
    // The composed string used to be rebuilt from a four-level nested .arg()
    // chain plus a QDateTime format on every single paint, i.e. on every pan
    // pixel and every 16 ms probe tick. Cache it; only the visible slice still
    // depends on the panel width.
    if (!statusTextCacheValid_) {
        const auto dataDate = dataTime_.isValid()
                                  ? dataTime_.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) +
                                        QStringLiteral(" local")
                                  : QStringLiteral("time unavailable");
        const auto rayCount = chunk_ ? chunk_->rays.size() : 0;
        statusTextCache_ = QStringLiteral("%1 real rays  |  NOAA data: %2").arg(rayCount).arg(dataDate);
        if (!qualityText_.isEmpty() || !scanQualityText_.isEmpty()) {
            statusTextCache_ += QStringLiteral("  |  ");
            if (!qualityText_.isEmpty()) statusTextCache_ += qualityText_;
            if (!qualityText_.isEmpty() && !scanQualityText_.isEmpty()) statusTextCache_ += QStringLiteral(" | ");
            statusTextCache_ += scanQualityText_;
        }
        statusTextCacheValid_ = true;
    }
    return statusTextCache_;
}

void RadarMapCanvas::invalidateStatusText() {
    statusTextCacheValid_ = false;
}

void RadarMapCanvas::paintLoadingChip(QPainter& painter) {
    if (!loading_) return;
    const auto chipWidth = std::min(282.0, std::max(120.0, width() - 24.0));
    const QRectF chip(std::max(12.0, width() - chipWidth - 12.0), 64.0, chipWidth, 40.0);
    painter.save();
    painter.setPen(QPen(QColor(128, 211, 255, 170), 1));
    painter.setBrush(QColor(5, 12, 16, 232));
    painter.drawRoundedRect(chip, 5, 5);
    const QPointF center(chip.left() + 21.0, chip.center().y());
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(128, 211, 255), 3, Qt::SolidLine, Qt::RoundCap));
    // QPainter::drawArc measures counter-clockwise from 3 o'clock, so negate the
    // accumulator to make the spinner actually turn clockwise.
    painter.drawArc(QRectF(center.x() - 10, center.y() - 10, 20, 20),
                    -loadingAngle_ * 16, 250 * 16);
    painter.setPen(QColor(235, 243, 247));
    painter.setFont(hudFont(8, QFont::DemiBold));
    const auto text = loadingMessage_.isEmpty() ? QStringLiteral("Loading radar data")
                                               : loadingMessage_;
    painter.drawText(chip.adjusted(42, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft,
                     painter.fontMetrics().elidedText(text, Qt::ElideRight,
                                                      static_cast<int>(chipWidth) - 52));
    painter.restore();
}

void RadarMapCanvas::inspectAt(const QPointF& point) {
    const auto hit = overlayAt(point);
    if (!hit.text.isEmpty()) {
        setPinnedOverlay(hit, point);
        return;
    }
    // Nothing under the cursor: freeze the sampled value into a card. Writing
    // the transient probe text instead meant the very next mouse move erased
    // it, so "Inspect at cursor" looked like a no-op.
    updateProbe(point);
    if (probeText_.isEmpty()) return;
    const auto rows = probeText_.split(QLatin1Char('|'));
    QStringList parts;
    parts << QStringLiteral("Cursor readout");
    for (const auto& row : rows) {
        const auto trimmed = row.trimmed();
        if (!trimmed.isEmpty()) parts << trimmed;
    }
    setPinnedOverlay(OverlayHit(parts.join(QLatin1Char('\n')), PinnedInfoKind::Probe), point);
}

QPoint RadarMapCanvas::lightningCellScreenPoint(const int index) const {
    if (index < 0 || index >= static_cast<int>(lightningCells_.size())) return {};
    const auto& cell = lightningCells_.at(static_cast<std::size_t>(index));
    return screenPoint(cell.latitudeCenterDeg, cell.longitudeCenterDeg).toPoint();
}

int RadarMapCanvas::warningCount() const {
    return warnings_.size();
}

int RadarMapCanvas::stormReportCount() const {
    return stormReports_.size();
}

int RadarMapCanvas::placefileElementCount() const {
    int count = 0;
    for (const auto& document : placefiles_) count += document.elements.size();
    return count;
}

QPointF RadarMapCanvas::worldPoint(const double latitude, const double longitude) const {
    const auto world = static_cast<double>(kTileSize * (1 << zoom_));
    const auto lat = qDegreesToRadians(std::clamp(latitude, -85.0511, 85.0511));
    const auto sinLat = std::sin(lat);
    const auto x = (longitude + 180.0) / 360.0 * world;
    const auto y = (0.5 - std::log((1.0 + sinLat) / (1.0 - sinLat)) / (4.0 * kPi)) * world;
    return {x, y};
}

QPointF RadarMapCanvas::mapCenterWorld() const {
    if (cachedWorldCenterZoom_ != zoom_ || cachedCenterCoordinate_ != center_) {
        cachedWorldCenter_ = worldPoint(center_.y(), center_.x());
        cachedCenterCoordinate_ = center_;
        cachedWorldCenterZoom_ = zoom_;
    }
    return cachedWorldCenter_;
}

QPointF RadarMapCanvas::screenPointFromWorld(const QPointF& source) const {
    const auto world = static_cast<double>(kTileSize * (1 << zoom_));
    const auto center = mapCenterWorld();
    auto dx = source.x() - center.x();
    if (dx > world / 2.0) dx -= world;
    if (dx < -world / 2.0) dx += world;
    return {width() / 2.0 + dx, height() / 2.0 + source.y() - center.y()};
}

QPointF RadarMapCanvas::screenPoint(const double latitude, const double longitude) const {
    return screenPointFromWorld(worldPoint(latitude, longitude));
}

QPointF RadarMapCanvas::geographicPoint(const QPointF& screen) const {
    const auto world = static_cast<double>(kTileSize * (1 << zoom_));
    const auto center = mapCenterWorld();
    auto x = center.x() + screen.x() - width() / 2.0;
    x = std::fmod(x + world, world);
    if (x < 0.0) x += world;
    const auto y = std::clamp(center.y() + screen.y() - height() / 2.0, 0.0, world);
    const auto longitude = x / world * 360.0 - 180.0;
    const auto n = kPi - 2.0 * kPi * y / world;
    return {longitude, qRadiansToDegrees(std::atan(std::sinh(n)))};
}

RadarMapCanvas::GeoBounds RadarMapCanvas::visibleLonLatBounds() const {
    GeoBounds bounds;
    if (width() <= 0 || height() <= 0) return bounds;
    const auto topLeft = geographicPoint(QPointF(0.0, 0.0));
    const auto bottomRight = geographicPoint(QPointF(width(), height()));
    bounds.west = topLeft.x();
    bounds.north = topLeft.y();
    bounds.east = bottomRight.x();
    bounds.south = bottomRight.y();
    bounds.valid = std::isfinite(bounds.west) && std::isfinite(bounds.south) &&
                   std::isfinite(bounds.east) && std::isfinite(bounds.north) &&
                   bounds.north > bounds.south && bounds.east > bounds.west;
    return bounds;
}

QPoint RadarMapCanvas::radarScreenPoint(const QString& radarId) const {
    const auto found = siteIndexes_.constFind(radarId);
    if (found == siteIndexes_.cend()) return {};
    ensureSiteWorldPoints();
    const auto index = *found;
    if (index < static_cast<std::size_t>(siteWorldPoints_.size()) && !siteWorldPoints_.at(index).isNull())
        return screenPointFromWorld(siteWorldPoints_.at(index)).toPoint();
    return {};
}

const RadarSite* RadarMapCanvas::selectedSite() const {
    const auto found = siteIndexes_.constFind(selectedRadar_);
    return found == siteIndexes_.cend() || *found >= sites_.size() ? nullptr : &sites_[*found];
}

void RadarMapCanvas::invalidateWorldCaches(const bool zoomChanged) {
    cachedWorldCenterZoom_ = -1;
    if (zoomChanged) {
        siteWorldZoom_ = -1;
        warningPathsZoom_ = -1;
    }
}

void RadarMapCanvas::ensureSiteWorldPoints() const {
    if (siteWorldZoom_ == zoom_ && siteWorldPoints_.size() == static_cast<qsizetype>(sites_.size())) return;
    siteWorldPoints_.clear();
    siteWorldPoints_.reserve(static_cast<qsizetype>(sites_.size()));
    for (const auto& site : sites_) {
        siteWorldPoints_.push_back(site.latitude && site.longitude
                                       ? worldPoint(*site.latitude, *site.longitude)
                                       : QPointF{});
    }
    siteWorldZoom_ = zoom_;
}

void RadarMapCanvas::invalidateOverlay(const bool preserveForZoomTransition) {
    ++overlayGeneration_;
    if (!preserveForZoomTransition) {
        overlayImage_ = {};
        overlayImageLogicalSize_ = {};
        overlayZoom_ = -1;
    }
}

bool RadarMapCanvas::overlayCacheCoversView() const {
    if (overlayImage_.isNull() || overlayZoom_ != zoom_ || overlayImageGeneration_ != overlayGeneration_ ||
        overlayImageLogicalSize_.isEmpty()) return false;
    const auto world = static_cast<double>(kTileSize * (1 << zoom_));
    const auto currentCenter = mapCenterWorld();
    auto dx = currentCenter.x() - overlayImageCenterWorld_.x();
    if (dx > world / 2.0) dx -= world;
    if (dx < -world / 2.0) dx += world;
    const auto dy = currentCenter.y() - overlayImageCenterWorld_.y();
    const auto horizontalMargin = (overlayImageLogicalSize_.width() - width()) / 2.0;
    const auto verticalMargin = (overlayImageLogicalSize_.height() - height()) / 2.0;
    return std::abs(dx) <= std::max(0.0, horizontalMargin - 24.0) &&
           std::abs(dy) <= std::max(0.0, verticalMargin - 24.0);
}

bool RadarMapCanvas::weatherOverlayCacheCoversView() const {
    if (weatherOverlayImage_.isNull() || weatherOverlayZoom_ != zoom_ ||
        weatherOverlayImageGeneration_ != weatherGeneration_ ||
        weatherOverlayImageLogicalSize_.isEmpty()) return false;
    const auto world = static_cast<double>(kTileSize * (1 << zoom_));
    const auto currentCenter = mapCenterWorld();
    auto dx = currentCenter.x() - weatherOverlayImageCenterWorld_.x();
    if (dx > world / 2.0) dx -= world;
    if (dx < -world / 2.0) dx += world;
    const auto dy = currentCenter.y() - weatherOverlayImageCenterWorld_.y();
    const auto horizontalMargin = (weatherOverlayImageLogicalSize_.width() - width()) / 2.0;
    const auto verticalMargin = (weatherOverlayImageLogicalSize_.height() - height()) / 2.0;
    return std::abs(dx) <= std::max(0.0, horizontalMargin - 24.0) &&
           std::abs(dy) <= std::max(0.0, verticalMargin - 24.0);
}

void RadarMapCanvas::rebuildWeatherOverlay() {
    if (weatherOverlayBuildPending_ || width() <= 0 || height() <= 0 || weatherOverlayCacheCoversView())
        return;
    // Early-out present: skip raster work (and by-value polygon/report copies)
    // when both warning and report layers are hidden or have no data.
    // Nothing to rasterize. Bail before the by-value captures below, which copy
    // the full warning polygons and report vectors on the GUI thread - during a
    // drag-pan that is once per mouse-move frame, and it happened even with
    // both layers switched off.
    const auto warningsVisible = warningsVisible_;
    const auto stormReportsVisible = stormReportsVisible_;
    if ((!warningsVisible || warnings_.isEmpty()) &&
        (!stormReportsVisible || stormReports_.isEmpty())) {
        weatherOverlayImage_ = {};
        weatherOverlayImageGeneration_ = weatherGeneration_;
        weatherOverlayZoom_ = zoom_;
        weatherOverlayImageLogicalSize_ = QSize(width(), height());
        update();
        return;
    }

    weatherOverlayBuildPending_ = true;
    const auto generation = weatherGeneration_;
    const auto targetZoom = zoom_;
    const auto centerCoordinate = center_;
    const auto viewportSize = size();
    const auto devicePixelRatio = devicePixelRatioF();
    const auto warnings = warnings_;
    const auto reports = stormReports_;
    const auto warningFilter = warningFilter_;
    const auto reportKindsVisible = stormReportKindsVisible_;
    const auto warningOpacity = warningOpacity_;
    const auto geometryIsCurrent = warningPathsZoom_ == targetZoom &&
                                   warningPathsGeneration_ == generation &&
                                   stormReportWorldPoints_.size() == reports.size();
    const auto cachedPaths = geometryIsCurrent ? warningPaths_ : QVector<CachedWarningPath>{};
    const auto cachedReportWorldPoints =
        geometryIsCurrent ? stormReportWorldPoints_ : QVector<QPointF>{};
    const QPointer<RadarMapCanvas> guard(this);
    QThreadPool::globalInstance()->start([
        guard, generation, targetZoom, centerCoordinate, viewportSize, devicePixelRatio,
        warnings, reports, warningFilter, warningsVisible, stormReportsVisible, reportKindsVisible,
        warningOpacity, geometryIsCurrent, cachedPaths, cachedReportWorldPoints
    ] {
        constexpr int kOverlayMargin = 384;
        const auto worldPointAt = [targetZoom](const double latitude, const double longitude) {
            const auto world = static_cast<double>(RadarMapCanvas::kTileSize * (1 << targetZoom));
            const auto lat = qDegreesToRadians(std::clamp(latitude, -85.0511, 85.0511));
            const auto sinLat = std::sin(lat);
            return QPointF((longitude + 180.0) / 360.0 * world,
                           (0.5 - std::log((1.0 + sinLat) / (1.0 - sinLat)) / (4.0 * kPi)) * world);
        };

        QVector<CachedWarningPath> paths = cachedPaths;
        QVector<QPointF> reportWorldPoints = cachedReportWorldPoints;
        if (!geometryIsCurrent) {
            QVector<int> warningOrder;
            warningOrder.reserve(warnings.size());
            for (int warningIndex = 0; warningIndex < warnings.size(); ++warningIndex) {
                const auto& warning = warnings.at(warningIndex);
                if (!warningFilter.isEmpty() &&
                    !warning.event.contains(warningFilter, Qt::CaseInsensitive) &&
                    !warning.severity.contains(warningFilter, Qt::CaseInsensitive)) continue;
                warningOrder.push_back(warningIndex);
            }
            std::sort(warningOrder.begin(), warningOrder.end(),
                      [&warnings](const int left, const int right) {
                const auto& leftWarning = warnings.at(left);
                const auto& rightWarning = warnings.at(right);
                if (leftWarning.alertPriority != rightWarning.alertPriority)
                    return leftWarning.alertPriority < rightWarning.alertPriority;
                return left < right;
            });

            paths.reserve(warnings.size());
            for (const auto warningIndex : warningOrder) {
                const auto& warning = warnings.at(warningIndex);
                const auto color = warning.alertColor.isValid()
                                       ? warning.alertColor
                                       : nwsAlertStyle(warning.event).color;
                for (const auto& geographicPolygon : warning.polygons) {
                    QPainterPath path;
                    path.setFillRule(Qt::OddEvenFill);
                    QPolygonF exterior;
                    exterior.reserve(geographicPolygon.exterior.size());
                    for (const auto& coordinate : geographicPolygon.exterior)
                        exterior.push_back(worldPointAt(coordinate.y(), coordinate.x()));
                    path.addPolygon(exterior);
                    for (const auto& geographicHole : geographicPolygon.holes) {
                        QPolygonF hole;
                        hole.reserve(geographicHole.size());
                        for (const auto& coordinate : geographicHole)
                            hole.push_back(worldPointAt(coordinate.y(), coordinate.x()));
                        path.addPolygon(hole);
                    }
                    const auto bounds = path.boundingRect();
                    paths.push_back({warningIndex, color, std::move(path), bounds});
                }
            }

            reportWorldPoints.reserve(reports.size());
            for (const auto& report : reports)
                reportWorldPoints.push_back(
                    worldPointAt(report.coordinate.y(), report.coordinate.x()));
        }

        const auto overlayLogicalSize = viewportSize + QSize(kOverlayMargin * 2, kOverlayMargin * 2);
        const auto overlayCenterWorld = worldPointAt(centerCoordinate.y(), centerCoordinate.x());
        const auto visibleWorldBounds = QRectF(
            overlayCenterWorld - QPointF(overlayLogicalSize.width() / 2.0,
                                         overlayLogicalSize.height() / 2.0),
            QSizeF(overlayLogicalSize));
        const auto world = static_cast<double>(RadarMapCanvas::kTileSize * (1 << targetZoom));

        QHash<QRgb, QPainterPath> warningGroups;
        QVector<QRgb> warningGroupOrder;
        if (warningsVisible) {
            for (const auto& cached : paths) {
                for (const auto xOffset : {0.0, -world, world}) {
                    if (!visibleWorldBounds.intersects(cached.bounds.translated(xOffset, 0.0))) continue;
                    auto& group = warningGroups[cached.color.rgba()];
                    if (group.isEmpty()) warningGroupOrder.push_back(cached.color.rgba());
                    group.addPath(xOffset == 0.0
                                      ? cached.path
                                      : cached.path.translated(xOffset, 0.0));
                }
            }
        }

        std::array<QPainterPath, 3> reportGroups;
        // Outbreak declutter: at zoom <= 6, hail/wind reports collapse to one
        // marker per ~14 px grid cell so a 200-report outbreak doesn't overpaint
        // into a solid blob. Tornadoes are never thinned; hail outranks wind.
        QSet<qint64> occupiedReportCells;
        const bool declutterReports = targetZoom <= 6;
        // Tornadoes first so they win their grid cell.
        for (int pass = 0; pass < 3; ++pass) {
            for (qsizetype reportIndex = 0; reportIndex < reports.size(); ++reportIndex) {
            const auto& report = reports.at(reportIndex);
            const auto kindIndex = static_cast<std::size_t>(report.kind);
            const int kindRank = report.kind == StormReportKind::Tornado ? 0
                               : report.kind == StormReportKind::Hail ? 1 : 2;
            if (kindRank != pass) continue;
            if (!stormReportsVisible || kindIndex >= reportKindsVisible.size() ||
                !reportKindsVisible.at(kindIndex)) continue;
            if (reportIndex >= reportWorldPoints.size()) continue;
            const auto basePoint = reportWorldPoints.at(reportIndex);
            for (const auto xOffset : {0.0, -world, world}) {
                const auto point = basePoint + QPointF(xOffset, 0.0);
                if (!visibleWorldBounds.adjusted(-8.0, -8.0, 8.0, 8.0).contains(point)) continue;
                if (declutterReports && kindRank > 0) {
                    const auto cellX = static_cast<qint64>(std::floor(point.x() / 14.0));
                    const auto cellY = static_cast<qint64>(std::floor(point.y() / 14.0));
                    const auto cellKey = (cellX << 32) | (cellY & 0xffffffff);
                    if (occupiedReportCells.contains(cellKey)) continue;
                    occupiedReportCells.insert(cellKey);
                }
                auto& group = reportGroups.at(kindIndex);
                if (report.kind == StormReportKind::Tornado) {
                    group.addPolygon(QPolygonF{{point.x(), point.y() - 6.0},
                                               {point.x() - 5.5, point.y() + 5.0},
                                               {point.x() + 5.5, point.y() + 5.0}});
                } else if (report.kind == StormReportKind::Hail) {
                    group.addEllipse(point, 5.0, 5.0);
                } else {
                    group.addRect(QRectF(point.x() - 4.5, point.y() - 4.5, 9.0, 9.0));
                }
            }
            }
        }

        const auto pixelSize = QSize(
            std::max(1, static_cast<int>(std::ceil(overlayLogicalSize.width() * devicePixelRatio))),
            std::max(1, static_cast<int>(std::ceil(overlayLogicalSize.height() * devicePixelRatio))));
        QImage overlayImage(pixelSize, QImage::Format_ARGB32_Premultiplied);
        overlayImage.setDevicePixelRatio(devicePixelRatio);
        overlayImage.fill(Qt::transparent);
        QPainter overlayPainter(&overlayImage);
        overlayPainter.setRenderHint(QPainter::Antialiasing, true);
        overlayPainter.setPen(Qt::NoPen);
        overlayPainter.translate(overlayLogicalSize.width() / 2.0 - overlayCenterWorld.x(),
                                 overlayLogicalSize.height() / 2.0 - overlayCenterWorld.y());
        if (warningsVisible) {
            const auto alphaScale = std::clamp(warningOpacity, 10, 100) / 100.0;
            for (const auto rgba : warningGroupOrder) {
                const auto group = warningGroups.constFind(rgba);
                if (group == warningGroups.cend()) continue;
                auto color = QColor::fromRgba(rgba);
                color.setAlpha(std::clamp(static_cast<int>(std::round(color.alpha() * alphaScale)), 0, 255));
                auto fill = color;
                fill.setAlpha(std::clamp(static_cast<int>(std::round(90.0 * alphaScale)), 8, 110));
                // Two-pass stroke: a dark halo under the alert color keeps the
                // polygon readable over bright echo (white/pink velocity wash)
                // instead of dissolving into it.
                overlayPainter.setPen(QPen(QColor(5, 8, 10, 210), 3.5));
                overlayPainter.setBrush(fill);
                overlayPainter.drawPath(*group);
                overlayPainter.setPen(QPen(color, 1.5));
                overlayPainter.setBrush(Qt::NoBrush);
                overlayPainter.drawPath(*group);
            }
        }
        if (stormReportsVisible) {
            const std::array reportColors{QColor("#ff3b4d"), QColor("#53d769"), QColor("#44a7ff")};
            for (std::size_t kindIndex = 0; kindIndex < reportGroups.size(); ++kindIndex) {
                if (!reportKindsVisible.at(kindIndex) || reportGroups.at(kindIndex).isEmpty()) continue;
                overlayPainter.setPen(QPen(QColor(245, 248, 249), 1.0));
                overlayPainter.setBrush(reportColors.at(kindIndex));
                overlayPainter.drawPath(reportGroups.at(kindIndex));
            }
        }
        overlayPainter.end();
        if (!guard) return;
        QMetaObject::invokeMethod(guard, [
            guard, generation, targetZoom, paths = std::move(paths),
            reportWorldPoints = std::move(reportWorldPoints),
            overlayImage = std::move(overlayImage), overlayLogicalSize, overlayCenterWorld
        ]() mutable {
            if (!guard) return;
            guard->weatherOverlayBuildPending_ = false;
            if (generation == guard->weatherGeneration_ && targetZoom == guard->zoom_) {
                guard->warningPaths_ = std::move(paths);
                guard->warningPathsZoom_ = targetZoom;
                guard->warningPathsGeneration_ = generation;
                guard->stormReportWorldPoints_ = std::move(reportWorldPoints);
                guard->weatherOverlayImage_ = std::move(overlayImage);
                guard->weatherOverlayImageLogicalSize_ = overlayLogicalSize;
                guard->weatherOverlayImageCenterWorld_ = overlayCenterWorld;
                guard->weatherOverlayZoom_ = targetZoom;
                guard->weatherOverlayImageGeneration_ = generation;
            }
            // A drag or data refresh may have overtaken this worker. Start the
            // newest view immediately instead of waiting for another paint pass.
            guard->rebuildWeatherOverlay();
            guard->update();
        }, Qt::QueuedConnection);
    });
}

namespace {

bool echoGeometryMatches(const EchoGateGeometry& geometry,
                         const std::shared_ptr<const DecodedChunk>& chunk,
                         const std::shared_ptr<const ProcessedVelocity>& velocity,
                         const ProductKind product, const std::optional<int>& elevation,
                         const int zoom, const bool smooth, const bool belowVisible) {
    return geometry.chunk == chunk && geometry.velocity == velocity &&
           geometry.product == product && geometry.elevation == elevation &&
           geometry.zoom == zoom && geometry.smooth == smooth &&
           geometry.belowVisible == belowVisible;
}

// Direct scanline rasterizer for cached gate quads: no QPainterPath
// tessellation, no per-rebuild trig. Each quad is convex (azimuth/range
// cell), so a scanline hits exactly two edges; half-open edge tests keep
// shared edges single-covered, and the +1px right/bottom bleed closes the
// same 1px seams the old same-color outline trick handled. Runs in the
// worker thread on a Format_ARGB32 image (premultiplied at the end).
void rasterizeEchoGates(QImage& image, const std::vector<EchoGateGeometry::Gate>& gates,
                        const QPointF& centerWorld, const double dpr,
                        const std::array<QRgb, 64>& lut, const QRgb foldedRgb,
                        const QRgb belowRgb) {
    const int width = image.width();
    const int height = image.height();
    if (width <= 0 || height <= 0 || gates.empty()) return;
    const double originX = width * 0.5 - centerWorld.x() * dpr;
    const double originY = height * 0.5 - centerWorld.y() * dpr;
    for (const auto& gate : gates) {
        double px[4];
        double py[4];
        double xMin = std::numeric_limits<double>::infinity();
        double xMax = -std::numeric_limits<double>::infinity();
        double yMin = std::numeric_limits<double>::infinity();
        double yMax = -std::numeric_limits<double>::infinity();
        for (int corner = 0; corner < 4; ++corner) {
            px[corner] = static_cast<double>(gate.x[corner]) * dpr + originX;
            py[corner] = static_cast<double>(gate.y[corner]) * dpr + originY;
            xMin = std::min(xMin, px[corner]);
            xMax = std::max(xMax, px[corner]);
            yMin = std::min(yMin, py[corner]);
            yMax = std::max(yMax, py[corner]);
        }
        int y0 = std::max(0, static_cast<int>(std::floor(yMin)));
        int y1 = std::min(height - 1, static_cast<int>(std::ceil(yMax)) + 1);
        if (y0 > y1) continue;
        const int xLo = std::max(0, static_cast<int>(std::floor(xMin)));
        const int xHi = std::min(width - 1, static_cast<int>(std::ceil(xMax)) + 1);
        if (xLo > xHi) continue;
        const QRgb color = gate.kind == 0
                               ? lut[std::clamp(static_cast<int>(std::lround(
                                                     static_cast<double>(gate.value01) * 63.0)),
                                                 0, 63)]
                               : (gate.kind == 1 ? foldedRgb : belowRgb);
        for (int y = y0; y <= y1; ++y) {
            const double yc = static_cast<double>(y) + 0.5;
            double xa = std::numeric_limits<double>::infinity();
            double xb = -std::numeric_limits<double>::infinity();
            for (int edge = 0; edge < 4; ++edge) {
                const double ex0 = px[edge];
                const double ey0 = py[edge];
                const double ex1 = px[(edge + 1) & 3];
                const double ey1 = py[(edge + 1) & 3];
                if ((ey0 <= yc && yc < ey1) || (ey1 <= yc && yc < ey0)) {
                    const double xi = ex0 + (yc - ey0) / (ey1 - ey0) * (ex1 - ex0);
                    xa = std::min(xa, xi);
                    xb = std::max(xb, xi);
                }
            }
            if (xa > xb) continue;
            const int xs = std::max(xLo, static_cast<int>(std::floor(xa)));
            const int xe = std::min(xHi, static_cast<int>(std::ceil(xb)));
            QRgb* row = reinterpret_cast<QRgb*>(image.scanLine(y));
            for (int x = xs; x <= xe; ++x) row[x] = color;
        }
    }
}

}  // namespace

void RadarMapCanvas::rebuildOverlay() {
    if (overlayBuildPending_ || velocityBuildPending_ || width() <= 0 || height() <= 0) return;
    const auto* site = selectedSite();
    if (!site || !site->latitude || !site->longitude || !chunk_) {
        overlayZoom_ = zoom_;
        return;
    }
    overlayBuildPending_ = true;
    // Every (re)build request bumps the id, even the early-outs below: a
    // pan or zoom that lands while a worker is mid-volume aborts it at the
    // next checkpoint instead of burning a full stale raster.
    const auto requestId = overlayRequestId_.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto generation = overlayGeneration_;
    const auto targetZoom = zoom_;
    const auto baseLatitude = *site->latitude;
    const auto baseLongitude = *site->longitude;
    const auto product = product_;
    const auto elevation = elevationFilter_;
    const auto belowThresholdVisible = belowThresholdVisible_;
    const auto palette = palette_;
    const auto customPalette = customPalette_;
    const auto opacity = radarOpacity_;
    const auto smoothDisplay = displaySmoothing_;
    const auto chunk = chunk_;
    const auto processedVelocity = processedVelocity_;
    const auto cachedGeometry = echoGeometry_;
    const auto viewportSize = size();
    const auto devicePixelRatio = devicePixelRatioF();
    const auto centerCoordinate = center_;
    const QPointer<RadarMapCanvas> guard(this);
    QThreadPool::globalInstance()->start([
        guard, requestId, generation, targetZoom, baseLatitude, baseLongitude, product, elevation,
        belowThresholdVisible, palette, customPalette, opacity, smoothDisplay, chunk, processedVelocity,
        cachedGeometry, viewportSize, devicePixelRatio, centerCoordinate
    ] {
        constexpr int kOverlayMargin = 256;
        std::shared_ptr<const EchoGateGeometry> geometry = cachedGeometry;
        const bool geometryReusable =
            geometry &&
            echoGeometryMatches(*geometry, chunk, processedVelocity, product, elevation,
                                targetZoom, smoothDisplay, belowThresholdVisible);
        std::vector<EchoGateGeometry::Gate> builtGates;
        double maximumRangeMeters = 0.0;
        bool cacheable = geometryReusable;
        const auto& descriptor = productDescriptor(product);
        const auto worldPointAt = [targetZoom](const double latitude, const double longitude) {
            const auto world = static_cast<double>(RadarMapCanvas::kTileSize * (1 << targetZoom));
            const auto lat = qDegreesToRadians(std::clamp(latitude, -85.0511, 85.0511));
            const auto sinLat = std::sin(lat);
            return QPointF((longitude + 180.0) / 360.0 * world,
                           (0.5 - std::log((1.0 + sinLat) / (1.0 - sinLat)) / (4.0 * kPi)) * world);
        };
        if (!geometryReusable) {
        // Azimuth tiling from ACTUAL neighbor spacing, not just the resolution
        // code. Where ray spacing outgrows the coded resolution (super-res
        // dropout at far range, mixed sweeps), width-from-code leaves
        // ray-wide background corduroy. Tile by azimuth midpoints, clamped to
        // [0.5x, 2x] the coded half-width: regular data renders exactly as
        // before, mismatched zones fill, and real data gaps stay honestly
        // dark instead of being smeared shut.
        struct AzSpan {
            double lower = 0.0;
            double upper = 0.0;
            bool valid = false;
        };
        std::vector<AzSpan> azSpans(chunk->rays.size());
        {
            std::vector<std::pair<double, std::size_t>> order;
            order.reserve(chunk->rays.size());
            for (std::size_t rayIndex = 0; rayIndex < chunk->rays.size(); ++rayIndex) {
                const auto& ray = chunk->rays[rayIndex];
                if (elevation && ray.elevationNumber != *elevation) continue;
                if (!ray.has(product)) continue;
                auto az = std::fmod(static_cast<double>(ray.azimuthDegrees), 360.0);
                if (az < 0.0) az += 360.0;
                order.emplace_back(az, rayIndex);
            }
            std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) {
                return a.first < b.first;
            });
            const std::size_t count = order.size();
            for (std::size_t pos = 0; pos < count; ++pos) {
                const auto [centerAz, rayIndex] = order[pos];
                const auto& ray = chunk->rays[rayIndex];
                const double resHalf = std::clamp(
                    static_cast<double>(ray.azimuthResolutionDegrees) * 0.5, 0.1, 0.75);
                double halfLo = resHalf;
                double halfHi = resHalf;
                if (count > 1) {
                    double prevAz = order[(pos + count - 1) % count].first;
                    double nextAz = order[(pos + 1) % count].first;
                    double distPrev = centerAz - prevAz;
                    if (distPrev <= 0.0) distPrev += 360.0;
                    double distNext = nextAz - centerAz;
                    if (distNext <= 0.0) distNext += 360.0;
                    halfLo = std::clamp(distPrev * 0.5, resHalf * 0.5, resHalf * 2.0);
                    halfHi = std::clamp(distNext * 0.5, resHalf * 0.5, resHalf * 2.0);
                }
                const double factor = smoothDisplay ? 1.35 : 1.0;
                azSpans[rayIndex] = {centerAz - halfLo * factor, centerAz + halfHi * factor, true};
            }
        }
        for (std::size_t rayIndex = 0; rayIndex < chunk->rays.size(); ++rayIndex) {
            // Stale-work checkpoint: a pan/zoom since this worker started
            // makes its center obsolete, so drop it instead of rasterizing
            // a full volume nobody will ever see.
            if ((rayIndex & 63) == 0 && (!guard || requestId != guard->overlayRequestId_.load(
                                                                     std::memory_order_relaxed))) {
                QMetaObject::invokeMethod(
                    guard,
                    [guard] {
                        if (guard) guard->overlayBuildPending_ = false;
                    },
                    Qt::QueuedConnection);
                return;
            }
            const auto& ray = chunk->rays[rayIndex];
            if (elevation && ray.elevationNumber != *elevation) continue;
            if (!ray.has(product)) continue;
            const auto productIndex = static_cast<std::size_t>(product);
            const auto& values = ray.fields.at(productIndex);
            const auto firstGateMeters = ray.firstGateByProduct.at(productIndex);
            const auto gateSpacingMeters = ray.gateSpacingByProduct.at(productIndex);
            if (gateSpacingMeters == 0) continue;
            maximumRangeMeters = std::max(
                maximumRangeMeters,
                static_cast<double>(firstGateMeters) +
                    std::max(0.0, static_cast<double>(values.size()) - 0.5) * gateSpacingMeters);
            const auto halfRange = static_cast<double>(gateSpacingMeters) * 0.5;
            for (std::size_t gate = 0; gate < values.size(); ++gate) {
                std::optional<double> value;
                if (product == ProductKind::Velocity && processedVelocity &&
                    rayIndex < processedVelocity->size() && gate < processedVelocity->at(rayIndex).size() &&
                    std::isfinite(processedVelocity->at(rayIndex)[gate])) {
                    value = processedVelocity->at(rayIndex)[gate];
                } else {
                    value = nativeValue(values[gate], ray.scaleByProduct.at(productIndex),
                                        ray.offsetByProduct.at(productIndex));
                }
                const auto belowThreshold = values[gate] == 0;
                const auto rangeFolded = values[gate] == 1;
                if (!value && !(belowThresholdVisible && (belowThreshold || rangeFolded))) continue;
                EchoGateGeometry::Gate quad;
                quad.value01 = value ? static_cast<float>(std::clamp(
                                           (*value - descriptor.minimum) /
                                               (descriptor.maximum - descriptor.minimum),
                                           0.0, 1.0))
                                     : 0.0F;
                quad.kind = static_cast<std::uint8_t>(value ? 0 : (rangeFolded ? 1 : 2));
                const auto rangeCenter = static_cast<double>(firstGateMeters) +
                                         static_cast<double>(gate) * gateSpacingMeters;
                const auto lowerRange = std::max(0.0, rangeCenter - halfRange);
                const auto upperRange = rangeCenter + halfRange;
                if (upperRange <= 0.0) continue;
                const AzSpan& span = azSpans[rayIndex];
                const double resHalf = std::clamp(
                    static_cast<double>(ray.azimuthResolutionDegrees) * 0.5, 0.1, 0.75);
                const double spanFactor = smoothDisplay ? 1.35 : 1.0;
                const auto lowerAzimuth =
                    span.valid ? span.lower
                               : static_cast<double>(ray.azimuthDegrees) - resHalf * spanFactor;
                const auto upperAzimuth =
                    span.valid ? span.upper
                               : static_cast<double>(ray.azimuthDegrees) + resHalf * spanFactor;
                const auto cornerAt = [&](const double range, const double azimuth) {
                    const auto beam = standardBeamGeometry(range, ray.elevationDegrees);
                    const auto geo = destinationPoint({baseLatitude, baseLongitude}, azimuth,
                                                      beam.surfaceDistanceMeters);
                    return worldPointAt(geo.latitude, geo.longitude);
                };
                const QPointF corners[4] = {
                    cornerAt(lowerRange, lowerAzimuth),
                    cornerAt(upperRange, lowerAzimuth),
                    cornerAt(upperRange, upperAzimuth),
                    cornerAt(lowerRange, upperAzimuth),
                };
                for (int corner = 0; corner < 4; ++corner) {
                    quad.x[corner] = static_cast<float>(corners[corner].x());
                    quad.y[corner] = static_cast<float>(corners[corner].y());
                }
                builtGates.push_back(quad);
            }
        }
        // Publish the fresh snapshot: the next pan-triggered rebuild reuses
        // these quads instead of redoing per-gate trig. Oversized all-tilt
        // volumes skip the cache (transient build only) to bound memory.
        constexpr std::size_t kMaxCachedQuads = 1200000;
        auto fresh = std::make_shared<EchoGateGeometry>();
        fresh->chunk = chunk;
        fresh->velocity = processedVelocity;
        fresh->product = product;
        fresh->elevation = elevation;
        fresh->zoom = targetZoom;
        fresh->smooth = smoothDisplay;
        fresh->belowVisible = belowThresholdVisible;
        fresh->maximumRangeMeters = maximumRangeMeters;
        fresh->gates = std::move(builtGates);
        cacheable = fresh->gates.size() <= kMaxCachedQuads;
        geometry = std::move(fresh);
        }  // end if (!geometryReusable)
        const auto overlayLogicalSize = viewportSize + QSize(kOverlayMargin * 2, kOverlayMargin * 2);
        const auto pixelSize = QSize(
            std::max(1, static_cast<int>(std::ceil(overlayLogicalSize.width() * devicePixelRatio))),
            std::max(1, static_cast<int>(std::ceil(overlayLogicalSize.height() * devicePixelRatio))));
        QImage overlayImage(pixelSize, QImage::Format_ARGB32);
        overlayImage.fill(Qt::transparent);
        const auto overlayCenterWorld = worldPointAt(centerCoordinate.y(), centerCoordinate.x());
        // 64-entry color LUT: the old code ran productColor() per gate.
        std::array<QRgb, 64> colorLut;
        for (int lutIndex = 0; lutIndex < 64; ++lutIndex) {
            colorLut[lutIndex] =
                productColor(product, static_cast<double>(lutIndex) / 63.0, palette, opacity,
                             customPalette)
                    .rgba();
        }
        rasterizeEchoGates(overlayImage, geometry->gates, overlayCenterWorld, devicePixelRatio,
                           colorLut, QColor(150, 150, 160, 95).rgba(), QColor(45, 70, 78, 55).rgba());
        overlayImage = overlayImage.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        overlayImage.setDevicePixelRatio(devicePixelRatio);
        if (!guard) return;
        QMetaObject::invokeMethod(guard, [guard, generation, targetZoom,
                                          overlayImage = std::move(overlayImage),
                                          overlayLogicalSize, overlayCenterWorld,
                                          maximumRangeMeters, cacheable,
                                          geometry = std::move(geometry)]() mutable {
            if (!guard) return;
            guard->overlayBuildPending_ = false;
            if (generation == guard->overlayGeneration_ && targetZoom == guard->zoom_) {
                guard->overlayImage_ = std::move(overlayImage);
                guard->overlayImageLogicalSize_ = overlayLogicalSize;
                guard->overlayImageCenterWorld_ = overlayCenterWorld;
                guard->maximumRangeMeters_ = maximumRangeMeters > 0.0
                                                  ? maximumRangeMeters
                                                  : 230000.0;
                guard->overlayZoom_ = targetZoom;
                guard->overlayImageGeneration_ = generation;
                if (cacheable) guard->echoGeometry_ = std::move(geometry);
            }
            guard->update();
        }, Qt::QueuedConnection);
    });
}

void RadarMapCanvas::paintRadarOverlay(QPainter& painter) {
    const auto* site = selectedSite();
    if (!site || !site->latitude || !site->longitude) {
        return;
    }

    if (!overlayCacheCoversView()) rebuildOverlay();
    if (!chunk_) return;
    painter.save();
    if (!overlayImage_.isNull()) {
        const auto world = static_cast<double>(kTileSize * (1 << zoom_));
        const auto currentCenter = mapCenterWorld();
        const auto zoomScale = std::ldexp(1.0, zoom_ - overlayZoom_);
        const auto cachedCenterAtCurrentZoom = overlayImageCenterWorld_ * zoomScale;
        auto dx = cachedCenterAtCurrentZoom.x() - currentCenter.x();
        if (dx > world / 2.0) dx -= world;
        if (dx < -world / 2.0) dx += world;
        const auto dy = cachedCenterAtCurrentZoom.y() - currentCenter.y();
        const QSizeF scaledSize(overlayImageLogicalSize_.width() * zoomScale,
                                overlayImageLogicalSize_.height() * zoomScale);
        const QPointF topLeft(
            std::round(width() / 2.0 - scaledSize.width() / 2.0 + dx),
            std::round(height() / 2.0 - scaledSize.height() / 2.0 + dy));
        painter.drawImage(QRectF(topLeft, scaledSize), overlayImage_);
    }
    painter.restore();

    painter.save();
    painter.setPen(QPen(QColor(255, 255, 255, 150), 1));
    painter.setBrush(Qt::NoBrush);
    const auto center = screenPoint(*site->latitude, *site->longitude);
    const auto maximumRangeMeters = maximumRangeMeters_;
    painter.setFont(hudFont(7));
    // Fixed ring set: the operational distance rings plus the radar's own
    // maximum range. Built as a small array instead of a heap-allocated vector
    // that was sorted and de-duplicated on every single paint.
    std::array<double, 4> ringRanges{25000.0, 50000.0, 100000.0, maximumRangeMeters};
    std::sort(ringRanges.begin(), ringRanges.end());
    double previousRing = -1.0;
    for (const auto rangeMeters : ringRanges) {
        if (rangeMeters > maximumRangeMeters + 1000.0) continue;
        // Skip near-duplicate rings (e.g. a 230 km radar whose max is 235 km).
        if (previousRing >= 0.0 && rangeMeters - previousRing < 1000.0) continue;
        previousRing = rangeMeters;
        const auto edgeCoordinate = destinationPoint({*site->latitude, *site->longitude}, 90.0, rangeMeters);
        const auto edge = screenPoint(edgeCoordinate.latitude, edgeCoordinate.longitude);
        const auto radius = std::abs(edge.x() - center.x());
        if (radius < 12.0 || radius > std::max(width(), height()) * 1.5) continue;
        painter.drawEllipse(center, radius, radius);
        // Range labels sit directly on the echo, so a 55-65 dBZ orange core
        // swallowed them completely. Draw a dark halo first, then the glyph.
        const auto label = QStringLiteral("%1 km").arg(rangeMeters / 1000.0, 0, 'f', 0);
        const auto labelWidth = painter.fontMetrics().horizontalAdvance(label) + 6.0;
        const QRectF labelRect(edge.x() - labelWidth / 2.0, edge.y() - 7.0, labelWidth, 13.0);
        if (!QRectF(rect()).intersects(labelRect)) continue;
        painter.setPen(QPen(QColor(0, 0, 0, 200), 2.5));
        for (const QPointF& offset : {QPointF(1.0, 0.0), QPointF(-1.0, 0.0), QPointF(0.0, 1.0),
                                      QPointF(0.0, -1.0)}) {
            painter.drawText(labelRect.translated(offset), Qt::AlignCenter, label);
        }
        painter.setPen(QPen(QColor(255, 255, 255, 175), 1));
        painter.drawText(labelRect, Qt::AlignCenter, label);
    }

    painter.restore();
}

void RadarMapCanvas::ensureWeatherPaths() const {
    if (warningPathsZoom_ == zoom_ && warningPathsGeneration_ == weatherGeneration_) return;
    warningPaths_.clear();
    stormReportWorldPoints_.clear();
    stormReportWorldPoints_.reserve(stormReports_.size());
    for (const auto& report : stormReports_)
        stormReportWorldPoints_.push_back(worldPoint(report.coordinate.y(), report.coordinate.x()));
    const auto radiusMeters = proximityRadiusKm_ * 1000.0;
    for (int warningIndex = 0; warningIndex < warnings_.size(); ++warningIndex) {
        const auto& warning = warnings_.at(warningIndex);
        if (!warningFilter_.isEmpty() &&
            !warning.event.contains(warningFilter_, Qt::CaseInsensitive) &&
            !warning.severity.contains(warningFilter_, Qt::CaseInsensitive)) continue;
        if (proximityAnchor_.has_value()) {
            const auto anchor = *proximityAnchor_;
            bool inRange = false;
            for (const auto& polygon : warning.polygons) {
                for (const auto& point : polygon.exterior) {
                    if (haversineMeters(anchor.y(), anchor.x(), point.y(), point.x()) <= radiusMeters) {
                        inRange = true;
                        break;
                    }
                }
                if (inRange) break;
            }
            if (!inRange) continue;
        }
        const auto color = warning.alertColor.isValid()
                               ? warning.alertColor
                               : nwsAlertStyle(warning.event).color;
        for (const auto& geographicPolygon : warning.polygons) {
            QPainterPath path;
            path.setFillRule(Qt::OddEvenFill);
            QPolygonF exterior;
            exterior.reserve(geographicPolygon.exterior.size());
            for (const auto& coordinate : geographicPolygon.exterior)
                exterior.push_back(worldPoint(coordinate.y(), coordinate.x()));
            path.addPolygon(exterior);
            for (const auto& geographicHole : geographicPolygon.holes) {
                QPolygonF hole;
                hole.reserve(geographicHole.size());
                for (const auto& coordinate : geographicHole)
                    hole.push_back(worldPoint(coordinate.y(), coordinate.x()));
                path.addPolygon(hole);
            }
            const auto bounds = path.boundingRect();
            warningPaths_.push_back({warningIndex, color, std::move(path), bounds});
        }
    }
    warningPathsZoom_ = zoom_;
    warningPathsGeneration_ = weatherGeneration_;
}

void RadarMapCanvas::paintWeatherOverlays(QPainter& painter) {
    if (!weatherOverlayCacheCoversView()) rebuildWeatherOverlay();
    if (weatherOverlayImage_.isNull() || weatherOverlayImageLogicalSize_.isEmpty() ||
        weatherOverlayZoom_ < kMinZoom) return;

    painter.save();
    const auto world = static_cast<double>(kTileSize * (1 << zoom_));
    const auto currentCenter = mapCenterWorld();
    const auto zoomScale = std::ldexp(1.0, zoom_ - weatherOverlayZoom_);
    const auto cachedCenterAtCurrentZoom = weatherOverlayImageCenterWorld_ * zoomScale;
    auto dx = cachedCenterAtCurrentZoom.x() - currentCenter.x();
    if (dx > world / 2.0) dx -= world;
    if (dx < -world / 2.0) dx += world;
    const auto dy = cachedCenterAtCurrentZoom.y() - currentCenter.y();
    const QSizeF scaledSize(weatherOverlayImageLogicalSize_.width() * zoomScale,
                            weatherOverlayImageLogicalSize_.height() * zoomScale);
    const QPointF topLeft(
        std::round(width() / 2.0 - scaledSize.width() / 2.0 + dx),
        std::round(height() / 2.0 - scaledSize.height() / 2.0 + dy));
    painter.drawImage(QRectF(topLeft, scaledSize), weatherOverlayImage_);
    painter.restore();
}

void RadarMapCanvas::paintSpcOutlook(QPainter& painter) {
    if (!spcOutlookVisible_ || spcOutlooks_.isEmpty()) return;
    // Background context under warnings: official SPC fills at low alpha
    // with the official outline. Day 2 draws first so Day 1 wins overlaps.
    painter.save();    painter.setRenderHint(QPainter::Antialiasing, true);
    QVector<const SpcOutlookFeature*> ordered;
    for (const auto& feature : spcOutlooks_) ordered.push_back(&feature);
    std::sort(ordered.begin(), ordered.end(), [](const SpcOutlookFeature* a, const SpcOutlookFeature* b) {
        // Day 2 draws first so Day 1 wins overlaps; within a day higher
        // severity paints last.
        if (a->day != b->day) return a->day > b->day;
        return a->dn < b->dn;
    });
    const QRectF viewport(rect().adjusted(-80, -80, 80, 80));
    for (const auto* feature : ordered) {
        QColor fill(feature->fill.isEmpty() ? QStringLiteral("#94a3b8") : feature->fill);
        QColor stroke(feature->stroke.isEmpty() ? QStringLiteral("#475569") : feature->stroke);
        fill.setAlpha(34);
        painter.setBrush(fill);
        painter.setPen(QPen(stroke, 1.5));
        for (const auto& ring : feature->rings) {
            QPolygonF polygon;
            polygon.reserve(ring.exterior.size());
            for (const auto& lonLat : ring.exterior)
                polygon.push_back(screenPoint(lonLat.y(), lonLat.x()));
            if (!viewport.intersects(polygon.boundingRect())) continue;
            painter.drawPolygon(polygon, Qt::OddEvenFill);
            if (zoom_ >= 6 && !feature->label2.isEmpty()) {
                double cx = 0.0, cy = 0.0;
                for (const auto& point : polygon) {
                    cx += point.x();
                    cy += point.y();
                }
                cx /= std::max(1.0, static_cast<double>(polygon.size()));
                cy /= std::max(1.0, static_cast<double>(polygon.size()));
                painter.setPen(stroke);
                painter.setFont(hudFont(7, QFont::DemiBold));
                painter.drawText(QRectF(cx - 90, cy - 10, 180, 20),
                                 Qt::AlignCenter,
                                 QStringLiteral("Day %1 %2").arg(feature->day).arg(feature->label2));
            }
        }
    }
    painter.restore();
}

void RadarMapCanvas::paintSpcFireOutlook(QPainter& painter) {
    if (!spcFireVisible_ || spcFireOutlooks_.isEmpty()) return;
    // Same background slot as the categorical outlook: official SPC fire
    // fills at low alpha. Day 2 first so Day 1 wins; Critical over Elevated.
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    QVector<const SpcOutlookFeature*> ordered;
    for (const auto& feature : spcFireOutlooks_) ordered.push_back(&feature);
    std::sort(ordered.begin(), ordered.end(), [](const SpcOutlookFeature* a, const SpcOutlookFeature* b) {
        if (a->day != b->day) return a->day > b->day;
        return a->dn < b->dn;
    });
    const QRectF viewport(rect().adjusted(-80, -80, 80, 80));
    for (const auto* feature : ordered) {
        QColor fill(feature->fill.isEmpty() ? QStringLiteral("#fdba74") : feature->fill);
        QColor stroke(feature->stroke.isEmpty() ? QStringLiteral("#c2410c") : feature->stroke);
        fill.setAlpha(34);
        painter.setBrush(fill);
        painter.setPen(QPen(stroke, 1.5));
        for (const auto& ring : feature->rings) {
            QPolygonF polygon;
            polygon.reserve(ring.exterior.size());
            for (const auto& lonLat : ring.exterior)
                polygon.push_back(screenPoint(lonLat.y(), lonLat.x()));
            if (!viewport.intersects(polygon.boundingRect())) continue;
            painter.drawPolygon(polygon, Qt::OddEvenFill);
            if (zoom_ >= 6 && !feature->label2.isEmpty()) {
                double cx = 0.0, cy = 0.0;
                for (const auto& point : polygon) {
                    cx += point.x();
                    cy += point.y();
                }
                cx /= std::max(1.0, static_cast<double>(polygon.size()));
                cy /= std::max(1.0, static_cast<double>(polygon.size()));
                painter.setPen(stroke);
                painter.setFont(hudFont(7, QFont::DemiBold));
                const auto kind =
                    feature->fireKind.isEmpty() ? QString{} : QStringLiteral(" %1").arg(feature->fireKind);
                painter.drawText(QRectF(cx - 90, cy - 10, 180, 20), Qt::AlignCenter,
                                 QStringLiteral("Fire D%1%2 %3")
                                     .arg(feature->day)
                                     .arg(kind, feature->label2));
            }
        }
    }
    painter.restore();
}

void RadarMapCanvas::paintNhcOverlays(QPainter& painter) {
    if (!nhcVisible_ || nhcStorms_.isEmpty()) return;
    // Cyclone context above warnings: translucent cone + white track line.
    // Cones are large; the viewport check keeps off-screen storms cheap.
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF viewport(rect().adjusted(-80, -80, 80, 80));
    for (const auto& storm : nhcStorms_) {
        if (storm.cone.size() >= 4) {
            QPolygonF polygon;
            polygon.reserve(storm.cone.size());
            for (const auto& lonLat : storm.cone) polygon.push_back(screenPoint(lonLat.y(), lonLat.x()));
            if (viewport.intersects(polygon.boundingRect())) {
                QColor fill(255, 123, 0);
                fill.setAlpha(30);
                painter.setBrush(fill);
                painter.setPen(QPen(QColor(255, 123, 0), 1.5));
                painter.drawPolygon(polygon, Qt::OddEvenFill);
                if (zoom_ >= 5 && !storm.name.isEmpty()) {
                    const auto bounds = polygon.boundingRect();
                    painter.setPen(QColor(255, 200, 150));
                    painter.setFont(hudFont(7, QFont::DemiBold));
                    painter.drawText(QRectF(bounds.left(), bounds.top() - 18, 240, 16),
                                     Qt::AlignLeft | Qt::AlignVCenter, storm.name);
                }
            }
        }
        if (storm.track.size() >= 2) {
            QPolygonF line;
            for (const auto& lonLat : storm.track) line.push_back(screenPoint(lonLat.y(), lonLat.x()));
            if (viewport.intersects(line.boundingRect())) {
                painter.setBrush(Qt::NoBrush);
                painter.setPen(QPen(QColor(245, 245, 245), 1.5));
                painter.drawPolyline(line);
            }
        }
    }
    painter.restore();
}

void RadarMapCanvas::paintWpcOutlook(QPainter& painter) {
    if (!wpcVisible_ || wpcOutlooks_.isEmpty()) return;
    // Same background slot as the SPC outlooks: official WPC fills at low
    // alpha. Day 1 paints last so it wins overlaps; MDT over SLGT over MRGL.
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    QVector<const SpcOutlookFeature*> ordered;
    for (const auto& feature : wpcOutlooks_) ordered.push_back(&feature);
    std::sort(ordered.begin(), ordered.end(), [](const SpcOutlookFeature* a, const SpcOutlookFeature* b) {
        if (a->day != b->day) return a->day > b->day;
        return a->dn < b->dn;
    });
    const QRectF viewport(rect().adjusted(-80, -80, 80, 80));
    for (const auto* feature : ordered) {
        QColor fill(feature->fill.isEmpty() ? QStringLiteral("#38A800") : feature->fill);
        QColor stroke(feature->stroke.isEmpty() ? QStringLiteral("#00734C") : feature->stroke);
        fill.setAlpha(34);
        painter.setBrush(fill);
        painter.setPen(QPen(stroke, 1.5));
        for (const auto& ring : feature->rings) {
            QPolygonF polygon;
            polygon.reserve(ring.exterior.size());
            for (const auto& lonLat : ring.exterior)
                polygon.push_back(screenPoint(lonLat.y(), lonLat.x()));
            if (!viewport.intersects(polygon.boundingRect())) continue;
            painter.drawPolygon(polygon, Qt::OddEvenFill);
            if (zoom_ >= 6 && !feature->label2.isEmpty()) {
                double cx = 0.0, cy = 0.0;
                for (const auto& point : polygon) {
                    cx += point.x();
                    cy += point.y();
                }
                cx /= std::max(1.0, static_cast<double>(polygon.size()));
                cy /= std::max(1.0, static_cast<double>(polygon.size()));
                painter.setPen(stroke);
                painter.setFont(hudFont(7, QFont::DemiBold));
                painter.drawText(QRectF(cx - 90, cy - 10, 180, 20), Qt::AlignCenter,
                                 QStringLiteral("ERO D%1 %2").arg(feature->day).arg(feature->label2));
            }
        }
    }
    painter.restore();
}

void RadarMapCanvas::paintHmsSmoke(QPainter& painter) {
    if (!hmsVisible_ || hmsPlumes_.isEmpty()) return;
    // Smoke shading above warnings: official HMS hues (green/yellow/red)
    // at low alpha, density in the label. Never hides what is underneath.
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF viewport(rect().adjusted(-80, -80, 80, 80));
    for (const auto& plume : hmsPlumes_) {
        if (plume.ring.size() < 4) continue;
        QPolygonF polygon;
        polygon.reserve(plume.ring.size());
        for (const auto& lonLat : plume.ring) polygon.push_back(screenPoint(lonLat.y(), lonLat.x()));
        if (!viewport.intersects(polygon.boundingRect())) continue;
        const QColor base = plume.density == HmsSmokeDensity::Heavy     ? QColor(255, 0, 0)
                            : plume.density == HmsSmokeDensity::Medium ? QColor(255, 255, 0)
                                                                       : QColor(0, 255, 0);
        QColor fill = base;
        fill.setAlpha(40);
        painter.setBrush(fill);
        painter.setPen(QPen(base, 1.0));
        painter.drawPolygon(polygon, Qt::OddEvenFill);
        if (zoom_ >= 6) {
            const auto bounds = polygon.boundingRect();
            painter.setPen(base);
            painter.setFont(hudFont(7, QFont::DemiBold));
            painter.drawText(QRectF(bounds.left(), bounds.top() - 18, 200, 16),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             QStringLiteral("SMOKE %1").arg(hmsSmokeDensityLabel(plume.density)));
        }
    }
    painter.restore();
}

std::optional<HmsFirePoint> RadarMapCanvas::hmsFireNear(const QPointF& point) const {
    if (!hmsVisible_ || hmsFires_.isEmpty()) return std::nullopt;
    const HmsFirePoint* best = nullptr;
    double bestDistance = 12.0;
    for (const auto& fire : hmsFires_) {
        const auto firePoint = screenPoint(fire.latitudeDeg, fire.longitudeDeg);
        const double distance = QLineF(firePoint, point).length();
        if (distance < bestDistance) {
            bestDistance = distance;
            best = &fire;
        }
    }
    if (!best) return std::nullopt;
    return *best;
}

void RadarMapCanvas::paintHmsFires(QPainter& painter) {
    if (!hmsVisible_ || hmsFires_.isEmpty()) return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF viewport(rect().adjusted(-40, -40, 40, 40));
    // Declutter like METAR dots: one marker per cell below zoom 7, so a
    // big complex reads as embers instead of an orange blob.
    QSet<QString> occupiedCells;
    const bool declutter = zoom_ < 7;
    painter.setFont(hudFont(7, QFont::DemiBold));
    for (const auto& fire : hmsFires_) {
        const auto point = screenPoint(fire.latitudeDeg, fire.longitudeDeg);
        if (!viewport.contains(point)) continue;
        if (declutter) {
            const auto cell = QStringLiteral("%1/%2")
                                  .arg(static_cast<int>(point.x()) / 14)
                                  .arg(static_cast<int>(point.y()) / 14);
            if (occupiedCells.contains(cell)) continue;
            occupiedCells.insert(cell);
        }
        painter.setPen(QPen(QColor(10, 14, 17), 1.5));
        painter.setBrush(QColor(255, 140, 0));
        painter.drawEllipse(point, 3.0, 3.0);
    }
    painter.restore();
}

void RadarMapCanvas::paintDatPaths(QPainter& painter) {
    if (!datVisible_ || datPaths_.isEmpty()) return;
    // Survey centerlines above warnings: EF-colored polylines with a start
    // dot. Post-event ground truth, never a forecast.
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF viewport(rect().adjusted(-80, -80, 80, 80));
    for (const auto& path : datPaths_) {
        if (path.line.size() < 2) continue;
        QPolygonF line;
        line.reserve(path.line.size());
        for (const auto& lonLat : path.line) line.push_back(screenPoint(lonLat.y(), lonLat.x()));
        if (!viewport.intersects(line.boundingRect())) continue;
        QString stroke;
        QString fill;
        datPathColors(path.efScale, stroke, fill);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(stroke), 2.0));
        painter.drawPolyline(line);
        painter.setPen(QPen(QColor(10, 14, 17), 1.5));
        painter.setBrush(QColor(stroke));
        painter.drawEllipse(line.front(), 3.5, 3.5);
        if (zoom_ >= 6 && !path.efScale.isEmpty()) {
            painter.setPen(QColor(stroke));
            painter.setFont(hudFont(7, QFont::DemiBold));
            painter.drawText(QRectF(line.front().x() + 8.0, line.front().y() - 18, 120, 16),
                             Qt::AlignLeft | Qt::AlignVCenter, path.efScale.toUpper());
        }
    }
    painter.restore();
}

void RadarMapCanvas::paintHrrr(QPainter& painter) {
    // NOTE: no overlayAt branch for HRRR on purpose. The model field covers
    // the whole view wherever it holds echo, so a click card would steal
    // every echo probe underneath it. Valid time reads on the legend chip
    // and the layer badge instead; probing stays on live gates.
    if (!hrrrVisible_ || !hrrrFrame_.has_value()) return;
    // Model context under live echo: operational reflectivity colors at
    // reduced alpha, viewport-culled per cell. Row orientation is never
    // assumed: every cell inverts through the frame projection.
    const HrrrFrame& frame = *hrrrFrame_;
    if (frame.nx == 0 || frame.ny == 0 ||
        frame.reflectivityDbz.size() != frame.nx * frame.ny)
        return;
    // Opt-in paint profile (CAPPEDWEATHER_PAINT_PROFILE=1): wall time plus
    // painted cells on stderr. Off by default: zero overhead.
    QElapsedTimer profileClock;
    std::size_t profileCells = 0;
    const bool profiling = qEnvironmentVariableIsSet("CAPPEDWEATHER_PAINT_PROFILE");
    if (profiling) profileClock.start();
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    const QRectF viewport(rect().adjusted(-40, -40, 40, 40));
    const double cell = zoom_ >= 6 ? 4.0 : 2.5;
    for (std::size_t iy = 0; iy < frame.ny; ++iy) {
        for (std::size_t ix = 0; ix < frame.nx; ++ix) {
            const float dbz = frame.reflectivityDbz[iy * frame.nx + ix];
            if (!std::isfinite(dbz) || dbz < 5.0F) continue;
            const double xMeters = (frame.x0Km + ix * frame.dxKm) * 1000.0;
            const double yMeters = (frame.y0Km + iy * frame.dyKm) * 1000.0;
            const auto lonLat = lambertInverse(frame.projection, xMeters, yMeters);
            if (!std::isfinite(lonLat.longitudeDeg) || !std::isfinite(lonLat.latitudeDeg))
                continue;
            const auto point =
                screenPoint(lonLat.latitudeDeg, lonLat.longitudeDeg);
            if (!viewport.contains(point)) continue;
            painter.fillRect(QRectF(point.x() - cell / 2.0, point.y() - cell / 2.0, cell, cell),
                             colorForValue(ProductKind::Reflectivity, dbz, palette_, 45,
                                           customPalette_));
            ++profileCells;
        }
    }
    if (profiling) {
        std::fprintf(stderr, "paintHrrr: %lld ms, %zu cells\n",
                     static_cast<long long>(profileClock.elapsed()),
                     static_cast<std::size_t>(profileCells));
    }
    painter.restore();
}

void RadarMapCanvas::paintGoes(QPainter& painter) {
    if (!goesVisible_ || !goesFrame_.has_value()) return;
    // Observed IR context under live echo: grayscale by cloud-top coldness,
    // warm ground skipped so the map never washes out. Same raster idiom
    // and no-click rule as HRRR.
    const GoesFrame& frame = *goesFrame_;
    if (frame.nx == 0 || frame.ny == 0 ||
        frame.brightnessCelsius.size() != frame.nx * frame.ny)
        return;
    QElapsedTimer profileClock;
    std::size_t profileCells = 0;
    const bool profiling = qEnvironmentVariableIsSet("CAPPEDWEATHER_PAINT_PROFILE");
    if (profiling) profileClock.start();
    // Shared grid (see requestGoesGeo): null while the off-thread build
    // is in flight, which is the honest waiting state. The legend chip
    // already reads the new frame time; raster pops in on delivery.
    const std::shared_ptr<const GoesGeoData> geo = goesGeo_;
    if (!geo) return;
    if (geo->lonDeg.size() != frame.nx * frame.ny ||
        geo->latDeg.size() != frame.nx * frame.ny)
        return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    // Screen-space raster: one direct-pixel pass plus a single drawImage,
    // instead of a QPainter fillRect per cell. Same sampled cells, same
    // gray ramp and alpha; dense overlaps resolve last-wins instead of
    // double-blending, which reads marginally lighter and is the more
    // honest composite.
    const int imgW = width();
    const int imgH = height();
    if (imgW <= 0 || imgH <= 0) {
        painter.restore();
        return;
    }
    QImage image(imgW, imgH, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    const double worldSize = static_cast<double>(kTileSize * (1 << zoom_));
    const QPointF centerWorld = mapCenterWorld();
    const double halfW = imgW / 2.0;
    const double halfH = imgH / 2.0;
    // Block matches the old rect size (2.5 px zoomed out, 4 px close).
    // Zoom 5 uses 3 px blocks so stride-3 sampling stays continuous.
    const int block = zoom_ >= 6 ? 4 : (zoom_ == 5 ? 3 : 2);
    // Zoom stride: zoomed-out cells share screen pixels, so sample coarsely
    // instead of all ~3.75M. At zoom 4+ the source oversamples the screen
    // even at stride 4, so coverage is preserved.
    const std::size_t stride = zoom_ >= 6 ? 1 : (zoom_ == 5 ? 3 : (zoom_ == 4 ? 4 : 6));
    // Viewport-limited subwindow: forward-project viewport edge samples to
    // fixed-grid radians and iterate only that index range. The mapping is
    // smooth over small on-disc areas, so edge samples bound the interior
    // (plus a 2-cell margin). Mixed success means the disc only partly
    // covers the view, so fall back to the full scan rather than risk
    // clipping real coverage. No success means nothing visible: skip.
    std::size_t ixBegin = 0, ixEnd = frame.nx - 1;
    std::size_t iyBegin = 0, iyEnd = frame.ny - 1;
    // The oracle switch skips this whole block: same lattice, colors and
    // raster path below, but the full grid, so any divergence is a real
    // viewport-optimization bug, never a sampling artifact.
    if (!goesExhaustiveForTest_) {
        const GeoBounds bounds = visibleLonLatBounds();
        if (bounds.valid) {
            double xMin = std::numeric_limits<double>::infinity();
            double xMax = -std::numeric_limits<double>::infinity();
            double yMin = std::numeric_limits<double>::infinity();
            double yMax = -std::numeric_limits<double>::infinity();
            std::size_t ok = 0;
            constexpr int kEdge = 5;
            for (int e = 0; e < kEdge; ++e) {
                const double f = static_cast<double>(e) / (kEdge - 1);
                const double lons[4] = {
                    bounds.west + f * (bounds.east - bounds.west),
                    bounds.west + f * (bounds.east - bounds.west),
                    bounds.west, bounds.east,
                };
                const double lats[4] = {
                    bounds.north, bounds.south,
                    bounds.south + f * (bounds.north - bounds.south),
                    bounds.south + f * (bounds.north - bounds.south),
                };
                for (int s = 0; s < 4; ++s) {
                    const auto angles = goesFixedForward(frame.grid, lons[s], lats[s]);
                    if (!std::isfinite(angles.xRadians) || !std::isfinite(angles.yRadians))
                        continue;
                    ++ok;
                    xMin = std::min(xMin, angles.xRadians);
                    xMax = std::max(xMax, angles.xRadians);
                    yMin = std::min(yMin, angles.yRadians);
                    yMax = std::max(yMax, angles.yRadians);
                }
            }
            if (ok == 0) {
                if (profiling) {
                    std::fprintf(stderr, "paintGoes: %lld ms, 0 cells (off-disc)\n",
                                 static_cast<long long>(profileClock.elapsed()));
                }
                painter.restore();
                return;
            }
            if (ok == 4 * kEdge) {
                const double marginX = 2.0 * std::abs(frame.dxRadians);
                const double marginY = 2.0 * std::abs(frame.dyRadians);
                const double ax0 = (xMin - marginX - frame.x0Radians) / frame.dxRadians;
                const double ax1 = (xMax + marginX - frame.x0Radians) / frame.dxRadians;
                const double ay0 = (yMin - marginY - frame.y0Radians) / frame.dyRadians;
                const double ay1 = (yMax + marginY - frame.y0Radians) / frame.dyRadians;
                ixBegin = static_cast<std::size_t>(std::clamp(
                    std::floor(std::min(ax0, ax1)), 0.0,
                    static_cast<double>(frame.nx - 1)));
                ixEnd = static_cast<std::size_t>(std::clamp(
                    std::ceil(std::max(ax0, ax1)), 0.0,
                    static_cast<double>(frame.nx - 1)));
                iyBegin = static_cast<std::size_t>(std::clamp(
                    std::floor(std::min(ay0, ay1)), 0.0,
                    static_cast<double>(frame.ny - 1)));
                iyEnd = static_cast<std::size_t>(std::clamp(
                    std::ceil(std::max(ay0, ay1)), 0.0,
                    static_cast<double>(frame.ny - 1)));
                // Stride-align the starts down so the sampled lattice is
                // identical to a full scan: same cells, same picture.
                ixBegin = (ixBegin / stride) * stride;
                iyBegin = (iyBegin / stride) * stride;
            }
        }
    }
    for (std::size_t iy = iyBegin; iy <= iyEnd; iy += stride) {
        for (std::size_t ix = ixBegin; ix <= ixEnd; ix += stride) {
            const std::size_t i = iy * frame.nx + ix;
            const float bt = frame.brightnessCelsius[i];
            if (!std::isfinite(bt) || bt > 10.0F) continue;
            const float lon = geo->lonDeg[i];
            const float lat = geo->latDeg[i];
            if (!std::isfinite(lon) || !std::isfinite(lat)) continue;
            // Inline worldPoint + screenPointFromWorld: one sin/log pair,
            // no QPointF temporaries, integer screen pixels for the raster.
            const double latRad = qDegreesToRadians(
                std::clamp(static_cast<double>(lat), -85.0511, 85.0511));
            const double sinLat = std::sin(latRad);
            const double wx =
                (static_cast<double>(lon) + 180.0) / 360.0 * worldSize;
            const double wy =
                (0.5 - std::log((1.0 + sinLat) / (1.0 - sinLat)) / (4.0 * kPi)) * worldSize;
            double dx = wx - centerWorld.x();
            if (dx > worldSize / 2.0) dx -= worldSize;
            if (dx < -worldSize / 2.0) dx += worldSize;
            const int px = static_cast<int>(halfW + dx);
            const int py = static_cast<int>(halfH + wy - centerWorld.y());
            if (px < -block || py < -block || px >= imgW + block || py >= imgH + block)
                continue;
            // Cold white to warm dark over -90..+10 C, same ramp as before.
            const int gray = std::clamp(
                static_cast<int>(255.0F * (10.0F - bt) / 100.0F), 0, 255);
            const QRgb pixel = qRgba(gray, gray, gray, 110);
            for (int by = 0; by < block; ++by) {
                const int yy = py + by;
                if (yy < 0 || yy >= imgH) continue;
                QRgb* line = reinterpret_cast<QRgb*>(image.scanLine(yy));
                for (int bx = 0; bx < block; ++bx) {
                    const int xx = px + bx;
                    if (xx < 0 || xx >= imgW) continue;
                    line[xx] = pixel;
                }
            }
            ++profileCells;
        }
    }
    painter.drawImage(0, 0, image);
    if (profiling) {
        std::fprintf(stderr, "paintGoes: %lld ms, %zu cells\n",
                     static_cast<long long>(profileClock.elapsed()),
                     static_cast<std::size_t>(profileCells));
    }
    painter.restore();
}

void RadarMapCanvas::paintMosaic(QPainter& painter) {
    if (!mosaicVisible_ || !mosaicFrame_.has_value()) return;
    // National context under live echo: native IEM colors at reduced alpha,
    // viewport-culled per cell. Near-black stays out (NODATA plus sub-zero
    // echo share the black ramp entry), so the map never washes out.
    const MosaicFrame& frame = *mosaicFrame_;
    const QImage& image = frame.image;
    if (image.isNull() || frame.grid.nx == 0 || frame.grid.ny == 0 ||
        static_cast<std::size_t>(image.width()) != frame.grid.nx ||
        static_cast<std::size_t>(image.height()) != frame.grid.ny)
        return;
    QElapsedTimer profileClock;
    std::size_t profileCells = 0;
    const bool profiling = qEnvironmentVariableIsSet("CAPPEDWEATHER_PAINT_PROFILE");
    if (profiling) profileClock.start();
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    const QRectF viewport(rect().adjusted(-40, -40, 40, 40));
    const double cell = zoom_ >= 6 ? 4.0 : 2.5;
    for (std::size_t iy = 0; iy < frame.grid.ny; ++iy) {
        for (std::size_t ix = 0; ix < frame.grid.nx; ++ix) {
            const QRgb pixel = image.pixel(static_cast<int>(ix), static_cast<int>(iy));
            if (std::max({qRed(pixel), qGreen(pixel), qBlue(pixel)}) < 12) continue;
            double lon = 0.0, lat = 0.0;
            mosaicCellLonLat(frame.grid, ix, iy, lon, lat);
            if (!std::isfinite(lon) || !std::isfinite(lat)) continue;
            const auto point = screenPoint(lat, lon);
            if (!viewport.contains(point)) continue;
            QColor color(pixel);
            color.setAlpha(100);
            painter.fillRect(QRectF(point.x() - cell / 2.0, point.y() - cell / 2.0, cell, cell),
                             color);
            ++profileCells;
        }
    }
    if (profiling) {
        std::fprintf(stderr, "paintMosaic: %lld ms, %zu cells\n",
                     static_cast<long long>(profileClock.elapsed()),
                     static_cast<std::size_t>(profileCells));
    }
    painter.restore();
}

void RadarMapCanvas::paintMcdOverlays(QPainter& painter) {    if (!mcdVisible_ || mcds_.isEmpty()) return;
    // Watch-area outlines above warnings: dashed violet, never filled enough
    // to hide what is underneath. Labels carry the watch probability.
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF viewport(rect().adjusted(-80, -80, 80, 80));
    for (const auto& mcd : mcds_) {
        if (mcd.exterior.size() < 4) continue;
        QPolygonF polygon;
        polygon.reserve(mcd.exterior.size());
        for (const auto& lonLat : mcd.exterior) polygon.push_back(screenPoint(lonLat.y(), lonLat.x()));
        if (!viewport.intersects(polygon.boundingRect())) continue;
        QColor violet(QStringLiteral("#c084fc"));
        QColor fill = violet;
        fill.setAlpha(22);
        painter.setBrush(fill);
        painter.setPen(QPen(violet, 2.0, Qt::DashLine));
        painter.drawPolygon(polygon, Qt::OddEvenFill);
        if (zoom_ >= 6) {
            const auto bounds = polygon.boundingRect();
            painter.setPen(QColor(233, 213, 255));
            painter.setFont(hudFont(7, QFont::DemiBold));
            const auto label = mcd.watchProbabilityPercent >= 0
                                   ? QStringLiteral("MCD %1 · %2%").arg(mcd.number).arg(
                                         mcd.watchProbabilityPercent)
                                   : QStringLiteral("MCD %1").arg(mcd.number);
            painter.drawText(QRectF(bounds.left(), bounds.top() - 18, bounds.width(), 16),
                             Qt::AlignLeft | Qt::AlignVCenter, label);
        }
    }
    painter.restore();
}

std::optional<MetarObs> RadarMapCanvas::metarNear(const QPointF& point) const {
    if (!metarVisible_ || metars_.isEmpty()) return std::nullopt;
    const MetarObs* best = nullptr;
    double bestDistance = 14.0;
    for (const auto& metar : metars_) {
        const auto stationPoint = screenPoint(metar.latitude, metar.longitude);
        const double distance = QLineF(stationPoint, point).length();
        if (distance < bestDistance) {
            bestDistance = distance;
            best = &metar;
        }
    }
    if (!best) return std::nullopt;
    return *best;
}

void RadarMapCanvas::paintMetars(QPainter& painter) {
    if (!metarVisible_ || metars_.isEmpty()) return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF viewport(rect().adjusted(-40, -40, 40, 40));
    // Declutter: one label per 30x16 cell at low zoom, like the storm-report
    // grid, so a metroplex reads as dots instead of a text pile.
    QSet<QString> occupiedCells;
    const bool showLabels = zoom_ >= 6;
    painter.setFont(hudFont(7, QFont::DemiBold));
    for (const auto& metar : metars_) {
        const auto point = screenPoint(metar.latitude, metar.longitude);
        if (!viewport.contains(point)) continue;
        if (showLabels) {
            const auto cell = QStringLiteral("%1/%2")
                                  .arg(static_cast<int>(point.x()) / 30)
                                  .arg(static_cast<int>(point.y()) / 16);
            if (occupiedCells.contains(cell)) continue;
            occupiedCells.insert(cell);
        }
        const auto color = metarCategoryColor(metar.flightCategory);
        painter.setPen(QPen(QColor(10, 14, 17), 2.0));
        painter.setBrush(color);
        painter.drawEllipse(point, 4.0, 4.0);
        painter.setPen(QPen(color, 1.0));
        painter.drawEllipse(point, 5.5, 5.5);
        // Experimental precip-type ring: surface temp crossed with the
        // measured melting level. No ring when the melting level is invalid
        // (fail closed) or the station reports no temp.
        if (brightBand_.valid) {
            const auto precip = precipTypeForStation(metar.tempC, true);
            const QColor ring = precip == PrecipType::Snow   ? QColor(125, 211, 252)
                                : precip == PrecipType::Mix   ? QColor(192, 132, 252)
                                : precip == PrecipType::Rain  ? QColor(74, 222, 128)
                                                             : QColor(0, 0, 0, 0);
            if (ring.alpha() != 0) {
                painter.setPen(QPen(ring, 1.5, Qt::DotLine));
                painter.setBrush(Qt::NoBrush);
                painter.drawEllipse(point, 8.5, 8.5);
            }
        }
        if (showLabels) {
            const auto tempText = std::isnan(metar.tempC)
                                      ? QStringLiteral("--")
                                      : QString::number(static_cast<int>(std::lround(metar.tempC)));
            const auto dewText = std::isnan(metar.dewpointC)
                                     ? QStringLiteral("--")
                                     : QString::number(static_cast<int>(std::lround(metar.dewpointC)));
            painter.setPen(QColor(232, 238, 240));
            painter.drawText(QRectF(point.x() + 8.0, point.y() - 8.0, 64.0, 14.0),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             QStringLiteral("%1/%2").arg(tempText, dewText));
        }
    }
    painter.restore();
}

void RadarMapCanvas::paintPlacefiles(QPainter& painter) {
    if (!placefilesVisible_ || placefiles_.isEmpty() || width() <= 0 || height() <= 0) return;
    const auto edge = geographicPoint(QPointF(width(), height() / 2.0));
    const auto visibleRadiusNauticalMiles =
        rangeAndBearing({center_.y(), center_.x()}, {edge.y(), edge.x()}).distanceMeters / 1852.0;
    const auto viewport = QRectF(rect()).adjusted(-80.0, -80.0, 80.0, 80.0);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setFont(hudFont(8, QFont::DemiBold));
    for (const auto& document : placefiles_) {
        for (const auto& element : document.elements) {
            if (visibleRadiusNauticalMiles > element.thresholdNauticalMiles || element.points.isEmpty())
                continue;
            QPolygonF polygon;
            polygon.reserve(element.points.size());
            for (const auto& coordinate : element.points)
                polygon.push_back(screenPoint(coordinate.y(), coordinate.x()));
            if (!viewport.intersects(polygon.boundingRect().adjusted(-12.0, -12.0, 12.0, 12.0))) continue;

            auto color = element.color.isValid() ? element.color : QColor(Qt::white);
            painter.setPen(QPen(color, std::max(1, element.lineWidth), Qt::SolidLine,
                                Qt::RoundCap, Qt::RoundJoin));
            painter.setBrush(Qt::NoBrush);
            if (element.kind == PlacefileElementKind::Line) {
                painter.drawPolyline(polygon);
            } else if (element.kind == PlacefileElementKind::Polygon) {
                auto fill = color;
                fill.setAlpha(48);
                painter.setBrush(fill);
                painter.drawPolygon(polygon, Qt::OddEvenFill);
            } else {
                const auto point = polygon.constFirst();
                if (element.kind == PlacefileElementKind::Icon) {
                    painter.save();
                    painter.translate(point);
                    painter.rotate(element.rotationDegrees);
                    painter.setBrush(color);
                    painter.drawPolygon(QPolygonF{{0.0, -7.0}, {-5.0, 5.0}, {5.0, 5.0}});
                    painter.restore();
                } else if (element.kind == PlacefileElementKind::Marker) {
                    painter.setBrush(color);
                    painter.drawEllipse(point, 4.5, 4.5);
                }
                if (!element.text.isEmpty() && zoom_ >= 7) {
                    painter.setPen(QPen(QColor(8, 15, 19, 230), 3.0));
                    painter.drawText(point + QPointF(8.0, 4.0), element.text);
                    painter.setPen(color);
                    painter.drawText(point + QPointF(8.0, 4.0), element.text);
                }
            }
        }
    }
    painter.restore();
}

void RadarMapCanvas::paintLightningOverlay(QPainter& painter) {
    // Fusion features are needed for the independent shear overlay too, so
    // always refresh even when lightning cells are hidden.
    if (lightningFusionComputedGeneration_ != lightningFusionGeneration_)
        rebuildLightningFusion();

    paintStormTrail(painter);
    paintShearOverlay(painter);
    paintCirculationMarkers(painter);
    paintHailSwaths(painter);
    paintHailCores(painter);
    paintZdrColumns(painter);
    paintTbss(painter);
    paintTdsMarkers(painter);

    if (!lightningVisible_) return;

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    for (std::size_t index = 0; index < lightningCells_.size(); ++index) {
        const auto& cell = lightningCells_[index];
        const auto half = cell.cellSizeDeg * 0.5;
        QPolygonF polygon;
        polygon << screenPoint(cell.latitudeCenterDeg - half, cell.longitudeCenterDeg - half)
                << screenPoint(cell.latitudeCenterDeg - half, cell.longitudeCenterDeg + half)
                << screenPoint(cell.latitudeCenterDeg + half, cell.longitudeCenterDeg + half)
                << screenPoint(cell.latitudeCenterDeg + half, cell.longitudeCenterDeg - half);
        const auto bounds = polygon.boundingRect();
        if (!QRectF(rect().adjusted(-80, -80, 80, 80)).intersects(bounds)) continue;
        const auto color = lightningCellColor(cell);
        const auto selected = pinnedLightningCell_.has_value() &&
                              pinnedLightningCell_->first == cell.cellX &&
                              pinnedLightningCell_->second == cell.cellY;
        if (selected) {
            auto halo = color;
            halo.setAlpha(90);
            painter.setPen(QPen(halo, 7.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.setBrush(Qt::NoBrush);
            painter.drawPolygon(polygon);
        }
        painter.setPen(QPen(color, selected ? 3.5 : cell.active() ? 2.0 : 1.5,
                            cell.active() ? Qt::SolidLine : Qt::DashLine));
        QColor fill = color;
        fill.setAlpha(selected ? 82 : cell.active() ? 48 : 20);
        painter.setBrush(fill);
        painter.drawPolygon(polygon);
        if (zoom_ >= 6 && cell.active()) {
            painter.setPen(QColor(245, 247, 249));
            painter.setFont(hudFont(7, QFont::DemiBold));
            painter.drawText(bounds.adjusted(3, 2, -3, -2), Qt::AlignTop | Qt::AlignLeft,
                             QStringLiteral("%1 sigma | %2/min")
                                 .arg(cell.sigmaLevel, 0, 'f', 1)
                                 .arg(cell.flashRatePerMin, 0, 'f', 0));
        }
    }

    if (lightningFusionVisible_) {
        QSet<int> paintedFeatures;
        for (const auto& association : lightningAssociations_) {
            if (association.lightningCellIndex >= lightningCells_.size() ||
                association.radarFeatureIndex >= radarFeatures_.size()) continue;
            const auto& cell = lightningCells_[association.lightningCellIndex];
            const auto& feature = radarFeatures_[association.radarFeatureIndex];
            const auto cellPoint = screenPoint(cell.latitudeCenterDeg, cell.longitudeCenterDeg);
            const auto featurePoint = screenPoint(feature.latitudeDeg, feature.longitudeDeg);
            if (!rect().adjusted(-100, -100, 100, 100).contains(featurePoint.toPoint()) &&
                !rect().adjusted(-100, -100, 100, 100).contains(cellPoint.toPoint())) continue;
            // Association spokes stay tied to the fusion toggle; the storm
            // object itself is painted by paintShearOverlay so it survives
            // with lightning hidden.
            const QColor fusionColor = feature.rotating ? QColor(255, 78, 210) : QColor(170, 190, 205);
            painter.setPen(QPen(fusionColor, feature.rotating ? 2.0 : 1.0, Qt::DashLine));
            painter.drawLine(cellPoint, featurePoint);
            if (paintedFeatures.contains(static_cast<int>(association.radarFeatureIndex))) continue;
            paintedFeatures.insert(static_cast<int>(association.radarFeatureIndex));
            if (!shearVisible_) continue;
            painter.setBrush(QColor(fusionColor.red(), fusionColor.green(), fusionColor.blue(), 55));
            painter.setPen(QPen(fusionColor, feature.rotating ? 2.5 : 1.5));
            painter.drawEllipse(featurePoint, feature.rotating ? 9.0 : 6.0,
                                feature.rotating ? 9.0 : 6.0);
            if (zoom_ >= 6) {
                painter.setPen(QColor(238, 241, 244));
                painter.setFont(hudFont(7));
                painter.drawText(QRectF(featurePoint.x() + 11, featurePoint.y() - 18, 190, 36),
                                 Qt::AlignLeft | Qt::AlignVCenter,
                                 QStringLiteral("%1 dBZ | shear %2 s^-1 | %3 km")
                                     .arg(feature.maximumReflectivityDbz, 0, 'f', 0)
                                     .arg(feature.azimuthalShearPerSecond, 0, 'f', 3)
                                     .arg(association.distanceKm, 0, 'f', 0));
            }
        }
    }
    painter.restore();
}

void RadarMapCanvas::paintShearOverlay(QPainter& painter) {
    if (!shearVisible_) return;
    // First-class rotation display independent of lightning. The watch tier
    // below TVS/meso markers: vertically-coherent, reflectivity-guarded
    // couplet columns at meso strength that miss classification on depth or
    // diameter. Rotating candidates are pink + labeled; strong non-rotating
    // reflectivity cores are small gray dots (fusion spokes handled above).
    QSet<int> fused;
    if (lightningFusionVisible_ && lightningVisible_) {
        for (const auto& a : lightningAssociations_) {
            if (a.radarFeatureIndex < radarFeatures_.size())
                fused.insert(static_cast<int>(a.radarFeatureIndex));
        }
    }
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    for (std::size_t i = 0; i < radarFeatures_.size(); ++i) {
        const auto& feature = radarFeatures_[i];
        if (fused.contains(static_cast<int>(i))) continue; // already drawn with spoke
        if (feature.rotating) continue; // watch tier below owns rotation display
        if (feature.maximumReflectivityDbz >= 50.0 && zoom_ >= 7) {
            const auto point = screenPoint(feature.latitudeDeg, feature.longitudeDeg);
            if (!rect().adjusted(-40, -40, 40, 40).contains(point.toPoint())) continue;
            // Strong non-rotating cores get a subtle marker so hail cores
            // aren't invisible when the fusion toggle is off.
            painter.setBrush(QColor(170, 190, 205, 40));
            painter.setPen(QPen(QColor(170, 190, 205), 1.0));
            painter.drawEllipse(point, 5.0, 5.0);
        }
    }
    // Bounded by construction: strongest 12 candidates draw rings, and a
    // label must clear 64 px from every accepted label, so even a noisy
    // volume stays readable instead of the old unbounded label soup.
    constexpr int kMaxWatchRings = 12;
    constexpr double kLabelClearPx = 64.0;
    std::vector<QPointF> labeled;
    int drawn = 0;
    for (const auto& candidate : shearWatch_) {
        if (drawn >= kMaxWatchRings) break;
        const auto point = screenPoint(candidate.latitudeDeg, candidate.longitudeDeg);
        if (!rect().adjusted(-40, -40, 40, 40).contains(point.toPoint())) continue;
        const QColor c(255, 78, 210);
        painter.setBrush(QColor(c.red(), c.green(), c.blue(), 60));
        painter.setPen(QPen(c, 2.5));
        painter.drawEllipse(point, 10.0, 10.0);
        painter.setPen(QPen(c, 1.5));
        painter.drawEllipse(point, 14.0, 14.0);
        if (zoom_ >= 6) {
            bool crowded = false;
            for (const auto& accepted : labeled) {
                const double dx = accepted.x() - point.x();
                const double dy = accepted.y() - point.y();
                if (dx * dx + dy * dy < kLabelClearPx * kLabelClearPx) {
                    crowded = true;
                    break;
                }
            }
            if (!crowded) {
                painter.setPen(QColor(255, 220, 245));
                painter.setFont(hudFont(7, QFont::DemiBold));
                painter.drawText(QRectF(point.x() + 16, point.y() - 18, 170, 36),
                                 Qt::AlignLeft | Qt::AlignVCenter,
                                 QStringLiteral("ROT %1 m/s | %2 km deep")
                                     .arg(candidate.maxDeltaVelocityMetersPerSecond, 0, 'f', 0)
                                     .arg(candidate.depthMeters / 1000.0, 0, 'f', 1));
                labeled.push_back(point);
            }
        }
        ++drawn;
    }
    painter.restore();
}

void RadarMapCanvas::paintCirculationMarkers(QPainter& painter) {
    if (!tvsVisible_ || circulations_.empty()) return;
    // GR2-style algorithm layer: TVS = red triangle, mesocyclone = orange
    // diamond, each labeled with delta-V and depth. Markers draw at any zoom;
    // labels need zoom 5+. Only time-persisted detections reach this list.
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    for (const auto& det : circulations_) {
        const auto point = screenPoint(det.latitudeDeg, det.longitudeDeg);
        if (!rect().adjusted(-60, -60, 60, 60).contains(point.toPoint())) continue;
        const bool tvs = det.kind == CirculationKind::TornadoVortex;
        const QColor c = tvs ? QColor(255, 45, 45) : QColor(255, 150, 30);
        QPolygonF marker;
        if (tvs) {
            marker << point + QPointF(0.0, -11.0) << point + QPointF(-9.0, 7.0)
                   << point + QPointF(9.0, 7.0);
        } else {
            marker << point + QPointF(0.0, -9.0) << point + QPointF(9.0, 0.0)
                   << point + QPointF(0.0, 9.0) << point + QPointF(-9.0, 0.0);
        }
        painter.setBrush(QColor(c.red(), c.green(), c.blue(), 70));
        painter.setPen(QPen(c, 2.5));
        painter.drawPolygon(marker);
        if (zoom_ >= 5) {
            painter.setPen(tvs ? QColor(255, 200, 200) : QColor(255, 220, 180));
            painter.setFont(hudFont(7, QFont::DemiBold));
            painter.drawText(
                QRectF(point.x() + 13, point.y() - 20, 200, 36), Qt::AlignLeft | Qt::AlignVCenter,
                QStringLiteral("%1 %2 m/s | %3 km deep")
                    .arg(tvs ? QStringLiteral("TVS") : QStringLiteral("MESO"))
                    .arg(det.maxDeltaVelocityMetersPerSecond, 0, 'f', 0)
                    .arg(det.depthMeters / 1000.0, 0, 'f', 1));
        }
    }
    painter.restore();
}

void RadarMapCanvas::paintHailSwaths(QPainter& painter) {
    if (!hailVisible_) return;
    // Per-storm footprints: convex hulls of persisted hail fixes, tinted by
    // peak tier. Hulls with fewer than 3 distinct points never form (a line
    // is a path, not a footprint), so this loop simply skips them.
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF viewport(rect().adjusted(-80, -80, 80, 80));
    for (const auto& swath : hailSwaths_.swaths()) {
        const auto ring = hailSwathHull(swath);
        if (ring.size() < 3) continue;
        QPolygonF polygon;
        for (const auto& [lat, lon] : ring) polygon.push_back(screenPoint(lat, lon));
        if (!viewport.intersects(polygon.boundingRect())) continue;
        const QColor base = swath.maxTier >= 3   ? QColor(255, 80, 200)
                            : swath.maxTier == 2 ? QColor(64, 224, 255)
                                                 : QColor(120, 180, 220);
        QColor fill = base;
        fill.setAlpha(28);
        painter.setBrush(fill);
        painter.setPen(QPen(base, 1.5, Qt::DashLine));
        painter.drawPolygon(polygon, Qt::OddEvenFill);
        if (zoom_ >= 6) {
            const auto bounds = polygon.boundingRect();
            painter.setPen(base);
            painter.setFont(hudFont(7, QFont::DemiBold));
            painter.drawText(QRectF(bounds.left(), bounds.top() - 18, 200, 16),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             QStringLiteral("HAIL SWATH T%1").arg(swath.maxTier));
        }
    }
    painter.restore();
}

void RadarMapCanvas::paintHailCores(QPainter& painter) {
    if (!hailVisible_ || hailCores_.empty()) return;
    // VIL-density cores: cyan hexagon, labeled with density plus a guidance
    // size range. The label is a range on purpose: VIL density correlates
    // with hail size but never measures a diameter.
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    for (const auto& core : hailCores_) {
        const auto point = screenPoint(core.latitudeDeg, core.longitudeDeg);
        if (!rect().adjusted(-60, -60, 60, 60).contains(point.toPoint())) continue;
        const QColor c = core.tier >= 3   ? QColor(255, 80, 200)
                         : core.tier == 2 ? QColor(64, 224, 255)
                                          : QColor(120, 180, 220);
        QPolygonF marker;
        for (int k = 0; k < 6; ++k) {
            const double a = 3.14159265358979323846 / 3.0 * k + 3.14159265358979323846 / 6.0;
            marker << point + QPointF(9.0 * std::cos(a), 9.0 * std::sin(a));
        }
        painter.setBrush(QColor(c.red(), c.green(), c.blue(), 70));
        painter.setPen(QPen(c, 2.5));
        painter.drawPolygon(marker);
        if (zoom_ >= 5) {
            painter.setPen(QColor(210, 240, 255));
            painter.setFont(hudFont(7, QFont::DemiBold));
            const auto hint = hailSizeHint(core.tier);
            painter.drawText(
                QRectF(point.x() + 13, point.y() - 20, 240, 36), Qt::AlignLeft | Qt::AlignVCenter,
                QStringLiteral("HAIL %1 g/m3 ~%2")
                    .arg(core.maxVildGPerCubicMeter, 0, 'f', 1)
                    .arg(QString::fromUtf8(hint.data(), static_cast<int>(hint.size()))));
        }
    }
    painter.restore();
}

void RadarMapCanvas::paintZdrColumns(QPainter& painter) {
    if (!zdrVisible_ || zdrColumns_.empty()) return;
    // Updraft proxy: green square on the column centroid, labeled with peak
    // ZDR and column-top height. Guidance only: a column marks a strong
    // updraft, never a hail size or a warning.
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    for (const auto& column : zdrColumns_) {
        const auto point = screenPoint(column.latitudeDeg, column.longitudeDeg);
        if (!rect().adjusted(-60, -60, 60, 60).contains(point.toPoint())) continue;
        const QColor c(80, 220, 120);
        painter.setBrush(QColor(c.red(), c.green(), c.blue(), 70));
        painter.setPen(QPen(c, 2.5));
        painter.drawRect(QRectF(point.x() - 8.0, point.y() - 8.0, 16.0, 16.0));
        if (zoom_ >= 5) {
            painter.setPen(QColor(215, 245, 220));
            painter.setFont(hudFont(7, QFont::DemiBold));
            painter.drawText(
                QRectF(point.x() + 13, point.y() - 20, 240, 36), Qt::AlignLeft | Qt::AlignVCenter,
                QStringLiteral("ZDR %1 dB | top %2 km")
                    .arg(column.maxZdrDb, 0, 'f', 1)
                    .arg(column.topMslMeters / 1000.0, 0, 'f', 1));
        }
    }
    painter.restore();
}

void RadarMapCanvas::paintTbss(QPainter& painter) {
    if (!tbssVisible_ || tbssDetections_.empty()) return;
    // Hail-spike flag: magenta down-triangle on the core, labeled with core
    // strength and spike length. Guidance only: a spike says large hail is
    // likely inside, never a size or a warning.
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    for (const auto& det : tbssDetections_) {
        const auto point = screenPoint(det.latitudeDeg, det.longitudeDeg);
        if (!rect().adjusted(-60, -60, 60, 60).contains(point.toPoint())) continue;
        const QColor c(255, 80, 200);
        painter.setBrush(QColor(c.red(), c.green(), c.blue(), 70));
        painter.setPen(QPen(c, 2.5));
        QPolygonF marker;
        marker << point + QPointF(0.0, 10.0) << point + QPointF(-9.0, -7.0)
               << point + QPointF(9.0, -7.0);
        painter.drawPolygon(marker);
        if (zoom_ >= 5) {
            painter.setPen(QColor(250, 210, 240));
            painter.setFont(hudFont(7, QFont::DemiBold));
            painter.drawText(
                QRectF(point.x() + 13, point.y() - 20, 240, 36), Qt::AlignLeft | Qt::AlignVCenter,
                QStringLiteral("TBSS %1 dBZ | %2 km")
                    .arg(det.maxCoreDbz, 0, 'f', 0)
                    .arg(det.spikeLengthKm, 0, 'f', 1));
        }
    }
    painter.restore();
}

void RadarMapCanvas::paintTdsMarkers(QPainter& painter) {
    if (!tdsVisible_ || tdsDetections_.empty()) return;
    // Debris ball: brown ring with the CC value. Only persisted (2+ volume)
    // detections reach this list. Confirmation, not prediction: debris is
    // already lofted by the time this draws.
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    for (const auto& det : tdsDetections_) {
        const auto point = screenPoint(det.latitudeDeg, det.longitudeDeg);
        if (!rect().adjusted(-60, -60, 60, 60).contains(point.toPoint())) continue;
        const QColor c(139, 90, 43);
        painter.setBrush(QColor(c.red(), c.green(), c.blue(), 110));
        painter.setPen(QPen(QColor(255, 200, 140), 2.5));
        painter.drawEllipse(point, 9.0, 9.0);
        painter.setPen(QPen(QColor(255, 200, 140), 1.5));
        painter.drawEllipse(point, 13.0, 13.0);
        if (zoom_ >= 5) {
            painter.setPen(QColor(255, 225, 190));
            painter.setFont(hudFont(7, QFont::DemiBold));
            painter.drawText(
                QRectF(point.x() + 15, point.y() - 20, 200, 36), Qt::AlignLeft | Qt::AlignVCenter,
                QStringLiteral("TDS CC %1").arg(det.minCorrelationCoefficient, 0, 'f', 2));
        }
    }
    painter.restore();
}

void RadarMapCanvas::paintStormTrail(QPainter& painter) {
    if (!stormTrailVisible_) return;
    const auto& tracks = stormTracker_.tracks();
    if (tracks.empty()) return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    // Ranked tracks by history length (tie: strongest latest fix). The longest
    // keeps the full dot + label treatment; the top 3 get +10/+20/+30
    // extrapolation; every other storm draws as a thin history line with its
    // track id so multi-cell days read at a glance.
    std::vector<const StormTrack*> ranked;
    ranked.reserve(tracks.size());
    for (const auto& track : tracks) {
        if (track.fixes.size() >= 2) ranked.push_back(&track);
    }
    std::sort(ranked.begin(), ranked.end(), [](const StormTrack* a, const StormTrack* b) {
        if (a->fixes.size() != b->fixes.size()) return a->fixes.size() > b->fixes.size();
        return a->fixes.back().maximumReflectivityDbz > b->fixes.back().maximumReflectivityDbz;
    });
    const StormTrack* primary = ranked.empty() ? nullptr : ranked.front();
    for (const auto* track : ranked) {
        if (track == primary) continue;
        QPolygonF line;
        for (const auto& fix : track->fixes) line << screenPoint(fix.latitudeDeg, fix.longitudeDeg);
        painter.setPen(QPen(QColor(255, 209, 122, 90), 1.5, Qt::SolidLine, Qt::RoundCap,
                            Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawPolyline(line);
        // Track id + motion vector for every storm at usable zooms.
        if (zoom_ >= 6) {
            const auto& newest = track->fixes.back();
            const auto p = screenPoint(newest.latitudeDeg, newest.longitudeDeg);
            if (rect().adjusted(-30, -30, 30, 30).contains(p.toPoint())) {
                painter.setPen(QColor(255, 235, 200, 160));
                painter.setFont(hudFont(7, QFont::DemiBold));
                painter.drawText(QRectF(p.x() + 9, p.y() - 20, 80, 16),
                                 Qt::AlignLeft | Qt::AlignVCenter,
                                 QStringLiteral("#%1").arg(track->id));
                // 5-min motion vector from the tracker's velocity estimate.
                const double vLat = track->velocityLatDegPerSec;
                const double vLon = track->velocityLonDegPerSec;
                const double vMag = std::hypot(vLat * 111000.0, vLon * 111000.0);
                if (vMag >= 1.0 && vMag <= 60.0) {
                    const auto tip = screenPoint(newest.latitudeDeg + vLat * 300.0,
                                                 newest.longitudeDeg + vLon * 300.0);
                    painter.setPen(QPen(QColor(255, 209, 122, 140), 1.5, Qt::SolidLine,
                                        Qt::RoundCap));
                    painter.drawLine(p, tip);
                }
            }
        }
    }
    if (!primary) {
        painter.restore();
        return;
    }
    const auto& fixes = primary->fixes;
    QPolygonF track;
    for (const auto& fix : fixes) track << screenPoint(fix.latitudeDeg, fix.longitudeDeg);
    painter.setPen(QPen(QColor(255, 209, 122, 200), 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawPolyline(track);
    for (std::size_t i = 0; i < fixes.size(); ++i) {
        const auto& fix = fixes[i];
        const auto p = screenPoint(fix.latitudeDeg, fix.longitudeDeg);
        if (!rect().adjusted(-30, -30, 30, 30).contains(p.toPoint())) continue;
        const auto age = static_cast<double>(fixes.size() - 1 - i) /
                         static_cast<double>(std::max<std::size_t>(1, fixes.size() - 1));
        const int alpha = static_cast<int>(std::round(220.0 * (1.0 - age * 0.75)));
        QColor dot = fix.rotating ? QColor(255, 78, 210, alpha) : QColor(255, 209, 122, alpha);
        painter.setBrush(dot);
        painter.setPen(QPen(QColor(8, 15, 19, alpha), 1.0));
        const double r = fix.rotating ? 6.0 : 4.5 - age * 1.5;
        painter.drawEllipse(p, r, r);
        if (i + 1 == fixes.size() && zoom_ >= 6) {
            painter.setPen(QColor(255, 235, 200));
            painter.setFont(hudFont(7, QFont::DemiBold));
            painter.drawText(QRectF(p.x() + 9, p.y() - 20, 140, 16),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             QStringLiteral("#%1 trail %2 dBZ")
                                 .arg(primary->id)
                                 .arg(fix.maximumReflectivityDbz, 0, 'f', 0));
        }
    }
    // Extrapolated motion for the top-3 ranked tracks (+10/+20/+30 min).
    // Least-squares fit over each track's newest fixes; dead reckoning, not a
    // forecast. Three gates keep a centroid jump from becoming a cross-state
    // arrow: enough fixes with a real time span, a plausible storm speed, and
    // a hard cap on the drawn length.
    if (zoom_ >= 6) {
        int extrapolated = 0;
        for (const auto* track : ranked) {
            if (extrapolated >= 3) break;
            const auto& tfixes = track->fixes;
            if (tfixes.size() < 3) continue;
            const auto window = std::min<std::size_t>(5, tfixes.size());
            const auto& newest = tfixes.back();
            double sumT = 0.0, sumTT = 0.0, sumLat = 0.0, sumLon = 0.0, sumTLat = 0.0,
                   sumTLon = 0.0;
            int fitted = 0;
            const StormTrackFix* oldest = nullptr;
            for (std::size_t i = tfixes.size() - window; i < tfixes.size(); ++i) {
                const auto& fix = tfixes[i];
            // Seconds before the newest fix (<= 0), so the fit is anchored at now.
            const auto dt = fix.epochSeconds - newest.epochSeconds;
            sumT += dt;
            sumTT += dt * dt;
            sumLat += fix.latitudeDeg;
            sumLon += fix.longitudeDeg;
            sumTLat += dt * fix.latitudeDeg;
            sumTLon += dt * fix.longitudeDeg;
            oldest = &fix;
            ++fitted;
        }
        const auto denom = fitted * sumTT - sumT * sumT;
        const auto spanSeconds = (fitted > 0 && oldest) ? newest.epochSeconds - oldest->epochSeconds : 0.0;
        // Gate 1: at least 3 fixes spanning 2+ minutes — two points only ever
        // describe a jump, never motion.
        if (fitted >= 3 && spanSeconds >= 120.0 && std::abs(denom) > 1e-6 && oldest) {
            const auto latRate = (fitted * sumTLat - sumT * sumLat) / denom;
            const auto lonRate = (fitted * sumTLon - sumT * sumLon) / denom;
            // Gate 2: the window's end-to-end speed must look like weather.
            // Below ~1 m/s there is no meaningful direction (just centroid
            // wobble); above ~35 m/s the max jumped cells and the vector is
            // fiction — exactly the cross-state arrow from the KAMX screenshot.
            const auto windowSpeed = haversineMeters(oldest->latitudeDeg, oldest->longitudeDeg,
                                                     newest.latitudeDeg, newest.longitudeDeg) /
                                     spanSeconds;
            if (windowSpeed >= 1.0 && windowSpeed <= 35.0) {
                // Gate 3: never draw past 120 km from the current core.
                constexpr double kExtrapolationCapMeters = 120000.0;
                QPolygonF path;
                path << screenPoint(newest.latitudeDeg, newest.longitudeDeg);
                for (int step = 1; step <= 3; ++step) {
                    const auto seconds = step * 600.0;
                    const auto lat =
                        std::clamp(newest.latitudeDeg + latRate * seconds, -85.0, 85.0);
                    const auto lon = newest.longitudeDeg + lonRate * seconds;
                    if (haversineMeters(newest.latitudeDeg, newest.longitudeDeg, lat, lon) >
                        kExtrapolationCapMeters)
                        break;
                    path << screenPoint(lat, lon);
                }
                if (path.size() >= 2) {
                    // Dim non-primary tracks so the longest stays dominant.
                    const bool isPrimary = (track == primary);
                    const int alpha = isPrimary ? 160 : 110;
                    painter.setPen(
                        QPen(QColor(255, 209, 122, alpha), 1.5, Qt::DashLine, Qt::RoundCap));
                    painter.setBrush(Qt::NoBrush);
                    painter.drawPolyline(path);
                    painter.setFont(hudFont(7));
                    for (int step = 1; step < path.size(); ++step) {
                        const auto point = path.at(step);
                        if (!rect().adjusted(-30, -30, 30, 30).contains(point.toPoint())) continue;
                        painter.setPen(QPen(QColor(255, 209, 122, 190), 1.5));
                        painter.setBrush(Qt::NoBrush);
                        painter.drawEllipse(point, 4.0, 4.0);
                        painter.setPen(QColor(255, 235, 200, 220));
                        painter.drawText(QRectF(point.x() + 7, point.y() - 8, 44, 14),
                                         Qt::AlignLeft | Qt::AlignVCenter,
                                         QStringLiteral("+%1").arg(step * 10));
                    }
                    ++extrapolated;
                }
            }
        }
        }  // for track : ranked
    }
    painter.restore();
}

bool RadarMapCanvas::legendVisibleForWidth(int width) { return width >= 274; }

std::array<QRectF, 5> RadarMapCanvas::legendTickRects(const QRectF& gradientRect) {
    // 40 px boxes: 5 ticks over a 242 px gradient sit 60.5 px apart, so a
    // centered middle box (half-width 20) clears both neighbors, and the
    // inside-anchored ends clear theirs. Any wider and the ends overlap.
    constexpr double kTickWidth = 40.0;
    constexpr double kTickY = 45.0;
    constexpr double kTickHeight = 12.0;
    const auto tickLeft = [&](const int tick, const double x) -> double {
        if (tick == 0) return gradientRect.left();
        if (tick == 4) return gradientRect.right() - kTickWidth;
        return std::max(gradientRect.left(), x - kTickWidth / 2.0);
    };
    std::array<QRectF, 5> rects;
    for (int tick = 0; tick < 5; ++tick) {
        const auto fraction = tick / 4.0;
        const auto x = gradientRect.left() + fraction * gradientRect.width();
        rects[static_cast<std::size_t>(tick)] =
            QRectF(tickLeft(tick, x), kTickY, kTickWidth, kTickHeight);
    }
    return rects;
}

std::array<bool, 3> RadarMapCanvas::legendReportKeys(bool layerVisible,
                                                     const std::array<bool, 3>& kindsVisible,
                                                     const std::array<bool, 3>& kindsPresent) {
    return {layerVisible && kindsVisible[0] && kindsPresent[0],
            layerVisible && kindsVisible[1] && kindsPresent[1],
            layerVisible && kindsVisible[2] && kindsPresent[2]};
}

void RadarMapCanvas::paintLegend(QPainter& painter) {
    const auto& descriptor = productDescriptor(product_);
    // The legend column is a fixed 258 px stack. Panels get that narrow in a
    // 2x2 layout on small laptops, so skip the stack instead of drawing three
    // boxes that all get clipped on the right.
    if (!legendVisibleForWidth(width())) return;
    const QRectF panel(12, 12, 258, 50);
    painter.setPen(QPen(QColor(205, 216, 222, 130), 1));
    painter.setBrush(QColor(8, 15, 19, 220));
    painter.drawRoundedRect(panel, 4, 4);
    painter.setPen(QColor(235, 240, 243));
    painter.setFont(hudFont(8, QFont::DemiBold));
    auto label = QString::fromUtf8(descriptor.label.data());
    if (product_ == ProductKind::Velocity && velocityOptions_.mode == VelocityProcessingMode::Dealiased)
        label = QStringLiteral("Dealiased velocity");
    else if (product_ == ProductKind::Velocity && velocityOptions_.mode == VelocityProcessingMode::StormRelative)
        label = QStringLiteral("Storm-relative velocity");
    painter.drawText(QRectF(20, 17, 242, 14), Qt::AlignLeft | Qt::AlignVCenter,
                     painter.fontMetrics().elidedText(
                         QStringLiteral("%1 (%2)").arg(label, QString::fromUtf8(descriptor.unit.data())),
                         Qt::ElideRight, 242));
    const QRectF gradientRect(20, 34, 242, 10);
    QLinearGradient gradient(gradientRect.topLeft(), gradientRect.topRight());
    for (int step = 0; step <= 16; ++step) {
        const auto position = step / 16.0;
        // Always sample the palette at full opacity. Folding the user's echo
        // opacity in here made the swatch drift toward black as they turned
        // transparency up, so the key stopped describing what is on the map.
        gradient.setColorAt(position, productColor(product_, position, palette_, 100, customPalette_));
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(gradient);
    painter.drawRect(gradientRect);
    painter.setPen(QColor(205, 215, 220));
    painter.setFont(hudFont(7));
    // Anchor the end labels inside the bar and let the middle two share the
    // span. Boxes are 40 px: at a 242 px gradient the 60.5 px tick spacing
    // leaves a gap everywhere (the old 58 px boxes overlapped their
    // neighbors by ~26 px at both ends), so cramped numbers can't return.
    const auto tickText = [](const double value) { return QString::number(value, 'g', 4); };
    const auto tickRects = legendTickRects(gradientRect);
    for (int tick = 0; tick < 5; ++tick) {
        const auto fraction = tick / 4.0;
        const auto value = descriptor.minimum + fraction * (descriptor.maximum - descriptor.minimum);
        painter.drawText(tickRects[static_cast<std::size_t>(tick)],
                         tick == 0 ? Qt::AlignLeft : tick == 4 ? Qt::AlignRight : Qt::AlignCenter,
                         tickText(value));
    }

    // Only key the report symbols that are actually drawn: the layer must be
    // on, the kind enabled, and at least one report of that kind loaded.
    // Hiding hail and wind in the Layers group used to leave the full
    // three-symbol key on screen, and an empty feed keyed symbols for
    // reports that were never painted.
    const auto reportKinds = std::array<bool, 3>{
        stormReportKindsVisible_[0], stormReportKindsVisible_[1], stormReportKindsVisible_[2]};
    auto kindsPresent = std::array<bool, 3>{false, false, false};
    for (const auto& report : stormReports_)
        kindsPresent[static_cast<std::size_t>(report.kind)] = true;
    const auto reportKeys = legendReportKeys(stormReportsVisible_, reportKinds, kindsPresent);
    const auto anyReportKind = reportKeys[0] || reportKeys[1] || reportKeys[2];
    if (anyReportKind) {
        const QRectF reportPanel(12, 68, 258, 24);
        painter.setPen(QPen(QColor(205, 216, 222, 100), 1));
        painter.setBrush(QColor(8, 15, 19, 205));
        painter.drawRoundedRect(reportPanel, 4, 4);
        painter.setFont(hudFont(7, QFont::DemiBold));
        const std::array<QString, 3> symbols{QStringLiteral("▲ Tornado"), QStringLiteral("● Hail"),
                                            QStringLiteral("■ Wind")};
        const std::array<QColor, 3> colors{QColor("#ff3b4d"), QColor("#53d769"), QColor("#44a7ff")};
        const std::array<double, 3> offsets{20.0, 92.0, 153.0};
        for (int index = 0; index < 3; ++index) {
            if (!reportKeys[static_cast<std::size_t>(index)]) continue;
            painter.setPen(colors[static_cast<std::size_t>(index)]);
            painter.drawText(QRectF(offsets[static_cast<std::size_t>(index)], 72, 70, 16),
                             Qt::AlignLeft | Qt::AlignVCenter, symbols[static_cast<std::size_t>(index)]);
        }
    }

    if ((shearVisible_ && !radarFeatures_.empty()) || (stormTrailVisible_ && hasStormTracks())) {
        const QRectF analysisPanel(12, 96, 258, 20);
        painter.setPen(QPen(QColor(205, 216, 222, 100), 1));
        painter.setBrush(QColor(8, 15, 19, 205));
        painter.drawRoundedRect(analysisPanel, 4, 4);
        painter.setFont(hudFont(7, QFont::DemiBold));
        int x = 20;
        if (shearVisible_ && !radarFeatures_.empty()) {
            painter.setPen(QColor(255, 78, 210));
            painter.drawText(QRectF(x, 98, 90, 16), Qt::AlignLeft | Qt::AlignVCenter,
                             QStringLiteral("◎ ROT/shear"));
            x += 92;
        }
        if (stormTrailVisible_ && hasStormTracks()) {
            painter.setPen(QColor(255, 209, 122));
            painter.drawText(QRectF(x, 98, 110, 16), Qt::AlignLeft | Qt::AlignVCenter,
                             QStringLiteral("— storm trail"));
        }
    }
    if (tvsVisible_ && !circulations_.empty()) {
        int tvs = 0;
        for (const auto& det : circulations_) {
            if (det.kind == CirculationKind::TornadoVortex) ++tvs;
        }
        const int meso = static_cast<int>(circulations_.size()) - tvs;
        const QRectF tvsPanel(12, 118, 258, 20);
        painter.setPen(QPen(QColor(205, 216, 222, 100), 1));
        painter.setBrush(QColor(8, 15, 19, 205));
        painter.drawRoundedRect(tvsPanel, 4, 4);
        painter.setFont(hudFont(7, QFont::DemiBold));
        painter.setPen(QColor(255, 45, 45));
        painter.drawText(QRectF(20, 120, 120, 16), Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("▲ TVS %1 · ◆ MESO %2").arg(tvs).arg(meso));
    }
    if ((hailVisible_ && !hailCores_.empty()) || (tdsVisible_ && !tdsDetections_.empty())) {
        const QRectF extraPanel(12, 140, 258, 20);
        painter.setPen(QPen(QColor(205, 216, 222, 100), 1));
        painter.setBrush(QColor(8, 15, 19, 205));
        painter.drawRoundedRect(extraPanel, 4, 4);
        painter.setFont(hudFont(7, QFont::DemiBold));
        int x = 20;
        if (hailVisible_ && !hailCores_.empty()) {
            painter.setPen(QColor(64, 224, 255));
            painter.drawText(QRectF(x, 142, 130, 16), Qt::AlignLeft | Qt::AlignVCenter,
                             QStringLiteral("⬢ HAIL %1").arg(static_cast<int>(hailCores_.size())));
            x += 90;
        }
        if (tdsVisible_ && !tdsDetections_.empty()) {
            painter.setPen(QColor(255, 200, 140));
            painter.drawText(QRectF(x, 142, 130, 16), Qt::AlignLeft | Qt::AlignVCenter,
                             QStringLiteral("● TDS %1").arg(static_cast<int>(tdsDetections_.size())));
        }
    }
    if ((spcOutlookVisible_ && !spcOutlooks_.isEmpty()) || (spcFireVisible_ && !spcFireOutlooks_.isEmpty()) ||
        (mcdVisible_ && !mcds_.isEmpty())) {
        const QRectF spcPanel(12, 162, 258, 20);
        painter.setPen(QPen(QColor(205, 216, 222, 100), 1));
        painter.setBrush(QColor(8, 15, 19, 205));
        painter.drawRoundedRect(spcPanel, 4, 4);
        painter.setFont(hudFont(7, QFont::DemiBold));
        int x = 20;
        if (spcOutlookVisible_ && !spcOutlooks_.isEmpty()) {
            // Highest Day 1 risk reads at a glance; the map shows the rest.
            const SpcOutlookFeature* top = nullptr;
            for (const auto& feature : spcOutlooks_) {
                if (feature.day != 1) continue;
                if (!top || feature.dn > top->dn) top = &feature;
            }
            painter.setPen(QColor(134, 239, 172));
            painter.drawText(
                QRectF(x, 164, 110, 16), Qt::AlignLeft | Qt::AlignVCenter,
                top ? QStringLiteral("SPC: %1").arg(top->label2.isEmpty() ? top->label : top->label2)
                    : QStringLiteral("SPC D1+2"));
            x += 105;
        }
        if (spcFireVisible_ && !spcFireOutlooks_.isEmpty()) {
            painter.setPen(QColor(253, 186, 116));
            painter.drawText(QRectF(x, 164, 80, 16), Qt::AlignLeft | Qt::AlignVCenter,
                             QStringLiteral("FIRE %1").arg(static_cast<int>(spcFireOutlooks_.size())));
            x += 60;
        }
        if (mcdVisible_ && !mcds_.isEmpty()) {
            painter.setPen(QColor(216, 180, 254));
            painter.drawText(QRectF(x, 164, 70, 16), Qt::AlignLeft | Qt::AlignVCenter,
                             QStringLiteral("MCD %1").arg(static_cast<int>(mcds_.size())));
        }
    } else if (spcOutlookVisible_ || spcFireVisible_ || mcdVisible_) {
        // Quiet-day confirmation: layers are on, feeds answered, nothing
        // issued. Without this the map gives zero feedback and reads as
        // a broken feed.
        const QRectF spcPanel(12, 162, 258, 20);
        painter.setPen(QPen(QColor(205, 216, 222, 60), 1));
        painter.setBrush(QColor(8, 15, 19, 150));
        painter.drawRoundedRect(spcPanel, 4, 4);
        painter.setFont(hudFont(7, QFont::DemiBold));
        painter.setPen(QColor(148, 163, 184));
        QStringList quiet;
        if (spcOutlookVisible_) quiet.push_back(QStringLiteral("SPC: quiet"));
        if (spcFireVisible_) quiet.push_back(QStringLiteral("Fire: none"));
        if (mcdVisible_) quiet.push_back(QStringLiteral("MCD: none"));
        painter.drawText(QRectF(20, 164, 242, 16), Qt::AlignLeft | Qt::AlignVCenter,
                         quiet.join(QStringLiteral(" · ")));
    }
    // Second context row: Day 1 ERO risk plus active cyclones. Either side
    // may be quiet; the row draws whenever at least one layer is on so the
    // map confirms what the feeds said.
    if (wpcVisible_ || nhcVisible_) {
        const SpcOutlookFeature* eroTop = nullptr;
        if (wpcVisible_) {
            for (const auto& feature : wpcOutlooks_) {
                if (feature.day != 1) continue;
                if (!eroTop || feature.dn > eroTop->dn) eroTop = &feature;
            }
        }
        const bool active = eroTop || (nhcVisible_ && !nhcStorms_.isEmpty());
        {
            const QRectF eroPanel(12, 184, 258, 20);
            painter.setPen(QPen(QColor(205, 216, 222, active ? 100 : 60), 1));
            painter.setBrush(QColor(8, 15, 19, active ? 205 : 150));
            painter.drawRoundedRect(eroPanel, 4, 4);
            painter.setFont(hudFont(7, QFont::DemiBold));
            int ex = 20;
            if (wpcVisible_) {
                painter.setPen(eroTop ? QColor(125, 211, 252) : QColor(148, 163, 184));
                painter.drawText(
                    QRectF(ex, 186, 80, 16), Qt::AlignLeft | Qt::AlignVCenter,
                    eroTop ? QStringLiteral("ERO: %1").arg(
                                 eroTop->label.isEmpty() ? eroTop->label2 : eroTop->label)
                           : QStringLiteral("ERO: quiet"));
                ex += 75;
            }
            if (nhcVisible_) {
                painter.setPen(!nhcStorms_.isEmpty() ? QColor(253, 186, 116)
                                                     : QColor(148, 163, 184));
                const auto label =
                    nhcStorms_.isEmpty()
                        ? QStringLiteral("NHC: none")
                        : nhcStorms_.size() == 1
                              ? QStringLiteral("🌀 %1").arg(nhcStorms_.front().name)
                              : QStringLiteral("🌀 %1 + %2").arg(nhcStorms_.front().name).arg(
                                    nhcStorms_.size() - 1);
                painter.drawText(QRectF(ex, 186, 258 - (ex - 12), 16), Qt::AlignLeft | Qt::AlignVCenter,
                                 label);
            }
        }
    }
    // Third context row: HMS fire/smoke plus DAT surveys, packed left so
    // the row never overflows when every layer is on at once.
    if (hmsVisible_ || datVisible_) {
        const bool active =
            (hmsVisible_ && (!hmsPlumes_.isEmpty() || !hmsFires_.isEmpty())) ||
            (datVisible_ && !datPaths_.isEmpty());
        const QRectF smokePanel(12, 206, 258, 20);
        painter.setPen(QPen(QColor(205, 216, 222, active ? 100 : 60), 1));
        painter.setBrush(QColor(8, 15, 19, active ? 205 : 150));
        painter.drawRoundedRect(smokePanel, 4, 4);
        painter.setFont(hudFont(7, QFont::DemiBold));
        int fx = 20;
        if (hmsVisible_) {
            const bool hmsActive = !hmsPlumes_.isEmpty() || !hmsFires_.isEmpty();
            painter.setPen(hmsActive ? QColor(180, 180, 180) : QColor(148, 163, 184));
            painter.drawText(
                QRectF(fx, 208, 130, 16), Qt::AlignLeft | Qt::AlignVCenter,
                hmsActive ? QStringLiteral("FIRE %1 · SMOKE %2")
                                .arg(static_cast<int>(hmsFires_.size()))
                                .arg(static_cast<int>(hmsPlumes_.size()))
                          : QStringLiteral("Fire/smoke: none"));
            fx += 125;
        }
        if (datVisible_) {
            painter.setPen(!datPaths_.isEmpty() ? QColor(192, 132, 252) : QColor(148, 163, 184));
            painter.drawText(QRectF(fx, 208, 258 - (fx - 12), 16), Qt::AlignLeft | Qt::AlignVCenter,
                             !datPaths_.isEmpty()
                                 ? QStringLiteral("DAT %1").arg(static_cast<int>(datPaths_.size()))
                                 : QStringLiteral("DAT: none"));
        }
    }
    // Fourth context row: HRRR valid time. Own row: model context reads
    // apart from every observation layer, with its valid hour on display.
    if (hrrrVisible_) {
        const QRectF hrrrPanel(12, 228, 258, 20);
        const bool active = hrrrFrame_.has_value();
        painter.setPen(QPen(QColor(205, 216, 222, active ? 100 : 60), 1));
        painter.setBrush(QColor(8, 15, 19, active ? 205 : 150));
        painter.drawRoundedRect(hrrrPanel, 4, 4);
        painter.setFont(hudFont(7, QFont::DemiBold));
        painter.setPen(active ? QColor(125, 211, 252) : QColor(148, 163, 184));
        QString label = QStringLiteral("HRRR: none");
        if (active) {
            const auto valid =
                QDateTime::fromSecsSinceEpoch(hrrrFrame_->validEpochSeconds, QTimeZone::UTC);
            label = QStringLiteral("HRRR %1 (model)")
                        .arg(valid.isValid() ? valid.toString(QStringLiteral("HH:mm'Z'"))
                                             : QStringLiteral("?"));
        }
        painter.drawText(QRectF(20, 230, 242, 16), Qt::AlignLeft | Qt::AlignVCenter, label);
    }
    // Fifth context row: GOES scan time. Same model/observed-context idiom.
    if (goesVisible_) {
        const QRectF goesPanel(12, 250, 258, 20);
        const bool active = goesFrame_.has_value();
        painter.setPen(QPen(QColor(205, 216, 222, active ? 100 : 60), 1));
        painter.setBrush(QColor(8, 15, 19, active ? 205 : 150));
        painter.drawRoundedRect(goesPanel, 4, 4);
        painter.setFont(hudFont(7, QFont::DemiBold));
        painter.setPen(active ? QColor(200, 200, 200) : QColor(148, 163, 184));
        QString label = QStringLiteral("GOES: none");
        if (active) {
            const auto valid =
                QDateTime::fromSecsSinceEpoch(goesFrame_->validEpochSeconds, QTimeZone::UTC);
            label = QStringLiteral("GOES C%1 %2")
                        .arg(goesFrame_->channel)
                        .arg(valid.isValid() ? valid.toString(QStringLiteral("HH:mm'Z'"))
                                             : QStringLiteral("?"));
        }
        painter.drawText(QRectF(20, 252, 242, 16), Qt::AlignLeft | Qt::AlignVCenter, label);
    }
    // Sixth context row: IEM mosaic scan time. Same observed-context idiom.
    if (mosaicVisible_) {
        const QRectF mosaicPanel(12, 272, 258, 20);
        const bool active = mosaicFrame_.has_value();
        painter.setPen(QPen(QColor(205, 216, 222, active ? 100 : 60), 1));
        painter.setBrush(QColor(8, 15, 19, active ? 205 : 150));
        painter.drawRoundedRect(mosaicPanel, 4, 4);
        painter.setFont(hudFont(7, QFont::DemiBold));
        painter.setPen(active ? QColor(134, 239, 172) : QColor(148, 163, 184));
        QString label = QStringLiteral("Mosaic: none");
        if (active) {
            const auto valid =
                QDateTime::fromSecsSinceEpoch(mosaicFrame_->validEpochSeconds, QTimeZone::UTC);
            label = QStringLiteral("Mosaic %1")
                        .arg(valid.isValid() ? valid.toString(QStringLiteral("HH:mm'Z'"))
                                             : QStringLiteral("?"));
        }
        painter.drawText(QRectF(20, 274, 242, 16), Qt::AlignLeft | Qt::AlignVCenter, label);
    }

    // North arrow: always north-up in Web-Mercator, but chasers in a moving
    // vehicle lose orientation at a glance. Paint a compact N arrow top-right.
    {
        const QRectF compass(width() - 44.0, 12.0, 32.0, 38.0);
        painter.setPen(QPen(QColor(205, 216, 222, 130), 1));
        painter.setBrush(QColor(8, 15, 19, 220));
        painter.drawRoundedRect(compass, 4, 4);
        painter.setPen(QPen(QColor(235, 240, 243), 2.0));
        painter.setBrush(Qt::NoBrush);
        const QPointF tip(compass.center().x(), compass.top() + 7.0);
        const QPointF base(compass.center().x(), compass.bottom() - 12.0);
        painter.drawLine(tip, base);
        QPolygonF head;
        head << tip << (tip + QPointF(-5.0, 7.0)) << (tip + QPointF(5.0, 7.0));
        painter.setBrush(QColor(235, 240, 243));
        painter.setPen(Qt::NoPen);
        painter.drawPolygon(head);
        painter.setPen(QColor(235, 240, 243));
        painter.setFont(hudFont(8, QFont::DemiBold));
        painter.drawText(QRectF(compass.left(), compass.bottom() - 12.0, 32.0, 11.0),
                         Qt::AlignCenter, QStringLiteral("N"));
    }
}

void RadarMapCanvas::paintScaleBar(QPainter& painter) {
    if (width() < 280 || height() < 200) return;
    // Sample the ground scale at the bar's own latitude (the widget's vertical
    // center is well above where the bar is drawn) and clamp the bar to what
    // actually fits, so a deep zoom cannot paint thousands of pixels past the
    // right edge of the panel.
    const auto barY = height() - 58.0;
    const auto leftGeo = geographicPoint(QPointF(width() / 2.0 - 55.0, barY));
    const auto rightGeo = geographicPoint(QPointF(width() / 2.0 + 55.0, barY));
    const auto metersPerPixel =
        std::max(0.01, rangeAndBearing({leftGeo.y(), leftGeo.x()}, {rightGeo.y(), rightGeo.x()})
                          .distanceMeters / 110.0);
    // Start at 1 m so a deep zoom still gets a bar instead of falling off the
    // end of the table and reporting 1 km for a 300 m-wide view.
    static const double kNice[] = {1,     2,     5,     10,    20,     50,     100,    200,    500,
                                   1000,  2000,  5000,  10000, 20000,  50000,  100000, 200000, 500000};
    const auto maximumBarPx = std::min(150.0, std::max(40.0, width() - 190.0));
    double chosen = kNice[0];
    for (const auto candidate : kNice) {
        if (candidate / metersPerPixel <= maximumBarPx) chosen = candidate;
        else break;
    }
    const auto barPx = std::clamp(chosen / metersPerPixel, 24.0, maximumBarPx);
    const QRectF panel(12.0, barY, barPx + 88.0, 22.0);
    painter.setPen(QPen(QColor(205, 216, 222, 130), 1));
    painter.setBrush(QColor(8, 15, 19, 220));
    painter.drawRoundedRect(panel, 4, 4);
    const auto barTop = panel.center().y() - 3.0;
    painter.setPen(QPen(QColor(235, 240, 243), 2.0));
    painter.drawLine(QPointF(20.0, barTop + 6.0), QPointF(20.0 + barPx, barTop + 6.0));
    painter.drawLine(QPointF(20.0, barTop + 2.0), QPointF(20.0, barTop + 6.0));
    painter.drawLine(QPointF(20.0 + barPx, barTop + 2.0), QPointF(20.0 + barPx, barTop + 6.0));
    painter.setPen(QColor(235, 240, 243));
    painter.setFont(hudFont(7, QFont::DemiBold));
    // Label the distance the bar actually spans, not the table entry: clamping
    // can shorten the bar away from `chosen`.
    const auto barMeters = barPx * metersPerPixel;
    const auto label = barMeters >= 1000.0
        ? QStringLiteral("%1 km").arg(barMeters / 1000.0, 0, 'f', barMeters < 10000.0 ? 1 : 0)
        : QStringLiteral("%1 m").arg(barMeters, 0, 'f', 0);
    painter.drawText(QRectF(20.0 + barPx + 6.0, panel.top() + 3.0,
                            std::max(40.0, panel.right() - (20.0 + barPx + 6.0) - 4.0), 16.0),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     painter.fontMetrics().elidedText(label, Qt::ElideRight,
                                                      static_cast<int>(panel.right() - (20.0 + barPx + 6.0) - 4.0)));
}

void RadarMapCanvas::paintProbe(QPainter& painter) {
    if (probeText_.isEmpty()) return;
    if (!pinnedInfo_.isEmpty()) {
        const auto parts = probeText_.split(QLatin1Char('\n'));
        const auto isWarning = pinnedInfoKind_ == PinnedInfoKind::Warning;
        const auto title = parts.value(0).trimmed();
        const auto severity = isWarning ? parts.value(1).trimmed() : QString{};
        const auto headline = isWarning ? parts.value(2).trimmed() : QString{};
        const auto bodyStart = isWarning ? 3 : 1;
        const auto body = parts.mid(bodyStart).join(QLatin1Char('\n')).trimmed();

        auto accent = isWarning ? nwsAlertStyle(title).color : QColor("#80d3ff");
        if (pinnedInfoKind_ == PinnedInfoKind::Lightning && pinnedLightningCell_) {
            const auto selected = std::find_if(
                lightningCells_.cbegin(), lightningCells_.cend(), [this](const auto& cell) {
                    return cell.cellX == pinnedLightningCell_->first &&
                           cell.cellY == pinnedLightningCell_->second;
                });
            if (selected != lightningCells_.cend()) accent = lightningCellColor(*selected);
        }

        const auto cardWidth = std::min(390.0, std::max(280.0, width() - 24.0));
        // Double-clicking the card toggles the expanded budget so a long
        // warning description (the Flash Flood instruction text runs long)
        // reads without scrolling.
        const auto maximumHeight = std::max(
            130.0, std::min(pinnedInfoExpanded_ ? 520.0 : 270.0, height() - 24.0));
        const auto contentWidth = cardWidth - 32.0;
        const auto fixedPrefix = 38.0 + (isWarning ? 29.0 : 0.0);
        painter.setFont(hudFont(9, QFont::DemiBold));
        auto headlineHeight = headline.isEmpty()
            ? 0.0
            : static_cast<double>(painter.fontMetrics().boundingRect(
                  QRect(0, 0, static_cast<int>(contentWidth), 1000),
                  Qt::TextWordWrap, headline).height());
        // A long NWS headline used to be measured at full height and then
        // subtracted from the card, which drove the body rect negative: the clip
        // collapsed, the description silently vanished, and the scrollbar thumb
        // was drawn outside its own track. Cap the headline so the card always
        // keeps a readable body region, and elide the headline if even that is
        // not enough.
        const auto headlineBudget = std::max(0.0, (maximumHeight - fixedPrefix - 12.0) * 0.5);
        auto headlineTruncated = false;
        if (headlineHeight > headlineBudget) {
            headlineTruncated = true;
            headlineHeight = headlineBudget;
        }
        painter.setFont(hudFont(8));
        const auto bodyHeight = static_cast<double>(painter.fontMetrics().boundingRect(
            QRect(0, 0, static_cast<int>(contentWidth - 8.0), 10000),
            Qt::TextWordWrap, body).height());
        const auto contentTopOffset = fixedPrefix + (headline.isEmpty() ? 0.0 : headlineHeight + 3.0);
        const auto naturalHeight = contentTopOffset + bodyHeight + 12.0;
        const auto cardHeight = std::clamp(naturalHeight, 130.0, maximumHeight);
        if (pinnedInfoCardPosition_.x() < 0.0 || pinnedInfoCardPosition_.y() < 0.0) {
            pinnedInfoCardPosition_ = {
                std::max(12.0, width() - cardWidth - 12.0),
                std::clamp(64.0, 12.0, std::max(12.0, height() - cardHeight - 12.0)),
            };
        }
        pinnedInfoCardPosition_.setX(std::clamp(
            pinnedInfoCardPosition_.x(), 8.0, std::max(8.0, width() - cardWidth - 8.0)));
        pinnedInfoCardPosition_.setY(std::clamp(
            pinnedInfoCardPosition_.y(), 8.0, std::max(8.0, height() - cardHeight - 8.0)));
        const auto x = pinnedInfoCardPosition_.x();
        const auto y = pinnedInfoCardPosition_.y();
        pinnedInfoCardRect_ = QRectF(x, y, cardWidth, cardHeight);

        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 75));
        painter.drawRoundedRect(pinnedInfoCardRect_.translated(3.0, 4.0), 8.0, 8.0);
        painter.setPen(QPen(QColor(188, 202, 210, 150), 1.0));
        painter.setBrush(QColor(8, 15, 20, 246));
        painter.drawRoundedRect(pinnedInfoCardRect_, 8.0, 8.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(accent);
        painter.drawRoundedRect(QRectF(x, y, 5.0, cardHeight), 3.0, 3.0);

        // Reserve room for the lock glyph as well as the close button so an
        // elided title never runs underneath either one.
        const auto headerTextWidth = cardWidth - 54.0 - (pinnedInfoLocked_ ? 20.0 : 0.0);
        painter.setFont(hudFont(10, QFont::DemiBold));
        painter.setPen(QColor("#f5f8fa"));
        painter.drawText(QRectF(x + 16.0, y + 10.0, headerTextWidth, 21.0),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         painter.fontMetrics().elidedText(title, Qt::ElideRight,
                                                          static_cast<int>(headerTextWidth)));

        const QRectF closeRect(x + cardWidth - 34.0, y + 7.0, 25.0, 25.0);
        painter.setPen(QColor("#aebbc2"));
        painter.setFont(hudFont(11, QFont::DemiBold));
        painter.drawText(closeRect, Qt::AlignCenter, QStringLiteral("×"));

        if (pinnedInfoLocked_) {
            // Small lock glyph in the header so the user can see the active
            // card is protected from replacement. Drawn as a primitive so it
            // doesn't depend on emoji font support.
            const QRectF lockRect(x + cardWidth - 60.0, y + 12.0, 14.0, 14.0);
            painter.setPen(QPen(QColor("#ffd17a"), 1.5));
            painter.setBrush(Qt::NoBrush);
            painter.drawArc(lockRect.adjusted(1.0, 2.0, -1.0, -3.0), 180 * 16, 180 * 16);
            painter.setBrush(QColor("#ffd17a"));
            painter.drawRoundedRect(lockRect.adjusted(3.0, 7.0, -3.0, -1.0), 1.0, 1.0);
        }

        auto contentTop = y + 38.0;
        if (isWarning) {
            // Measure the badge with the font it is actually drawn in. The old
            // code measured at 11 pt and rendered at 8 pt, leaving ~17 px of
            // dead pill on each side of the most prominent element on the card.
            const auto badgeFont = hudFont(8, QFont::DemiBold);
            const auto badgeText = severity.toUpper();
            const auto badgeWidth = std::min(150.0,
                QFontMetrics(badgeFont).horizontalAdvance(badgeText) + 18.0);
            const QRectF badge(x + 16.0, contentTop, badgeWidth, 22.0);
            auto badgeFill = accent;
            badgeFill.setAlpha(45);
            painter.setPen(QPen(accent, 1.0));
            painter.setBrush(badgeFill);
            painter.drawRoundedRect(badge, 5.0, 5.0);
            painter.setPen(accent.lighter(125));
            painter.setFont(badgeFont);
            painter.drawText(badge, Qt::AlignCenter, badgeText);
            contentTop += 29.0;
        }

        if (!headline.isEmpty()) {
            painter.setPen(QColor("#e5edf1"));
            painter.setFont(hudFont(9, QFont::DemiBold));
            // Clip the headline to the card and mark the cut, rather than
            // letting it spill unbacked over the map below.
            painter.save();
            painter.setClipRect(QRectF(x, contentTop, cardWidth, headlineHeight));
            painter.drawText(QRectF(x + 16.0, contentTop, cardWidth - 32.0, headlineHeight),
                             Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, headline);
            painter.restore();
            if (headlineTruncated) {
                // Vertical fade on the cut edge so it reads as "more above"
                // rather than as a rendering glitch.
                QLinearGradient fade(contentTop, contentTop + headlineHeight - 9.0,
                                     contentTop, contentTop + headlineHeight);
                fade.setColorAt(0.0, QColor(8, 15, 20, 0));
                fade.setColorAt(1.0, QColor(8, 15, 20, 235));
                painter.fillRect(QRectF(x + 5.0, contentTop + headlineHeight - 9.0,
                                        cardWidth - 10.0, 9.0), fade);
            }
            contentTop += headlineHeight + 3.0;
        }

        // The headline cap above guarantees a non-negative body, but a tiny
        // panel can still squeeze it; clamp defensively so the clip rect is
        // never degenerate (which silently blanks the whole card body).
        const auto bodyTop = std::min(contentTop, y + cardHeight - 12.0);
        const QRectF bodyRect(x + 16.0, bodyTop, cardWidth - 32.0,
                              std::max(0.0, y + cardHeight - bodyTop - 12.0));
        pinnedInfoBodyRect_ = bodyRect;
        pinnedInfoContentHeight_ = static_cast<int>(std::ceil(bodyHeight));
        const auto maximumScroll = std::max(
            0, pinnedInfoContentHeight_ - static_cast<int>(std::floor(bodyRect.height())));
        pinnedInfoScrollOffset_ = std::clamp(
            pinnedInfoScrollOffset_, 0.0, static_cast<double>(maximumScroll));
        pinnedInfoScrollTarget_ = std::clamp(
            pinnedInfoScrollTarget_, 0.0, static_cast<double>(maximumScroll));
        painter.save();
        painter.setClipRect(bodyRect);
        painter.setPen(QColor("#cbd6dc"));
        painter.setFont(hudFont(8));
        painter.drawText(QRectF(bodyRect.left(), bodyRect.top() - pinnedInfoScrollOffset_,
                                bodyRect.width() - (maximumScroll > 0 ? 8.0 : 0.0),
                                std::max(bodyRect.height(), bodyHeight)),
                         Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, body);
        painter.restore();
        if (maximumScroll > 0) {
            const QRectF track(bodyRect.right() - 3.0, bodyRect.top(), 3.0, bodyRect.height());
            const auto thumbHeight = std::max(
                22.0, track.height() * bodyRect.height() / pinnedInfoContentHeight_);
            const auto thumbTravel = track.height() - thumbHeight;
            const auto thumbY = track.top() + thumbTravel * pinnedInfoScrollOffset_ / maximumScroll;
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(118, 143, 156, 70));
            painter.drawRoundedRect(track, 1.5, 1.5);
            painter.setBrush(QColor(151, 181, 196, 190));
            painter.drawRoundedRect(QRectF(track.left(), thumbY, track.width(), thumbHeight),
                                    1.5, 1.5);
        }
        painter.restore();
        return;
    }

    pinnedInfoCardRect_ = {};
    pinnedInfoBodyRect_ = {};
    pinnedInfoContentHeight_ = 0;
    pinnedInfoScrollOffset_ = 0.0;
    pinnedInfoScrollTarget_ = 0.0;
    pinnedInfoScrollTimer_->stop();
    // The transient readout used to inherit whatever font the last HUD helper
    // happened to leave on the painter. Set it explicitly so it can never
    // silently change appearance.
    painter.setFont(hudFont(8, QFont::DemiBold));
    const auto metrics = painter.fontMetrics();
    const auto boxText = metrics.elidedText(probeText_, Qt::ElideRight,
                                            std::max(80, width() - 28));
    const auto textWidth = metrics.horizontalAdvance(boxText) + 12.0;
    QRectF box(0, 0, textWidth, 23.0);
    // Prefer down-right of the cursor, then flip to whichever quadrant has room,
    // and finally clamp. The old two-branch flip drove x to about -270 in the
    // left third of a 2x2 panel, hiding most of the readout.
    const double margin = 6.0;
    auto x = probePosition_.x() + 14.0;
    if (x + textWidth + margin > width()) x = probePosition_.x() - textWidth - 14.0;
    if (x < margin) x = std::min(probePosition_.x() + 14.0, width() - textWidth - margin);
    auto y = probePosition_.y() + 14.0;
    if (y + box.height() + margin > height()) y = probePosition_.y() - box.height() - 14.0;
    y = std::clamp(y, margin, std::max(margin, height() - box.height() - margin));
    box.moveTo(std::max(margin, x), y);
    painter.setPen(QPen(QColor(210, 220, 225), 1));
    painter.setBrush(QColor(5, 11, 14, 235));
    painter.drawRoundedRect(box, 3, 3);
    painter.setPen(QColor(242, 246, 248));
    painter.drawText(box.adjusted(6, 0, -6, 0), Qt::AlignVCenter | Qt::AlignLeft, boxText);
}

void RadarMapCanvas::paintStationCard(QPainter& painter) {
    if (hoverRadarId_.isEmpty()) return;
    // siteIndexes_ is already an id -> position map; walking sites_ and
    // converting every std::string to a QString cost ~200 conversions per frame
    // just to resolve a hover.
    const auto index = siteIndexes_.value(hoverRadarId_, static_cast<std::size_t>(-1));
    if (index == static_cast<std::size_t>(-1) || index >= sites_.size()) return;
    const auto& site = sites_[index];
    const auto elevation = QStringLiteral("%1 m").arg(site.antennaElevationMeters, 0, 'f', 0);
    const auto text = QStringLiteral("%1  %2\n%3  |  %4")
                          .arg(hoverRadarId_, QString::fromStdString(site.name),
                               QString::fromStdString(site.owner), elevation);
    // Sit below the north arrow instead of under it (the card used to contain
    // the compass entirely, hiding it for the whole hover), and clamp to the
    // panel so the radar ID is never clipped on a 300 px-wide panel.
    const auto cardWidth = std::min(318.0, std::max(150.0, width() - 24.0));
    const QRectF panel(std::max(6.0, width() - cardWidth - 12.0), 56.0, cardWidth, 45.0);
    painter.setPen(QPen(QColor(128, 211, 255, 170), 1));
    painter.setBrush(QColor(8, 15, 19, 230));
    painter.drawRoundedRect(panel, 4, 4);
    painter.setPen(QColor(238, 244, 247));
    painter.setFont(hudFont(8, QFont::DemiBold));
    // Elide line by line: QFontMetrics::elidedText is single-line, so eliding
    // the joined string only trimmed the end of the last row and let a long
    // site name still be cut mid-glyph by the card edge.
    const int textWidth = static_cast<int>(cardWidth) - 20;
    const auto first = text.split(QLatin1Char('\n'));
    const auto elide = [&](const QString& line) {
        return painter.fontMetrics().elidedText(line, Qt::ElideRight, textWidth);
    };
    painter.drawText(panel.adjusted(9, 4, -8, -4), Qt::AlignLeft | Qt::AlignVCenter,
                     elide(first.value(0)) + QLatin1Char('\n') + elide(first.value(1)));
}

void RadarMapCanvas::paintMeasurement(QPainter& painter) {
    if (!measureStart_.has_value() || !measureEnd_.has_value()) return;
    const auto start = screenPoint(measureStart_->y(), measureStart_->x());
    const auto end = screenPoint(measureEnd_->y(), measureEnd_->x());
    const auto measurement = rangeAndBearing({measureStart_->y(), measureStart_->x()},
                                             {measureEnd_->y(), measureEnd_->x()});
    const auto distanceKm = measurement.distanceMeters / 1000.0;
    const auto bearing = measurement.bearingDegrees;
    painter.setPen(QPen(QColor(255, 230, 120), 2, Qt::DashLine));
    painter.drawLine(start, end);
    painter.setBrush(QColor(255, 230, 120));
    painter.drawEllipse(start, 3, 3);
    painter.drawEllipse(end, 3, 3);
    const auto label = QStringLiteral("%1%2 km  %3%4")
                           .arg(crossSectionMode_ ? QStringLiteral("Cross-section  ") : QString{})
                           .arg(distanceKm, 0, 'f', 1)
                           .arg(bearing, 0, 'f', 1)
                           .arg(QChar(0x00b0));
    // Size the plate to the text and keep it on screen. The old fixed 112 px box
    // cut roughly 60 px off a cross-section label, and nothing clamped the
    // plate, so a measurement near any edge drew it off-panel.
    painter.setFont(hudFont(8, QFont::DemiBold));
    const auto labelWidth =
        std::min(painter.fontMetrics().horizontalAdvance(label) + 14.0,
                 std::max(60.0, width() - 12.0));
    const double labelHeight = 20.0;
    const auto midpoint = (start + end) * 0.5;
    const double boxX = std::clamp(midpoint.x() - labelWidth / 2.0, 6.0,
                                   std::max(6.0, width() - labelWidth - 6.0));
    const double boxY = std::clamp(midpoint.y() - 24.0, 6.0,
                                   std::max(6.0, height() - labelHeight - 6.0));
    const QRectF box(boxX, boxY, labelWidth, labelHeight);
    painter.setPen(QColor(245, 245, 225));
    painter.setBrush(QColor(8, 15, 19, 235));
    painter.drawRoundedRect(box, 3, 3);
    painter.drawText(box, Qt::AlignCenter,
                     painter.fontMetrics().elidedText(label, Qt::ElideRight,
                                                      static_cast<int>(labelWidth) - 8));
}

QString RadarMapCanvas::tileKey(const int x, const int y) const {
    return QStringLiteral("%1/%2/%3").arg(zoom_).arg(x).arg(y);
}

void RadarMapCanvas::requestVisibleTiles() {
    auto& store = tileStore();
    const auto center = worldPoint(center_.y(), center_.x());
    const auto left = center.x() - width() / 2.0;
    const auto top = center.y() - height() / 2.0;
    const auto firstX = static_cast<int>(std::floor(left / kTileSize));
    const auto lastX = static_cast<int>(std::floor((left + width()) / kTileSize));
    const auto firstY = std::max(0, static_cast<int>(std::floor(top / kTileSize)));
    const auto lastY = std::min((1 << zoom_) - 1,
                                static_cast<int>(std::floor((top + height()) / kTileSize)));
    const auto tileCount = 1 << zoom_;

    for (int tileY = firstY; tileY <= lastY; ++tileY) {
        for (int tileX = firstX; tileX <= lastX; ++tileX) {
            const auto wrappedX = wrappedTileX(tileX, tileCount);
            const auto key = tileKey(wrappedX, tileY);
            if (store.tiles.contains(key)) continue;
            const auto urls = basemapTileUrls(zoom_, wrappedX, tileY);
            if (urls.base.isEmpty()) continue;  // misconfigured custom template: nothing to fetch
            watchTile(store, key, this);
            const auto referenceKey = key + QStringLiteral("@ref");
            if (store.pendingKeys.contains(key) || store.pendingKeys.contains(referenceKey)) continue;
            store.pendingKeys.insert(key);
            auto* baseReply = store.network->get(makeRequest(urls.base));
            store.replies.insert(baseReply, key);
            if (urls.reference.isEmpty()) {
                // Single-layer providers (CARTO, custom): no second request,
                // so mark the reference leg resolved up front and let the
                // base arrival publish the tile on its own.
                store.staging[key].referenceDone = true;
                continue;
            }
            store.pendingKeys.insert(referenceKey);
            auto* referenceReply = store.network->get(makeRequest(urls.reference));
            store.replies.insert(referenceReply, referenceKey);
        }
    }
}

void RadarMapCanvas::panBy(const QPointF& delta) {
    const auto world = static_cast<double>(kTileSize * (1 << zoom_));
    const auto center = worldPoint(center_.y(), center_.x()) - delta;
    const auto wrappedX = std::fmod(center.x() + world, world);
    const auto x = wrappedX < 0.0 ? wrappedX + world : wrappedX;
    const auto normalizedY = std::clamp(center.y(), 0.0, world);
    const auto longitude = x / world * 360.0 - 180.0;
    const auto n = kPi - 2.0 * kPi * normalizedY / world;
    const auto latitude = qRadiansToDegrees(std::atan(std::sinh(n)));
    center_ = {longitude, latitude};
    invalidateWorldCaches(false);
    update();
    emit viewChanged(center_, zoom_);
}

QString RadarMapCanvas::radarAt(const QPointF& point) const {
    ensureSiteWorldPoints();
    // Mirror the declutter rules paintEvent applies, otherwise the hit test
    // disagrees with the screen: at zoom 5 a hidden marker was still clickable
    // and aiming at empty space selected a different radar than the one drawn.
    ensureSiteWorldPoints();
    QSet<QString> occupiedMarkerCells;
    for (std::size_t siteIndex = 0; siteIndex < sites_.size(); ++siteIndex) {
        const auto& site = sites_[siteIndex];
        if (!site.latitude || !site.longitude) continue;
        const auto marker = siteIndex < static_cast<std::size_t>(siteWorldPoints_.size())
                                ? screenPointFromWorld(siteWorldPoints_.at(static_cast<qsizetype>(siteIndex)))
                                : screenPoint(*site.latitude, *site.longitude);
        if (!QRectF(rect()).adjusted(-50, -30, 50, 30).contains(marker)) continue;
        const auto id = QString::fromStdString(site.id);
        const auto compact = zoom_ <= 4 && id != selectedRadar_;
        if (zoom_ == 5 && id != selectedRadar_ && id != hoverRadarId_) {
            const auto cell = QStringLiteral("%1/%2")
                                  .arg(static_cast<int>(marker.x()) / 54)
                                  .arg(static_cast<int>(marker.y()) / 25);
            if (occupiedMarkerCells.contains(cell)) continue;
            occupiedMarkerCells.insert(cell);
        }
        const QRectF pill = compact ? QRectF(marker.x() - 7.0, marker.y() - 7.0, 14.0, 14.0)
                                    : QRectF(marker.x() - 24.0, marker.y() - 11.0, 48.0, 22.0);
        if (pill.contains(point)) return id;
    }
    return {};
}

std::optional<std::size_t> RadarMapCanvas::lightningCellAt(const QPointF& point) const {
    if (!lightningVisible_) return std::nullopt;
    std::optional<std::size_t> best;
    auto bestDistance = std::numeric_limits<double>::infinity();
    for (std::size_t index = 0; index < lightningCells_.size(); ++index) {
        const auto& cell = lightningCells_[index];
        const auto half = cell.cellSizeDeg * 0.5;
        QPolygonF polygon;
        polygon << screenPoint(cell.latitudeCenterDeg - half, cell.longitudeCenterDeg - half)
                << screenPoint(cell.latitudeCenterDeg - half, cell.longitudeCenterDeg + half)
                << screenPoint(cell.latitudeCenterDeg + half, cell.longitudeCenterDeg + half)
                << screenPoint(cell.latitudeCenterDeg + half, cell.longitudeCenterDeg - half);
        const auto center = screenPoint(cell.latitudeCenterDeg, cell.longitudeCenterDeg);
        const auto distance = QLineF(center, point).length();
        QPainterPath path;
        path.addPolygon(polygon);
        if (!path.contains(point) && distance > 10.0) continue;
        if (distance < bestDistance) {
            best = index;
            bestDistance = distance;
        }
    }
    return best;
}

QString RadarMapCanvas::lightningInfo(const std::size_t index) const {
    if (index >= lightningCells_.size()) return {};
    const auto& cell = lightningCells_[index];
    const auto now = QDateTime::currentSecsSinceEpoch();
    const auto phase = cell.kind == JumpEventKind::JumpCessation
        ? QStringLiteral("CESSATION")
        : cell.kind == JumpEventKind::JumpStart
            ? QStringLiteral("NEW JUMP")
            : QStringLiteral("ACTIVE UPDATE");
    const auto signal = !cell.active()
        ? QStringLiteral("Flash rate is declining after an active jump")
        : cell.sigmaLevel >= 5.0
            ? QStringLiteral("Exceptional electrical intensification")
            : cell.sigmaLevel >= 3.0
                ? QStringLiteral("Strong electrical intensification")
                : QStringLiteral("Elevated electrical intensification");
    const auto satellite = cell.satelliteNumber > 0
        ? QStringLiteral("GOES-%1 %2")
              .arg(cell.satelliteNumber)
              .arg(cell.satelliteNumber == 18 ? QStringLiteral("West")
                                               : QStringLiteral("East"))
        : QStringLiteral("GOES GLM");
    const auto feedAge = cell.latestDataEpochSec > 0
        ? std::max<std::int64_t>(0, now - cell.latestDataEpochSec)
        : std::numeric_limits<std::int64_t>::max();
    const auto feedHealth = feedAge <= 120
        ? QStringLiteral("LIVE")
        : feedAge <= 300 ? QStringLiteral("AGING") : QStringLiteral("STALE");

    QStringList lines;
    lines << QStringLiteral("GLM %1 | %2 sigma")
                 .arg(cell.active() ? QStringLiteral("LIGHTNING JUMP")
                                    : QStringLiteral("JUMP CESSATION"))
                 .arg(cell.sigmaLevel, 0, 'f', 1)
          << QStringLiteral("Phase: %1 | %2 flashes/min")
                 .arg(phase)
                 .arg(cell.flashRatePerMin, 0, 'f', 1)
          << QStringLiteral("Signal: %1").arg(signal)
          << QStringLiteral("Age: %1 | %2")
                 .arg(elapsedText(cell.eventTimeEpochSec, now), satellite)
          << QStringLiteral("Feed: %1 | newest scan %2 ago")
                 .arg(feedHealth, elapsedText(cell.latestDataEpochSec, now))
          << QStringLiteral("Location: %1, %2 | grid %3,%4")
                 .arg(cell.latitudeCenterDeg, 0, 'f', 3)
                 .arg(cell.longitudeCenterDeg, 0, 'f', 3)
                 .arg(cell.cellX)
                 .arg(cell.cellY);

    const auto association = std::find_if(
        lightningAssociations_.cbegin(), lightningAssociations_.cend(),
        [index](const auto& candidate) { return candidate.lightningCellIndex == index; });
    if (association != lightningAssociations_.cend() &&
        association->radarFeatureIndex < radarFeatures_.size()) {
        const auto& feature = radarFeatures_[association->radarFeatureIndex];
        lines << QStringLiteral("Radar fusion: %1 | object %2 | %3 km")
                     .arg(selectedRadar_.isEmpty() ? QStringLiteral("selected radar")
                                                   : selectedRadar_)
                     .arg(feature.id)
                     .arg(association->distanceKm, 0, 'f', 1)
              << QStringLiteral("Reflectivity: %1 dBZ max | %2 km2")
                     .arg(feature.maximumReflectivityDbz, 0, 'f', 1)
                     .arg(feature.areaKm2, 0, 'f', 1)
              << QStringLiteral("Rotation: %1 | shear %2 s^-1 | span %3 m/s")
                     .arg(feature.rotating ? QStringLiteral("DETECTED")
                                           : QStringLiteral("not detected"))
                     .arg(feature.azimuthalShearPerSecond, 0, 'f', 4)
                     .arg(feature.velocitySpanMetersPerSecond, 0, 'f', 1);
    } else if (!chunk_) {
        lines << QStringLiteral("Radar fusion: waiting for a selected radar volume");
    } else {
        lines << QStringLiteral("Radar fusion: no storm object within 45 km");
    }
    return lines.join(QLatin1Char('\n'));
}

void RadarMapCanvas::setPinnedOverlay(const OverlayHit& hit, const QPointF& point) {
    // When the user has locked the active card, ignore new overlay clicks
    // entirely so the card survives comparison clicks. Empty hits still clear
    // (so the explicit close path works).
    if (pinnedInfoLocked_ && !hit.text.isEmpty() &&
        (hit.text != pinnedInfo_ || hit.kind != pinnedInfoKind_)) {
        return;
    }
    const auto changed = hit.text != pinnedInfo_ || hit.kind != pinnedInfoKind_;
    if (changed) {
        pinnedInfoScrollOffset_ = 0.0;
        pinnedInfoScrollTarget_ = 0.0;
        pinnedInfoScrollTimer_->stop();
    }
    pinnedInfo_ = hit.text;
    pinnedInfoKind_ = hit.kind;
    pinnedLightningCell_.reset();
    if (hit.lightningCellIndex && *hit.lightningCellIndex < lightningCells_.size()) {
        const auto& cell = lightningCells_[*hit.lightningCellIndex];
        pinnedLightningCell_ = std::pair(cell.cellX, cell.cellY);
    }
    probePosition_ = point;
    probeText_ = pinnedInfo_;
    if (pinnedInfo_.isEmpty()) {
        pinnedInfoCardRect_ = {};
        pinnedInfoBodyRect_ = {};
        pinnedInfoLocked_ = false;
    }
    update();
}

void RadarMapCanvas::setPinnedOverlayLocked(bool locked) {
    pinnedInfoLocked_ = locked;
    update();
}

void RadarMapCanvas::refreshPinnedLightningInfo() {
    if (!pinnedLightningCell_) return;
    const auto selected = std::find_if(
        lightningCells_.cbegin(), lightningCells_.cend(), [this](const auto& cell) {
            return cell.cellX == pinnedLightningCell_->first &&
                   cell.cellY == pinnedLightningCell_->second;
        });
    if (selected == lightningCells_.cend()) {
        setPinnedOverlay({}, probePosition_);
        return;
    }
    const auto index = static_cast<std::size_t>(
        std::distance(lightningCells_.cbegin(), selected));
    pinnedInfo_ = lightningInfo(index);
    probeText_ = pinnedInfo_;
}

RadarMapCanvas::OverlayHit RadarMapCanvas::overlayAt(const QPointF& point) const {
    if (const auto lightning = lightningCellAt(point)) {
        return {lightningInfo(*lightning), PinnedInfoKind::Lightning, lightning};
    }
    if (placefilesVisible_ && !placefiles_.isEmpty()) {
        const auto edge = geographicPoint(QPointF(width(), height() / 2.0));
        const auto visibleRadiusNauticalMiles =
            rangeAndBearing({center_.y(), center_.x()}, {edge.y(), edge.x()}).distanceMeters / 1852.0;
        for (auto document = placefiles_.crbegin(); document != placefiles_.crend(); ++document) {
            for (auto item = document->elements.crbegin(); item != document->elements.crend(); ++item) {
                if (visibleRadiusNauticalMiles > item->thresholdNauticalMiles || item->points.isEmpty())
                    continue;
                QPolygonF polygon;
                polygon.reserve(item->points.size());
                for (const auto& coordinate : item->points)
                    polygon.push_back(screenPoint(coordinate.y(), coordinate.x()));
                bool hit = false;
                if (item->kind == PlacefileElementKind::Polygon) {
                    hit = polygon.containsPoint(point, Qt::OddEvenFill);
                } else if (item->kind == PlacefileElementKind::Line) {
                    for (qsizetype index = 1; index < polygon.size(); ++index) {
                        if (distanceToSegment(point, polygon.at(index - 1), polygon.at(index)) <=
                            std::max(6.0, item->lineWidth + 3.0)) {
                            hit = true;
                            break;
                        }
                    }
                } else {
                    hit = QLineF(point, polygon.constFirst()).length() <= 11.0;
                }
                if (!hit) continue;
                const auto title = document->title.isEmpty() ? QStringLiteral("PLACEFILE")
                                                              : document->title;
                auto detail = !item->hoverText.isEmpty() ? item->hoverText : item->text;
                if (detail.isEmpty()) detail = QStringLiteral("GR placefile overlay feature");
                const auto source = document->source.isEmpty()
                                        ? QStringLiteral("source unavailable")
                                        : document->source;
                return {QStringLiteral("%1\n%2\nSource: %3").arg(title, detail, source),
                        PinnedInfoKind::Placefile, std::nullopt};
            }
        }
    }
    ensureWeatherPaths();
    if (stormReportsVisible_) {
        for (qsizetype reportIndex = 0; reportIndex < stormReports_.size(); ++reportIndex) {
            const auto& report = stormReports_.at(reportIndex);
            if (!stormReportKindsVisible_.at(static_cast<std::size_t>(report.kind))) continue;
            const auto reportPoint = reportIndex < stormReportWorldPoints_.size()
                                         ? screenPointFromWorld(stormReportWorldPoints_.at(reportIndex))
                                         : screenPoint(report.coordinate.y(), report.coordinate.x());
            if (QLineF(reportPoint, point).length() > 9.0) continue;
            const auto kind = report.kind == StormReportKind::Tornado ? QStringLiteral("Tornado")
                              : report.kind == StormReportKind::Hail ? QStringLiteral("Hail")
                                                                    : QStringLiteral("Wind");
            const auto timestamp = report.occurredAt.isValid()
                                       ? report.occurredAt.toString(QStringLiteral("yyyy-MM-dd HH:mm 'UTC'"))
                                       : report.time + QStringLiteral(" UTC");
            const auto ageMinutes = report.occurredAt.isValid()
                                        ? report.occurredAt.secsTo(QDateTime::currentDateTimeUtc()) / 60
                                        : -1;
            const auto age = ageMinutes >= 0 ? QStringLiteral(" | %1m old").arg(ageMinutes) : QString{};
            return {
                QStringLiteral("%1 report | %2%3 | %4 %5 | %6, %7\n%8")
                    .arg(kind, timestamp, age, report.time, report.magnitude,
                         report.location, report.state, report.comments),
                PinnedInfoKind::StormReport,
                std::nullopt,
            };
        }
    }
    if (const auto metar = metarNear(point)) {
        const auto tempText = std::isnan(metar->tempC)
                                  ? QStringLiteral("--")
                                  : QStringLiteral("%1 C").arg(metar->tempC, 0, 'f', 1);
        const auto dewText = std::isnan(metar->dewpointC)
                                 ? QStringLiteral("--")
                                 : QStringLiteral("%1 C").arg(metar->dewpointC, 0, 'f', 1);
        const auto windText = metar->windSpeedKnots < 0
                                  ? QStringLiteral("wind: n/a")
                                  : metar->windGustKnots >= 0
                                        ? QStringLiteral("wind: %1 at %2 kt gust %3 kt")
                                              .arg(metar->windDirectionDegrees)
                                              .arg(metar->windSpeedKnots)
                                              .arg(metar->windGustKnots)
                                        : QStringLiteral("wind: %1 at %2 kt")
                                              .arg(metar->windDirectionDegrees)
                                              .arg(metar->windSpeedKnots);
        const auto ageMinutes = metar->obsTime.isValid()
                                    ? metar->obsTime.secsTo(QDateTime::currentDateTimeUtc()) / 60
                                    : -1;
        const auto age = ageMinutes >= 0 ? QStringLiteral(" | %1m old").arg(ageMinutes)
                                         : QStringLiteral(" | age unknown");
        const auto category = metar->flightCategory.isEmpty() ? QStringLiteral("category unknown")
                                                              : metar->flightCategory;
        const auto name = metar->stationName.isEmpty() ? metar->icaoId : metar->stationName;
        const auto obsTime = metar->obsTime.isValid()
                                 ? metar->obsTime.toString(QStringLiteral("MMM d HH:mm 'UTC'"))
                                 : QStringLiteral("time unknown");
        const auto precip = precipTypeForStation(metar->tempC, brightBand_.valid);
        const auto precipText =
            precip == PrecipType::Unknown
                ? QStringLiteral("Precip: unknown (needs a measured melting level)")
                : QStringLiteral("Precip: %1 (experimental)").arg(precipTypeLabel(precip));
        return {QStringLiteral("%1 (%2)\n%3 | dewpoint %4\n%5 | %6%7\n%8\nObserved: %9")
                    .arg(metar->icaoId, name, tempText, dewText, windText, category, age,
                         precipText, obsTime),
                PinnedInfoKind::Metar, std::nullopt};
    }
    if (mcdVisible_) {
        for (const auto& mcd : mcds_) {
            if (mcd.exterior.size() < 4) continue;
            QPolygonF polygon;
            for (const auto& lonLat : mcd.exterior) polygon.push_back(screenPoint(lonLat.y(), lonLat.x()));
            if (!polygon.containsPoint(point, Qt::OddEvenFill)) continue;
            const auto valid = mcd.validFrom.isValid() && mcd.validTo.isValid()
                                   ? QStringLiteral("Valid (UTC): %1 - %2")
                                         .arg(mcd.validFrom.toString(QStringLiteral("MMM d HH:mm")),
                                              mcd.validTo.toString(QStringLiteral("MMM d HH:mm")))
                                   : QStringLiteral("Valid: unknown");
            const auto probability = mcd.watchProbabilityPercent >= 0
                                         ? QStringLiteral("Watch issuance: %1%")
                                               .arg(mcd.watchProbabilityPercent)
                                         : QStringLiteral("Watch issuance: unstated");
            return {QStringLiteral("MCD %1 | %2\n%3 | %4\n%5\n\n%6\n\n%7")
                        .arg(mcd.number)
                        .arg(mcd.areas, mcd.concerning, valid, probability, mcd.summary,
                             mcd.discussion),
                    PinnedInfoKind::SpcMcd, std::nullopt};
        }
    }
    if (warningsVisible_) {
        const auto center = mapCenterWorld();
        auto worldPointAtCursor = QPointF(center.x() + point.x() - width() / 2.0,
                                          center.y() + point.y() - height() / 2.0);
        const auto world = static_cast<double>(kTileSize * (1 << zoom_));
        worldPointAtCursor.setX(std::fmod(worldPointAtCursor.x() + world, world));
        for (const auto& cached : warningPaths_) {
            if (!cached.bounds.contains(worldPointAtCursor) ||
                !cached.path.contains(worldPointAtCursor)) continue;
            if (cached.warningIndex >= 0 && cached.warningIndex < warnings_.size()) {
                    const auto& warning = warnings_.at(cached.warningIndex);
                    const auto onset = warning.onset.isValid()
                                           ? warning.onset.toLocalTime().toString(QStringLiteral("MMM d, HH:mm"))
                                           : QStringLiteral("unknown");
                    const auto expires = warning.expires.isValid()
                                             ? warning.expires.toLocalTime().toString(QStringLiteral("MMM d, HH:mm"))
                                             : QStringLiteral("unknown");
                    return QStringLiteral("%1\n%2\n%3\nValid (local): %4 – %5\nSource: %6\n\nDetails: %7\n\nInstructions: %8")
                        .arg(warning.event, warning.severity, warning.headline, onset, expires,
                             warning.senderName, warning.description, warning.instruction);
            }
        }
    }
    if (nhcVisible_) {
        // Above background outlooks, below NWS warnings (hurricane warnings
        // already arrive through the warnings path and win the click above).
        for (const auto& storm : nhcStorms_) {
            if (storm.cone.size() < 4) continue;
            QPolygonF polygon;
            for (const auto& lonLat : storm.cone) polygon.push_back(screenPoint(lonLat.y(), lonLat.x()));
            if (!polygon.containsPoint(point, Qt::OddEvenFill)) continue;
            return {QStringLiteral("%1 (%2)\nNHC forecast cone + track. Watches/warnings for this storm arrive through NWS alerts.\nSource: NHC GIS (experimental product)")
                        .arg(storm.name.isEmpty() ? QStringLiteral("Tropical cyclone") : storm.name,
                             storm.id),
                    PinnedInfoKind::NhcStorm, std::nullopt};
        }
    }
    if (spcOutlookVisible_) {
        // Lowest click priority: the outlook is background context, so
        // warnings, reports, and MCDs all win the click above.
        const SpcOutlookFeature* best = nullptr;
        for (const auto& feature : spcOutlooks_) {
            for (const auto& ring : feature.rings) {
                QPolygonF polygon;
                for (const auto& lonLat : ring.exterior)
                    polygon.push_back(screenPoint(lonLat.y(), lonLat.x()));
                if (!polygon.containsPoint(point, Qt::OddEvenFill)) continue;
                // Higher severity wins overlaps (ENH over SLGT over TSTM).
                if (!best || feature.dn > best->dn ||
                    (feature.dn == best->dn && feature.day < best->day))
                    best = &feature;
                break;
            }
        }
        if (best) {
            const auto window = best->valid.isValid() && best->expire.isValid()
                                    ? QStringLiteral("Valid (UTC): %1 - %2")
                                          .arg(best->valid.toString(QStringLiteral("MMM d HH:mm")),
                                               best->expire.toString(QStringLiteral("MMM d HH:mm")))
                                    : QStringLiteral("Valid: see SPC");
            const auto issuer = best->forecaster.isEmpty()
                                    ? QStringLiteral("SPC")
                                    : QStringLiteral("SPC (%1)").arg(best->forecaster);
            return {QStringLiteral("Day %1 outlook: %2\n%3\n%4")
                        .arg(best->day)
                        .arg(best->label2.isEmpty() ? best->label : best->label2, window, issuer),
                    PinnedInfoKind::SpcOutlook, std::nullopt};
        }
    }
    if (spcFireVisible_) {
        // Same lowest priority as the categorical outlook, checked after it
        // so fire wins an overlap (rarer, more urgent context).
        const SpcOutlookFeature* best = nullptr;
        for (const auto& feature : spcFireOutlooks_) {
            for (const auto& ring : feature.rings) {
                QPolygonF polygon;
                for (const auto& lonLat : ring.exterior)
                    polygon.push_back(screenPoint(lonLat.y(), lonLat.x()));
                if (!polygon.containsPoint(point, Qt::OddEvenFill)) continue;
                if (!best || feature.dn > best->dn ||
                    (feature.dn == best->dn && feature.day < best->day))
                    best = &feature;
                break;
            }
        }
        if (best) {
            const auto window = best->valid.isValid() && best->expire.isValid()
                                    ? QStringLiteral("Valid (UTC): %1 - %2")
                                          .arg(best->valid.toString(QStringLiteral("MMM d HH:mm")),
                                               best->expire.toString(QStringLiteral("MMM d HH:mm")))
                                    : QStringLiteral("Valid: see SPC");
            const auto issuer = best->forecaster.isEmpty()
                                    ? QStringLiteral("SPC")
                                    : QStringLiteral("SPC (%1)").arg(best->forecaster);
            const auto kind = best->fireKind.isEmpty()
                                  ? QStringLiteral("Fire weather")
                                  : QStringLiteral("Fire %1").arg(best->fireKind);
            return {QStringLiteral("Day %1 %2: %3\n%4\n%5")
                        .arg(best->day)
                        .arg(kind, best->label2.isEmpty() ? best->label : best->label2, window,
                             issuer),
                    PinnedInfoKind::SpcOutlook, std::nullopt};
        }
    }
    if (wpcVisible_) {
        // Same background priority as the other outlooks, checked last so
        // rainfall context never steals a severe-weather click.
        const SpcOutlookFeature* best = nullptr;
        for (const auto& feature : wpcOutlooks_) {
            for (const auto& ring : feature.rings) {
                QPolygonF polygon;
                for (const auto& lonLat : ring.exterior)
                    polygon.push_back(screenPoint(lonLat.y(), lonLat.x()));
                if (!polygon.containsPoint(point, Qt::OddEvenFill)) continue;
                if (!best || feature.dn > best->dn ||
                    (feature.dn == best->dn && feature.day < best->day))
                    best = &feature;
                break;
            }
        }
        if (best) {
            const auto window = best->valid.isValid() && best->expire.isValid()
                                    ? QStringLiteral("Valid (UTC): %1 - %2")
                                          .arg(best->valid.toString(QStringLiteral("MMM d HH:mm")),
                                               best->expire.toString(QStringLiteral("MMM d HH:mm")))
                                    : QStringLiteral("Valid: see WPC");
            return {QStringLiteral("Day %1 ERO: %2\n%3\nWPC")
                        .arg(best->day)
                        .arg(best->label2.isEmpty() ? best->label : best->label2, window),
                    PinnedInfoKind::SpcOutlook, std::nullopt};
        }
    }
    if (hmsVisible_) {
        // Fire points win over the smoke polygons they often sit inside.
        if (const auto fire = hmsFireNear(point)) {
            const auto frp = std::isnan(fire->frpMw)
                                 ? QStringLiteral("FRP: unmeasured")
                                 : QStringLiteral("FRP: %1 MW").arg(fire->frpMw, 0, 'f', 1);
            const auto when = fire->yearDay > 0
                                  ? QStringLiteral("Day %1 %2 UTC").arg(fire->yearDay).arg(
                                        fire->timeHhmm, 4, 10, QLatin1Char('0'))
                                  : QStringLiteral("time unknown");
            const auto source = fire->satellite.isEmpty()
                                    ? QStringLiteral("NOAA HMS")
                                    : QStringLiteral("NOAA HMS (%1)").arg(fire->satellite);
            return {QStringLiteral("Fire detection (%1°, %2°)\n%3 | %4\n%5")
                        .arg(fire->latitudeDeg, 0, 'f', 3)
                        .arg(fire->longitudeDeg, 0, 'f', 3)
                        .arg(frp, when, source),
                    PinnedInfoKind::HmsSmoke, std::nullopt};
        }
        // Above warnings like the other translucent context: smoke never
        // hides radar, and NWS warnings still win the click above it.
        // (This branch sits after warnings in overlayAt order.)
        for (const auto& plume : hmsPlumes_) {
            if (plume.ring.size() < 4) continue;
            QPolygonF polygon;
            for (const auto& lonLat : plume.ring) polygon.push_back(screenPoint(lonLat.y(), lonLat.x()));
            if (!polygon.containsPoint(point, Qt::OddEvenFill)) continue;
            const auto window = !plume.startText.isEmpty() && !plume.endText.isEmpty()
                                    ? QStringLiteral("Window (UTC): %1 - %2").arg(plume.startText,
                                                                                 plume.endText)
                                    : QStringLiteral("Window: see HMS");
            const auto source = plume.satellite.isEmpty()
                                    ? QStringLiteral("NOAA HMS")
                                    : QStringLiteral("NOAA HMS (%1)").arg(plume.satellite);
            return {QStringLiteral("Smoke (%1 density)\n%2\n%3")
                        .arg(hmsSmokeDensityLabel(plume.density), window, source),
                    PinnedInfoKind::HmsSmoke, std::nullopt};
        }
    }
    if (datVisible_) {
        // Lowest click priority alongside the other background context:
        // live warnings, cyclones, and outlooks all win the click above.
        // Ground truth, but stale by nature: never steal a live click.
        const DatPath* best = nullptr;
        double bestDistance = 10.0;
        for (const auto& path : datPaths_) {
            if (path.line.size() < 2) continue;
            QPolygonF line;
            for (const auto& lonLat : path.line) line.push_back(screenPoint(lonLat.y(), lonLat.x()));
            for (int i = 1; i < line.size(); ++i) {
                const QLineF segment(line.at(i - 1), line.at(i));
                QPointF nearest;
                // Project the cursor onto the segment for a true distance.
                const QPointF direction = segment.p2() - segment.p1();
                const double lengthSquared =
                    QPointF::dotProduct(direction, direction);
                double t = 0.0;
                if (lengthSquared > 0.0) {
                    t = QPointF::dotProduct(point - segment.p1(), direction) / lengthSquared;
                    t = std::clamp(t, 0.0, 1.0);
                }
                nearest = segment.p1() + direction * t;
                const double distance = QLineF(nearest, point).length();
                if (distance < bestDistance) {
                    bestDistance = distance;
                    best = &path;
                }
            }
        }
        if (best) {
            const auto when = best->stormDate.isValid()
                                  ? best->stormDate.toString(QStringLiteral("MMM d yyyy 'UTC'"))
                                  : QStringLiteral("date unknown");
            const auto rating =
                best->efScale.isEmpty() ? QStringLiteral("unrated") : best->efScale.toUpper();
            return {QStringLiteral("Tornado survey (%1)\nSurveyed: %2\nNWS Damage Assessment Toolkit, preliminary")
                        .arg(rating, when),
                    PinnedInfoKind::DatSurvey, std::nullopt};
        }
    }
    return {};
}

RadarMapCanvas::ProbeSample RadarMapCanvas::sampleAtGeo(const QPointF& lonLat) const {
    // Shared sampler behind the local hover probe and the linked cross-panel
    // readout: same rays, same gates, same value formatting. Invalid when
    // this panel has nothing to sample (no radar, no volume, no ray near the
    // bearing) so callers can fall back silently.
    ProbeSample sample;
    sample.lonLat = lonLat;
    const auto* site = selectedSite();
    if (!site || !site->latitude || !site->longitude || !chunk_ || chunk_->rays.empty()) return sample;
    if (probeRayIndex_.empty()) return sample;
    const auto location = rangeAndBearing({*site->latitude, *site->longitude},
                                          {lonLat.y(), lonLat.x()});
    sample.groundRangeMeters = location.distanceMeters;
    sample.azimuthDegrees = location.bearingDegrees;
    const auto candidate = std::lower_bound(
        probeRayIndex_.cbegin(), probeRayIndex_.cend(), sample.azimuthDegrees,
        [](const auto& indexedRay, const double target) { return indexedRay.first < target; });
    const auto circularDifference = [azimuth = sample.azimuthDegrees](const double candidateAzimuth) {
        const auto direct = std::abs(candidateAzimuth - azimuth);
        return std::min(direct, 360.0 - direct);
    };
    auto nearest = candidate == probeRayIndex_.cend() ? probeRayIndex_.cbegin() : candidate;
    auto nearestDifference = circularDifference(nearest->first);
    const auto previous = candidate == probeRayIndex_.cbegin()
                              ? std::prev(probeRayIndex_.cend())
                              : std::prev(candidate);
    const auto previousDifference = circularDifference(previous->first);
    if (previousDifference < nearestDifference) {
        nearest = previous;
        nearestDifference = previousDifference;
    }
    const auto nearestRayIndex = nearest->second;
    const auto* nearestRay = &chunk_->rays.at(nearestRayIndex);
    if (!nearestRay || nearestDifference > std::max(1.5, nearestRay->azimuthResolutionDegrees * 1.5)) {
        sample.valueText = QStringLiteral("No ray at this bearing");
        sample.groundRangeMeters = location.distanceMeters;
        sample.azimuthDegrees = sample.azimuthDegrees;
        sample.valid = true;
        return sample;
    }
    const auto index = static_cast<std::size_t>(product_);
    const auto spacing = nearestRay->gateSpacingByProduct.at(index);
    if (spacing == 0) {
        sample.valueText = QStringLiteral("Product unavailable here");
        sample.groundRangeMeters = location.distanceMeters;
        sample.valid = true;
        return sample;
    }
    const auto firstGate = nearestRay->firstGateByProduct.at(index);
    const auto slantRangeMeters = slantRangeForSurfaceDistance(sample.groundRangeMeters,
                                                               nearestRay->elevationDegrees);
    const auto& values = nearestRay->fields.at(index);
    const auto gate = gateIndexForSlantRange(slantRangeMeters, firstGate, spacing, values.size());
    if (!gate) {
        sample.groundRangeMeters = location.distanceMeters;
        sample.azimuthDegrees = nearestRay->azimuthDegrees;
        const double maxSlant = static_cast<double>(firstGate) +
                                static_cast<double>(spacing) * static_cast<double>(values.size());
        sample.valueText = slantRangeMeters < static_cast<double>(firstGate)
                               ? QStringLiteral("Inside the blind range")
                               : slantRangeMeters > maxSlant
                                     ? QStringLiteral("Beyond radar range")
                                     : QStringLiteral("No gate at this range");
        sample.valid = true;
        return sample;
    }
    const auto encoded = values.at(*gate);
    if (encoded == 0) {
        // Derived fields encode missing as 0: above the CAPPI height or
        // outside the volume is "no data", never "below threshold".
        sample.valueText = isDerivedProduct(product_) ? QStringLiteral("No data at this gate")
                                                      : QStringLiteral("Below threshold");
    } else if (encoded == 1) {
        sample.valueText = QStringLiteral("Range folded");
    } else {
        std::optional<double> value;
        if (product_ == ProductKind::Velocity && processedVelocity_ &&
            nearestRayIndex < processedVelocity_->size() &&
            *gate < processedVelocity_->at(nearestRayIndex).size() &&
            std::isfinite(processedVelocity_->at(nearestRayIndex)[*gate])) {
            value = processedVelocity_->at(nearestRayIndex)[*gate];
        } else {
            value = nativeValue(encoded, nearestRay->scaleByProduct.at(index),
                                nearestRay->offsetByProduct.at(index));
        }
        if (value.has_value()) {
        const auto unit = QString::fromUtf8(productDescriptor(product_).unit.data());
        sample.valueText = QStringLiteral("%1%2%3")
                        .arg(*value, 0, 'f', probeDecimals(product_))
                        .arg(unit.isEmpty() ? QString{} : QStringLiteral(" "))
                        .arg(unit);
        } else {
            sample.valueText = QStringLiteral("No data at this gate");
        }
    }
    sample.elevationDegrees = nearestRay->elevationDegrees;
    const auto beam = standardBeamGeometry(slantRangeMeters, nearestRay->elevationDegrees);
    sample.beamMslMeters = beam.heightAboveRadarMeters + site->antennaElevationMeters;
    sample.valid = true;
    return sample;
}

void RadarMapCanvas::updateProbe(const QPointF& point) {
    probePosition_ = point;
    if (!pinnedInfo_.isEmpty()) {
        probeText_ = pinnedInfo_;
        update();
        return;
    }
    // Nearest surface station, shared by every probe path below so hover
    // and click-pin agree on which station is "here".
    const auto metarSuffix = [this, &point] {
        const auto metar = metarNear(point);
        if (!metar) return QString{};
        const auto tempText = std::isnan(metar->tempC)
                                  ? QStringLiteral("--")
                                  : QString::number(static_cast<int>(std::lround(metar->tempC)));
        const auto dewText = std::isnan(metar->dewpointC)
                                 ? QStringLiteral("--")
                                 : QString::number(static_cast<int>(std::lround(metar->dewpointC)));
        return QStringLiteral(" | %1 %2/%3").arg(metar->icaoId, tempText, dewText);
    };
    probeText_.clear();
    const auto geographic = geographicPoint(point);
    const auto sample = sampleAtGeo(QPointF(geographic.x(), geographic.y()));
    const auto metar = metarSuffix();
    if (!sample.valid) {
        // No radar gate here, but a nearby station still answers the hover.
        probeText_ = metar.isEmpty() ? QString{} : metar.mid(3);
        update();
        return;
    }
    // Reason-only samples (no gate under the cursor) carry no tilt/beam:
    // show the reason with range instead of a bogus 0.0 tilt.
    if (sample.beamMslMeters == 0.0 && sample.elevationDegrees == 0.0 &&
        (sample.valueText.startsWith(QStringLiteral("No ")) ||
         sample.valueText.startsWith(QStringLiteral("Inside")) ||
         sample.valueText.startsWith(QStringLiteral("Beyond")) ||
         sample.valueText.startsWith(QStringLiteral("Product")))) {
        probeText_ = QStringLiteral("%1 | %2 km")
                         .arg(sample.valueText)
                         .arg(sample.groundRangeMeters / 1000.0, 0, 'f', 1) +
                     metar;
        update();
        return;
    }
    probeText_ = QStringLiteral("%1 | %2 km | %3%4 | tilt %5%4 | beam %6 km MSL")
                     .arg(sample.valueText)
                     .arg(sample.groundRangeMeters / 1000.0, 0, 'f', 1)
                     .arg(sample.azimuthDegrees, 0, 'f', 1)
                     .arg(QChar(0x00b0))
                     .arg(sample.elevationDegrees, 0, 'f', 1)
                     .arg(sample.beamMslMeters / 1000.0, 0, 'f', 2) +
                 metar;
    update();
}

// TODO: future extraction point — move radar sweep rasterization out of the
// canvas into a RadarRasterizer (offscreen tile + sweep compositor) without moving code yet.
void RadarMapCanvas::setVolume3dMode(const bool enabled) {
    if (volume3d_ == enabled) return;
    volume3d_ = enabled;
    if (volume3d_) {
        // Start from a readable oblique view instead of wherever the last
        // session left the camera.
        volumeOrbitAzimuth_ = 180.0;
        volumeOrbitElevation_ = 40.0;
        volumeDistance_ = 420000.0;
        volumeTarget_[0] = 0.0;
        volumeTarget_[1] = 0.0;
        volumeTarget_[2] = 7000.0;
        rebuildVolumeRender();
    }
    update();
    emit volume3dModeToggled(volume3d_);
}

namespace {

// Project an ENU point to panel pixels with a volume camera. False when the
// point is behind the eye. Inverse of the marcher's pixel-to-ray mapping.
bool projectVolumePoint(const VolumeCamera& camera, const double point[3], const int width,
                        const int height, QPointF* out) {
    const double dx = point[0] - camera.eye[0];
    const double dy = point[1] - camera.eye[1];
    const double dz = point[2] - camera.eye[2];
    const double xd = dx * camera.right[0] + dy * camera.right[1] + dz * camera.right[2];
    const double yd = dx * camera.up[0] + dy * camera.up[1] + dz * camera.up[2];
    const double zd = dx * camera.forward[0] + dy * camera.forward[1] + dz * camera.forward[2];
    if (zd <= 1.0) return false;
    const double aspect = static_cast<double>(width) / static_cast<double>(std::max(1, height));
    const double px = camera.focal * xd / zd;
    const double py = camera.focal * yd / zd;
    out->setX((px / aspect * 0.5 + 0.5) * width);
    out->setY((0.5 - py * 0.5) * height);
    return true;
}

// Panel pixel -> ground-plane ENU point: the exact inverse of the marcher's
// pixel-to-ray mapping (same aspect term, same focal), so a box dragged on
// screen lands where the pixel looks. A pixel above the horizon has no ground
// point; with `clampAboveHorizon` it is clamped to the coverage circle along
// its own heading, so a low orbit still boxes instead of refusing the gesture,
// and without it the mapping honestly reports that there is no ground there.
bool groundPointFromCamera(const double target[3], const double azimuthDeg,
                           const double elevationDeg, const double distanceMeters,
                           const QPointF& pixel, const int width, const int height,
                           const double coverageMeters, const bool clampAboveHorizon,
                           double* xMeters, double* yMeters) {
    if (width <= 0 || height <= 0) return false;
    const VolumeBrick frame;
    const VolumeCamera camera =
        volumeCameraFor(frame, azimuthDeg, elevationDeg, distanceMeters, target);
    const double aspect = static_cast<double>(width) / static_cast<double>(height);
    const double px = ((pixel.x() + 0.5) / static_cast<double>(width) * 2.0 - 1.0) * aspect;
    const double py = 1.0 - (pixel.y() + 0.5) / static_cast<double>(height) * 2.0;
    double dir[3] = {
        camera.forward[0] * camera.focal + camera.right[0] * px + camera.up[0] * py,
        camera.forward[1] * camera.focal + camera.right[1] * px + camera.up[1] * py,
        camera.forward[2] * camera.focal + camera.right[2] * px + camera.up[2] * py,
    };
    const double length = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (!(length > 1e-12)) return false;
    for (double& component : dir) component /= length;
    if (dir[2] < -1e-6) {
        const double t = -camera.eye[2] / dir[2];
        if (t > 1.0) {
            *xMeters = camera.eye[0] + dir[0] * t;
            *yMeters = camera.eye[1] + dir[1] * t;
            return true;
        }
    }
    if (!clampAboveHorizon) return false;
    const double heading = std::hypot(dir[0], dir[1]);
    if (!(heading > 1e-9)) return false; // straight down: no heading to follow
    const double dx = dir[0] / heading;
    const double dy = dir[1] / heading;
    const double along = camera.eye[0] * dx + camera.eye[1] * dy;
    const double eyeRadiusSq = camera.eye[0] * camera.eye[0] + camera.eye[1] * camera.eye[1];
    const double discriminant = along * along - eyeRadiusSq + coverageMeters * coverageMeters;
    const double travel = discriminant > 0.0 ? (-along + std::sqrt(discriminant)) : 0.0;
    *xMeters = camera.eye[0] + dx * travel;
    *yMeters = camera.eye[1] + dy * travel;
    return true;
}

// Boxes are derived from a drag, so exact double equality would rebuild the
// brick on a floating-point no-op gesture. One metre is below a voxel anywhere.
bool sameVolume3dBox(const RadarMapCanvas::Volume3dBox& a,
                     const RadarMapCanvas::Volume3dBox& b) {
    return std::fabs(a.centerXMeters - b.centerXMeters) < 1.0 &&
           std::fabs(a.centerYMeters - b.centerYMeters) < 1.0 &&
           std::fabs(a.halfExtentMeters - b.halfExtentMeters) < 1.0;
}

} // namespace

void RadarMapCanvas::rebuildVolumeRender() {
    rebuildVolumeRender(false);
}

void RadarMapCanvas::rebuildVolumeRender(const bool fast) {
    if (!volume3d_ || width() <= 0 || height() <= 0) return;
    const auto requestId = volumeRequestId_.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto chunk = chunk_;
    const auto product = product_;
    const auto palette = palette_;
    const auto customPalette = customPalette_;
    const auto opacity = radarOpacity_;
    const auto brick = brick_;
    const auto brickChunk = brickChunk_;
    const auto brickProduct = brickProduct_;
    const auto box = volumeBox_;
    const auto cachedBox = brickBox_;
    const QSize logicalSize = size();
    const auto orbitAzimuth = volumeOrbitAzimuth_;
    const auto orbitElevation = volumeOrbitElevation_;
    const auto orbitDistance = volumeDistance_;
    double orbitTarget[3] = {volumeTarget_[0], volumeTarget_[1], volumeTarget_[2]};
    // While orbiting, march at third resolution for responsiveness and
    // refine on release; idle renders use half resolution.
    const int divisor = fast ? 3 : 2;
    const QPointer<RadarMapCanvas> guard(this);
    QThreadPool::globalInstance()->start(
        [guard, requestId, chunk, product, palette, customPalette, opacity, brick, brickChunk,
         brickProduct, box, cachedBox, logicalSize, orbitAzimuth, orbitElevation, orbitDistance,
         orbitTarget, divisor] {
            auto stale = [&] {
                return !guard || requestId != guard->volumeRequestId_.load(std::memory_order_relaxed);
            };
            const auto publishEmpty = [&] {
                if (stale()) return;
                QMetaObject::invokeMethod(guard, [guard, requestId, chunk, product] {
                    if (!guard || requestId != guard->volumeRequestId_.load(std::memory_order_relaxed))
                        return;
                    guard->brick_.reset();
                    guard->brickChunk_ = chunk;
                    guard->brickProduct_ = product;
                    guard->brickBox_ = Volume3dBox{};
                    guard->volumeImage_ = QImage();
                    guard->update();
                }, Qt::QueuedConnection);
            };
            // Derived grids (Echo Tops, VIL) are 2D products: no volume to march.
            if (isDerivedProduct(product) || !chunk || chunk->rays.empty()) {
                publishEmpty();
                return;
            }
            std::shared_ptr<const VolumeBrick> brickNow = brick;
            double buildMs = 0.0;
            // The box is part of the cache key: a new box is a new brick, and
            // the same box must not rebuild on a no-op drag.
            if (!brickNow || brickChunk != chunk || brickProduct != product ||
                !sameVolume3dBox(cachedBox, box)) {
                VolumeBrickDims brickDims;
                brickDims.nz = 128;
                brickDims.halfExtentMeters = box.halfExtentMeters;
                brickDims.centerXMeters = box.centerXMeters;
                brickDims.centerYMeters = box.centerYMeters;
                QElapsedTimer buildTimer;
                buildTimer.start();
                VolumeBrick fresh = buildVolumeBrick(*chunk, product, brickDims);
                buildMs = static_cast<double>(buildTimer.elapsed());
                brickNow = std::make_shared<const VolumeBrick>(std::move(fresh));
            }
            if (stale()) return;
            // Transfer stops sampled from the live palette so 2D and 3D agree
            // on color. Alpha stays a fixed ramp here (the RHI LUT mirrors it)
            // scaled by the echo opacity. Shared with the RHI backend through
            // volumeTransferStops() so both marches use one ramp.
            const std::vector<RgbaStop> stops = volumeTransferStops(product, palette, customPalette);
            const int renderWidth = std::max(1, logicalSize.width() / divisor);
            const int renderHeight = std::max(1, logicalSize.height() / divisor);
            QImage image(renderWidth, renderHeight, QImage::Format_ARGB32);
            image.fill(Qt::transparent);
            const double frameTarget[3] = {brickNow->centerXMeters, brickNow->centerYMeters,
                                           orbitTarget[2]};
            const VolumeCamera camera =
                volumeCameraFor(*brickNow, orbitAzimuth, orbitElevation, orbitDistance, frameTarget);
            const double spanX = 2.0 * brickNow->halfExtentMeters / brickNow->nx;
            const double spanY = 2.0 * brickNow->halfExtentMeters / brickNow->ny;
            const double spanZ = brickNow->topMeters / brickNow->nz;
            const double stepMeters = std::max({spanX, spanY, spanZ}) * 0.5;
            const double diagonalMeters =
                std::sqrt(8.0 * brickNow->halfExtentMeters * brickNow->halfExtentMeters +
                          brickNow->topMeters * brickNow->topMeters);
            const int maxSteps = static_cast<int>(diagonalMeters / stepMeters) + 2;
            const double aspect = static_cast<double>(renderWidth) / renderHeight;
            const float opacityScale = static_cast<float>(opacity) / 100.0F;
            constexpr float kAmbient = 0.46F;
            constexpr double kLight[3] = {-0.5, -0.5, 0.7071};
            constexpr float kDensityPerMeter = 1.2e-3F;
            QElapsedTimer timer;
            timer.start();
            for (int y = 0; y < renderHeight; ++y) {
                if ((y & 15) == 0 && stale()) return;
                auto* scan = reinterpret_cast<QRgb*>(image.scanLine(y));
                for (int x = 0; x < renderWidth; ++x) {
                    const double px = ((static_cast<double>(x) + 0.5) / renderWidth * 2.0 - 1.0) * aspect;
                    const double py = 1.0 - (static_cast<double>(y) + 0.5) / renderHeight * 2.0;
                    double dir[3] = {
                        camera.forward[0] * camera.focal + camera.right[0] * px + camera.up[0] * py,
                        camera.forward[1] * camera.focal + camera.right[1] * px + camera.up[1] * py,
                        camera.forward[2] * camera.focal + camera.right[2] * px + camera.up[2] * py,
                    };
                    const double dirLen = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
                    for (int axis = 0; axis < 3; ++axis) dir[axis] /= dirLen;
                    const auto hit = intersectVolumeBox(*brickNow, camera.eye, dir);
                    if (!hit.has_value() || hit->tExit <= 0.0) continue;
                    double t = std::max(hit->tEnter, 0.0);
                    float r = 0.0F, g = 0.0F, b = 0.0F, a = 0.0F;
                    int steps = 0;
                    while (t < hit->tExit && a < 0.98F && steps < maxSteps) {
                        const double sx = camera.eye[0] + dir[0] * t;
                        const double sy = camera.eye[1] + dir[1] * t;
                        const double sz = camera.eye[2] + dir[2] * t;
                        const auto sample = sampleBrickTrilinear(*brickNow, sx, sy, sz);
                        if (sample.has_value()) {
                            const Rgba color = evaluateTransfer(stops, sample->value01);
                            const float sigma = color.a * sample->coverage * opacityScale;
                            // Beer-Lambert along the step, not a per-sample alpha:
                            // a fixed alpha saturates within two samples and the
                            // ray becomes a fog shell instead of an integrated
                            // volume, which is what makes shading unreadable.
                            const float gate =
                                1.0F - std::exp(-sigma * kDensityPerMeter *
                                                static_cast<float>(stepMeters));
                            if (gate > 0.002F) {
                                float shade = kAmbient;
                                if (const auto normal = brickSurfaceNormal(*brickNow, sx, sy, sz)) {
                                    const double lit = normal->x * kLight[0] +
                                                       normal->y * kLight[1] +
                                                       normal->z * kLight[2];
                                    shade += (1.0F - kAmbient) *
                                             static_cast<float>(std::max(0.0, lit));
                                }
                                r += (1.0F - a) * color.r * shade * gate;
                                g += (1.0F - a) * color.g * shade * gate;
                                b += (1.0F - a) * color.b * shade * gate;
                                a += (1.0F - a) * gate;
                            }
                        }
                        t += stepMeters;
                        ++steps;
                    }
                    if (a <= 0.0F) continue;
                    scan[x] = qRgba(static_cast<int>(std::clamp(r, 0.0F, 1.0F) * 255.0F),
                                    static_cast<int>(std::clamp(g, 0.0F, 1.0F) * 255.0F),
                                    static_cast<int>(std::clamp(b, 0.0F, 1.0F) * 255.0F),
                                    static_cast<int>(std::clamp(a, 0.0F, 1.0F) * 255.0F));
                }
            }
            const double renderMs = static_cast<double>(timer.elapsed());
            if (stale()) return;
            QMetaObject::invokeMethod(guard, [guard, requestId, brickNow,
                                             image = std::move(image), renderMs, buildMs,
                                             logicalSize, orbitAzimuth, orbitElevation,
                                             orbitDistance, orbitTarget] {
                if (!guard || requestId != guard->volumeRequestId_.load(std::memory_order_relaxed))
                    return;
                guard->brick_ = brickNow;
                guard->brickChunk_ = guard->chunk_;
                guard->brickProduct_ = guard->product_;
                // The box this brick really covers, read back from the brick
                // itself so the cache key can never disagree with the data.
                guard->brickBox_ = Volume3dBox{brickNow->centerXMeters, brickNow->centerYMeters,
                                               brickNow->halfExtentMeters};
                guard->volumeImage_ = image;
                guard->volumeImageLogicalSize_ = logicalSize;
                guard->volumeRenderMs_ = renderMs;
                if (buildMs > 0.0) guard->volumeBuildMs_ = buildMs;
                guard->volumeImageAzimuth_ = orbitAzimuth;
                guard->volumeImageElevation_ = orbitElevation;
                guard->volumeImageDistance_ = orbitDistance;
                for (int axis = 0; axis < 3; ++axis)
                    guard->volumeImageTarget_[axis] = orbitTarget[axis];
                guard->volumeTarget_[0] = brickNow->centerXMeters;
                guard->volumeTarget_[1] = brickNow->centerYMeters;
                guard->update();
            }, Qt::QueuedConnection);
        });
}

bool RadarMapCanvas::Volume3dBox::fullRange() const {
    return std::fabs(centerXMeters) < 1.0 && std::fabs(centerYMeters) < 1.0 &&
           std::fabs(halfExtentMeters - kVolume3dFullRangeHalfMeters) < 1.0;
}

RadarMapCanvas::Volume3dBox RadarMapCanvas::clampVolume3dBox(Volume3dBox box,
                                                             const double coverageMeters) {
    const double coverage = std::max(coverageMeters, kVolume3dMinimumBoxMeters);
    if (!std::isfinite(box.centerXMeters) || !std::isfinite(box.centerYMeters) ||
        !std::isfinite(box.halfExtentMeters)) {
        // A broken drag falls back to the box that is never empty rather than
        // handing NaN extents to the builder.
        return Volume3dBox{};
    }
    // A square box fits the coverage circle only below half = R / sqrt(2); the
    // minimum is the floor, so a two-pixel drag still builds a real grid.
    const double minimumHalf = 0.5 * kVolume3dMinimumBoxMeters;
    const double maximumHalf = coverage / std::sqrt(2.0);
    box.halfExtentMeters = std::clamp(box.halfExtentMeters, minimumHalf, maximumHalf);
    // Pull the centre in along its own bearing so the far corner stays inside
    // coverage. Snapping the centre onto the site would silently move the box
    // the user dragged; this keeps the direction and only shortens the reach.
    const double centreRadius = std::hypot(box.centerXMeters, box.centerYMeters);
    const double maximumCentreRadius =
        std::max(0.0, coverage - box.halfExtentMeters * std::sqrt(2.0));
    if (centreRadius > maximumCentreRadius) {
        const double scale = centreRadius > 0.0 ? maximumCentreRadius / centreRadius : 0.0;
        box.centerXMeters *= scale;
        box.centerYMeters *= scale;
    }
    return box;
}

double RadarMapCanvas::volume3dCoverageMeters() const {
    // The radar's own coverage: the ground range of the farthest decoded gate.
    // Never the echo extent - automatic ROI selection failed three times by
    // guessing where the weather was, so this ceiling is what the data reaches,
    // not what happens to light up.
    double coverage = 0.0;
    if (chunk_) {
        const auto productIndex = static_cast<std::size_t>(product_);
        for (const DecodedRay& ray : chunk_->rays) {
            if (!ray.has(product_)) continue;
            const auto& field = ray.fields.at(productIndex);
            if (field.empty()) continue;
            const double slant = gateCenterSlantRange(ray.firstGateByProduct.at(productIndex),
                                                      ray.gateSpacingByProduct.at(productIndex),
                                                      field.size() - 1);
            const EnuPoint point = gateEnu(ray.azimuthDegrees, slant, ray.elevationDegrees);
            coverage = std::max(coverage, std::hypot(point.x, point.y));
        }
    }
    if (!(coverage > 0.0)) coverage = maximumRangeMeters_;
    return std::clamp(coverage, kVolume3dMinimumBoxMeters, 500000.0);
}

bool RadarMapCanvas::volume3dProjectPoint(const double xMeters, const double yMeters,
                                          const double zMeters, QPointF* out) const {
    double target[3] = {volumeImageTarget_[0], volumeImageTarget_[1], volumeImageTarget_[2]};
    const VolumeBrick frame;
    const VolumeCamera camera = volumeCameraFor(frame, volumeImageAzimuth_, volumeImageElevation_,
                                                volumeImageDistance_, target);
    const double point[3] = {xMeters, yMeters, zMeters};
    return projectVolumePoint(camera, point, width(), height(), out);
}

bool RadarMapCanvas::volume3dGroundPointAt(const QPoint& pixel, double* xMeters,
                                           double* yMeters) const {
    double target[3] = {volumeImageTarget_[0], volumeImageTarget_[1], volumeImageTarget_[2]};
    return groundPointFromCamera(target, volumeImageAzimuth_, volumeImageElevation_,
                                 volumeImageDistance_, QPointF(pixel), width(), height(),
                                 volume3dCoverageMeters(), false, xMeters, yMeters);
}

void RadarMapCanvas::fitVolumeCameraToBox() {
    volumeTarget_[0] = volumeBox_.centerXMeters;
    volumeTarget_[1] = volumeBox_.centerYMeters;
    volumeTarget_[2] = 7000.0;
    // 2.8x the half extent is the full-range default (420 km for the 150 km
    // box), so a storm box is framed the way the radar view always was. The
    // floor keeps an 8 km box from putting the eye inside the volume.
    volumeDistance_ = std::clamp(2.8 * volumeBox_.halfExtentMeters, 60000.0, 1200000.0);
}

RadarMapCanvas::Volume3dBox RadarMapCanvas::volumeBoxFromDrag(const QPoint& start,
                                                              const QPoint& end,
                                                              bool* mapped) const {
    *mapped = false;
    const double coverage = volume3dCoverageMeters();
    double target[3] = {volumeImageTarget_[0], volumeImageTarget_[1], volumeImageTarget_[2]};
    double x0 = 0.0;
    double y0 = 0.0;
    double x1 = 0.0;
    double y1 = 0.0;
    if (!groundPointFromCamera(target, volumeImageAzimuth_, volumeImageElevation_,
                               volumeImageDistance_, QPointF(start), width(), height(), coverage,
                               true, &x0, &y0) ||
        !groundPointFromCamera(target, volumeImageAzimuth_, volumeImageElevation_,
                               volumeImageDistance_, QPointF(end), width(), height(), coverage,
                               true, &x1, &y1)) {
        return Volume3dBox{};
    }
    *mapped = true;
    Volume3dBox box;
    box.centerXMeters = 0.5 * (x0 + x1);
    box.centerYMeters = 0.5 * (y0 + y1);
    // The brick has one half extent for x and y, so the box that can be built
    // around a rectangle is the square that covers it. The drag paints that
    // square while the mouse moves, so the box on screen is the box built.
    box.halfExtentMeters = 0.5 * std::max(std::fabs(x1 - x0), std::fabs(y1 - y0));
    return clampVolume3dBox(box, coverage);
}

void RadarMapCanvas::setVolume3dBox(const Volume3dBox& box) {
    const Volume3dBox clamped = clampVolume3dBox(box, volume3dCoverageMeters());
    if (sameVolume3dBox(clamped, volumeBox_) && sameVolume3dBox(clamped, brickBox_)) return;
    volumeBox_ = clamped;
    fitVolumeCameraToBox();
    rebuildVolumeRender();
    update();
}

void RadarMapCanvas::resetVolume3dBox() {
    volumeBox_ = Volume3dBox{}; // full range: the default that is never empty
    volumeOrbitAzimuth_ = 180.0;
    volumeOrbitElevation_ = 40.0;
    volumeDistance_ = 420000.0;
    volumeTarget_[0] = 0.0;
    volumeTarget_[1] = 0.0;
    volumeTarget_[2] = 7000.0;
    rebuildVolumeRender();
    update();
}

bool RadarMapCanvas::applyVolume3dBoxDrag(const QPoint& start, const QPoint& end) {
    bool mapped = false;
    const Volume3dBox box = volumeBoxFromDrag(start, end, &mapped);
    if (!mapped) return false;
    setVolume3dBox(box);
    return true;
}

void RadarMapCanvas::paintVolume3d(QPainter& painter) {
    painter.fillRect(rect(), QColor(8, 13, 16));
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    if (!volumeImage_.isNull()) {
        QSize fitted = volumeImage_.size();
        fitted.scale(size(), Qt::KeepAspectRatio);
        const QRect target((width() - fitted.width()) / 2, (height() - fitted.height()) / 2,
                           fitted.width(), fitted.height());
        painter.drawImage(target, volumeImage_);
    } else {
        painter.setPen(QColor(200, 215, 220));
        painter.setFont(hudFont(10));
        QString message = tr("Building 3D volume...");
        if (!chunk_ || chunk_->rays.empty())
            message = tr("No volume loaded");
        else if (isDerivedProduct(product_))
            message = tr("3D needs a base product (REF, VEL, ...)");
        painter.drawText(rect(), Qt::AlignCenter, message);
    }
    if (brick_ && !volumeImage_.isNull()) paintVolumeGrid(painter);
    const auto& descriptor = productDescriptor(product_);
    const QString shortName =
        QString::fromUtf8(descriptor.shortName.data(),
                          static_cast<int>(descriptor.shortName.size()));
    painter.setFont(hudFont(8, QFont::DemiBold));
    painter.setPen(QColor(232, 238, 240));
    painter.setBrush(QColor(8, 15, 19, 205));
    QString label = QStringLiteral("3D preview (CPU)  %1 %2  %3 ms")
                        .arg(selectedRadar_.isEmpty() ? QStringLiteral("...") : selectedRadar_)
                        .arg(shortName)
                        .arg(static_cast<int>(volumeRenderMs_));
    if (brick_ && !brickBox_.fullRange()) {
        // The box is the user's choice, so say which one is on screen: a 40 km
        // box and the full range are not the same picture.
        label += QStringLiteral("  - %1 km box").arg(brickBox_.widthMeters() / 1000.0, 0, 'f', 0);
    }
    if (brick_ && brick_->interpolatedVoxels > 0) {
        // Filled voxels are reconstructed, not observed. Say so, with the share,
        // the same way an incomplete volume is flagged rather than hidden.
        const std::size_t occupied = brick_->measuredVoxels + brick_->interpolatedVoxels;
        const int filledPercent = occupied > 0
                                      ? static_cast<int>(std::lround(
                                            100.0 * static_cast<double>(brick_->interpolatedVoxels) /
                                            static_cast<double>(occupied)))
                                      : 0;
        label += QStringLiteral("  - %1 %2 interpolated (%3% reconstructed)")
                     .arg(brick_->interpolatedTilts)
                     .arg(brick_->interpolatedTilts == 1 ? QStringLiteral("tilt")
                                                        : QStringLiteral("tilts"))
                     .arg(filledPercent);
    }
    if (brick_ && brick_->measuredVoxels + brick_->interpolatedVoxels == 0) {
        // An empty box is not a quiet storm. Say it in words.
        label += QStringLiteral("  - no gates in box");
    }
    // Size the chip to its own text so the disclosure is never elided away on
    // a wide panel; elide only when the panel itself is too narrow.
    const double chipWidth =
        std::min(static_cast<double>(width()) - 24.0,
                 static_cast<double>(painter.fontMetrics().horizontalAdvance(label)) + 18.0);
    const QRectF chip(12.0, 10.0, std::max(140.0, chipWidth), 22.0);
    painter.drawRoundedRect(chip, 4, 4);
    painter.drawText(chip.adjusted(8.0, 0.0, -8.0, 0.0), Qt::AlignLeft | Qt::AlignVCenter,
                     painter.fontMetrics().elidedText(label, Qt::ElideRight,
                                                      static_cast<int>(chip.width()) - 16));
    painter.setFont(hudFont(8));
    painter.setPen(QColor(160, 175, 182));
    painter.drawText(QRectF(12.0, height() - 30.0, width() - 24.0, 20.0),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     tr("Left-drag orbits - middle-drag moves - wheel zooms - Alt+drag boxes - "
                        "right-click exits 3D"));
}

void RadarMapCanvas::paintVolumeGrid(QPainter& painter) {
    // Ground orientation: grid, range rings, site marker, north label, all
    // projected with the image's own camera so grid and echo move as one.
    // Everything is clipped to the brick's footprint: with a dragged box the
    // frame covers 40 km, and a grid drawn out to 150 km would claim ground the
    // picture does not show.
    double target[3] = {volumeImageTarget_[0], volumeImageTarget_[1], volumeImageTarget_[2]};
    const VolumeCamera camera = volumeCameraFor(*brick_, volumeImageAzimuth_, volumeImageElevation_,
                                                volumeImageDistance_, target);
    const int w = width();
    const int h = height();
    const auto project = [&](double x, double y, double z, QPointF* out) {
        const double point[3] = {x, y, z};
        return projectVolumePoint(camera, point, w, h, out);
    };
    const Volume3dBox box{brick_->centerXMeters, brick_->centerYMeters,
                          brick_->halfExtentMeters};
    const double west = box.centerXMeters - box.halfExtentMeters;
    const double east = box.centerXMeters + box.halfExtentMeters;
    const double south = box.centerYMeters - box.halfExtentMeters;
    const double north = box.centerYMeters + box.halfExtentMeters;
    const auto inside = [&](double x, double y) {
        return x >= west - 1.0 && x <= east + 1.0 && y >= south - 1.0 && y <= north + 1.0;
    };
    const auto segment = [&](double x0, double y0, double x1, double y1) {
        if (!inside(x0, y0) || !inside(x1, y1)) return;
        QPointF a, b;
        if (project(x0, y0, 0.0, &a) && project(x1, y1, 0.0, &b)) painter.drawLine(a, b);
    };
    painter.setPen(QPen(QColor(92, 112, 122, 150), 1.0));
    // One grid step per sixth of the box: 50 km over the full range (the grid
    // this view always had) down to 10 km around a storm, never finer.
    const double gridStep = std::max(10000.0, box.widthMeters() / 6.0);
    for (double line = std::ceil(west / gridStep) * gridStep; line <= east; line += gridStep)
        segment(line, south, line, north);
    for (double line = std::ceil(south / gridStep) * gridStep; line <= north; line += gridStep)
        segment(west, line, east, line);
    // Range rings around the site: only the arcs that fall inside the box.
    painter.setPen(QPen(QColor(150, 175, 188, 170), 1.0));
    for (const double radius : {50000.0, 100000.0, 150000.0, 200000.0, 250000.0}) {
        QPointF previous;
        bool havePrevious = false;
        for (int step = 0; step <= 144; ++step) {
            const double angle = static_cast<double>(step) / 144.0 * 2.0 * kPi;
            const double x = radius * std::sin(angle);
            const double y = radius * std::cos(angle);
            if (!inside(x, y)) {
                havePrevious = false;
                continue;
            }
            QPointF point;
            if (!project(x, y, 0.0, &point)) {
                havePrevious = false;
                continue;
            }
            if (havePrevious) painter.drawLine(previous, point);
            previous = point;
            havePrevious = true;
        }
    }
    // The brick's own box as a wireframe: ground rectangle, top rectangle, four
    // risers. This is the volume on screen; during a drag the dashed one is the
    // box that will be built on release, so nothing is guessed at.
    const auto drawWire = [&](const Volume3dBox& wire, const QColor& color,
                              const Qt::PenStyle style) {
        const double xs[2] = {wire.centerXMeters - wire.halfExtentMeters,
                              wire.centerXMeters + wire.halfExtentMeters};
        const double ys[2] = {wire.centerYMeters - wire.halfExtentMeters,
                              wire.centerYMeters + wire.halfExtentMeters};
        painter.setPen(QPen(color, 1.3, style));
        const auto rectangle = [&](const double z) {
            for (int i = 0; i < 2; ++i) {
                QPointF a, b, c, d;
                if (project(xs[0], ys[i], z, &a) && project(xs[1], ys[i], z, &b))
                    painter.drawLine(a, b);
                if (project(xs[i], ys[0], z, &c) && project(xs[i], ys[1], z, &d))
                    painter.drawLine(c, d);
            }
        };
        rectangle(0.0);
        rectangle(brick_->topMeters);
        for (int ix = 0; ix < 2; ++ix) {
            for (int iy = 0; iy < 2; ++iy) {
                QPointF base, apex;
                if (project(xs[ix], ys[iy], 0.0, &base) &&
                    project(xs[ix], ys[iy], brick_->topMeters, &apex))
                    painter.drawLine(base, apex);
            }
        }
    };
    drawWire(box, QColor(255, 214, 140, 150), Qt::SolidLine);
    if (volumeBoxDragActive_ && volumeBoxDragPreviewValid_)
        drawWire(volumeBoxDragPreview_, QColor(255, 240, 200, 235), Qt::DashLine);
    // Site marker + height axis.
    QPointF site;
    if (project(0.0, 0.0, 0.0, &site)) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255, 214, 140));
        painter.drawEllipse(site, 5.0, 5.0);
        painter.setPen(QColor(255, 214, 140));
        painter.setFont(hudFont(8, QFont::DemiBold));
        painter.drawText(QRectF(site.x() + 9.0, site.y() - 18.0, 120.0, 18.0),
                         Qt::AlignLeft | Qt::AlignVCenter, selectedRadar_);
        // Height axis out of the site: the one vertical cue in a pancake
        // volume (300 km wide, 20 km tall).
        QPointF axisTopPoint;
        const double axisTop[3] = {0.0, 0.0, brick_->topMeters};
        if (projectVolumePoint(camera, axisTop, w, h, &axisTopPoint)) {
            painter.setPen(QPen(QColor(255, 214, 140, 170), 1.0));
            painter.drawLine(site, axisTopPoint);
            painter.setPen(QColor(255, 214, 140, 200));
            painter.setFont(hudFont(7, QFont::DemiBold));
            painter.drawText(QRectF(axisTopPoint.x() + 6.0, axisTopPoint.y() - 16.0, 80.0, 16.0),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             QStringLiteral("%1 km").arg(brick_->topMeters / 1000.0, 0, 'f', 0));
        }
    }
    // North label rides the box's own north edge, not a fixed 168 km.
    QPointF northLabel;
    if (project(box.centerXMeters, north + 8000.0, 0.0, &northLabel)) {
        painter.setPen(QColor(200, 215, 220));
        painter.setFont(hudFont(10, QFont::DemiBold));
        painter.drawText(QRectF(northLabel.x() - 20.0, northLabel.y() - 12.0, 40.0, 24.0),
                         Qt::AlignCenter, QStringLiteral("N"));
    }
}

void RadarMapCanvas::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    if (volume3d_) {
        paintVolume3d(painter);
        return;
    }
    painter.fillRect(rect(), QColor(18, 24, 28));
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const auto world = static_cast<double>(kTileSize * (1 << zoom_));
    const auto center = worldPoint(center_.y(), center_.x());
    const auto left = center.x() - width() / 2.0;
    const auto top = center.y() - height() / 2.0;
    const auto firstX = static_cast<int>(std::floor(left / kTileSize));
    const auto lastX = static_cast<int>(std::floor((left + width()) / kTileSize));
    const auto firstY = std::max(0, static_cast<int>(std::floor(top / kTileSize)));
    const auto lastY = std::min((1 << zoom_) - 1,
                                static_cast<int>(std::floor((top + height()) / kTileSize)));
    const auto tileCount = 1 << zoom_;
    const auto& store = tileStore();
    for (int tileY = firstY; tileY <= lastY; ++tileY) {
        for (int tileX = firstX; tileX <= lastX; ++tileX) {
            const auto key = tileKey(wrappedTileX(tileX, tileCount), tileY);
            const QRectF target(tileX * kTileSize - left, tileY * kTileSize - top, kTileSize, kTileSize);
            if (const auto* tile = store.tiles.object(key)) {
                painter.drawImage(target, *tile);
            } else {
                painter.fillRect(target, QColor(28, 36, 41));
                painter.setPen(QColor(47, 59, 66));
                painter.drawRect(target);
            }
        }
    }
    requestVisibleTiles();
    if (warningsOnTop_) {
        paintSpcOutlook(painter);
        paintSpcFireOutlook(painter);
        paintWpcOutlook(painter);
        paintMosaic(painter);
        paintHrrr(painter);
        paintGoes(painter);
        paintRadarOverlay(painter);
        paintWeatherOverlays(painter);
    } else {
        paintSpcOutlook(painter);
        paintSpcFireOutlook(painter);
        paintWpcOutlook(painter);
        paintMosaic(painter);
        paintHrrr(painter);
        paintGoes(painter);
        paintWeatherOverlays(painter);
        paintRadarOverlay(painter);
    }
    paintMcdOverlays(painter);
    paintNhcOverlays(painter);
    paintHmsSmoke(painter);
    paintHmsFires(painter);
    paintDatPaths(painter);
    paintMetars(painter);
    paintPlacefiles(painter);
    paintLightningOverlay(painter);

    painter.setFont(hudFont(8, QFont::DemiBold));
    QSet<QString> occupiedMarkerCells;
    ensureSiteWorldPoints();
    for (std::size_t siteIndex = 0; siteIndex < sites_.size(); ++siteIndex) {
        const auto& site = sites_[siteIndex];
        if (!site.latitude || !site.longitude) continue;
        const auto point = siteIndex < static_cast<std::size_t>(siteWorldPoints_.size())
                               ? screenPointFromWorld(siteWorldPoints_.at(static_cast<qsizetype>(siteIndex)))
                               : screenPoint(*site.latitude, *site.longitude);
        if (!rect().adjusted(-50, -30, 50, 30).contains(point.toPoint())) continue;
        const auto id = QString::fromStdString(site.id);
        const auto isTdwr = site.owner == "TDWR";
        const auto selected = id == selectedRadar_;
        if (zoom_ <= 4 && !selected) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(isTdwr ? QColor(188, 105, 223, 205) : QColor(40, 71, 82, 210));
            painter.drawEllipse(point, hoverRadarId_ == id ? 5.0 : 3.0, hoverRadarId_ == id ? 5.0 : 3.0);
            continue;
        }
        if (zoom_ == 5 && !selected && hoverRadarId_ != id) {
            const auto cell = QStringLiteral("%1/%2")
                                  .arg(static_cast<int>(point.x()) / 54)
                                  .arg(static_cast<int>(point.y()) / 25);
            if (occupiedMarkerCells.contains(cell)) continue;
            occupiedMarkerCells.insert(cell);
        }
        const QRectF pill(point.x() - 24.0, point.y() - 11.0, 48.0, 22.0);
        painter.setPen(QPen(selected ? QColor(128, 211, 255)
                                                 : isTdwr ? QColor(223, 177, 255)
                                                          : QColor(210, 220, 224),
                            selected ? 2.0 : 1.0));
        painter.setBrush(selected ? QColor(16, 74, 101, 235)
                                               : isTdwr ? QColor(68, 32, 82, 225)
                                                        : QColor(15, 27, 34, 225));
        painter.drawRoundedRect(pill, 5, 5);
        painter.setPen(selected ? QColor(235, 248, 255) : QColor(230, 235, 238));
        painter.drawText(pill, Qt::AlignCenter, id);
    }

    // Bottom chrome: the ray/quality readout on the left, attribution on the
    // right. The two used to overlap on any panel narrower than 772 px, which
    // is every panel in a 2x2 layout on a 1366x768 or 1440x900 screen. The
    // readout wins the space; the attribution yields when it cannot fit, and
    // both elide rather than hard-cut.
    const double attributionWidth = 238.0;
    const double chromeLeft = 12.0;
    const double chromeRight = width() - 12.0;
    const double statusWidth =
        std::clamp(chromeRight - chromeLeft - attributionWidth - 12.0, 0.0, 510.0);

    if (chunk_ && !chunk_->rays.empty() && statusWidth >= 90.0) {
        painter.setFont(hudFont(9));
        painter.setPen(QColor(232, 238, 240));
        painter.setBrush(QColor(8, 15, 19, 205));
        const QRectF statusPlate(chromeLeft, height() - 30.0, statusWidth, 22.0);
        painter.drawRoundedRect(statusPlate, 4, 4);
        const QRectF statusText(statusPlate.left() + 8.0, statusPlate.top() + 3.0,
                                statusPlate.width() - 16.0, 16.0);
        painter.drawText(statusText, Qt::AlignLeft | Qt::AlignVCenter,
                         painter.fontMetrics().elidedText(elidedStatusText(), Qt::ElideRight,
                                                          static_cast<int>(statusText.width())));
    }

    const double attributionLeft = chromeRight - attributionWidth;
    const bool attributionFits = attributionLeft >= chromeLeft + statusWidth + 8.0;
    if (attributionFits) {
        const auto drawAttributionPlate = [&](const double y, const QString& label) {
            const QRectF plate(attributionLeft, y, attributionWidth, 18.0);
            painter.setPen(QPen(QColor(205, 216, 222, 130), 1));
            painter.setBrush(QColor(8, 15, 19, 225));
            painter.drawRoundedRect(plate, 3, 3);
            painter.setPen(QColor(146, 152, 160));
            painter.setFont(hudFont(7, QFont::DemiBold));
            const QRectF text(plate.left() + 8.0, plate.top(), plate.width() - 16.0, plate.height());
            painter.drawText(text, Qt::AlignRight | Qt::AlignVCenter,
                             painter.fontMetrics().elidedText(label, Qt::ElideRight,
                                                              static_cast<int>(text.width())));
        };
        drawAttributionPlate(height() - 25.0, basemapAttribution());
        if (!supplementalAttribution_.isEmpty())
            drawAttributionPlate(height() - 46.0, supplementalAttribution_);
    }

    paintLegend(painter);
    paintScaleBar(painter);
    // The loading chip used to paint last, on top of the pinned detail card, so
    // an alert you had just clicked vanished behind a spinner for the whole
    // download. Draw it under the cards instead.
    paintLoadingChip(painter);
    paintProbe(painter);
    paintWarningHighlight(painter);
    paintLinkedProbe(painter);
    paintMeasurement(painter);
    paintStationCard(painter);

    if (activePanel_) {
        auto border = activeBorderColor_;
        border.setAlpha(220);
        painter.setPen(QPen(border, 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(rect().adjusted(1, 1, -2, -2));
    }
}

void RadarMapCanvas::mousePressEvent(QMouseEvent* event) {
    emit activated();
    if (volume3d_) {
        // 3D preview owns the mouse: left-drag orbits, middle-drag moves the
        // focus point (like the 2D middle-button pan), Alt+left-drag boxes a
        // new brick region, everything else falls through to the context menu.
        // Map tools stay parked while in 3D.
        if (event->button() == Qt::LeftButton && event->modifiers().testFlag(Qt::AltModifier)) {
            // Not an orbit: nothing re-renders until release, and the dashed
            // wireframe under the cursor is the box that will be built.
            volumeBoxDragActive_ = true;
            volumeBoxDragStart_ = event->pos();
            volumeBoxDragEnd_ = event->pos();
            volumeBoxDragPreviewValid_ = false;
            volumeDragging_ = false;
            volumePanDragging_ = false;
            setCursor(Qt::CrossCursor);
            event->accept();
            return;
        }
        if (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton) {
            volumeDragging_ = true;
            volumePanDragging_ = (event->button() == Qt::MiddleButton);
            lastMousePosition_ = event->pos();
            setCursor(Qt::ClosedHandCursor);
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
        return;
    }
    if (event->button() == Qt::LeftButton) {
        if (!pinnedInfo_.isEmpty() && pinnedInfoCardRect_.contains(event->position())) {
            const QRectF closeRect(pinnedInfoCardRect_.right() - 34.0,
                                   pinnedInfoCardRect_.top() + 7.0, 25.0, 25.0);
            if (closeRect.contains(event->position())) {
                setPinnedOverlay({}, event->position());
            } else if (event->position().y() <= pinnedInfoCardRect_.top() + 38.0) {
                draggingPinnedInfoCard_ = true;
                pinnedInfoDragOffset_ = event->position() - pinnedInfoCardRect_.topLeft();
                setCursor(Qt::ClosedHandCursor);
            }
            event->accept();
            return;
        }
        if (measureMode_ || crossSectionMode_) {
            measureStart_ = geographicPoint(event->position());
            measureEnd_ = measureStart_;
            event->accept();
            update();
            return;
        }
        dragging_ = true;
        dragged_ = false;
        dragPushedHistory_ = false;
        lastMousePosition_ = event->pos();
        event->accept();
        return;
    }
    // Middle-button pan: the near-universal map gesture, and the only one that
    // works without risking a click on a warning polygon.
    if (event->button() == Qt::MiddleButton) {
        dragging_ = true;
        dragged_ = true; // never treat a middle drag as a click
        dragPushedHistory_ = false;
        lastMousePosition_ = event->pos();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void RadarMapCanvas::mouseMoveEvent(QMouseEvent* event) {
    if (volume3d_) {
        if (volumeBoxDragActive_) {
            volumeBoxDragEnd_ = event->pos();
            bool mapped = false;
            volumeBoxDragPreview_ =
                volumeBoxFromDrag(volumeBoxDragStart_, volumeBoxDragEnd_, &mapped);
            volumeBoxDragPreviewValid_ = mapped;
            update(); // wireframe only: the brick is untouched until release
            event->accept();
            return;
        }
        if (volumeDragging_) {
            const auto delta = event->pos() - lastMousePosition_;
            if (!delta.isNull()) {
                if (volumePanDragging_ && brick_) {
                    // Slide the focus point in the view plane. Scale comes
                    // from the frustum height at the focus distance.
                    const VolumeCamera camera = volumeCameraFor(*brick_, volumeOrbitAzimuth_,
                                                                volumeOrbitElevation_,
                                                                volumeDistance_, volumeTarget_);
                    const double worldPerPixel =
                        (volumeDistance_ * 2.0 * std::tan(22.5 * kPi / 180.0)) /
                        std::max(1, height());
                    for (int axis = 0; axis < 3; ++axis) {
                        volumeTarget_[axis] +=
                            (-static_cast<double>(delta.x()) * camera.right[axis] +
                             static_cast<double>(delta.y()) * camera.up[axis]) *
                            worldPerPixel;
                    }
                    volumeTarget_[0] = std::clamp(volumeTarget_[0], -brick_->halfExtentMeters,
                                                  brick_->halfExtentMeters);
                    volumeTarget_[1] = std::clamp(volumeTarget_[1], -brick_->halfExtentMeters,
                                                  brick_->halfExtentMeters);
                    volumeTarget_[2] = std::clamp(volumeTarget_[2], 0.0, brick_->topMeters);
                } else {
                    volumeOrbitAzimuth_ += static_cast<double>(delta.x()) * 0.3;
                    if (volumeOrbitAzimuth_ >= 360.0) volumeOrbitAzimuth_ -= 360.0;
                    if (volumeOrbitAzimuth_ < 0.0) volumeOrbitAzimuth_ += 360.0;
                    volumeOrbitElevation_ =
                        std::clamp(volumeOrbitElevation_ - static_cast<double>(delta.y()) * 0.2,
                                   4.0, 85.0);
                }
                lastMousePosition_ = event->pos();
                rebuildVolumeRender(true); // fast pass while moving; release refines
            }
            event->accept();
            return;
        }
        event->accept();
        return;
    }
    if (draggingPinnedInfoCard_) {
        pinnedInfoCardPosition_ = event->position() - pinnedInfoDragOffset_;
        pinnedInfoCardPosition_.setX(std::clamp(
            pinnedInfoCardPosition_.x(), 8.0,
            std::max(8.0, width() - pinnedInfoCardRect_.width() - 8.0)));
        pinnedInfoCardPosition_.setY(std::clamp(
            pinnedInfoCardPosition_.y(), 8.0,
            std::max(8.0, height() - pinnedInfoCardRect_.height() - 8.0)));
        update();
        event->accept();
        return;
    }
    // Hover feedback. unsetCursor() falls back to the *parent's* cursor, not to
    // the one setMeasureMode/setCrossSectionMode installed, so the crosshair
    // used to disappear for the rest of the session after a single card drag.
    const auto toolCursor = (measureMode_ || crossSectionMode_) ? Qt::CrossCursor
                                                               : Qt::ArrowCursor;
    const auto overCardHeader = !pinnedInfo_.isEmpty() &&
                                pinnedInfoCardRect_.contains(event->position()) &&
                                event->position().y() <= pinnedInfoCardRect_.top() + 38.0;
    if (overCardHeader && event->buttons() == Qt::NoButton) {
        setCursor(Qt::OpenHandCursor);
    } else if (!dragging_ && !measureMode_ && !crossSectionMode_ &&
               !radarAt(event->position()).isEmpty()) {
        // Stations are the most-clicked thing on the map and nothing on screen
        // said so. Point at them.
        setCursor(Qt::PointingHandCursor);
    } else if (cursor().shape() != toolCursor) {
        setCursor(toolCursor);
    }
    if ((measureMode_ || crossSectionMode_) && measureStart_.has_value()) {
        measureEnd_ = geographicPoint(event->position());
        update();
        event->accept();
        return;
    }
    if (dragging_) {
        const auto delta = event->pos() - lastMousePosition_;
        if (!delta.isNull()) {
            dragged_ = true;
            // One undo entry per drag, recording the pre-gesture view:
            // push lazily on the first real move so a click pushes nothing.
            if (!dragPushedHistory_) {
                pushViewHistory();
                dragPushedHistory_ = true;
            }
            panBy(delta);
            lastMousePosition_ = event->pos();
        }
        event->accept();
        return;
    }
    pendingProbePosition_ = event->position();
    if (!probeTimer_->isActive()) probeTimer_->start();
    event->accept();
    return;
}

void RadarMapCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if (volume3d_) {
        if (volumeBoxDragActive_ && event->button() == Qt::LeftButton) {
            volumeBoxDragActive_ = false;
            volumeBoxDragPreviewValid_ = false;
            setCursor(Qt::ArrowCursor);
            // Release is the only moment the brick is rebuilt: the drag is
            // geometry, not a live render.
            applyVolume3dBoxDrag(volumeBoxDragStart_, event->pos());
            update();
            event->accept();
            return;
        }
        if ((event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton) &&
            volumeDragging_) {
            volumeDragging_ = false;
            volumePanDragging_ = false;
            setCursor(Qt::ArrowCursor);
            rebuildVolumeRender(); // refine at full preview resolution
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
        return;
    }
    if (event->button() == Qt::LeftButton && draggingPinnedInfoCard_) {
        draggingPinnedInfoCard_ = false;
        setCursor((measureMode_ || crossSectionMode_) ? Qt::CrossCursor : Qt::ArrowCursor);
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && (measureMode_ || crossSectionMode_) && measureStart_.has_value()) {
        measureEnd_ = geographicPoint(event->position());
        if (crossSectionMode_) emit crossSectionRequested(*measureStart_, *measureEnd_);
        update();
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && dragging_) {
        dragging_ = false;
        if (!dragged_) {
            const auto radarId = radarAt(event->pos());
            if (!radarId.isEmpty()) {
                setPinnedOverlay({}, event->position());
                emit radarClicked(radarId);
            } else {
                setPinnedOverlay(overlayAt(event->position()), event->position());
            }
        }
        event->accept();
        return;
    }
    if (event->button() == Qt::MiddleButton && dragging_) {
        dragging_ = false;
        dragged_ = false;
        setCursor((measureMode_ || crossSectionMode_) ? Qt::CrossCursor : Qt::ArrowCursor);
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

bool RadarMapCanvas::event(QEvent* event) {
    if (event->type() == QEvent::Gesture) {
        if (handleGestureEvent(static_cast<QGestureEvent*>(event))) return true;
    } else if (event->type() == QEvent::NativeGesture) {
        if (handleNativeGestureEvent(static_cast<QNativeGestureEvent*>(event))) return true;
    }
    return QWidget::event(event);
}

bool RadarMapCanvas::handleGestureEvent(QGestureEvent* gesture) {
    auto* pinch = static_cast<QPinchGesture*>(gesture->gesture(Qt::PinchGesture));
    if (pinch == nullptr) return false;
    // Touchscreen pinch: accumulate the multiplicative scale and step the
    // integer zoom each 1.25x around the pinch center, reusing the
    // cursor-anchored path. One undo entry per pinch via the lazy push
    // below, mirroring the drag gesture boundary.
    switch (pinch->state()) {
        case Qt::GestureStarted:
            pinchBaseZoom_ = zoom_;
            pinchAccum_ = 1.0;
            pinchPushedHistory_ = false;
            break;
        case Qt::GestureUpdated:
            pinchAccum_ *= pinch->scaleFactor();
            if (pinchAccum_ >= 1.25 || pinchAccum_ <= 0.8) {
                const int steps = pinchAccum_ >= 1.0
                    ? static_cast<int>(std::floor(std::log(pinchAccum_) / std::log(1.25)))
                    : -static_cast<int>(std::floor(std::log(1.0 / pinchAccum_) / std::log(1.25)));
                pinchBaseZoom_ = std::clamp(pinchBaseZoom_ + steps, kMinZoom, kMaxZoom);
                pinchAccum_ = 1.0;
                // Clamped at a limit: nothing to apply, so bank nothing.
                if (pinchBaseZoom_ == zoom_) {
                    updateProbe(pinch->centerPoint());
                    break;
                }
                // One undo entry per pinch, recording the pre-gesture view
                // on the first applied step (a motionless pinch banks
                // nothing, mirroring the drag behavior).
                if (!pinchPushedHistory_) {
                    pushViewHistory();
                    pinchPushedHistory_ = true;
                }
                setZoomAt(pinchBaseZoom_, pinch->centerPoint());
                updateProbe(pinch->centerPoint());
            }
            break;
        case Qt::GestureFinished:
        case Qt::GestureCanceled:
            pinchAccum_ = 1.0;
            break;
        default:
            break;
    }
    gesture->accept(pinch);
    return true;
}

bool RadarMapCanvas::handleNativeGestureEvent(QNativeGestureEvent* gesture) {
    // Precision-trackpad pinch arrives as a native Zoom gesture (scale in
    // value()), not as wheel ticks. Same 1.25x-per-step accumulation. No
    // begin/end exists here, so each applied step is its own undo entry
    // like a classic wheel notch.
    if (gesture->gestureType() != Qt::ZoomNativeGesture || gesture->value() <= 0.0) return false;
    pinchAccum_ *= static_cast<double>(gesture->value());
    if (pinchAccum_ >= 1.25 || pinchAccum_ <= 0.8) {
        const int steps = pinchAccum_ >= 1.0
            ? static_cast<int>(std::floor(std::log(pinchAccum_) / std::log(1.25)))
            : -static_cast<int>(std::floor(std::log(1.0 / pinchAccum_) / std::log(1.25)));
        pinchAccum_ = 1.0;
        const auto target = std::clamp(zoom_ + steps, kMinZoom, kMaxZoom);
        if (target == zoom_) {
            gesture->accept();
            return true;
        }
        pushViewHistory();
        setZoomAt(target, gesture->position());
        updateProbe(gesture->position());
    }
    gesture->accept();
    return true;
}

void RadarMapCanvas::noteWheelNavigated(const bool discrete) {
    // One undo entry per wheel/trackpad gesture, recording pre-gesture.
    // Discrete notches stay single-shot; a continuous gesture shares the
    // entry opened by its first applied step, and momentum arriving after
    // ScrollEnd opens a fresh one.
    if (wheelGestureActive_) return;
    pushViewHistory();
    wheelGestureActive_ = !discrete;
}

void RadarMapCanvas::wheelEvent(QWheelEvent* event) {
    if (volume3d_) {
        // Dolly the orbit camera. Same fractional-delta normalization as the
        // map zoom so precision trackpads work here too.
        const auto angleDelta = static_cast<double>(event->angleDelta().y());
        const auto pixelDelta = static_cast<double>(event->pixelDelta().y());
        double steps = angleDelta / 120.0;
        if (steps == 0.0) {
            if (pixelDelta == 0.0) return;
            steps = pixelDelta / 40.0;
        }
        const auto delta = std::clamp(steps, -4.0, 4.0);
        if (delta == 0.0) return;
        volumeDistance_ = std::clamp(volumeDistance_ * std::pow(1.12, -delta), 60000.0, 1200000.0);
        rebuildVolumeRender();
        event->accept();
        return;
    }
    // Wheel/trackpad gesture boundary for view history: ScrollEnd closes
    // the gesture opened by the first applied step, so one continuous
    // gesture is one undo entry. Classic discrete notches (NoScrollPhase)
    // never set the flag and stay single-shot via noteWheelNavigated.
    if (event->phase() == Qt::ScrollEnd) {
        wheelGestureActive_ = false;
        event->accept();
        return;
    }
    if (!pinnedInfo_.isEmpty() && pinnedInfoCardRect_.contains(event->position()) &&
        hasScrollablePinnedCard()) {
        const auto pixelDelta = event->pixelDelta().y();
        const auto scrollDelta = pixelDelta != 0
            ? static_cast<double>(pixelDelta)
            : static_cast<double>(event->angleDelta().y()) * 0.45;
        if (scrollDelta == 0.0) {
            event->accept();
            return;
        }
        scrollPinnedCard(static_cast<int>(scrollDelta));
        event->accept();
        return;
    }
    // Dominant-horizontal trackpad scroll pans instead of zooming: vertical
    // stays on the zoom path below, so existing wheel behavior is untouched.
    const double panX = event->pixelDelta().x() != 0
        ? static_cast<double>(event->pixelDelta().x())
        : static_cast<double>(event->angleDelta().x()) / 8.0;
    const double panY = event->pixelDelta().y() != 0
        ? static_cast<double>(event->pixelDelta().y())
        : static_cast<double>(event->angleDelta().y()) / 8.0;
    if (panX != 0.0 && std::abs(panX) >= std::abs(panY)) {
        // Same sign convention as mouse drag (panBy takes last - pos), so a
        // swipe moves the map with the fingers like a drag does. One undo
        // entry per gesture via the same lazy boundary as zoom.
        noteWheelNavigated(event->phase() == Qt::NoScrollPhase);
        panBy(QPointF(-panX, 0.0));
        event->accept();
        return;
    }
    // Sub-notch wheel/trackpad input accumulates until a whole zoom step
    // is owed; truncating each event (the old behavior) dropped every
    // fractional gesture to zero. Remainder clears at the zoom limits so
    // reversing direction always responds on the next gesture.
    const auto angleDelta = static_cast<double>(event->angleDelta().y());
    const auto pixelDelta = static_cast<double>(event->pixelDelta().y());
    double steps = angleDelta / 120.0;
    if (steps == 0.0) {
        if (pixelDelta == 0.0) {
            event->accept();
            return;
        }
        steps = pixelDelta / 40.0;
    }
    wheelZoomRemainder_ += std::clamp(steps, -4.0, 4.0);
    // Bound overscroll can never apply, so drop it at once: at max zoom
    // keep only inward (negative) remainder, at min only outward. Without
    // this, fractions banked against a bound swallow the reversing
    // gesture (e.g. seven +15s at max then a full -120 notch zooms
    // nowhere). Ordinary accumulation away from bounds is untouched.
    if (zoom_ == kMaxZoom)
        wheelZoomRemainder_ = std::min(wheelZoomRemainder_, 0.0);
    else if (zoom_ == kMinZoom)
        wheelZoomRemainder_ = std::max(wheelZoomRemainder_, 0.0);
    const auto whole = static_cast<int>(std::trunc(wheelZoomRemainder_));
    if (whole == 0) {
        updateProbe(event->position());
        event->accept();
        return;
    }
    const auto target = std::clamp(zoom_ + whole, kMinZoom, kMaxZoom);
    if (target == zoom_) {
        // Pushing past a limit: drop the overscroll instead of banking it.
        wheelZoomRemainder_ = 0.0;
        updateProbe(event->position());
        event->accept();
        return;
    }
    wheelZoomRemainder_ -= whole;
    // Landing exactly on a bound consumes the leftover fraction with it:
    // it lies beyond the limit just like banked overscroll, so the first
    // inward notch always responds.
    if (target == kMaxZoom || target == kMinZoom) wheelZoomRemainder_ = 0.0;
    // Gesture-scoped history: the pre-step view is one undo entry per
    // gesture (classic notches single-shot, trackpad gestures shared).
    noteWheelNavigated(event->phase() == Qt::NoScrollPhase);
    setZoomAt(target, event->position());
    updateProbe(event->position());
    event->accept();
}

void RadarMapCanvas::setZoomAt(const int zoom, const QPointF& cursor) {
    const auto oldWorld = static_cast<double>(kTileSize * (1 << zoom_));
    const auto oldCenter = worldPoint(center_.y(), center_.x());
    const auto cursorWorld = oldCenter + cursor - QPointF(width() / 2.0, height() / 2.0);
    zoom_ = std::clamp(zoom, kMinZoom, kMaxZoom);
    const auto newWorld = static_cast<double>(kTileSize * (1 << zoom_));
    const auto factor = newWorld / oldWorld;
    const auto newCenter = cursorWorld * factor - (cursor - QPointF(width() / 2.0, height() / 2.0));
    const auto longitude = newCenter.x() / newWorld * 360.0 - 180.0;
    const auto n = kPi - 2.0 * kPi * newCenter.y() / newWorld;
    center_ = {longitude, qRadiansToDegrees(std::atan(std::sinh(n)))};
    invalidateWorldCaches(true);
    invalidateOverlay(true);
    update();
    emit viewChanged(center_, zoom_);
}

void RadarMapCanvas::enterEvent(QEnterEvent* event) {
    cursorInside_ = true;
    QWidget::enterEvent(event);
}

void RadarMapCanvas::leaveEvent(QEvent* event) {
    cursorInside_ = false;
    hoverRadarId_.clear();
    probeTimer_->stop();  // a debounced hover must not fire after the cursor left
    if (pinnedInfo_.isEmpty()) probeText_.clear();
    emit probeHidden();
    update();
    QWidget::leaveEvent(event);
}

void RadarMapCanvas::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    rebuildProbeIndex();
    refreshScanDiagnostics();
    if (chunk_ && product_ == ProductKind::Velocity &&
        velocityOptions_.mode != VelocityProcessingMode::Native && !processedVelocity_)
        refreshVelocityProcessing();
    update();
}

void RadarMapCanvas::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    // The 3D preview marches at half the panel size, so a resize invalidates
    // the cached image. 2D overlays handle resizes through their own caches.
    if (volume3d_) rebuildVolumeRender();
}

void RadarMapCanvas::contextMenuEvent(QContextMenuEvent* event) {
    if (volume3d_) {
        QMenu menu(this);
        const auto resetBox = menu.addAction(tr("Reset 3D box to full range"));
        resetBox->setEnabled(!volumeBox_.fullRange());
        connect(resetBox, &QAction::triggered, this, [this] { resetVolume3dBox(); });
        const auto resetView = menu.addAction(tr("Reset 3D view"));
        connect(resetView, &QAction::triggered, this, [this] {
            volumeOrbitAzimuth_ = 180.0;
            volumeOrbitElevation_ = 40.0;
            // Keep the current box and frame it: resetting the angle must not
            // silently zoom a storm box back out to the full range.
            fitVolumeCameraToBox();
            rebuildVolumeRender();
        });
        const auto backTo2d = menu.addAction(tr("Back to 2D map"));
        connect(backTo2d, &QAction::triggered, this, [this] { setVolume3dMode(false); });
        menu.exec(event->globalPos());
        event->accept();
        return;
    }
    // Translate the right-click into a canvas-space point and build a menu
    // tailored to whatever the cursor is over. Right-click on a station mark
    // offers a "Select this radar" path; right-click on the map offers
    // copy-lat/lon, recenter, zoom, and tool toggles.
    const auto point = event->pos();
    const auto geographic = geographicPoint(point);
    const auto radarId = radarAt(point);
    QMenu menu(this);

    if (!radarId.isEmpty()) {
        const auto selectAction = menu.addAction(tr("Select radar %1").arg(radarId));
        connect(selectAction, &QAction::triggered, this, [this, radarId] { emit radarClicked(radarId); });
        menu.addSeparator();
    }

    if (!selectedRadar_.isEmpty()) {
        const auto recenterAction = menu.addAction(tr("Recenter on %1").arg(selectedRadar_));
        connect(recenterAction, &QAction::triggered, this, [this] { recenterSelected(); });
    }
    const auto homeAction = menu.addAction(tr("National view"));
    connect(homeAction, &QAction::triggered, this, [this] { showNationalView(); });

    menu.addSeparator();
    const auto zoomInAction = menu.addAction(tr("Zoom in"));
    connect(zoomInAction, &QAction::triggered, this, [this] { zoomIn(); });
    const auto zoomOutAction = menu.addAction(tr("Zoom out"));
    connect(zoomOutAction, &QAction::triggered, this, [this] { zoomOut(); });

    menu.addSeparator();
    const auto copyLatLonAction = menu.addAction(tr("Copy lat/lon (%1, %2)")
                                                     .arg(geographic.y(), 0, 'f', 4)
                                                     .arg(geographic.x(), 0, 'f', 4));
    connect(copyLatLonAction, &QAction::triggered, this, [geographic] {
        const auto text = QStringLiteral("%1, %2")
                              .arg(geographic.y(), 0, 'f', 6)
                              .arg(geographic.x(), 0, 'f', 6);
        if (auto* clipboard = QGuiApplication::clipboard()) clipboard->setText(text);
    });
    const auto inspectAction = menu.addAction(tr("Inspect at cursor"));
    connect(inspectAction, &QAction::triggered, this, [this, point] { inspectAt(point); });

    // Escape hatches. The right-click menu is where users go to get out of a
    // state, but it previously offered no way to dismiss the detail card or
    // cancel a tool - both were only reachable from the toolbar or Esc.
    if (!pinnedInfo_.isEmpty()) {
        const auto clearCard = menu.addAction(pinnedInfoLocked_ ? tr("Close card (unlock and dismiss)")
                                                                : tr("Close card"));
        connect(clearCard, &QAction::triggered, this, [this, point] {
            setPinnedOverlayLocked(false);
            setPinnedOverlay({}, point);
        });
    }
    if (measureMode_ || crossSectionMode_ || measureStart_.has_value()) {
        const auto cancelTool = menu.addAction(measureMode_ ? tr("Cancel measure")
                                                             : tr("Cancel cross-section"));
        connect(cancelTool, &QAction::triggered, this, [this] {
            if (measureMode_) {
                setMeasureMode(false);
                emit measureModeToggled(false);
            } else if (crossSectionMode_) {
                setCrossSectionMode(false);
                emit crossSectionModeToggled(false);
            }
        });
    }

    menu.addSeparator();
    const auto volume3dAction = menu.addAction(tr("3D volume preview (CPU)"));
    volume3dAction->setCheckable(true);
    volume3dAction->setChecked(false);
    connect(volume3dAction, &QAction::toggled, this, [this](bool enabled) {
        setVolume3dMode(enabled);
    });
    const auto measureAction = menu.addAction(tr("Measure (two-click)"));
    measureAction->setCheckable(true);
    measureAction->setChecked(measureMode_);
    connect(measureAction, &QAction::toggled, this, [this](bool enabled) {
        if (crossSectionMode_) {
            setCrossSectionMode(false);
            emit crossSectionModeToggled(false);
        }
        setMeasureMode(enabled);
        emit measureModeToggled(enabled);
    });
    const auto xsAction = menu.addAction(tr("Cross-section (two-click)"));
    xsAction->setCheckable(true);
    xsAction->setChecked(crossSectionMode_);
    // Never leave the item checked-but-greyed: if the active product cannot
    // support a cross-section, clear the mode rather than showing a dead tick.
    if (isDerivedProduct(product_)) {
        if (crossSectionMode_) {
            setCrossSectionMode(false);
            emit crossSectionModeToggled(false);
        }
        xsAction->setChecked(false);
    }
    xsAction->setEnabled(!isDerivedProduct(product_));
    connect(xsAction, &QAction::toggled, this, [this](bool enabled) {
        if (measureMode_) {
            setMeasureMode(false);
            emit measureModeToggled(false);
        }
        setCrossSectionMode(enabled);
        emit crossSectionModeToggled(enabled);
    });

    menu.exec(event->globalPos());
    event->accept();
}

void RadarMapCanvas::pushViewHistory() {
    if (suppressHistory_) return;
    // Skip a no-op push (same view) so a click that doesn't pan/zoom doesn't
    // pollute the back stack.
    if (!viewBack_.isEmpty()) {
        const auto& last = viewBack_.constLast();
        if (qFuzzyCompare(last.center.x() + 1.0, center_.x() + 1.0) &&
            qFuzzyCompare(last.center.y() + 1.0, center_.y() + 1.0) &&
            last.zoom == zoom_) {
            return;
        }
    }
    viewBack_.push_back({center_, zoom_});
    while (viewBack_.size() > kViewHistoryLimit) viewBack_.removeFirst();
    viewForward_.clear();
}

void RadarMapCanvas::undoView() {
    if (viewBack_.isEmpty()) return;
    viewForward_.push_back({center_, zoom_});
    while (viewForward_.size() > kViewHistoryLimit) viewForward_.removeFirst();
    const auto snapshot = viewBack_.takeLast();
    suppressHistory_ = true;
    setView(snapshot.center, snapshot.zoom);
    suppressHistory_ = false;
}

void RadarMapCanvas::redoView() {
    if (viewForward_.isEmpty()) return;
    viewBack_.push_back({center_, zoom_});
    while (viewBack_.size() > kViewHistoryLimit) viewBack_.removeFirst();
    const auto snapshot = viewForward_.takeLast();
    suppressHistory_ = true;
    setView(snapshot.center, snapshot.zoom);
    suppressHistory_ = false;
}

void RadarMapCanvas::clearViewHistory() {
    viewBack_.clear();
    viewForward_.clear();
}

void RadarMapCanvas::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        if (volume3d_ && pinnedInfo_.isEmpty() && !measureMode_ && !crossSectionMode_) {
            setVolume3dMode(false);
            event->accept();
            return;
        }
        if (!pinnedInfo_.isEmpty()) {
            clearPinnedOverlay();
            event->accept();
            return;
        }
        if (measureMode_ || crossSectionMode_) {
            setMeasureMode(false);
            setCrossSectionMode(false);
            emit measureModeToggled(false);
            emit crossSectionModeToggled(false);
            event->accept();
            return;
        }
    }
    QWidget::keyPressEvent(event);
}

} // namespace cappedweather
