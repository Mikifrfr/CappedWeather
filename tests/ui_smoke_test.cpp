#include "app/main_window.hpp"
#include "app/radar_map_canvas.hpp"
#include "app/radar_list_model.hpp"
#include "app/weather_overlay_source.hpp"
#include "app/spc_convective.hpp"
#include "app/nhc_cyclone.hpp"
#include "app/wpc_ero.hpp"
#include "app/hms_smoke.hpp"
#include "app/dat_paths.hpp"
#include "app/sounding_source.hpp"
#include "app/hodograph_widget.hpp"
#include "app/hrrr_source.hpp"
#include "app/goes_source.hpp"
#include "app/mosaic_source.hpp"
#include "app/palette_import.hpp"
#include "app/update_check.hpp"
#include "app/gif_writer.hpp"
#include "app/placefile.hpp"
#include "app/placefile_source.hpp"
#include "app/archive_level2.hpp"
#include "app/archive_source.hpp"
#include "app/glm_source.hpp"
#include "app/nexrad_source.hpp"
#include "app/opera_source.hpp"
#include "app/shortcut_help_dialog.hpp"
#include "app/theme.hpp"
#include "app/session_store.hpp"
#include "app/session_prefetch.hpp"
#include "app/metar_source.hpp"
#include "app/case_pack.hpp"
#include "app/offline_pack.hpp"
#include "core/volume_spill.hpp"
#include "core/product.hpp"
#include "core/goes_frame.hpp"
#include "core/hrrr_frame.hpp"

#include "app/alert_center.hpp"
#include "core/lightning_overlay.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDockWidget>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QList>
#include <QPointF>
#include <QPair>
#include <QPolygonF>
#include <QSet>
#include <QDataStream>
#include <QSlider>
#include <QSpinBox>
#include <QProgressBar>
#include <QScrollArea>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStringList>
#include <QTabWidget>
#include <QTest>
#include <QTemporaryDir>
#include <QBuffer>
#include <QImage>
#include <QPixmap>
#include <QTimeZone>
#include <QToolButton>

#include <iostream>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <optional>
#include <vector>

#include <bzlib.h>

using namespace cappedweather;

// --- Hermetic P1 helpers (no network, offscreen-safe) ---
static QPolygonF smokeSquare(double lon, double lat, double halfDeg) {
    QPolygonF polygon;
    polygon << QPointF(lon - halfDeg, lat - halfDeg) << QPointF(lon + halfDeg, lat - halfDeg)
            << QPointF(lon + halfDeg, lat + halfDeg) << QPointF(lon - halfDeg, lat + halfDeg);
    return polygon;
}

static WeatherWarning smokeWarning(const QString& id, const QString& event, const QString& severity,
                                   const QString& headline, const QPolygonF& exterior = {},
                                   const QDateTime& onset = {}) {
    WeatherWarning warning;
    warning.id = id;
    warning.event = event;
    warning.headline = headline;
    warning.severity = severity;
    warning.senderName = QStringLiteral("NWS Test");
    warning.description = headline;
    warning.instruction = QStringLiteral("Take shelter now");
    // Default to a fresh onset so the age gate does not filter fixtures;
    // pass an explicit onset to exercise stale-warning behavior.
    warning.onset =
        onset.isValid() ? onset : QDateTime::currentDateTimeUtc().addSecs(-5 * 60);
    warning.expires = warning.onset.addSecs(60 * 60);
    if (!exterior.isEmpty()) {
        WarningPolygon polygon;
        polygon.exterior = exterior;
        warning.polygons.append(polygon);
    }
    return warning;
}

static LightningCellSnapshot smokeCell(double sigma, JumpEventKind kind, int cellX, int cellY,
                                       int satellite = 19) {
    LightningCellSnapshot cell;
    cell.kind = kind;
    cell.sigmaLevel = sigma;
    cell.flashRatePerMin = 28.0;
    cell.latitudeCenterDeg = 33.65;
    cell.longitudeCenterDeg = -83.85;
    cell.cellX = cellX;
    cell.cellY = cellY;
    cell.satelliteNumber = satellite;
    cell.eventTimeEpochSec = QDateTime::currentSecsSinceEpoch() - 45;
    cell.latestDataEpochSec = QDateTime::currentSecsSinceEpoch() - 12;
    return cell;
}

static AlertConfig smokeQuietConfig() {
    AlertConfig config;
    config.enabled = true;
    config.soundEnabled = false;
    config.systemNotificationEnabled = false;
    config.geographicFilterEnabled = false;
    config.eventFilterEnabled = false;
    return config;
}

static bool testAlertCenterDedupe(QString* error) {
    AlertCenter center;
    auto config = smokeQuietConfig();
    config.minSeverity = 0;
    config.repeatSuppressionMinutes = 30;
    center.setConfig(config);
    const auto first =
        smokeWarning(QStringLiteral("urn:oid:smoke-dedupe-1"), QStringLiteral("Tornado Warning"),
                     QStringLiteral("Severe"), QStringLiteral("Dedup headline A"));
    center.ingestWarnings({first});
    if (!center.recent().isEmpty()) {
        *error = QStringLiteral("dedupe: first sync should prime silently, got %1")
                     .arg(center.recent().size());
        return false;
    }
    center.ingestWarnings({first});
    if (!center.recent().isEmpty()) {
        *error = QStringLiteral("dedupe: re-ingest of same @id should stay silent, got %1")
                     .arg(center.recent().size());
        return false;
    }
    const auto second =
        smokeWarning(QStringLiteral("urn:oid:smoke-dedupe-2"), QStringLiteral("Tornado Warning"),
                     QStringLiteral("Severe"), QStringLiteral("Dedup headline B"));
    center.ingestWarnings({second});
    if (center.recent().size() != 1) {
        *error = QStringLiteral("dedupe: new @id should yield 1 alert, got %1")
                     .arg(center.recent().size());
        return false;
    }
    auto noSuppression = center.config();
    noSuppression.repeatSuppressionMinutes = 0;
    center.setConfig(noSuppression);
    center.ingestWarnings({first});
    if (center.recent().size() != 2) {
        *error = QStringLiteral("suppression window 0 should re-alert, got %1")
                     .arg(center.recent().size());
        return false;
    }
    return true;
}

static bool testAlertCenterSeverity(QString* error) {
    AlertCenter center;
    auto config = smokeQuietConfig();
    config.minSeverity = 2;  // Severe default: Minor < Moderate < Severe < Extreme
    config.repeatSuppressionMinutes = 30;
    center.setConfig(config);
    QVector<WeatherWarning> batch;
    batch.append(smokeWarning(QStringLiteral("smoke-sev-minor"), QStringLiteral("Tornado Warning"),
                              QStringLiteral("Minor"), QStringLiteral("sev-minor")));
    batch.append(smokeWarning(QStringLiteral("smoke-sev-moderate"), QStringLiteral("Tornado Warning"),
                              QStringLiteral("Moderate"), QStringLiteral("sev-moderate")));
    batch.append(smokeWarning(QStringLiteral("smoke-sev-severe"), QStringLiteral("Tornado Warning"),
                              QStringLiteral("Severe"), QStringLiteral("sev-severe")));
    batch.append(smokeWarning(QStringLiteral("smoke-sev-extreme"), QStringLiteral("Tornado Warning"),
                              QStringLiteral("Extreme"), QStringLiteral("sev-extreme")));
    batch.append(smokeWarning(QStringLiteral("smoke-sev-unknown"), QStringLiteral("Tornado Warning"),
                              QStringLiteral("Unknown"), QStringLiteral("sev-unknown")));
    center.ingestWarnings(batch);  // first sync primes silently
    if (!center.recent().isEmpty()) {
        *error = QStringLiteral("severity: first sync should prime silently, got %1")
                     .arg(center.recent().size());
        return false;
    }
    // Re-issue the same severities under fresh ids (as the NWS feed would for
    // genuinely new alerts) so dedupe does not mask the severity filter.
    QVector<WeatherWarning> fresh;
    for (const auto& warning : batch) {
        auto copy = warning;
        copy.id = warning.id + QStringLiteral("-fresh");
        fresh.append(copy);
    }
    center.ingestWarnings(fresh);
    if (center.recent().size() != 2) {
        *error = QStringLiteral("minSeverity=Severe should keep 2 (Severe+Extreme), got %1")
                     .arg(center.recent().size());
        return false;
    }
    bool sawSevere = false;
    bool sawExtreme = false;
    for (const auto& record : center.recent()) {
        if (record.detail.contains(QStringLiteral("sev-severe"))) sawSevere = true;
        if (record.detail.contains(QStringLiteral("sev-extreme"))) sawExtreme = true;
        if (record.detail.contains(QStringLiteral("sev-minor")) ||
            record.detail.contains(QStringLiteral("sev-moderate")) ||
            record.detail.contains(QStringLiteral("sev-unknown"))) {
            *error = QStringLiteral("minSeverity=Severe leaked %1").arg(record.detail);
            return false;
        }
    }
    if (!sawSevere || !sawExtreme) {
        *error = QStringLiteral("minSeverity=Severe missing Severe/Extreme alerts");
        return false;
    }
    config.minSeverity = 0;  // Minor: everything (incl. Unknown-as-Moderate) passes
    center.setConfig(config);
    center.clearMemory();
    QVector<WeatherWarning> minorBatch;
    for (const auto& warning : batch) {
        auto copy = warning;
        copy.id = warning.id + QStringLiteral("-minor");
        minorBatch.append(copy);
    }
    center.ingestWarnings(minorBatch);  // primes after clearMemory
    QVector<WeatherWarning> minorFresh;
    for (const auto& warning : minorBatch) {
        auto copy = warning;
        copy.id = warning.id + QStringLiteral("-fresh");
        minorFresh.append(copy);
    }
    center.ingestWarnings(minorFresh);
    if (center.recent().size() != 5) {
        *error = QStringLiteral("minSeverity=Minor should keep 5, got %1")
                     .arg(center.recent().size());
        return false;
    }
    config.minSeverity = 3;  // Extreme only
    center.setConfig(config);
    center.clearMemory();
    QVector<WeatherWarning> extremeBatch;
    for (const auto& warning : batch) {
        auto copy = warning;
        copy.id = warning.id + QStringLiteral("-extreme");
        extremeBatch.append(copy);
    }
    center.ingestWarnings(extremeBatch);  // primes after clearMemory
    QVector<WeatherWarning> extremeFresh;
    for (const auto& warning : extremeBatch) {
        auto copy = warning;
        copy.id = warning.id + QStringLiteral("-fresh");
        extremeFresh.append(copy);
    }
    center.ingestWarnings(extremeFresh);
    if (center.recent().size() != 1 ||
        !center.recent().constFirst().detail.contains(QStringLiteral("sev-extreme"))) {
        *error = QStringLiteral("minSeverity=Extreme should keep only Extreme, got %1")
                     .arg(center.recent().size());
        return false;
    }
    return true;
}

static bool testAlertCenterLightning(QString* error) {
    AlertCenter center;
    auto config = smokeQuietConfig();
    config.lightningSigmaThreshold = 2.0;
    config.repeatSuppressionMinutes = 30;
    center.setConfig(config);
    const auto low = smokeCell(1.0, JumpEventKind::JumpStart, 11, 21);
    const auto high = smokeCell(3.0, JumpEventKind::JumpStart, 12, 22);
    const auto ceased = smokeCell(5.0, JumpEventKind::JumpCessation, 13, 23);
    center.ingestLightningCells({low, high, ceased});  // first sync primes silently
    if (!center.recent().isEmpty()) {
        *error = QStringLiteral("lightning: first sync should prime silently, got %1")
                     .arg(center.recent().size());
        return false;
    }
    const auto highFresh = smokeCell(3.0, JumpEventKind::JumpStart, 112, 122);
    center.ingestLightningCells({highFresh});
    if (center.recent().size() != 1) {
        *error = QStringLiteral("sigma 2.0 should keep only the 3.0 active cell, got %1")
                     .arg(center.recent().size());
        return false;
    }
    if (!center.recent().constFirst().title.contains(QStringLiteral("3.0"))) {
        *error = QStringLiteral("lightning title should mention sigma 3.0, got %1")
                     .arg(center.recent().constFirst().title);
        return false;
    }
    center.ingestLightningCells({highFresh});
    if (center.recent().size() != 1) {
        *error = QStringLiteral("lightning dedupe should suppress repeat cell, got %1")
                     .arg(center.recent().size());
        return false;
    }
    const auto update = smokeCell(3.5, JumpEventKind::JumpUpdate, 14, 24);
    center.ingestLightningCells({update});
    if (center.recent().size() != 2) {
        *error = QStringLiteral("active JumpUpdate should also alert, got %1")
                     .arg(center.recent().size());
        return false;
    }
    config.lightningSigmaThreshold = 4.0;
    center.setConfig(config);
    center.clearMemory();
    center.ingestLightningCells({high});  // re-primes after clearMemory
    if (!center.recent().isEmpty()) {
        *error = QStringLiteral("sigma 4.0 should drop the 3.0 cell");
        return false;
    }
    config.lightningSigmaThreshold = 0.5;
    center.setConfig(config);
    center.clearMemory();
    center.ingestLightningCells({low, high, ceased});  // re-primes after clearMemory
    const auto lowFresh = smokeCell(1.0, JumpEventKind::JumpStart, 111, 121);
    const auto highFresh2 = smokeCell(3.0, JumpEventKind::JumpStart, 112, 123);
    const auto ceasedFresh = smokeCell(5.0, JumpEventKind::JumpCessation, 113, 124);
    center.ingestLightningCells({lowFresh, highFresh2, ceasedFresh});
    if (center.recent().size() != 2) {
        *error = QStringLiteral("sigma 0.5 should keep both active cells but drop cessation, got %1")
                     .arg(center.recent().size());
        return false;
    }
    return true;
}

static bool testAlertCenterPrimingAndAge(QString* error) {
    AlertCenter center;
    auto config = smokeQuietConfig();
    config.minSeverity = 0;
    config.repeatSuppressionMinutes = 30;
    config.maxWarningAgeMinutes = 60;
    center.setConfig(config);
    const auto now = QDateTime::currentDateTimeUtc();
    const auto oldWarning =
        smokeWarning(QStringLiteral("urn:oid:smoke-age-old"), QStringLiteral("Flash Flood Warning"),
                     QStringLiteral("Severe"), QStringLiteral("Age headline old"), {},
                     now.addSecs(-5 * 3600));
    const auto freshWarning =
        smokeWarning(QStringLiteral("urn:oid:smoke-age-fresh"), QStringLiteral("Tornado Warning"),
                     QStringLiteral("Severe"), QStringLiteral("Age headline fresh"));
    center.ingestWarnings({oldWarning, freshWarning});
    if (!center.recent().isEmpty()) {
        *error = QStringLiteral("priming: first sync must stay silent, got %1")
                     .arg(center.recent().size());
        return false;
    }
    // A genuinely new, fresh warning notifies.
    const auto fresh2 =
        smokeWarning(QStringLiteral("urn:oid:smoke-age-fresh2"), QStringLiteral("Tornado Warning"),
                     QStringLiteral("Severe"), QStringLiteral("Age headline fresh2"));
    center.ingestWarnings({fresh2});
    if (center.recent().size() != 1) {
        *error = QStringLiteral("priming: new fresh warning should alert, got %1")
                     .arg(center.recent().size());
        return false;
    }
    // A new-but-stale warning is seeded silently and never fires later.
    const auto old2 =
        smokeWarning(QStringLiteral("urn:oid:smoke-age-old2"), QStringLiteral("Flash Flood Warning"),
                     QStringLiteral("Severe"), QStringLiteral("Age headline old2"), {},
                     now.addSecs(-5 * 3600));
    center.ingestWarnings({old2});
    if (center.recent().size() != 1) {
        *error = QStringLiteral("age gate: 5h-old warning must stay silent, got %1")
                     .arg(center.recent().size());
        return false;
    }
    center.ingestWarnings({old2});
    if (center.recent().size() != 1) {
        *error = QStringLiteral("age gate: stale warning must not fire on refresh either, got %1")
                     .arg(center.recent().size());
        return false;
    }
    // Warnings without an onset always pass (missing data must not suppress).
    auto noOnset = smokeWarning(QStringLiteral("urn:oid:smoke-age-noonset"),
                                QStringLiteral("Tornado Warning"), QStringLiteral("Severe"),
                                QStringLiteral("Age headline no-onset"));
    noOnset.onset = QDateTime();
    center.ingestWarnings({noOnset});
    if (center.recent().size() != 2) {
        *error = QStringLiteral("age gate: missing onset must still alert, got %1")
                     .arg(center.recent().size());
        return false;
    }
    // 0 disables the age limit entirely.
    auto unlimited = center.config();
    unlimited.maxWarningAgeMinutes = 0;
    center.setConfig(unlimited);
    const auto old3 =
        smokeWarning(QStringLiteral("urn:oid:smoke-age-old3"), QStringLiteral("Flash Flood Warning"),
                     QStringLiteral("Severe"), QStringLiteral("Age headline old3"), {},
                     now.addSecs(-5 * 3600));
    center.ingestWarnings({old3});
    if (center.recent().size() != 3) {
        *error = QStringLiteral("age 0 should notify regardless of onset, got %1")
                     .arg(center.recent().size());
        return false;
    }
    // Clearing memory re-arms priming instead of bursting for active alerts.
    center.clearMemory();
    const auto fresh3 =
        smokeWarning(QStringLiteral("urn:oid:smoke-age-fresh3"), QStringLiteral("Tornado Warning"),
                     QStringLiteral("Severe"), QStringLiteral("Age headline fresh3"));
    center.ingestWarnings({fresh3});
    if (!center.recent().isEmpty()) {
        *error = QStringLiteral("clearMemory should re-arm silent priming, got %1")
                     .arg(center.recent().size());
        return false;
    }
    return true;
}

namespace {
CirculationDetection smokeCirculation(const CirculationKind kind, const double lat,
                                      const double lon) {
    CirculationDetection detection;
    detection.kind = kind;
    detection.latitudeDeg = lat;
    detection.longitudeDeg = lon;
    detection.maxDeltaVelocityMetersPerSecond =
        kind == CirculationKind::TornadoVortex ? 35.0 : 15.0;
    detection.depthMeters = 3000.0;
    detection.tiltCount = 3;
    return detection;
}
}  // namespace

static bool testAlertCenterCirculations(QString* error) {
    // Opt-in: disabled by default, markers never notify unless asked.
    {
        AlertCenter center;
        auto config = smokeQuietConfig();
        config.minSeverity = 0;
        center.setConfig(config);
        if (center.config().circulationAlertsEnabled) {
            *error = QStringLiteral("circulation alerts must default off");
            return false;
        }
        center.ingestCirculations(
            {smokeCirculation(CirculationKind::TornadoVortex, 35.5, -97.5)}, QStringLiteral("KTLX"));
        center.ingestCirculations(
            {smokeCirculation(CirculationKind::TornadoVortex, 35.5, -97.5)}, QStringLiteral("KTLX"));
        if (!center.recent().isEmpty()) {
            *error = QStringLiteral("disabled sink must stay silent, got %1")
                          .arg(center.recent().size());
            return false;
        }
    }
    AlertCenter center;
    auto config = smokeQuietConfig();
    config.minSeverity = 0;
    config.repeatSuppressionMinutes = 30;
    config.circulationAlertsEnabled = true;
    center.setConfig(config);
    const std::vector<CirculationDetection> first = {
        smokeCirculation(CirculationKind::TornadoVortex, 35.5, -97.5),
        smokeCirculation(CirculationKind::Mesocyclone, 35.7, -97.2)};
    center.ingestCirculations(first, QStringLiteral("KTLX"));
    if (!center.recent().isEmpty()) {
        *error = QStringLiteral("circulations: first sync should prime silently, got %1")
                      .arg(center.recent().size());
        return false;
    }
    // Same markers repeat: suppression holds.
    center.ingestCirculations(first, QStringLiteral("KTLX"));
    if (!center.recent().isEmpty()) {
        *error = QStringLiteral("circulations: repeat markers must stay silent, got %1")
                      .arg(center.recent().size());
        return false;
    }
    // A new TVS notifies, labeled guidance with the radar id.
    center.ingestCirculations({smokeCirculation(CirculationKind::TornadoVortex, 35.9, -97.9)},
                              QStringLiteral("KTLX"));
    if (center.recent().size() != 1 || center.recent().front().kind != QStringLiteral("rotation") ||
        !center.recent().front().title.contains(QStringLiteral("guidance")) ||
        !center.recent().front().title.contains(QStringLiteral("KTLX")) ||
        !center.recent().front().detail.contains(QStringLiteral("not a warning"))) {
        *error = QStringLiteral("circulations: new TVS should alert once as guidance");
        return false;
    }
    // Same marker on another radar is a different key, not suppression.
    center.ingestCirculations({smokeCirculation(CirculationKind::TornadoVortex, 35.9, -97.9)},
                              QStringLiteral("KINX"));
    if (center.recent().size() != 2) {
        *error = QStringLiteral("circulations: radar id should scope dedup, got %1")
                      .arg(center.recent().size());
        return false;
    }
    // Severity knob: Severe+ drops the meso but keeps the TVS.
    {
        AlertCenter severe;
        auto severeConfig = smokeQuietConfig();
        severeConfig.minSeverity = 2;
        severeConfig.circulationAlertsEnabled = true;
        severe.setConfig(severeConfig);
        severe.ingestCirculations(first, QStringLiteral("KTLX"));  // prime
        severe.ingestCirculations({smokeCirculation(CirculationKind::Mesocyclone, 35.8, -97.0)},
                                  QStringLiteral("KTLX"));
        if (!severe.recent().isEmpty()) {
            *error = QStringLiteral("circulations: meso must not notify at Severe+, got %1")
                          .arg(severe.recent().size());
            return false;
        }
        severe.ingestCirculations({smokeCirculation(CirculationKind::TornadoVortex, 35.8, -97.0)},
                                  QStringLiteral("KTLX"));
        if (severe.recent().size() != 1) {
            *error = QStringLiteral("circulations: TVS must notify at Severe+, got %1")
                          .arg(severe.recent().size());
            return false;
        }
    }
    return true;
}

static bool testHodographPaint(QString* error) {
    // Paint smoke: synthetic sounding through the widget grabber. Catches
    // null/divide crashes in the hodograph painter without pixel asserts.
    Sounding sounding;
    sounding.stationId = "KOUN";
    for (int i = 0; i < 8; ++i) {
        SoundingLevel level;
        level.heightMslMeters = 400.0 + i * 800.0;
        level.tempCelsius = 20.0 - i * 3.0;
        level.directionFromDegrees = 200.0 + i * 5.0;
        level.speedKnots = 15.0 + i * 4.0;
        sounding.levels.push_back(level);
    }
    HodographWidget hodo;
    hodo.resize(400, 400);
    hodo.setSounding(sounding);
    hodo.setStormMotion(10.0, -5.0, true);
    const QImage plotted = hodo.grab().toImage();
    if (plotted.isNull() || plotted.width() <= 0) {
        *error = QStringLiteral("hodograph should render a sounding without crashing");
        return false;
    }
    // Empty sounding paints the quiet placeholder, also crash-free.
    hodo.setSounding(Sounding{});
    hodo.setStormMotion(0.0, 0.0, false);
    if (hodo.grab().toImage().isNull()) {
        *error = QStringLiteral("hodograph should render the empty state without crashing");
        return false;
    }
    return true;
}

static bool testBasemapTileUrls(QString* error) {
    // CARTO is the default: single layer, standard z/x/y order, key appended.
    const auto dark = basemapTileUrls(5, 7, 12, BasemapProvider::CartoDark,
                                      QStringLiteral("test-key-123"), QString());
    const auto darkBase = dark.base.toString();
    if (!darkBase.contains(QStringLiteral("basemaps.cartocdn.com")) ||
        !darkBase.contains(QStringLiteral("rastertiles/dark_all")) ||
        !darkBase.endsWith(QStringLiteral("/5/7/12.png?key=test-key-123"))) {
        *error = QStringLiteral("CARTO dark URL should be z/x/y with key, got %1").arg(darkBase);
        return false;
    }
    if (!dark.reference.isEmpty()) {
        *error = QStringLiteral("CARTO is single-layer, reference must be empty");
        return false;
    }
    const auto light = basemapTileUrls(5, 7, 12, BasemapProvider::CartoLight,
                                       QStringLiteral("k"), QString());
    if (!light.base.toString().contains(QStringLiteral("rastertiles/light_all"))) {
        *error = QStringLiteral("CARTO light should use light_all, got %1")
                     .arg(light.base.toString());
        return false;
    }
    // No key: tiles still addressable (CARTO watermarks instead of failing).
    const auto keyless = basemapTileUrls(5, 7, 12, BasemapProvider::CartoDark, QString(), QString());
    if (!keyless.base.toString().endsWith(QStringLiteral("/5/7/12.png?key="))) {
        *error = QStringLiteral("keyless CARTO URL should keep an empty ?key=, got %1")
                     .arg(keyless.base.toString());
        return false;
    }
    // Esri legacy keeps z/y/x order plus the reference layer.
    const auto esri =
        basemapTileUrls(5, 7, 12, BasemapProvider::EsriDark, QString(), QString());
    if (!esri.base.toString().endsWith(QStringLiteral("/5/12/7")) ||
        !esri.base.toString().contains(QStringLiteral("World_Dark_Gray_Base")) ||
        !esri.reference.toString().contains(QStringLiteral("World_Dark_Gray_Reference"))) {
        *error = QStringLiteral("Esri fallback should stay z/y/x with reference, got %1")
                     .arg(esri.base.toString());
        return false;
    }
    // Custom templates substitute every placeholder, subdomain included.
    const auto custom = basemapTileUrls(5, 7, 12, BasemapProvider::Custom, QStringLiteral("K"),
                                        QStringLiteral("https://{s}.tiles.example.com/{z}/{x}/{y}.png?key={key}"));
    if (custom.base.toString() !=
        QStringLiteral("https://d.tiles.example.com/5/7/12.png?key=K")) {
        *error = QStringLiteral("custom template substitution wrong, got %1")
                     .arg(custom.base.toString());
        return false;
    }
    // Attribution is provider-specific and CARTO credit is mandatory.
    if (!basemapAttribution(BasemapProvider::CartoDark, QString()).contains(
            QStringLiteral("CARTO")) ||
        !basemapAttribution(BasemapProvider::EsriDark, QString()).contains(
            QStringLiteral("Esri")) ||
        basemapAttribution(BasemapProvider::Custom, QStringLiteral("Mine")) !=
            QStringLiteral("Mine")) {
        *error = QStringLiteral("basemap attribution strings are wrong");
        return false;
    }
    if (basemapProviderFromInt(99) != BasemapProvider::CartoDark ||
        basemapProviderFromInt(2) != BasemapProvider::EsriDark) {
        *error = QStringLiteral("basemapProviderFromInt should default to CARTO Dark");
        return false;
    }
    // Settings-driven wrapper: CARTO selected but no key falls back to Esri
    // (tiles and attribution agree); pasting a key selects CARTO.
    QSettings settings;
    const auto savedProvider = settings.value(QStringLiteral("basemap/provider"));
    const auto savedKey = settings.value(QStringLiteral("basemap/apiKey"));
    const auto savedTemplate = settings.value(QStringLiteral("basemap/customTemplate"));
    const auto restoreBasemapSettings = [&] {
        if (savedProvider.isValid()) settings.setValue(QStringLiteral("basemap/provider"), savedProvider);
        else settings.remove(QStringLiteral("basemap/provider"));
        if (savedKey.isValid()) settings.setValue(QStringLiteral("basemap/apiKey"), savedKey);
        else settings.remove(QStringLiteral("basemap/apiKey"));
        if (savedTemplate.isValid())
            settings.setValue(QStringLiteral("basemap/customTemplate"), savedTemplate);
        else settings.remove(QStringLiteral("basemap/customTemplate"));
    };
    settings.setValue(QStringLiteral("basemap/provider"),
                      static_cast<int>(BasemapProvider::CartoDark));
    settings.remove(QStringLiteral("basemap/apiKey"));
    settings.remove(QStringLiteral("basemap/customTemplate"));
    const auto fallback = basemapTileUrls(5, 7, 12);
    const bool fallbackOk =
        fallback.base.toString().contains(QStringLiteral("World_Dark_Gray_Base")) &&
        basemapAttribution().contains(QStringLiteral("Esri"));
    settings.setValue(QStringLiteral("basemap/apiKey"), QStringLiteral("k"));
    const auto keyed = basemapTileUrls(5, 7, 12);
    const bool keyedOk = keyed.base.toString().contains(QStringLiteral("cartocdn")) &&
                         basemapAttribution().contains(QStringLiteral("CARTO"));
    restoreBasemapSettings();
    if (!fallbackOk || !keyedOk) {
        *error = QStringLiteral("keyless settings should fall back to Esri, keyed to CARTO");
        return false;
    }
    // Preset gallery: IEM template is a fixed XYZ path that serves without a
    // CARTO key (keyless Custom must not fall back to Esri), and the
    // RainViewer resolver pins the newest past frame from live-shaped JSON.
    const auto presets = basemapPresets();
    if (presets.isEmpty() || !presets.front().templateUrl.contains(QStringLiteral("{z}/{x}/{y}"))) {
        *error = QStringLiteral("basemap presets should ship a usable XYZ template");
        return false;
    }
    const auto iem = basemapTileUrls(4, 4, 5, BasemapProvider::Custom, QString(),
                                     presets.front().templateUrl);
    if (!iem.base.toString().contains(QStringLiteral("mesonet.agron.iastate.edu")) ||
        !iem.base.toString().endsWith(QStringLiteral("/4/4/5.png"))) {
        *error = QStringLiteral("IEM preset should substitute z/x/y, got %1")
                      .arg(iem.base.toString());
        return false;
    }
    const QByteArray rainviewer = R"JSON({"host":"https://tilecache.rainviewer.com","radar":{"past":[
      {"time":1791639600,"path":"/v2/radar/317ddcb9a978"},{"time":1791640200,"path":"/v2/radar/09738d02d97f"}]}})JSON";
    QString rainError;
    const auto rainTemplate = rainviewerTileTemplate(rainviewer, &rainError);
    if (!rainError.isEmpty() ||
        rainTemplate !=
            QStringLiteral("https://tilecache.rainviewer.com/v2/radar/09738d02d97f/"
                           "256/{z}/{x}/{y}/2/1_1.png")) {
        *error = QStringLiteral("RainViewer resolver should pin the newest frame, got %1 (%2)")
                      .arg(rainTemplate, rainError);
        return false;
    }
    if (!rainviewerTileTemplate(QByteArray("{bad json"), &rainError).isEmpty() ||
        rainError.isEmpty() ||
        !rainviewerTileTemplate(QByteArray(R"JSON({"host":"https://x","radar":{"past":[]}})JSON"),
                                &rainError)
             .isEmpty()) {
        *error = QStringLiteral("RainViewer resolver should fail closed on bad input");
        return false;
    }
    return true;
}

static bool testThemeSwitcher(MainWindow* window, QString* error) {
    if (appThemeName(AppTheme::Classic) == appThemeName(AppTheme::Prototype)) {
        *error = QStringLiteral("theme names must be distinct");
        return false;
    }
    if (appThemeFromInt(99) != AppTheme::Classic || appThemeFromInt(1) != AppTheme::Prototype) {
        *error = QStringLiteral("appThemeFromInt should map 1->Prototype, other->Classic");
        return false;
    }
    const auto classic = appStylesheet(AppTheme::Classic);
    const auto squall = appStylesheet(AppTheme::Prototype);
    if (classic.isEmpty() || squall.isEmpty() || classic == squall) {
        *error = QStringLiteral("both themes must ship distinct non-empty stylesheets");
        return false;
    }
    if (appAccent(AppTheme::Classic) == appAccent(AppTheme::Prototype)) {
        *error = QStringLiteral("theme accents must differ");
        return false;
    }
    window->applyTheme(AppTheme::Prototype);
    if (window->theme() != AppTheme::Prototype) {
        *error = QStringLiteral("applyTheme(Prototype) should stick");
        window->applyTheme(AppTheme::Classic);
        return false;
    }
    window->applyTheme(AppTheme::Classic);  // leave the shared window untouched
    if (window->theme() != AppTheme::Classic) {
        *error = QStringLiteral("applyTheme(Classic) should restore");
        return false;
    }
    return true;
}

static bool testSidebarRedesign(MainWindow* window, QString* error) {
    // Tabbed sidebar: Radar / Layers / Display / Alerts. Groups live on
    // tab pages now, so collapse assertions must target a visible header.
    auto* tabs = window->findChild<QTabWidget*>(QStringLiteral("sidebarTabs"));
    if (!tabs || tabs->count() != 4) {
        *error = QStringLiteral("sidebar should have 4 tabs, got %1")
                     .arg(tabs ? tabs->count() : -1);
        return false;
    }
    if (!window->findChild<QWidget*>(QStringLiteral("warningList"))) {
        *error = QStringLiteral("Alerts tab should host the warning inbox list");
        return false;
    }
    if (!window->findChild<QWidget*>(QStringLiteral("timelineStrip"))) {
        *error = QStringLiteral("playback dock should host the filmstrip timeline");
        return false;
    }
    // Collapsible task groups.
    const auto headers = window->findChildren<QToolButton*>(QStringLiteral("groupHeader"));
    if (headers.size() < 5) {
        *error = QStringLiteral("dock should have 5+ collapsible groups, got %1").arg(headers.size());
        return false;
    }
    QToolButton* firstHeader = nullptr;
    for (auto* header : headers) {
        if (header->isVisibleTo(window)) {
            firstHeader = header;
            break;
        }
    }
    if (!firstHeader) {
        *error = QStringLiteral("at least one group header should be visible");
        return false;
    }
    const auto bodies = window->findChildren<QWidget*>();
    QWidget* toggledBody = nullptr;
    // Collapse the first group via its header and confirm something hides.
    int visibleBefore = 0;
    for (auto* body : bodies) {
        if (body->isVisibleTo(window) && qobject_cast<QCheckBox*>(body) == nullptr) ++visibleBefore;
    }
    firstHeader->setChecked(false);
    QCoreApplication::processEvents();
    int visibleAfter = 0;
    for (auto* body : bodies) {
        if (body->isVisibleTo(window) && qobject_cast<QCheckBox*>(body) == nullptr) ++visibleAfter;
    }
    if (visibleAfter >= visibleBefore) {
        *error = QStringLiteral("collapsing a group should hide widgets");
        firstHeader->setChecked(true);
        return false;
    }
    firstHeader->setChecked(true);
    QCoreApplication::processEvents();
    Q_UNUSED(toggledBody);
    // Product chips mirror the product combo.
    auto* products = window->findChild<QComboBox*>(QStringLiteral("productCombo"));
    const auto chips = window->findChildren<QToolButton*>(QStringLiteral("productChip"));
    if (chips.size() != 10 || !products || chips.size() != products->count()) {
        *error = QStringLiteral("product chip grid should have 10 chips, got %1").arg(chips.size());
        return false;
    }
    for (const auto* chip : chips) {
        if (chip->icon().isNull() || chip->toolTip().isEmpty()) {
            *error = QStringLiteral("every product chip needs an icon and hover text");
            return false;
        }
    }
    const auto velocityIndex = products->findData(static_cast<int>(ProductKind::Velocity));
    chips.at(velocityIndex)->click();
    QCoreApplication::processEvents();
    if (products->currentIndex() != velocityIndex) {
        *error = QStringLiteral("clicking the Velocity chip should drive the product combo");
        return false;
    }
    const auto reflectivityIndex = products->findData(static_cast<int>(ProductKind::Reflectivity));
    chips.at(reflectivityIndex)->click();
    QCoreApplication::processEvents();
    if (products->currentIndex() != reflectivityIndex || !chips.at(reflectivityIndex)->isChecked()) {
        *error = QStringLiteral("clicking the Reflectivity chip should check it and drive the combo");
        return false;
    }
    // Tilt stepper + grid widgets exist (grid fills once volumes decode).
    if (!window->findChild<QToolButton*>(QStringLiteral("tiltMinusButton")) ||
        !window->findChild<QToolButton*>(QStringLiteral("tiltPlusButton")) ||
        !window->findChild<QToolButton*>(QStringLiteral("tiltValueButton")) ||
        !window->findChild<QWidget*>(QStringLiteral("tiltGrid"))) {
        *error = QStringLiteral("tilt stepper/grid widgets missing");
        return false;
    }
    // Scope selector drives the per-panel override box (Layers tab).
    tabs->setCurrentIndex(1);
    QCoreApplication::processEvents();
    auto* scope = window->findChild<QComboBox*>(QStringLiteral("scopeCombo"));
    auto* perPanelBox = window->findChild<QWidget*>(QStringLiteral("perPanelBox"));
    if (!scope || scope->count() != 2 || !perPanelBox) {
        *error = QStringLiteral("scope selector or per-panel box missing");
        return false;
    }
    if (perPanelBox->isVisibleTo(window)) {
        *error = QStringLiteral("per-panel box should hide under All-panels scope");
        return false;
    }
    scope->setCurrentIndex(1);
    QCoreApplication::processEvents();
    if (!perPanelBox->isVisibleTo(window)) {
        *error = QStringLiteral("per-panel box should show under Active-panel scope");
        scope->setCurrentIndex(0);
        return false;
    }
    scope->setCurrentIndex(0);
    QCoreApplication::processEvents();
    // Layer badges + dock alert controls exist.
    for (const auto& name : {QStringLiteral("warningsBadge"), QStringLiteral("reportsBadge"),
                             QStringLiteral("lightningBadge"), QStringLiteral("fusionBadge"),
                             QStringLiteral("placefilesBadge")}) {
        if (!window->findChild<QLabel*>(name)) {
            *error = QStringLiteral("layer badge missing: %1").arg(name);
            return false;
        }
    }
    auto* severity = window->findChild<QComboBox*>(QStringLiteral("severityCombo"));
    auto* age = window->findChild<QSpinBox*>(QStringLiteral("ageSpin"));
    auto* sound = window->findChild<QCheckBox*>(QStringLiteral("soundCheck"));
    auto* tray = window->findChild<QCheckBox*>(QStringLiteral("trayCheck"));
    if (!severity || severity->count() != 4 || !age || age->maximum() != 1440 || !sound || !tray) {
        *error = QStringLiteral("dock alert controls missing or misshapen");
        return false;
    }
    severity->setCurrentIndex(severity->findData(3));
    age->setValue(15);
    QCoreApplication::processEvents();
    if (severity->currentData().toInt() != 3 || age->value() != 15) {
        *error = QStringLiteral("dock alert controls should accept edits");
        return false;
    }
    // Velocity controls hide outside the Velocity product (Radar tab).
    tabs->setCurrentIndex(0);
    QCoreApplication::processEvents();
    auto* velocityBox = window->findChild<QWidget*>(QStringLiteral("velocityBox"));
    if (!velocityBox || velocityBox->isVisibleTo(window)) {
        *error = QStringLiteral("velocity box should hide while product is Reflectivity");
        return false;
    }
    return true;
}

static bool testOperaKeys(QString* error) {
    const auto prefix = OperaSource::pvolPrefix(QStringLiteral("NO"), QStringLiteral("nohur"),
                                                QDate(2026, 9, 5));
    if (prefix != QStringLiteral("2026/09/05/NO/nohur/PVOL/")) {
        *error = QStringLiteral("OPERA prefix mismatch: %1").arg(prefix);
        return false;
    }
    const auto stamp = OperaSource::timestampFromKey(
        QStringLiteral("2026/09/05/NO/nohur/PVOL/nohur@20260905T0005@0.5_1.0@DBZH.h5"));
    if (!stamp.isValid() || stamp.date() != QDate(2026, 9, 5) || stamp.time() != QTime(0, 5) ||
        stamp.timeSpec() != Qt::UTC || stamp.offsetFromUtc() != 0) {
        *error = QStringLiteral("OPERA timestamp parse failed");
        return false;
    }
    if (OperaSource::timestampFromKey(QStringLiteral("nohur@bogus@DBZH.h5")).isValid() ||
        OperaSource::timestampFromKey({}).isValid()) {
        *error = QStringLiteral("OPERA timestamp parse should reject garbage");
        return false;
    }
    if (!OperaSource::isDbzhKey(
            QStringLiteral("2026/09/05/NO/nohur/PVOL/nohur@20260905T0005@0.5@DBZH.h5"),
            QStringLiteral("nohur"))) {
        *error = QStringLiteral("OPERA DBZH key not recognized");
        return false;
    }
    if (OperaSource::isDbzhKey(
            QStringLiteral("2026/09/05/NO/nohur/PVOL/nohur@20260905T0006@2.6@VRADH.h5"),
            QStringLiteral("nohur")) ||
        OperaSource::isDbzhKey(
            QStringLiteral("2026/09/05/DE/deess/PVOL/deess@20260905T0005@0.5@DBZH.h5"),
            QStringLiteral("nohur"))) {
        *error = QStringLiteral("OPERA key filter should drop VRADH and foreign sites");
        return false;
    }
    // Heterogeneous bucket layouts (verified live 2026-09-06): SCAN prefixes,
    // combined moment lists, and multi-moment PVOL objects.
    if (OperaSource::scanPrefix(QStringLiteral("DE"), QStringLiteral("deess"),
                                QDate(2026, 9, 6)) !=
        QStringLiteral("2026/09/06/DE/deess/SCAN/")) {
        *error = QStringLiteral("OPERA SCAN prefix mismatch");
        return false;
    }
    if (!OperaSource::isDbzhKey(
            QStringLiteral("2026/09/06/FI/fianj/SCAN/fianj@20260906T0000@0.3@DBZH_TH_VRADH.h5"),
            QStringLiteral("fianj")) ||
        !OperaSource::isDbzhKey(
            QStringLiteral("2026/09/06/NL/nldhl/PVOL/nldhl@20260906T0000@0.3_0.8@CCORH_DBZH_PHIDP_RHOHV_SQIH_TH_VRADH_WRADH_ZDR.h5"),
            QStringLiteral("nldhl")) ||
        !OperaSource::isDbzhKey(
            QStringLiteral("2026/09/06/DE/deasb/SCAN/deasb@20260906T0000@0.5@DBZH.h5"),
            QStringLiteral("deasb"))) {
        *error = QStringLiteral("OPERA DBZH filter should accept SCAN/combined keys");
        return false;
    }
    if (OperaSource::isDbzhKey(
            QStringLiteral("2026/09/06/DE/deasb/SCAN/deasb@20260906T0000@0.5@TH.h5"),
            QStringLiteral("deasb"))) {
        *error = QStringLiteral("OPERA DBZH filter should still drop TH-only keys");
        return false;
    }
    // SCAN bucket grouping: same 5-minute cycle assembles one volume even when
    // providers stagger elevations across minutes (FR pattern).
    {
        QVector<QPair<QString, QDateTime>> candidates{
            {QStringLiteral("2026/09/06/FR/frabb/SCAN/frabb@20260906T0003@4.5@DBZH_TH_VRADH.h5"),
             QDateTime(QDate(2026, 9, 6), QTime(0, 3), QTimeZone::UTC)},
            {QStringLiteral("2026/09/06/FR/frabb/SCAN/frabb@20260906T0001@0.9@DBZH_TH_VRADH.h5"),
             QDateTime(QDate(2026, 9, 6), QTime(0, 1), QTimeZone::UTC)},
            {QStringLiteral("2026/09/06/FR/frabb/SCAN/frabb@20260906T0000@7.5@DBZH_TH_VRADH.h5"),
             QDateTime(QDate(2026, 9, 6), QTime(0, 0), QTimeZone::UTC)},
            {QStringLiteral("2026/09/06/FR/frabb/SCAN/frabb@20260906T0007@1.5@DBZH_TH_VRADH.h5"),
             QDateTime(QDate(2026, 9, 6), QTime(0, 7), QTimeZone::UTC)},
        };
        std::sort(candidates.begin(), candidates.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        const auto groups = OperaSource::groupScanCandidates(candidates, 2);
        if (groups.size() != 2 || groups.at(0).size() != 1 || groups.at(1).size() != 3) {
            *error = QStringLiteral("OPERA SCAN grouping should bucket by 5-minute cycle");
            return false;
        }
    }
    // Filling newest bucket: fewer files than the previous bucket and minutes
    // old means it is still uploading, so it is skipped (not marked
    // downloaded) and picked up complete on the next refresh.
    {
        const auto now = QDateTime::currentDateTimeUtc();
        const auto stamp = [now](int secsAgo) { return now.addSecs(-secsAgo); };
        QVector<QVector<QPair<QString, QDateTime>>> groups{
            {{{QStringLiteral("new1"), stamp(60)}, {QStringLiteral("new2"), stamp(120)}}},
            {{{QStringLiteral("old1"), stamp(600)},
              {QStringLiteral("old2"), stamp(660)},
              {QStringLiteral("old3"), stamp(720)}}},
        };
        const auto trimmed = OperaSource::trimScanGroups(groups, 1, now);
        if (trimmed.size() != 1 || trimmed.at(0).size() != 3 ||
            trimmed.at(0).at(0).first != QStringLiteral("old1")) {
            *error = QStringLiteral("OPERA SCAN should skip a still-filling newest bucket");
            return false;
        }
        // Old filling buckets (stale, e.g. after a scan-mode change) are still
        // delivered: bounded staleness beats a stuck timeline.
        QVector<QVector<QPair<QString, QDateTime>>> stale{
            {{{QStringLiteral("new1"), stamp(3600)}, {QStringLiteral("new2"), stamp(3660)}}},
            {{{QStringLiteral("old1"), stamp(4200)},
              {QStringLiteral("old2"), stamp(4260)},
              {QStringLiteral("old3"), stamp(4320)}}},
        };
        const auto kept = OperaSource::trimScanGroups(stale, 1, now);
        if (kept.size() != 1 || kept.at(0).at(0).first != QStringLiteral("new1")) {
            *error = QStringLiteral("OPERA SCAN should deliver stale buckets regardless of size");
            return false;
        }
    }
    // SCAN merge: independently decoded single-elevation parts (all numbered
    // tilt 1) must come out renumbered and sorted by angle.
    {
        std::vector<DecodedChunk> parts(2);
        parts[0].radarId = "FRABB";
        parts[0].messageCount = 1;
        DecodedRay high;
        high.elevationNumber = 1;
        high.elevationDegrees = 4.5F;
        high.azimuthDegrees = 10.0F;
        parts[0].rays.push_back(high);
        parts[1].radarId = "FRABB";
        parts[1].messageCount = 1;
        DecodedRay low;
        low.elevationNumber = 1;
        low.elevationDegrees = 0.9F;
        low.azimuthDegrees = 20.0F;
        parts[1].rays.push_back(low);
        const auto merged = OperaSource::mergeScanParts(std::move(parts), "FRABB");
        if (merged.rays.size() != 2 || merged.messageCount != 2 ||
            merged.rays.at(0).elevationNumber != 1 || merged.rays.at(1).elevationNumber != 2 ||
            merged.rays.at(0).elevationDegrees > merged.rays.at(1).elevationDegrees) {
            *error = QStringLiteral("OPERA SCAN merge should renumber tilts by angle");
            return false;
        }
    }
    return true;
}

static bool testProximityFilter(QString* error) {
    const auto nearWarning =
        smokeWarning(QStringLiteral("prox-near"), QStringLiteral("Tornado Warning"),
                     QStringLiteral("Severe"), QStringLiteral("PROX-NEAR-HEADLINE"),
                     smokeSquare(-97.4, 35.2, 0.1));
    const auto farWarning =
        smokeWarning(QStringLiteral("prox-far"), QStringLiteral("Severe Thunderstorm Warning"),
                     QStringLiteral("Severe"), QStringLiteral("PROX-FAR-HEADLINE"),
                     smokeSquare(-100.0, 40.0, 0.1));
    const QVector<WeatherWarning> warnings{nearWarning, farWarning};
    RadarMapCanvas nearby;
    nearby.resize(800, 600);
    nearby.setWarningsVisible(true);
    nearby.setWeatherOverlays(warnings, {});
    nearby.setView(QPointF(-97.4, 35.2), 8);
    nearby.setProximityFilter(QPointF(-97.4, 35.2), 150.0);
    if (!nearby.proximityFilterActive()) {
        *error = QStringLiteral("proximity filter should report active");
        return false;
    }
    nearby.inspectAt(QPointF(nearby.rect().center()));
    if (!nearby.probeText().contains(QStringLiteral("PROX-NEAR-HEADLINE"))) {
        *error = QStringLiteral("proximity should keep nearby warning, got: %1")
                     .arg(nearby.probeText().left(120));
        return false;
    }
    RadarMapCanvas distant;
    distant.resize(800, 600);
    distant.setWarningsVisible(true);
    distant.setWeatherOverlays(warnings, {});
    distant.setView(QPointF(-100.0, 40.0), 8);
    distant.setProximityFilter(QPointF(-97.4, 35.2), 150.0);
    distant.inspectAt(QPointF(distant.rect().center()));
    if (distant.probeText().contains(QStringLiteral("PROX-FAR-HEADLINE"))) {
        *error = QStringLiteral("proximity should drop distant warning");
        return false;
    }
    distant.setProximityFilter(std::nullopt, 150.0);
    if (distant.proximityFilterActive()) {
        *error = QStringLiteral("clearing proximity anchor should deactivate filter");
        return false;
    }
    distant.inspectAt(QPointF(distant.rect().center()));
    if (!distant.probeText().contains(QStringLiteral("PROX-FAR-HEADLINE"))) {
        *error = QStringLiteral("disabled proximity filter should restore distant warning, got: %1")
                     .arg(distant.probeText().left(120));
        return false;
    }
    return true;
}

static bool testOverlayVisibility(MainWindow* window, QString* error) {
    const auto warning =
        smokeWarning(QStringLiteral("ov-1"), QStringLiteral("Tornado Warning"),
                     QStringLiteral("Severe"), QStringLiteral("OVERLAY-VIS-HEADLINE"),
                     smokeSquare(-97.4, 35.2, 0.1));
    RadarMapCanvas globalCanvas;
    globalCanvas.resize(800, 600);
    globalCanvas.setWarningsVisible(true);
    globalCanvas.setWeatherOverlays({warning}, {});
    globalCanvas.setView(QPointF(-97.4, 35.2), 8);
    globalCanvas.inspectAt(QPointF(globalCanvas.rect().center()));
    if (!globalCanvas.probeText().contains(QStringLiteral("OVERLAY-VIS-HEADLINE"))) {
        *error = QStringLiteral("global warningsVisible should hit");
        return false;
    }
    RadarMapCanvas overridden;
    overridden.resize(800, 600);
    overridden.setWarningsVisible(true);
    overridden.setWeatherOverlays({warning}, {});
    overridden.setView(QPointF(-97.4, 35.2), 8);
    RadarMapCanvas::OverlayVisibility hideWarnings;
    hideWarnings.warnings = false;
    hideWarnings.stormReports = true;
    hideWarnings.lightning = true;
    hideWarnings.lightningFusion = true;
    overridden.setOverlayOverrides(hideWarnings);
    if (!overridden.overlayOverrides().has_value() || overridden.overlayOverrides()->warnings) {
        *error = QStringLiteral("per-panel override should store warnings=false");
        return false;
    }
    overridden.inspectAt(QPointF(overridden.rect().center()));
    if (overridden.probeText().contains(QStringLiteral("OVERLAY-VIS-HEADLINE"))) {
        *error = QStringLiteral("per-panel warnings=false should hide warning hit");
        return false;
    }
    // "Use global" reset: drop the override, then re-apply the global toggle.
    overridden.setOverlayOverrides(std::nullopt);
    if (overridden.overlayOverrides().has_value()) {
        *error = QStringLiteral("Use global should clear overlayOverrides");
        return false;
    }
    overridden.setWarningsVisible(true);
    overridden.inspectAt(QPointF(overridden.rect().center()));
    if (!overridden.probeText().contains(QStringLiteral("OVERLAY-VIS-HEADLINE"))) {
        *error = QStringLiteral("Use global + global visible should restore hit");
        return false;
    }
    bool foundUseGlobal = false;
    const auto buttons = window->findChildren<QToolButton*>();
    for (const auto* button : buttons) {
        if (button && button->text() == QStringLiteral("Use global")) {
            foundUseGlobal = true;
            break;
        }
    }
    if (!foundUseGlobal) {
        *error = QStringLiteral("MainWindow should expose a 'Use global' per-panel reset");
        return false;
    }
    return true;
}

static bool testPresets(MainWindow* window, QString* error) {
    auto* presets = window->findChild<QComboBox*>(QStringLiteral("presetsCombo"));
    auto* layoutCombo = window->findChild<QComboBox*>(QStringLiteral("panelLayoutCombo"));
    auto* secondMap = window->findChild<RadarMapCanvas*>(QStringLiteral("radarMapPanel2"));
    if (!presets || !layoutCombo || !secondMap) {
        *error = QStringLiteral("preset combos or second panel missing");
        return false;
    }
    if (presets->findText(QStringLiteral("Chase 1x1")) < 0 ||
        presets->findText(QStringLiteral("Analysis 2x2 dual-pol")) < 0 ||
        presets->findText(QStringLiteral("Lightning scout 1x2")) < 0) {
        *error = QStringLiteral("builtInPresets should contain Chase/Analysis/Lightning, got: %1")
                     .arg([presets] {
                         QStringList names;
                         for (int i = 0; i < presets->count(); ++i) names << presets->itemText(i);
                         return names.join(QStringLiteral(" | "));
                     }());
        return false;
    }
    const int originalLayout = layoutCombo->currentIndex();
    presets->setCurrentIndex(presets->findText(QStringLiteral("Chase 1x1")));
    QCoreApplication::processEvents();
    if (layoutCombo->currentIndex() != 0) {
        *error = QStringLiteral("presetByName Chase should map to layout 0, got %1")
                     .arg(layoutCombo->currentIndex());
        return false;
    }
    presets->setCurrentIndex(presets->findText(QStringLiteral("Analysis 2x2 dual-pol")));
    QCoreApplication::processEvents();
    if (layoutCombo->currentIndex() != 3) {
        *error = QStringLiteral("presetByName Analysis should map to layout 3, got %1")
                     .arg(layoutCombo->currentIndex());
        return false;
    }
    if (secondMap->product() != ProductKind::Velocity) {
        *error = QStringLiteral("Analysis preset should set panel2 to Velocity");
        return false;
    }
    presets->setCurrentIndex(presets->findText(QStringLiteral("Lightning scout 1x2")));
    QCoreApplication::processEvents();
    if (layoutCombo->currentIndex() != 1) {
        *error = QStringLiteral("presetByName Lightning should map to layout 1, got %1")
                     .arg(layoutCombo->currentIndex());
        return false;
    }
    presets->setCurrentIndex(0);  // "(none)" is a no-op lookup guard
    layoutCombo->setCurrentIndex(originalLayout);
    QCoreApplication::processEvents();
    return true;
}

static bool testViewHistory(QString* error) {
    RadarMapCanvas canvas;
    canvas.resize(800, 600);
    canvas.clearViewHistory();
    if (canvas.canUndoView() || canvas.canRedoView()) {
        *error = QStringLiteral("fresh history should have no undo/redo");
        return false;
    }
    canvas.setView(QPointF(-97.0, 35.0), 6);
    canvas.pushViewHistory();
    canvas.setView(QPointF(-96.0, 36.0), 7);
    if (!canvas.canUndoView()) {
        *error = QStringLiteral("history should offer undo after push+move");
        return false;
    }
    canvas.undoView();
    const auto undone = canvas.viewCenter();
    if (std::abs(undone.x() + 97.0) > 1e-9 || std::abs(undone.y() - 35.0) > 1e-9 ||
        canvas.viewZoom() != 6) {
        *error = QStringLiteral("undoView should restore center/zoom, got %1,%2 z%3")
                     .arg(undone.x())
                     .arg(undone.y())
                     .arg(canvas.viewZoom());
        return false;
    }
    if (!canvas.canRedoView()) {
        *error = QStringLiteral("history should offer redo after undo");
        return false;
    }
    canvas.redoView();
    const auto redone = canvas.viewCenter();
    if (std::abs(redone.x() + 96.0) > 1e-9 || std::abs(redone.y() - 36.0) > 1e-9 ||
        canvas.viewZoom() != 7) {
        *error = QStringLiteral("redoView should restore center/zoom, got %1,%2 z%3")
                     .arg(redone.x())
                     .arg(redone.y())
                     .arg(canvas.viewZoom());
        return false;
    }
    canvas.clearViewHistory();
    canvas.setView(QPointF(-100.0, 35.0), 5);
    canvas.pushViewHistory();
    const int sizeBefore = canvas.viewBackSizeForUi();
    canvas.pushViewHistory();  // same view: no-op dedupe
    if (canvas.viewBackSizeForUi() != sizeBefore) {
        *error = QStringLiteral("pushing the same view should not grow history");
        return false;
    }
    canvas.clearViewHistory();
    for (int i = 0; i < 60; ++i) {
        canvas.setView(QPointF(-100.0 + i * 0.1, 35.0 + i * 0.05), 4 + (i % 5));
        canvas.pushViewHistory();
    }
    if (canvas.viewBackSizeForUi() != 50) {
        *error = QStringLiteral("view history should be bounded to 50, got %1")
                     .arg(canvas.viewBackSizeForUi());
        return false;
    }
    canvas.clearViewHistory();
    if (canvas.canUndoView() || canvas.canRedoView()) {
        *error = QStringLiteral("clearViewHistory should empty both stacks");
        return false;
    }
    return true;
}

static bool testPinnedLock(QString* error) {
    const auto warningA =
        smokeWarning(QStringLiteral("pin-A"), QStringLiteral("Tornado Warning"),
                     QStringLiteral("Severe"), QStringLiteral("PIN-LOCK-A"),
                     smokeSquare(-97.4, 35.2, 0.1));
    const auto warningB =
        smokeWarning(QStringLiteral("pin-B"), QStringLiteral("Severe Thunderstorm Warning"),
                     QStringLiteral("Severe"), QStringLiteral("PIN-LOCK-B"),
                     smokeSquare(-96.0, 36.0, 0.1));
    RadarMapCanvas canvas;
    canvas.resize(800, 600);
    canvas.setWarningsVisible(true);
    canvas.setWeatherOverlays({warningA, warningB}, {});
    canvas.setView(QPointF(-97.4, 35.2), 8);
    canvas.inspectAt(QPointF(canvas.rect().center()));
    if (!canvas.probeText().contains(QStringLiteral("PIN-LOCK-A"))) {
        *error = QStringLiteral("pinned setup should show A, got: %1")
                     .arg(canvas.probeText().left(120));
        return false;
    }
    canvas.setPinnedOverlayLocked(true);
    if (!canvas.pinnedOverlayLocked()) {
        *error = QStringLiteral("setPinnedOverlayLocked(true) should stick");
        return false;
    }
    canvas.setView(QPointF(-96.0, 36.0), 8);
    canvas.inspectAt(QPointF(canvas.rect().center()));
    if (!canvas.probeText().contains(QStringLiteral("PIN-LOCK-A")) ||
        canvas.probeText().contains(QStringLiteral("PIN-LOCK-B"))) {
        *error = QStringLiteral("locked card should survive new clicks, got: %1")
                     .arg(canvas.probeText().left(160));
        return false;
    }
    canvas.setPinnedOverlayLocked(false);
    canvas.inspectAt(QPointF(canvas.rect().center()));
    if (!canvas.probeText().contains(QStringLiteral("PIN-LOCK-B"))) {
        *error = QStringLiteral("unlocked click should replace card with B, got: %1")
                     .arg(canvas.probeText().left(120));
        return false;
    }
    return true;
}

static bool testPinnedCardKeyboard(QString* error) {
    // A 2000-character headline overflows the card on a short panel, which
    // is exactly the state Up/Down serves.
    const QString longHeadline = QStringLiteral("SCROLL-ME ").repeated(200);
    const auto warning =
        smokeWarning(QStringLiteral("pin-scroll"), QStringLiteral("Tornado Warning"),
                     QStringLiteral("Severe"), longHeadline, smokeSquare(-97.4, 35.2, 0.1));
    RadarMapCanvas canvas;
    canvas.resize(800, 220);
    canvas.setWarningsVisible(true);
    canvas.setWeatherOverlays({warning}, {});
    canvas.setView(QPointF(-97.4, 35.2), 8);
    // No card yet: keyboard scroll must be a safe no-op, never a crash.
    if (canvas.hasScrollablePinnedCard()) {
        *error = QStringLiteral("no card means nothing scrollable");
        return false;
    }
    canvas.scrollPinnedCard(60);
    canvas.show();
    canvas.inspectAt(QPointF(canvas.rect().center()));
    QCoreApplication::processEvents();
    if (!canvas.probeText().contains(QStringLiteral("SCROLL-ME"))) {
        *error = QStringLiteral("long card should pin, got: %1").arg(canvas.probeText().left(80));
        return false;
    }
    if (!canvas.infoCardScrollable() || !canvas.hasScrollablePinnedCard()) {
        *error = QStringLiteral("long card on a short panel should overflow");
        return false;
    }
    canvas.scrollPinnedCard(-120);  // Down: reveal later content.
    // Poll with a deadline: the scroll animation converges on its own
    // timer, and a fixed sleep flakes under load.
    const auto waitFor = [&](const auto& settled) {
        const auto deadline = QDateTime::currentDateTimeUtc().addSecs(5);
        for (;;) {
            QTest::qWait(50);
            QCoreApplication::processEvents();
            if (settled()) return true;
            if (QDateTime::currentDateTimeUtc() >= deadline) return false;
        }
    };
    // Target selection and clamping are synchronous: assert those exactly.
    // The animation itself only needs to prove it moves (one timer tick).
    const double maxScroll = canvas.pinnedCardMaxScrollForTest();
    if (maxScroll <= 0.0) {
        *error = QStringLiteral("Overflow must report a positive max scroll");
        return false;
    }
    if (canvas.pinnedCardScrollTargetForTest() != std::min(120.0, maxScroll)) {
        *error = QStringLiteral("Down should target 120 px, got: %1")
                     .arg(canvas.pinnedCardScrollTargetForTest());
        return false;
    }
    if (!waitFor([&] { return canvas.infoCardScrollOffset() > 0.0; })) {
        *error = QStringLiteral("Scroll timer should advance the card");
        return false;
    }
    canvas.scrollPinnedCard(100000);  // Up past the top: clamps to zero.
    if (canvas.pinnedCardScrollTargetForTest() != 0.0) {
        *error = QStringLiteral("Up past the top should target zero");
        return false;
    }
    canvas.scrollPinnedCard(-100000);  // Down past the bottom: clamps to max.
    if (canvas.pinnedCardScrollTargetForTest() != maxScroll) {
        *error = QStringLiteral("Down past the bottom should target max scroll");
        return false;
    }
    return true;
}

static bool testProbeClearsOnLeave(QString* error) {
    // Regression: the 16ms hover debounce used to outlive the cursor, so fast
    // transits across panels left frozen probe readouts behind on every panel.
    DecodedRay ray;
    ray.azimuthDegrees = 90.0F;
    ray.azimuthResolutionDegrees = 1.0F;
    ray.elevationNumber = 1;
    ray.elevationDegrees = 0.5F;
    const auto reflectivityIndex = static_cast<std::size_t>(ProductKind::Reflectivity);
    ray.fields.at(reflectivityIndex).assign(1000, 86);
    ray.firstGateByProduct.at(reflectivityIndex) = 0;
    ray.gateSpacingByProduct.at(reflectivityIndex) = 250;
    ray.scaleByProduct.at(reflectivityIndex) = 2.0F;
    ray.offsetByProduct.at(reflectivityIndex) = 66.0F;
    RadarMapCanvas canvas;
    canvas.resize(800, 600);
    canvas.setSites({{"TATL", "Atlanta", "TDWR", 33.6470, -84.2620, 327.7}});
    canvas.setSelectedRadar(QStringLiteral("TATL"));
    canvas.setProduct(ProductKind::Reflectivity);
    canvas.setElevationFilter(1);
    canvas.setDecodedChunk({"TATL", {ray}, 1}, QDateTime::currentDateTimeUtc());
    canvas.setView(QPointF(-84.2620, 33.6470), 8);
    canvas.show();
    if (!QTest::qWaitForWindowExposed(&canvas, 5000)) {
        *error = QStringLiteral("probe canvas did not expose offscreen");
        return false;
    }
    const QPointF probePoint =
        QPointF(canvas.radarScreenPoint(QStringLiteral("TATL"))) + QPointF(20, 0);
    // Drive the event handlers directly: QTest mouse synthesis does not
    // reliably produce enter/move offscreen, and the race is in the handlers.
    const auto sendHover = [&canvas](const QPointF& point) {
        QEnterEvent enter(point, point, point);
        QCoreApplication::sendEvent(&canvas, &enter);
        QMouseEvent move(QEvent::MouseMove, point, point, point, Qt::NoButton,
                         Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&canvas, &move);
    };
    const auto sendLeave = [&canvas]() {
        QEvent leave(QEvent::Leave);
        QCoreApplication::sendEvent(&canvas, &leave);
    };
    // Arm the debounced hover, then leave before it can fire. The flag
    // asserts are synchronous (no event-loop timing involved); the qWait
    // afterwards lets a leaked timer fire so the empty check is behavioral.
    sendHover(probePoint);
    QCoreApplication::processEvents();
    if (!canvas.hoverActiveForTest()) {
        *error = QStringLiteral("synthetic enter should mark hover active");
        return false;
    }
    sendLeave();
    if (canvas.hoverActiveForTest()) {
        *error = QStringLiteral("leave should clear hover state");
        return false;
    }
    QTest::qWait(250);  // well past the 16ms debounce, even on loaded runners
    if (!canvas.probeText().isEmpty()) {
        *error = QStringLiteral("probe must stay empty after leave, got: %1")
                     .arg(canvas.probeText().left(120));
        return false;
    }
    // And a settled hover must still produce a readout (no over-correction).
    // This drives the same updateProbe the timer would call.
    sendHover(probePoint);
    if (!canvas.hoverActiveForTest()) {
        *error = QStringLiteral("re-enter should mark hover active again");
        return false;
    }
    canvas.inspectAt(probePoint);
    if (canvas.probeText().isEmpty() || !canvas.probeText().contains(QStringLiteral("dBZ"))) {
        *error = QStringLiteral("settled hover should still probe, got: %1")
                     .arg(canvas.probeText().left(120));
        return false;
    }
    return true;
}

static bool testPlaybackSpeeds(MainWindow* window, QString* error) {
    auto* speeds = window->findChild<QComboBox*>(QStringLiteral("speedCombo"));
    auto* slider = window->findChild<QSlider*>(QStringLiteral("frameSlider"));
    if (!speeds || !slider) {
        *error = QStringLiteral("speedCombo or frameSlider missing");
        return false;
    }
    const QStringList expectedTexts{QStringLiteral("0.5x"), QStringLiteral("1x"), QStringLiteral("2x"),
                                   QStringLiteral("4x"), QStringLiteral("8x"),
                                   QStringLiteral("16x")};
    const QList<int> expectedData{2000, 1000, 500, 250, 125, 62};
    if (speeds->count() != expectedTexts.size()) {
        *error = QStringLiteral("speedCombo should list 6 speeds, got %1").arg(speeds->count());
        return false;
    }
    for (int i = 0; i < expectedTexts.size(); ++i) {
        if (speeds->itemText(i) != expectedTexts.at(i) ||
            speeds->itemData(i).toInt() != expectedData.at(i)) {
            *error = QStringLiteral("speedCombo item %1 should be %2/%3, got %4/%5")
                         .arg(i)
                         .arg(expectedTexts.at(i))
                         .arg(expectedData.at(i))
                         .arg(speeds->itemText(i))
                         .arg(speeds->itemData(i).toInt());
            return false;
        }
    }
    // Home/End with an empty timeline must be safe no-ops offscreen.
    const int before = slider->value();
    QTest::keyClick(window, Qt::Key_Home);
    QCoreApplication::processEvents();
    QTest::keyClick(window, Qt::Key_End);
    QCoreApplication::processEvents();
    if (slider->value() != before) {
        *error = QStringLiteral("Home/End on empty history should not move the frame slider");
        return false;
    }
    const int original = speeds->currentIndex();
    for (int i = 0; i < speeds->count(); ++i) {
        speeds->setCurrentIndex(i);
        QCoreApplication::processEvents();
        if (speeds->currentData().toInt() != expectedData.at(i)) {
            *error = QStringLiteral("speedCombo selection should track interval data");
            speeds->setCurrentIndex(original);
            return false;
        }
    }
    speeds->setCurrentIndex(original);
    QCoreApplication::processEvents();
    return true;
}

static bool testHealthIndicator(MainWindow* window, QString* error) {
    auto* dot = window->findChild<QLabel*>(QStringLiteral("healthDot"));
    auto* label = window->findChild<QLabel*>(QStringLiteral("healthLabel"));
    if (!dot || !label) {
        *error = QStringLiteral("healthDot/healthLabel widgets missing");
        return false;
    }
    const QString text = label->text();
    if (text != QStringLiteral("Unknown") && text != QStringLiteral("Online") &&
        text != QStringLiteral("Stale") && text != QStringLiteral("Offline")) {
        *error = QStringLiteral("healthLabel should show a known state, got %1").arg(text);
        return false;
    }
    if (!dot->styleSheet().contains(QStringLiteral("background-color"))) {
        *error = QStringLiteral("healthDot should carry a colored status stylesheet");
        return false;
    }
    return true;
}

static bool testContextMenuToolModes(QString* error) {
    RadarMapCanvas canvas;
    canvas.resize(800, 600);
    canvas.setProduct(ProductKind::Reflectivity);
    canvas.setCrossSectionMode(false);
    canvas.setMeasureMode(false);
    canvas.setCrossSectionMode(true);
    if (!canvas.crossSectionMode()) {
        *error = QStringLiteral("cross-section should enable on native products");
        return false;
    }
    // Context-menu behavior: enabling Measure clears Cross-section.
    canvas.setMeasureMode(true);
    if (canvas.crossSectionMode()) {
        *error = QStringLiteral("measure mode should clear cross-section mode");
        return false;
    }
    canvas.setMeasureMode(false);
    canvas.setCrossSectionMode(true);
    // Derived products (what the context menu disables) reject cross-section.
    canvas.setProduct(ProductKind::EchoTop);
    if (canvas.crossSectionMode()) {
        *error = QStringLiteral("derived product should clear cross-section mode");
        return false;
    }
    canvas.setProduct(ProductKind::Reflectivity);
    return true;
}

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    // Unbuffered stdout: crash runs must keep their progress markers.
    std::cout << std::unitbuf;
    application.setOrganizationName(QStringLiteral("CappedWeatherTests"));
    application.setApplicationName(QStringLiteral("CappedWeatherUiSmoke"));
    QStandardPaths::setTestModeEnabled(true);
    QSettings().clear();
    MainWindow window;
    window.show();
    application.processEvents();

    // Hermetic suite: sever the live feeds. The constructor fires real
    // NWS/SPC/GLM requests whose responses can land mid-suite and clobber
    // fixture overlays (a load-dependent flake). Env-gated live tests use
    // their own sources and are unaffected.
    if (auto* liveNexrad = window.findChild<NexradSource*>()) liveNexrad->disconnect();
    if (auto* liveOverlays = window.findChild<WeatherOverlaySource*>()) liveOverlays->disconnect();
    if (auto* liveGlm = window.findChild<GlmSource*>()) liveGlm->disconnect();
    if (auto* liveSpc = window.findChild<SpcConvectiveSource*>()) liveSpc->disconnect();
    // Same treatment for every source added since: context layers (model
    // HRRR, satellite GOES, METAR, NHC/WPC/HMS/DAT) fetch live payloads on
    // the overlay timer too, and a 4 MB GOES frame landing mid-suite costs
    // seconds of main-thread parsing per panel repaint.
    if (auto* liveNhc = window.findChild<NhcCycloneSource*>()) liveNhc->disconnect();
    if (auto* liveWpc = window.findChild<WpcEroSource*>()) liveWpc->disconnect();
    if (auto* liveHms = window.findChild<HmsSmokeSource*>()) liveHms->disconnect();
    if (auto* liveDat = window.findChild<DatPathSource*>()) liveDat->disconnect();
    if (auto* liveHrrr = window.findChild<HrrrSource*>()) liveHrrr->disconnect();
    if (auto* liveGoes = window.findChild<GoesSource*>()) liveGoes->disconnect();
    if (auto* liveMosaic = window.findChild<MosaicSource*>()) liveMosaic->disconnect();
    if (auto* liveMetar = window.findChild<MetarSource*>()) liveMetar->disconnect();

    auto* map = window.findChild<RadarMapCanvas*>(QStringLiteral("radarMap"));
    auto* secondMap = window.findChild<RadarMapCanvas*>(QStringLiteral("radarMapPanel2"));
    auto* products = window.findChild<QComboBox*>(QStringLiteral("productCombo"));
    auto* tiltCombo = window.findChild<QComboBox*>(QStringLiteral("tiltCombo"));
    auto* crossSectionButton = window.findChild<QToolButton*>(QStringLiteral("crossSectionButton"));
    auto* frameSlider = window.findChild<QSlider*>(QStringLiteral("frameSlider"));
    auto* frameLabel = window.findChild<QLabel*>(QStringLiteral("frameLabel"));
    auto* playButton = window.findChild<QToolButton*>(QStringLiteral("playButton"));
    auto* snapshotButton = window.findChild<QToolButton*>(QStringLiteral("snapshotButton"));
    auto* timelineButton = window.findChild<QToolButton*>(QStringLiteral("timelineButton"));
    auto* warningsCheck = window.findChild<QCheckBox*>(QStringLiteral("warningsCheck"));
    auto* reportsCheck = window.findChild<QCheckBox*>(QStringLiteral("reportsCheck"));
    auto* lightningCheck = window.findChild<QCheckBox*>(QStringLiteral("lightningCheck"));
    auto* lightningFusionCheck = window.findChild<QCheckBox*>(QStringLiteral("lightningFusionCheck"));
    auto* placefilesCheck = window.findChild<QCheckBox*>(QStringLiteral("placefilesCheck"));
    auto* addPlacefileButton = window.findChild<QToolButton*>(QStringLiteral("addPlacefileButton"));
    auto* addPlacefileUrlButton = window.findChild<QToolButton*>(QStringLiteral("addPlacefileUrlButton"));
    auto* clearPlacefilesButton = window.findChild<QToolButton*>(QStringLiteral("clearPlacefilesButton"));
    auto* openArchiveFilesButton = window.findChild<QToolButton*>(QStringLiteral("openArchiveFilesButton"));
    auto* openArchiveFolderButton = window.findChild<QToolButton*>(QStringLiteral("openArchiveFolderButton"));
    auto* awsArchiveButton = window.findChild<QToolButton*>(QStringLiteral("awsArchiveButton"));
    auto* glmStatusNote = window.findChild<QLabel*>(QStringLiteral("glmStatusNote"));
    auto* belowThresholdCheck = window.findChild<QCheckBox*>(QStringLiteral("belowThresholdCheck"));
    auto* stationCombo = window.findChild<QComboBox*>(QStringLiteral("stationCombo"));
    auto* liveButton = window.findChild<QToolButton*>(QStringLiteral("liveButton"));
    auto* panelLayout = window.findChild<QComboBox*>(QStringLiteral("panelLayoutCombo"));
    auto* controlDock = window.findChild<QDockWidget*>(QStringLiteral("controlDock"));
    const auto radarPanels = window.findChildren<RadarMapCanvas*>();
    auto* loadingProgress = window.findChild<QProgressBar*>();
    auto* model = window.findChild<RadarListModel*>();
    if (!map || !secondMap || !products || products->count() != 10 ||
        products->findData(static_cast<int>(ProductKind::EchoTop)) < 0 ||
        products->findData(static_cast<int>(ProductKind::VerticallyIntegratedLiquid)) < 0 ||
        products->findData(static_cast<int>(ProductKind::CompositeReflectivity)) < 0 ||
        products->findData(static_cast<int>(ProductKind::Cappi)) < 0 ||
        !tiltCombo || !crossSectionButton || !frameSlider || !frameLabel || !playButton ||
        !snapshotButton || !timelineButton || !warningsCheck || !reportsCheck ||
        !lightningCheck || !lightningFusionCheck || !placefilesCheck ||
        !addPlacefileButton || !addPlacefileUrlButton || !clearPlacefilesButton ||
        !openArchiveFilesButton || !openArchiveFolderButton || !awsArchiveButton ||
        !belowThresholdCheck ||
        !stationCombo || stationCombo->count() < 200 || !liveButton || !loadingProgress ||
        !model || model->rowCount() < 200 || !panelLayout || panelLayout->count() != 4 ||
        !controlDock || controlDock->width() < 300 ||
        radarPanels.size() != 4 || !glmStatusNote) {
        std::cerr << "UI smoke setup failed\n";
        return 1;
    }
    auto* emptyStateHint = window.findChild<QLabel*>(QStringLiteral("emptyStateHint"));
    if (!emptyStateHint || emptyStateHint->text().isEmpty()) {
        std::cerr << "Empty-state hint is missing or empty\n";
        return 1;
    }

    const QByteArray placefileBytes = R"PF(
; GR-compatible subset fixture
Title: Chase network
RefreshSeconds: 45
Threshold: 125
Color: 12 34 56
Place: 35.222, -97.439, Norman spotter
Text: 35.300, -97.500, 1, "TEAM 7", "Moving east"
Icon: 35.400, -97.600, 270, 1, 2, "Unit 42"
Line: 3, 0, "County boundary"
 35.0, -98.0
 35.0, -97.0
End:
Polygon:
 35.1, -97.9
 35.1, -97.7
 35.3, -97.7
 35.1, -97.9
End:
UnsupportedThing: ignored safely
)PF";
    QString placefileError;
    const auto placefile = parsePlacefile(placefileBytes, &placefileError);
    if (!placefile.has_value() || !placefileError.isEmpty() ||
        placefile->title != QStringLiteral("Chase network") ||
        placefile->refreshSeconds != 45 || placefile->elements.size() != 5 ||
        placefile->warnings.size() != 1 ||
        placefile->elements.at(0).kind != PlacefileElementKind::Marker ||
        placefile->elements.at(0).text != QStringLiteral("Norman spotter") ||
        placefile->elements.at(0).color != QColor(12, 34, 56) ||
        placefile->elements.at(0).thresholdNauticalMiles != 125.0 ||
        placefile->elements.at(1).kind != PlacefileElementKind::Text ||
        placefile->elements.at(1).hoverText != QStringLiteral("Moving east") ||
        placefile->elements.at(2).kind != PlacefileElementKind::Icon ||
        placefile->elements.at(2).rotationDegrees != 270.0 ||
        placefile->elements.at(3).kind != PlacefileElementKind::Line ||
        placefile->elements.at(3).points.size() != 2 ||
        placefile->elements.at(3).lineWidth != 3 ||
        placefile->elements.at(4).kind != PlacefileElementKind::Polygon ||
        placefile->elements.at(4).points.size() != 4) {
        std::cerr << "GR placefile subset parsing failed: " << placefileError.toStdString()
                  << " title=" << (placefile ? placefile->title.toStdString() : "<none>")
                  << " refresh=" << (placefile ? placefile->refreshSeconds : -1)
                  << " elements=" << (placefile ? placefile->elements.size() : -1)
                  << " warnings=" << (placefile ? placefile->warnings.join(QStringLiteral(" | ")).toStdString()
                                                   : "<none>") << '\n';
        return 1;
    }
    const auto invalidPlacefile = parsePlacefile(
        QByteArray("Place: 95, -97, impossible\n"), &placefileError);
    if (invalidPlacefile.has_value() || !placefileError.contains(QStringLiteral("no drawable"))) {
        std::cerr << "Invalid placefile coordinates were accepted\n";
        return 1;
    }
    const auto placefileUrl = qEnvironmentVariable("CAPPEDWEATHER_PLACEFILE_URL");
    if (!placefileUrl.isEmpty()) {
        PlacefileSource networkPlacefileSource;
        QSignalSpy placefileLoaded(&networkPlacefileSource, &PlacefileSource::documentsChanged);
        if (!networkPlacefileSource.addUrl(QUrl(placefileUrl), &placefileError) ||
            !placefileLoaded.wait(30000) || networkPlacefileSource.documents().isEmpty()) {
            std::cerr << "HTTP placefile fetch/parse failed: " << placefileError.toStdString() << '\n';
            return 1;
        }
    }

    const auto archivePut16 = [](QByteArray& bytes, const qsizetype offset, const std::uint16_t value) {
        bytes[offset] = static_cast<char>(value >> 8U);
        bytes[offset + 1] = static_cast<char>(value & 0xffU);
    };
    const auto archivePut32 = [](QByteArray& bytes, const qsizetype offset, const std::uint32_t value) {
        bytes[offset] = static_cast<char>(value >> 24U);
        bytes[offset + 1] = static_cast<char>((value >> 16U) & 0xffU);
        bytes[offset + 2] = static_cast<char>((value >> 8U) & 0xffU);
        bytes[offset + 3] = static_cast<char>(value & 0xffU);
    };
    QByteArray syntheticRadial(103, '\0');
    syntheticRadial.replace(0, 4, "KTLX");
    archivePut16(syntheticRadial, 18, 103);
    archivePut32(syntheticRadial, 12, std::bit_cast<std::uint32_t>(12.5F));
    syntheticRadial[20] = 1;
    syntheticRadial[22] = 3;
    archivePut32(syntheticRadial, 24, std::bit_cast<std::uint32_t>(0.5F));
    archivePut16(syntheticRadial, 30, 10);
    archivePut32(syntheticRadial, 44, 72);
    syntheticRadial.replace(72, 4, "DREF");
    archivePut16(syntheticRadial, 80, 3);
    archivePut16(syntheticRadial, 82, static_cast<std::uint16_t>(static_cast<std::int16_t>(-375)));
    archivePut16(syntheticRadial, 84, 250);
    syntheticRadial[91] = 8;
    archivePut32(syntheticRadial, 92, std::bit_cast<std::uint32_t>(2.0F));
    archivePut32(syntheticRadial, 96, std::bit_cast<std::uint32_t>(66.0F));
    syntheticRadial[100] = 66;
    syntheticRadial[101] = 67;
    syntheticRadial[102] = 68;
    unsigned int compressedSize = static_cast<unsigned int>(syntheticRadial.size() * 2 + 600);
    QByteArray compressed(static_cast<qsizetype>(compressedSize), '\0');
    if (BZ2_bzBuffToBuffCompress(compressed.data(), &compressedSize, syntheticRadial.data(),
                                 static_cast<unsigned int>(syntheticRadial.size()), 9, 0, 30) != BZ_OK) {
        std::cerr << "Synthetic Archive-II fixture compression failed\n";
        return 1;
    }
    compressed.resize(static_cast<qsizetype>(compressedSize));
    QByteArray archive(24, '\0');
    archive.replace(0, 10, "AR2V0006.\0");
    archive.replace(20, 4, "KTLX");
    QByteArray blockLength(4, '\0');
    archivePut32(blockLength, 0, compressedSize);
    archive.append(blockLength);
    archive.append(compressed);
    QString archiveError;
    const auto decodedArchive = decodeLevel2Archive(archive, &archiveError);
    if (!decodedArchive || decodedArchive->radarId != QStringLiteral("KTLX") ||
        decodedArchive->compressedRecords != 1 || decodedArchive->decoded.rays.size() != 1 ||
        decodedArchive->decoded.rays.front().firstGateMeters != -375) {
        std::cerr << "Archive-II bzip framing/decode failed: " << archiveError.toStdString() << '\n';
        return 1;
    }
    QTemporaryDir archiveDirectory;
    const auto localArchivePath = archiveDirectory.filePath(QStringLiteral("KTLX20260729_121500_V06"));
    QFile localArchiveFile(localArchivePath);
    if (!localArchiveFile.open(QIODevice::WriteOnly) || localArchiveFile.write(archive) != archive.size()) {
        std::cerr << "Could not create local Archive-II source fixture\n";
        return 1;
    }
    localArchiveFile.close();
    ArchiveSource localArchiveSource;
    QSignalSpy localReady(&localArchiveSource, &ArchiveSource::volumeReady);
    localArchiveSource.loadFiles(QStringLiteral("KTLX"), {localArchivePath});
    if (!localReady.wait(5000) || localReady.constFirst().at(0).toString() != QStringLiteral("KTLX") ||
        localReady.constFirst().at(4).toDateTime() !=
            QDateTime(QDate(2026, 7, 29), QTime(12, 15), QTimeZone::UTC)) {
        std::cerr << "Asynchronous local Archive-II source did not publish a timed volume\n";
        return 1;
    }
    auto truncatedArchive = archive.left(32);
    archivePut32(truncatedArchive, 24, 5000);
    if (decodeLevel2Archive(truncatedArchive, &archiveError).has_value() ||
        !archiveError.contains(QStringLiteral("truncated"), Qt::CaseInsensitive)) {
        std::cerr << "Truncated Archive-II record was accepted\n";
        return 1;
    }
    const QByteArray listingXml = R"XML(<?xml version="1.0"?>
<ListBucketResult>
 <Contents><Key>2026/07/29/KTLX/KTLX20260729_121500_V06</Key><Size>12345</Size></Contents>
 <Contents><Key>2026/07/29/KTLX/KTLX20260729_121500_V06_MDM</Key><Size>900</Size></Contents>
 <Contents><Key>2026/07/29/KOUN/KOUN20260729_121600_V06</Key><Size>12345</Size></Contents>
 <Contents><Key>2026/07/29/KTLX/KTLX20260729_123000_V06</Key><Size>23456</Size></Contents>
</ListBucketResult>)XML";
    const auto archiveObjects = parseAwsArchiveListing(listingXml, QStringLiteral("KTLX"), &archiveError);
    const auto selectedArchiveObjects = selectArchiveObjects(
        archiveObjects,
        QDateTime(QDate(2026, 7, 29), QTime(12, 20), QTimeZone::UTC),
        QDateTime(QDate(2026, 7, 29), QTime(12, 40), QTimeZone::UTC), 0);
    const auto latestArchiveObject = selectArchiveObjects(
        archiveObjects,
        QDateTime(QDate(2026, 7, 29), QTime(12, 0), QTimeZone::UTC),
        QDateTime(QDate(2026, 7, 29), QTime(13, 0), QTimeZone::UTC), 1);
    if (!archiveError.isEmpty() || archiveObjects.size() != 2 || selectedArchiveObjects.size() != 1 ||
        !selectedArchiveObjects.constFirst().key.endsWith(QStringLiteral("123000_V06")) ||
        latestArchiveObject.size() != 1 ||
        !latestArchiveObject.constFirst().key.endsWith(QStringLiteral("123000_V06"))) {
        std::cerr << "AWS Level-II listing selection failed: " << archiveError.toStdString() << '\n';
        return 1;
    }
    const auto realArchivePath = qEnvironmentVariable("CAPPEDWEATHER_ARCHIVE_FILE");
    if (!realArchivePath.isEmpty()) {
        QFile realArchive(realArchivePath);
        if (!realArchive.open(QIODevice::ReadOnly)) {
            std::cerr << "Could not open optional real Archive-II fixture\n";
            return 1;
        }
        const auto decodedRealArchive = decodeLevel2Archive(realArchive.readAll(), &archiveError);
        if (!decodedRealArchive || decodedRealArchive->decoded.rays.size() < 100 ||
            decodedRealArchive->compressedRecords < 1) {
            std::cerr << "Real public Archive-II decode failed: " << archiveError.toStdString() << '\n';
            return 1;
        }
    }
    if (qEnvironmentVariableIsSet("CAPPEDWEATHER_TEST_AWS_ARCHIVE")) {
        ArchiveSource awsArchiveSource;
        QSignalSpy awsReady(&awsArchiveSource, &ArchiveSource::volumeReady);
        QSignalSpy awsFailed(&awsArchiveSource, &ArchiveSource::requestFailed);
        awsArchiveSource.requestAws(
            QStringLiteral("KTLX"),
            QDateTime(QDate(2026, 7, 29), QTime(0, 2), QTimeZone::UTC),
            QDateTime(QDate(2026, 7, 29), QTime(0, 10), QTimeZone::UTC), 2);
        if (!awsReady.wait(120000) || (awsReady.size() < 2 && !awsReady.wait(120000))) {
            const auto failure = awsFailed.isEmpty() ? QStringLiteral("timed out")
                                                     : awsFailed.constLast().at(1).toString();
            std::cerr << "Live public AWS Archive-II source failed: " << failure.toStdString() << '\n';
            return 1;
        }
    }

    auto renderedPlacefile = *placefile;
    renderedPlacefile.source = QStringLiteral("fixture.placefile");
    map->setPlacefiles({renderedPlacefile});
    map->setView({-97.439, 35.222}, 10);
    application.processEvents();
    map->inspectAt(map->rect().center());
    if (map->placefileElementCount() != 5 ||
        !map->probeText().contains(QStringLiteral("Norman spotter"))) {
        std::cerr << "Placefile document did not reach map inspection/render pipeline\n";
        return 1;
    }
    placefilesCheck->setChecked(false);
    application.processEvents();
    map->inspectAt(map->rect().center());
    if (map->placefilesVisible() || map->probeText().contains(QStringLiteral("Norman spotter"))) {
        std::cerr << "Placefile visibility control did not hide hit testing\n";
        return 1;
    }
    placefilesCheck->setChecked(true);

    products->setCurrentIndex(products->findData(static_cast<int>(ProductKind::EchoTop)));
    application.processEvents();
    if (map->product() != ProductKind::EchoTop || tiltCombo->isEnabled() ||
        tiltCombo->currentText() != QStringLiteral("All tilts (volume derived)") ||
        crossSectionButton->isEnabled()) {
        std::cerr << "Derived product controls did not switch to all-tilt 2D mode\n";
        return 1;
    }
    products->setCurrentIndex(products->findData(static_cast<int>(ProductKind::Reflectivity)));
    application.processEvents();
    if (!crossSectionButton->isEnabled()) {
        std::cerr << "Cross-section control did not recover for native products\n";
        return 1;
    }

    window.resize(1024, 700);
    application.processEvents();
    // The control dock scrolls vertically: bring the last row into view,
    // then assert nothing sticks out horizontally at 1024 px.
    if (auto* controlScroll = controlDock->findChild<QScrollArea*>()) {
        controlScroll->ensureWidgetVisible(belowThresholdCheck);
        application.processEvents();
    }
    const auto belowThresholdBottomRight = belowThresholdCheck->mapTo(
        &window, belowThresholdCheck->rect().bottomRight());
    if (window.width() > 1024 || !window.rect().contains(belowThresholdBottomRight)) {
        std::cerr << "Responsive controls overflow at 1024 px: window=" << window.width()
                  << " controlRight=" << belowThresholdBottomRight.x()
                  << " contentRight=" << window.rect().right() << '\n';
        return 1;
    }
    const auto frameLabelBottomRight = frameLabel->mapTo(&window, frameLabel->rect().bottomRight());
    const auto snapshotTopLeft = snapshotButton->mapTo(&window, snapshotButton->rect().topLeft());
    const auto timelineBottomRight = timelineButton->mapTo(&window, timelineButton->rect().bottomRight());
    if (frameLabelBottomRight.x() >= snapshotTopLeft.x() ||
        !window.rect().contains(timelineBottomRight) ||
        frameLabel->width() < 185) {
        std::cerr << "Playback details overlap export controls: labelRight=" << frameLabelBottomRight.x()
                  << " snapshotLeft=" << snapshotTopLeft.x()
                  << " labelWidth=" << frameLabel->width()
                  << " timelineRight=" << timelineBottomRight.x() << '\n';
        return 1;
    }

    panelLayout->setCurrentIndex(3);
    application.processEvents();
    const auto visiblePanels = std::count_if(radarPanels.cbegin(), radarPanels.cend(),
                                             [](const RadarMapCanvas* panel) { return panel->isVisible(); });
    if (visiblePanels != 4) {
        std::cerr << "2x2 panel layout did not expose four radar canvases\n";
        return 1;
    }
    panelLayout->setCurrentIndex(1);
    application.processEvents();

    const auto* mapHost = map->parentWidget();
    if (!mapHost || std::abs(map->geometry().bottom() - mapHost->rect().bottom()) > 5 ||
        std::abs(secondMap->geometry().bottom() - mapHost->rect().bottom()) > 5) {
        std::cerr << "1x2 panel layout did not use the full available map height\n";
        return 1;
    }

    secondMap->setProduct(ProductKind::EchoTop);
    secondMap->setCrossSectionMode(true);
    secondMap->activated();
    application.processEvents();
    if (crossSectionButton->isEnabled() || secondMap->crossSectionMode()) {
        std::cerr << "Derived-product panel activation exposed cross-section controls\n";
        return 1;
    }
    const auto secondRadarIndex = stationCombo->findData(QStringLiteral("KTLX"));
    if (secondRadarIndex < 0) {
        std::cerr << "Shared panel radar was not available in the station picker\n";
        return 1;
    }
    stationCombo->setCurrentIndex(secondRadarIndex);
    stationCombo->activated(secondRadarIndex);
    application.processEvents();
    if (secondMap->radarId() != QStringLiteral("KTLX") || map->radarId() != QStringLiteral("KTLX")) {
        std::cerr << "Radar selection was not synchronized across panels\n";
        return 1;
    }
    map->activated();
    application.processEvents();
    if (!crossSectionButton->isEnabled()) {
        std::cerr << "Native-product panel activation did not restore cross-section controls\n";
        return 1;
    }
    panelLayout->setCurrentIndex(0);
    application.processEvents();

    map->setSites({{"TATL", "Atlanta", "TDWR", 33.6470, -84.2620, 327.7}});
    map->setView({-84.2620, 33.6470}, 6);
    application.processEvents();
    const auto targetPoint = map->radarScreenPoint(QStringLiteral("TATL"));
    if (!map->rect().contains(targetPoint)) {
        std::cerr << "Radar pill is not visible: point=" << targetPoint.x() << ',' << targetPoint.y()
                  << " rect=" << map->rect().width() << 'x' << map->rect().height()
                  << " visible=" << map->isVisible() << '\n';
        return 1;
    }
    QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, targetPoint);
    application.processEvents();
    if (map->radarId() != QStringLiteral("TATL")) {
        std::cerr << "Radar pill click did not select the requested radar\n";
        return 1;
    }

    products->setCurrentIndex(2);
    application.processEvents();
    if (map->product() != ProductKind::SpectrumWidth) {
        std::cerr << "Product selection did not reach the canvas\n";
        return 1;
    }

    const QByteArray alerts = R"JSON({
      "type":"FeatureCollection","features":[{
        "type":"Feature",
        "geometry":{"type":"Polygon","coordinates":[[[-98.0,35.0],[-97.0,35.0],[-97.0,36.0],[-98.0,35.0]],
                                                               [[-97.8,35.1],[-97.5,35.1],[-97.6,35.3],[-97.8,35.1]]]},
        "properties":{"event":"Tornado Warning","headline":"Test warning","severity":"Extreme",
                      "senderName":"NWS Norman","description":"Observed tornado. Long warning details for smooth scrolling. Long warning details for smooth scrolling. Long warning details for smooth scrolling. Long warning details for smooth scrolling. Long warning details for smooth scrolling. Long warning details for smooth scrolling. Long warning details for smooth scrolling. Long warning details for smooth scrolling.","instruction":"Take shelter now",
                      "onset":"2026-07-13T19:00:00+00:00","expires":"2026-07-13T20:00:00+00:00"}
      }]})JSON";
    QString parseError;
    const auto warnings = WeatherOverlaySource::parseAlerts(alerts, &parseError);
    if (!parseError.isEmpty() || warnings.size() != 1 || warnings.front().polygons.size() != 1 ||
        warnings.front().polygons.front().holes.size() != 1 || warnings.front().senderName != QStringLiteral("NWS Norman") ||
        warnings.front().alertPriority != 2 ||
        warnings.front().alertColor.name(QColor::HexRgb).compare(QStringLiteral("#ff0000"), Qt::CaseInsensitive) != 0) {
        std::cerr << "NWS alert parsing failed\n";
        return 1;
    }
    if (nwsAlertStyle(QStringLiteral("Tornado Warning")).priority != 2 ||
        nwsAlertStyle(QStringLiteral("This Is Not A Real Alert")).priority != 1000 ||
        nwsAlertStyle(QStringLiteral("winter weather advisory")).priority != 60 ||
        nwsAlertStyle(QStringLiteral("Winter Weather Advisory")).color.name(QColor::HexRgb) !=
            QStringLiteral("#7b68ee") ||
        nwsAlertStyle(QStringLiteral("Air Quality Alert")).color.name(QColor::HexRgb) !=
            QStringLiteral("#808080")) {
        std::cerr << "NWS official alert color table is incomplete or incorrect\n";
        return 1;
    }
    const QByteArray zone = R"JSON({"type":"Feature","geometry":{"type":"MultiPolygon","coordinates":[
        [[[-98.0,35.0],[-97.0,35.0],[-97.0,36.0],[-98.0,35.0]]]
    ]}})JSON";
    const auto zonePolygons = WeatherOverlaySource::parseZone(zone, &parseError);
    if (!parseError.isEmpty() || zonePolygons.size() != 1 || zonePolygons.front().exterior.size() != 4) {
        std::cerr << "NWS zone geometry parsing failed\n";
        return 1;
    }
    const QByteArray reports =
        "Time,Size,Location,County,State,Lat,Lon,Comments\n"
        "1830,1.75,Norman,Cleveland,OK,35.22,-97.44,Quarter-size hail\n";
    const auto stormReports = WeatherOverlaySource::parseStormReports(
        reports, StormReportKind::Hail, &parseError, QDate(2026, 7, 13));
    if (!parseError.isEmpty() || stormReports.size() != 1 ||
        stormReports.front().kind != StormReportKind::Hail ||
        stormReports.front().occurredAt != QDateTime(QDate(2026, 7, 13), QTime(18, 30), QTimeZone::UTC)) {
        std::cerr << "SPC report parsing failed\n";
        return 1;
    }
    const auto noReports = WeatherOverlaySource::parseStormReports(
        QByteArray{}, StormReportKind::Tornado, &parseError);
    if (!parseError.isEmpty() || !noReports.isEmpty()) {
        std::cerr << "Empty SPC report response should be a valid zero-report result\n";
        return 1;
    }
    // SPC outlook GeoJSON: Day 1 TSTM + MDT, official colors, ISO windows.
    const QByteArray outlook = R"JSON({"type":"FeatureCollection","features":[
      {"type":"Feature","geometry":{"type":"MultiPolygon","coordinates":[[[[-100.0,35.0],[-99.0,35.0],[-99.0,36.0],[-100.0,35.0]]]]},
       "properties":{"DN":2,"VALID":"202610081300","EXPIRE":"202610091200","ISSUE":"202610081246",
         "VALID_ISO":"2026-10-08T13:00:00+00:00","EXPIRE_ISO":"2026-10-09T12:00:00+00:00","ISSUE_ISO":"2026-10-08T12:46:00+00:00",
         "FORECASTER":"Guyer","LABEL":"TSTM","LABEL2":"General Thunderstorms Risk","stroke":"#55BB55","fill":"#C1E9C1"}},
      {"type":"Feature","geometry":{"type":"Polygon","coordinates":[[[-97.0,36.0],[-96.0,36.0],[-96.0,37.0],[-97.0,36.0]]]}},
      {"type":"Feature","geometry":{"type":"Polygon","coordinates":[[[-97.0,36.0],[-96.0,36.0],[-96.0,37.0],[-97.0,36.0]]]},"properties":{"DN":6,"LABEL":"MDT","LABEL2":"Moderate Risk"}}
    ]})JSON";
    const auto outlooks = parseSpcOutlookGeoJson(outlook, 1, &parseError);
    if (!parseError.isEmpty() || outlooks.size() != 2 || outlooks.front().day != 1 ||
        outlooks.front().label2 != QStringLiteral("General Thunderstorms Risk") ||
        outlooks.front().rings.size() != 1 ||
        outlooks.front().rings.front().exterior.size() != 4 ||
        outlooks.front().valid !=
            QDateTime(QDate(2026, 10, 8), QTime(13, 0), QTimeZone::UTC) ||
        outlooks.back().dn != 6 ||
        outlooks.back().label != QStringLiteral("MDT")) {
        std::cerr << "SPC outlook parsing failed\n";
        return 1;
    }
    // MCD RSS: one active discussion plus the quiet-state item (skipped).
    const QByteArray mcdRss = R"XML(<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0"><channel>
<item><title>Mesoscale Discussion 2346</title><link>https://www.spc.noaa.gov/products/md/md2346.html</link></item>
<item><title>SPC - No MDs are in effect</title><link>https://www.spc.noaa.gov/products/md/</link></item>
</channel></rss>)XML";
    const auto candidates = parseMcdRss(mcdRss, &parseError);
    if (!parseError.isEmpty() || candidates.size() != 1 || candidates.front().number != 2346) {
        std::cerr << "MCD RSS parsing failed\n";
        return 1;
    }
    const QByteArray mcdQuiet = R"XML(<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0"><channel>
<item><title>SPC - No MDs are in effect as of Tue Oct 7 2026</title><link>https://www.spc.noaa.gov/products/md/</link></item>
</channel></rss>)XML";
    if (!parseMcdRss(mcdQuiet, &parseError).isEmpty()) {
        std::cerr << "Quiet MCD RSS should parse to zero candidates\n";
        return 1;
    }
    // MCD discussion page: all anchors present, positive-west longitudes.
    const QByteArray mcdPage = R"HTML(<html><head><title>Storm Prediction Center Mesoscale Discussion 2346</title></head><body><pre>
Mesoscale Discussion 2346
NWS Storm Prediction Center Norman OK
0204 PM CDT Tue Oct 06 2026
Areas affected...west-central Florida Peninsula
Concerning...Severe potential...Watch unlikely
Valid 061904Z - 062100Z
Probability of Watch Issuance...5 percent
SUMMARY...The risk for a brief tornado will increase near Tampa Bay.
DISCUSSION...A small supercell west of Tampa Bay may move onshore.
..Leitman/Smith.. 10/06/2026
ATTN...WFO...TBW...
LAT...LON 28128331 28218317 28288300 28188269 28128331
</pre></body></html>)HTML";
    const auto mcd = parseMcdPage(mcdPage, &parseError);
    if (!parseError.isEmpty() || mcd.number != 2346 ||
        mcd.areas != QStringLiteral("west-central Florida Peninsula") ||
        mcd.watchProbabilityPercent != 5 ||
        mcd.validFrom != QDateTime(QDate(2026, 10, 6), QTime(19, 4), QTimeZone::UTC) ||
        mcd.validTo != QDateTime(QDate(2026, 10, 6), QTime(21, 0), QTimeZone::UTC) ||
        mcd.exterior.size() != 5 ||
        std::abs(mcd.exterior.front().x() + 83.31) > 0.001 ||
        std::abs(mcd.exterior.front().y() - 28.12) > 0.001) {
        std::cerr << "MCD page parsing failed\n";
        return 1;
    }
    if (!parseMcdPage(QByteArray("<html><body>no discussion here</body></html>"), &parseError).exterior.isEmpty() ||
        parseError.isEmpty()) {
        std::cerr << "Non-MCD page should fail closed with an error\n";
        return 1;
    }
    // SPC fire-weather GeoJSON: same schema, DryT/WindRH kinds tagged by the
    // fetcher; live-shaped Elevated payload plus the "No Areas" quiet state
    // (GeometryCollection with DN 0, which parses to zero features).
    const QByteArray fireLive = R"JSON({"type":"FeatureCollection","features":[
      {"type":"Feature","geometry":{"type":"MultiPolygon","coordinates":[[[[-119.43,38.25],[-120.02,38.75],[-120.27,39.28],[-119.43,38.25]]]]},
       "properties":{"DN":5,"VALID":"202610101200","EXPIRE":"202610111200","ISSUE":"202610100600",
         "VALID_ISO":"2026-10-10T12:00:00+00:00","EXPIRE_ISO":"2026-10-11T12:00:00+00:00","ISSUE_ISO":"2026-10-10T06:00:00+00:00",
         "FORECASTER":"Chalmers","LABEL":"ELEV","LABEL2":"Elevated Fire Risk","stroke":"#FF7F00","fill":"#FFBF80"}}
    ]})JSON";
    const auto fire = parseSpcOutlookGeoJson(fireLive, 1, &parseError);
    if (!parseError.isEmpty() || fire.size() != 1 || fire.front().dn != 5 ||
        fire.front().label != QStringLiteral("ELEV") ||
        fire.front().label2 != QStringLiteral("Elevated Fire Risk") ||
        fire.front().stroke != QStringLiteral("#FF7F00") ||
        fire.front().fill != QStringLiteral("#FFBF80") ||
        fire.front().rings.front().exterior.size() != 4) {
        std::cerr << "SPC fire outlook parsing failed\n";
        return 1;
    }
    const QByteArray fireQuiet = R"JSON({"type":"FeatureCollection","features":[
      {"type":"Feature","geometry":{"type":"GeometryCollection","geometries":[]},
       "properties":{"DN":0,"LABEL":"No Areas","LABEL2":"","stroke":"","fill":""}}
    ]})JSON";
    if (!parseSpcOutlookGeoJson(fireQuiet, 1, &parseError).isEmpty() || !parseError.isEmpty()) {
        std::cerr << "Quiet fire outlook should parse to zero features\n";
        return 1;
    }
    // NHC cyclones: live-shaped active feed (two storm folders + a
    // non-storm folder), cone ring and track line in real KML shape.
    const QByteArray nhcActive = R"XML(<?xml version="1.0" encoding="UTF-8"?>
<kml xmlns="http://www.opengis.net/kml/2.2"><Document id="active">
<Folder id="wsp"><name>Wind Speed Probabilities (120 hours)</name>
<NetworkLink id="wsp34"><name>34-kt</name><Link><href>https://example.invalid/wsp34.kmz</href></Link></NetworkLink>
</Folder>
<Folder id="ep5"><name>Hurricane Simon</name>
<NetworkLink id="cone"><Link><href>https://www.nhc.noaa.gov/storm_graphics/api/EP202026_012Aadv_CONE.kmz</href></Link></NetworkLink>
<NetworkLink id="track"><Link><href>https://www.nhc.noaa.gov/storm_graphics/api/EP202026_012Aadv_TRACK.kmz</href></Link></NetworkLink>
</Folder>
<Folder id="ep3"><name>Tropical Storm Rachel</name>
<NetworkLink id="cone"><Link><href>https://www.nhc.noaa.gov/storm_graphics/api/EP182026_053Aadv_CONE.kmz</href></Link></NetworkLink>
<NetworkLink id="track"><Link><href>https://www.nhc.noaa.gov/storm_graphics/api/EP182026_053Aadv_TRACK.kmz</href></Link></NetworkLink>
</Folder>
</Document></kml>)XML";
    const auto nhcLinks = parseNhcActiveKml(nhcActive, &parseError);
    if (!parseError.isEmpty() || nhcLinks.size() != 2 || nhcLinks.front().id != QStringLiteral("EP202026") ||
        nhcLinks.front().name != QStringLiteral("Hurricane Simon") ||
        !nhcLinks.front().coneUrl.endsWith(QStringLiteral("EP202026_012Aadv_CONE.kmz")) ||
        !nhcLinks.front().trackUrl.endsWith(QStringLiteral("EP202026_012Aadv_TRACK.kmz")) ||
        nhcLinks.back().id != QStringLiteral("EP182026")) {
        std::cerr << "NHC active feed parsing failed\n";
        return 1;
    }
    if (!parseNhcActiveKml(QByteArray("<kml><broken"), &parseError).isEmpty() ||
        parseError.isEmpty()) {
        std::cerr << "Malformed NHC feed should fail closed with an error\n";
        return 1;
    }
    const QByteArray nhcCone = R"XML(<?xml version='1.0' encoding='UTF-8'?>
<kml xmlns='http://earth.google.com/kml/2.1'><Document>
<name>Hurricane Simon (Advisory #12A) - Forecast Track Uncertainty</name>
<Placemark><Polygon><outerBoundaryIs><LinearRing><coordinates>
-104.79318,17.033,0 -104.7859,17.0334,0 -104.77814,17.0346,0 -104.75231,17.03969,0
</coordinates></LinearRing></outerBoundaryIs></Polygon></Placemark>
</Document></kml>)XML";
    const auto coneRing = parseNhcConeRing(nhcCone, &parseError);
    if (!parseError.isEmpty() || coneRing.size() != 4 ||
        std::abs(coneRing.front().x() + 104.79318) > 0.00001 ||
        std::abs(coneRing.front().y() - 17.033) > 0.00001) {
        std::cerr << "NHC cone ring parsing failed\n";
        return 1;
    }
    if (!parseNhcConeRing(QByteArray("<kml><Document></Document></kml>"), &parseError).isEmpty() ||
        parseError.isEmpty()) {
        std::cerr << "Coneless KML should fail closed with an error\n";
        return 1;
    }
    const QByteArray nhcTrack = R"XML(<?xml version='1.0' encoding='UTF-8'?>
<kml xmlns='http://earth.google.com/kml/2.1'><Document>
<Placemark><LineString><coordinates>-104.8,17.3,0 -104.7,17.8,0 -104.9,18.8,0</coordinates></LineString></Placemark>
<Placemark><Point><coordinates>-104.8,17.3,0</coordinates></Point></Placemark>
</Document></kml>)XML";
    const auto trackLine = parseNhcTrackLine(nhcTrack, &parseError);
    if (!parseError.isEmpty() || trackLine.size() != 3 ||
        std::abs(trackLine.back().x() + 104.9) > 0.00001) {
        std::cerr << "NHC track line parsing failed\n";
        return 1;
    }
    // KMZ extraction: a stored (uncompressed) .kml entry needs no zlib and
    // exercises the header walk; garbage fails closed.
    {
        const QByteArray payload("<kml><Document><name>stored</name></Document></kml>");
        const QByteArray kmlName("t.kml");
        QByteArray kmz;
        QDataStream stream(&kmz, QIODevice::WriteOnly);
        stream.setByteOrder(QDataStream::LittleEndian);
        stream << static_cast<quint32>(0x04034B50) << static_cast<quint16>(20)
               << static_cast<quint16>(0) << static_cast<quint16>(0) << static_cast<quint16>(0)
               << static_cast<quint16>(0) << static_cast<quint32>(0)
               << static_cast<quint32>(payload.size()) << static_cast<quint32>(payload.size())
               << static_cast<quint16>(kmlName.size()) << static_cast<quint16>(0);
        stream.writeRawData(kmlName.constData(), kmlName.size());
        kmz.append(payload);
        QString kmzError;
        const auto extracted = extractKmlFromKmz(kmz, &kmzError);
        if (extracted != payload || !kmzError.isEmpty()) {
            std::cerr << "Stored KMZ entry should extract verbatim\n";
            return 1;
        }
        if (!extractKmlFromKmz(QByteArray("not a zip"), &kmzError).isEmpty() ||
            kmzError.isEmpty()) {
            std::cerr << "Garbage KMZ should fail closed with an error\n";
            return 1;
        }
    }
    // WPC ERO GeoJSON: live-shaped Day 1 Marginal + Slight, official
    // renderer colors, holes reduced to the exterior ring, UTC windows.
    const QByteArray wpcLive = R"JSON({"type":"FeatureCollection","features":[
      {"type":"Feature","id":1,"geometry":{"type":"MultiPolygon","coordinates":[[[[-83.21,27.79],[-83.10,27.63],[-83.17,27.78],[-83.21,27.79]]]]},
       "properties":{"objectid":1,"outlook":"Marginal (At Least 5%)","dn":1,"valid_time":"12Z 10/10/26 - 12Z 10/11/26",
         "issue_time":"2026-10-10 07:45:00","start_time":"2026-10-10 12:00:00","end_time":"2026-10-11 12:00:00"}},
      {"type":"Feature","id":3,"geometry":{"type":"Polygon","coordinates":[[[-83.17,27.78],[-82.92,27.26],[-82.81,27.3],[-83.17,27.78]],[[-83.41,33.37],[-84.01,33.27],[-84.22,33.42],[-83.41,33.37]]]},
       "properties":{"objectid":3,"outlook":"Slight (At Least 15%)","dn":2,"valid_time":"12Z 10/10/26 - 12Z 10/11/26",
         "issue_time":"2026-10-10 07:45:00","start_time":"2026-10-10 12:00:00","end_time":"2026-10-11 12:00:00"}}
    ]})JSON";
    const auto wpc = parseWpcEroGeoJson(wpcLive, 1, &parseError);
    if (!parseError.isEmpty() || wpc.size() != 2 || wpc.front().day != 1 ||
        wpc.front().fireKind != QStringLiteral("ERO") ||
        wpc.front().label != QStringLiteral("MRGL") ||
        wpc.front().label2 != QStringLiteral("Marginal (At Least 5%)") ||
        wpc.front().fill != QStringLiteral("#38A800") ||
        wpc.front().stroke != QStringLiteral("#00734C") ||
        wpc.front().rings.size() != 1 ||
        wpc.front().valid !=
            QDateTime(QDate(2026, 10, 10), QTime(12, 0), QTimeZone::UTC) ||
        wpc.front().expire !=
            QDateTime(QDate(2026, 10, 11), QTime(12, 0), QTimeZone::UTC) ||
        wpc.back().label != QStringLiteral("SLGT") ||
        wpc.back().fill != QStringLiteral("#FFFE00") ||
        wpc.back().rings.front().exterior.size() != 4) {
        std::cerr << "WPC ERO parsing failed\n";
        return 1;
    }
    const QByteArray wpcQuiet = R"JSON({"type":"FeatureCollection","features":[]})JSON";
    if (!parseWpcEroGeoJson(wpcQuiet, 2, &parseError).isEmpty() || !parseError.isEmpty()) {
        std::cerr << "Empty WPC outlook should parse to zero features\n";
        return 1;
    }
    if (!parseWpcEroGeoJson(QByteArray("not json"), 1, &parseError).isEmpty() ||
        parseError.isEmpty()) {
        std::cerr << "Malformed WPC payload should fail closed with an error\n";
        return 1;
    }
    // HMS smoke KML: live-shaped light + heavy plumes (styleUrl density,
    // CDATA satellite/times, newline-separated triples), overlay-only
    // Placemarks skipped, day-stamped URL builder.
    const QByteArray hmsLive = R"XML(<?xml version="1.0" encoding="UTF-8"?>
<kml xmlns="http://www.opengis.net/kml/2.2"><Document>
<Placemark><description><![CDATA[<div>Start Time: 2026282 1220UTC<br>End Time: 2026282 1500UTC<br>Density: Light<br>Satellite: GOES-WEST</div>]]></description>
<styleUrl>#Smoke_Light_style</styleUrl>
<Polygon><outerBoundaryIs><LinearRing><coordinates>
-93.24,31.36,0 -93.25,31.35,0 -93.26,31.34,0 -93.24,31.36,0
</coordinates></LinearRing></outerBoundaryIs></Polygon></Placemark>
<Placemark><description><![CDATA[<div>Start Time: 2026282 1220UTC<br>End Time: 2026282 1500UTC<br>Density: Heavy<br>Satellite: GOES-EAST</div>]]></description>
<styleUrl>#Smoke_Heavy</styleUrl>
<Polygon><outerBoundaryIs><LinearRing><coordinates>
-90.10,32.40,0 -90.20,32.30,0 -90.30,32.35,0 -90.10,32.40,0
</coordinates></LinearRing></outerBoundaryIs></Polygon></Placemark>
<Placemark><name>NOAA logo</name><ScreenOverlay><Icon><href>https://example.invalid/logo.png</href></Icon></ScreenOverlay></Placemark>
</Document></kml>)XML";
    const auto smoke = parseHmsSmokeKml(hmsLive, &parseError);
    if (!parseError.isEmpty() || smoke.size() != 2 ||
        smoke.front().density != HmsSmokeDensity::Light ||
        smoke.front().satellite != QStringLiteral("GOES-WEST") ||
        smoke.front().startText != QStringLiteral("2026282 1220UTC") ||
        smoke.front().ring.size() != 4 ||
        std::abs(smoke.front().ring.front().x() + 93.24) > 0.00001 ||
        smoke.back().density != HmsSmokeDensity::Heavy ||
        hmsSmokeDensityLabel(HmsSmokeDensity::Medium) != QStringLiteral("medium")) {
        std::cerr << "HMS smoke parsing failed\n";
        return 1;
    }
    // Clean-air day: overlays only, zero plumes, clear error.
    const QByteArray hmsQuiet = R"XML(<?xml version="1.0" encoding="UTF-8"?>
<kml xmlns="http://www.opengis.net/kml/2.2"><Document><name>Smoke-20261010</name>
<Placemark><name>Legend</name></Placemark>
</Document></kml>)XML";
    if (!parseHmsSmokeKml(hmsQuiet, &parseError).isEmpty() || !parseError.isEmpty()) {
        std::cerr << "Clean-air smoke file should parse to zero plumes\n";
        return 1;
    }
    if (!parseHmsSmokeKml(QByteArray("<kml><broken"), &parseError).isEmpty() ||
        parseError.isEmpty()) {
        std::cerr << "Malformed smoke KML should fail closed with an error\n";
        return 1;
    }
    if (hmsSmokeUrl(QDate(2026, 10, 10)).toString() !=
        QStringLiteral("https://satepsanone.nesdis.noaa.gov/pub/FIRE/web/HMS/Smoke_Polygons/"
                       "KML/2026/10/hms_smoke20261010.kml")) {
        std::cerr << "HMS smoke URL builder is wrong: "
                  << hmsSmokeUrl(QDate(2026, 10, 10)).toString().toStdString() << '\n';
        return 1;
    }
    // HMS fire text: live-shaped header plus detections (missing FRP stays
    // NaN, bad rows drop), day-stamped URL builder.
    const QByteArray hmsFires = "        Lon,        Lat, YearDay, Time,       Satellite,           Method, Ecosystem,        FRP\n"
                                " -96.431657,  34.289140, 2026283, 0201,       GOES-EAST,         ANALYSIS,        91,   -999.000\n"
                                "-101.698970,  48.485158, 2026283, 1530,       GOES-WEST,         ANALYSIS,        30,    12.500\n"
                                "999.0, 48.0, 2026283, 0201, GOES-EAST, ANALYSIS, 30, 5.0\n"
                                "short,row\n";
    QString fireError;
    const auto fires = parseHmsFireText(hmsFires, &fireError);
    if (!fireError.isEmpty() || fires.size() != 2 ||
        std::abs(fires.front().longitudeDeg + 96.431657) > 0.000001 ||
        std::abs(fires.front().latitudeDeg - 34.28914) > 0.000001 ||
        !std::isnan(fires.front().frpMw) || fires.front().satellite != QStringLiteral("GOES-EAST") ||
        fires.front().yearDay != 2026283 || fires.front().timeHhmm != 201 ||
        std::abs(fires.back().frpMw - 12.5) > 0.001) {
        std::cerr << "HMS fire text parsing failed\n";
        return 1;
    }
    if (hmsFireUrl(QDate(2026, 10, 10)).toString() !=
        QStringLiteral("https://satepsanone.nesdis.noaa.gov/pub/FIRE/web/HMS/Fire_Points/"
                       "Text/2026/10/hms_fire20261010.txt")) {
        std::cerr << "HMS fire URL builder is wrong\n";
        return 1;
    }
    // DAT survey paths: live-shaped EF1 + EF0 LineStrings (epoch-ms dates),
    // EF severity ramp, drawable-lines-only rule.
    const QByteArray datLive = R"JSON({"type":"FeatureCollection","features":[
      {"type":"Feature","id":2555,"geometry":{"type":"LineString","coordinates":[
        [-85.656585339,32.536967769],[-85.649847629,32.541236855],[-85.640964153,32.547567751]]},
       "properties":{"objectid":2555,"event_id":"","efscale":"EF1","stormdate":1321444320000,
         "startlat":32.537,"startlon":-85.6566,"endlat":32.547,"endlon":-85.641}},
      {"type":"Feature","id":2558,"geometry":{"type":"LineString","coordinates":[
        [-86.974216057,32.869205322],[-86.968207373,32.870503387]]},
       "properties":{"objectid":2558,"event_id":null,"efscale":"EF0","stormdate":1324560300000,
         "startlat":32.8692,"startlon":-86.9742,"endlat":32.8796,"endlon":-86.9307}},
      {"type":"Feature","id":9,"geometry":{"type":"Point","coordinates":[-86.9,32.4]},
       "properties":{"objectid":9,"efscale":"EF0"}}
    ]})JSON";
    const auto datPaths = parseDatPaths(datLive, &parseError);
    if (!parseError.isEmpty() || datPaths.size() != 2 ||
        datPaths.front().efScale != QStringLiteral("EF1") ||
        datPaths.front().line.size() != 3 ||
        std::abs(datPaths.front().line.front().x() + 85.656585339) > 0.000001 ||
        datPaths.front().stormDate !=
            QDateTime::fromMSecsSinceEpoch(1321444320000, QTimeZone::UTC) ||
        datPaths.back().efScale != QStringLiteral("EF0")) {
        std::cerr << "DAT path parsing failed\n";
        return 1;
    }
    {
        QString stroke;
        QString fill;
        datPathColors(QStringLiteral("EF3"), stroke, fill);
        QString mildStroke;
        QString mildFill;
        datPathColors(QStringLiteral("EF0"), mildStroke, mildFill);
        QString unknownStroke;
        QString unknownFill;
        datPathColors(QString{}, unknownStroke, unknownFill);
        if (stroke != QStringLiteral("#FF3B4D") || mildStroke != QStringLiteral("#53D769") ||
            unknownStroke != mildStroke || fill.isEmpty()) {
            std::cerr << "DAT EF colors are wrong\n";
            return 1;
        }
    }
    const QByteArray datQuiet = R"JSON({"type":"FeatureCollection","features":[]})JSON";
    if (!parseDatPaths(datQuiet, &parseError).isEmpty() || !parseError.isEmpty()) {
        std::cerr << "Empty DAT response should parse to zero paths\n";
        return 1;
    }
    if (!parseDatPaths(QByteArray("not json"), &parseError).isEmpty() ||
        parseError.isEmpty()) {
        std::cerr << "Malformed DAT payload should fail closed with an error\n";
        return 1;
    }
    // HRRR subset URL: radar-centered box clamped to CONUS, one valid time.
    {
        const auto url = hrrrSubsetUrl(
            35.3331, -97.2778, 3.0, QDateTime(QDate(2026, 10, 10), QTime(13, 0), QTimeZone::UTC));
        const auto text = url.toString();
        if (!url.isValid() || !text.contains(QStringLiteral("Composite_reflectivity_entire_atmosphere")) ||
            !text.contains(QStringLiteral("north=38.333")) ||
            !text.contains(QStringLiteral("south=32.333")) ||
            !text.contains(QStringLiteral("2026-10-10T13")) || !text.contains(QStringLiteral("accept=netCDF"))) {
            std::cerr << "HRRR subset URL is wrong: " << text.toStdString() << '\n';
            return 1;
        }
        if (hrrrSubsetUrl(35.0, -97.0, 3.0, QDateTime()).isValid() ||
            hrrrSubsetUrl(std::numeric_limits<double>::quiet_NaN(), -97.0, 3.0,
                          QDateTime(QDate(2026, 10, 10), QTime(13, 0), QTimeZone::UTC))
                .isValid()) {
            std::cerr << "HRRR URL builder should reject bad input\n";
            return 1;
        }
        const auto at = [](int hour, int minute) {
            return QDateTime(QDate(2026, 10, 10), QTime(hour, minute), QTimeZone::UTC);
        };
        if (latestHrrrTime(at(15, 30)) != at(13, 0) || latestHrrrTime(at(14, 5)) != at(12, 0) ||
            latestHrrrTime(QDateTime()).isValid()) {
            std::cerr << "HRRR clock is wrong\n";
            return 1;
        }
        // Lead hours ride the same clock (verified live: +3 h resolves).
        if (hrrrValidTime(at(15, 30), 0) != at(13, 0) || hrrrValidTime(at(15, 30), 3) != at(16, 0) ||
            hrrrValidTime(at(15, 30), 6) != at(19, 0) || hrrrValidTime(at(15, 30), -1).isValid() ||
            hrrrValidTime(at(15, 30), 19).isValid() || hrrrValidTime(QDateTime(), 2).isValid()) {
            std::cerr << "HRRR lead clock is wrong\n";
            return 1;
        }
    }
    // GOES listing URL, newest-key pick, and slot clock: hermetic against
    // hand-built XML in the live ListBucketResult shape.
    {
        const auto url = goesListUrl(QStringLiteral("noaa-goes19"),
                                     QDateTime(QDate(2026, 10, 10), QTime(15, 0), QTimeZone::UTC));
        const auto text = url.toString();
        if (!url.isValid() || !text.contains(QStringLiteral("noaa-goes19")) ||
            !text.contains(QStringLiteral("ABI-L1b-RadC/2026/283/15")) ||
            !text.contains(QStringLiteral("max-keys=300"))) {
            std::cerr << "GOES list URL is wrong: " << text.toStdString() << '\n';
            return 1;
        }
        if (goesListUrl(QString(), QDateTime(QDate(2026, 10, 10), QTime(15, 0), QTimeZone::UTC))
                .isValid() ||
            goesListUrl(QStringLiteral("noaa-goes19"), QDateTime()).isValid()) {
            std::cerr << "GOES list URL should reject bad input\n";
            return 1;
        }
        if (!goesChannelSupported(13) || !goesChannelSupported(7) ||
            !goesChannelSupported(8) || !goesChannelSupported(16) ||
            goesChannelSupported(2) || goesChannelSupported(6) || goesChannelSupported(17)) {
            std::cerr << "GOES channel gate is wrong\n";
            return 1;
        }
        const QByteArray listing =
            R"XML(<?xml version="1.0" encoding="UTF-8"?>
<ListBucketResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
<Contents><Key>ABI-L1b-RadC/2026/283/15/OR_ABI-L1b-RadC-M6C13_G18_s20262831501189_e20262831503574_c20262831504026.nc</Key></Contents>
<Contents><Key>ABI-L1b-RadC/2026/283/15/OR_ABI-L1b-RadC-M6C13_G18_s20262831506189_e20262831508574_c20262831509043.nc</Key></Contents>
<Contents><Key>ABI-L1b-RadC/2026/283/15/OR_ABI-L1b-RadC-M6C07_G18_s20262831506189_e20262831508574_c20262831509043.nc</Key></Contents>
</ListBucketResult>)XML";
        if (newestGoesKey(listing, 13) !=
                QStringLiteral("ABI-L1b-RadC/2026/283/15/OR_ABI-L1b-RadC-M6C13_G18_s20262831506189_e20262831508574_c20262831509043.nc") ||
            newestGoesKey(listing, 7) !=
                QStringLiteral("ABI-L1b-RadC/2026/283/15/OR_ABI-L1b-RadC-M6C07_G18_s20262831506189_e20262831508574_c20262831509043.nc") ||
            !newestGoesKey(listing, 2).isEmpty() || !newestGoesKey(QByteArray("garbage"), 13).isEmpty()) {
            std::cerr << "GOES newest-key pick is wrong\n";
            return 1;
        }
        const auto at = [](int hour, int minute) {
            return QDateTime(QDate(2026, 10, 10), QTime(hour, minute), QTimeZone::UTC);
        };
        if (latestGoesSlot(at(15, 37)) != at(15, 25) || latestGoesSlot(at(15, 4)) != at(14, 50) ||
            latestGoesSlot(QDateTime()).isValid()) {
            std::cerr << "GOES slot clock is wrong\n";
            return 1;
        }
    }
    // GRLevelX color tables: header plus Color/Color4/SolidColor rules in
    // the live RTWX_REF shape (comments, mixed case, trailing junk).
    {
        const QString gr =
            QStringLiteral("; RTWX-style reflectivity curve\n"
                           "Product: BR\n"
                           "Units: DBZ\n"
                           "Step: 5\n"
                           "Color: -15 115 98 130\n"
                           "color: 0 150 155 180\n"
                           "Color: 30 11 141 15\n"
                           "Color: 20 82 214 162 90 200 170\n"
                           "Color4: 30 11 141 15 200\n"
                           "SolidColor: 40 234 210 4\n"
                           "SOLIDCOLOR4: 50 227 0 0 255\n");
        if (!looksLikeGrColorTable(gr) || looksLikeGrColorTable(QStringLiteral("-30,#04e9e7\n"))) {
            std::cerr << "GR color-table detection is wrong\n";
            return 1;
        }
        QString parseError;
        const auto table = parseGrColorTable(gr, &parseError);
        if (!table.has_value() || table->product != QStringLiteral("BR") ||
            table->units != QStringLiteral("DBZ") || table->stops.size() != 7) {
            std::cerr << "GR color-table parse failed: " << parseError.toStdString() << '\n';
            return 1;
        }
        // Sorted stops with exact values and colors (RGB2/alpha collapse;
        // the out-of-order 30/20 pair plus the duplicate 30 prove sorting).
        const double values[] = {-15.0, 0.0, 20.0, 30.0, 30.0, 40.0, 50.0};
        const int reds[] = {115, 150, 82, 11, 11, 234, 227};
        for (int i = 0; i < 7; ++i) {
            if (std::abs(table->stops.at(i).value - values[i]) > 1e-9 ||
                table->stops.at(i).color.red() != reds[i]) {
                std::cerr << "GR color-table stop is wrong at index " << i << '\n';
                return 1;
            }
        }
        if (parseGrColorTable(QStringLiteral("Color: 0 300 0 0\nColor: 5 0 0 0\n"),
                              &parseError)
                .has_value() ||
            parseGrColorTable(QStringLiteral("Bogus: 1 2 3\nColor: 0 0 0 0\nColor: 5 0 0 0\n"),
                              &parseError)
                .has_value() ||
            parseGrColorTable(QStringLiteral("Color: 0 0 0 0\n"), &parseError).has_value() ||
            parseError.isEmpty()) {
            std::cerr << "GR color-table garbage should fail closed\n";
            return 1;
        }
    }
    // ESRI shapefiles: committed US-county fixture plus hand-built
    // binary (polyline with two parts, null filler, unsupported point
    // record) and every malformed shape.
    {
        const auto findFixture = [](const char* name) {
            for (const auto& root : {std::string("tests/fixtures/"),
                                     std::string("../tests/fixtures/"),
                                     std::string("../../tests/fixtures/")}) {
                QFile file(QString::fromStdString(root + name));
                if (file.open(QIODevice::ReadOnly)) return file.readAll();
            }
            return QByteArray{};
        };
        const auto fixturePath = [&](const char* name) {
            for (const auto& root : {std::string("tests/fixtures/"),
                                     std::string("../tests/fixtures/"),
                                     std::string("../../tests/fixtures/")}) {
                if (QFile::exists(QString::fromStdString(root + name)))
                    return QString::fromStdString(root + name);
            }
            return QString{};
        };
        const auto fixtureBytes = findFixture("us_counties_5.shp");
        if (fixtureBytes.isEmpty()) {
            std::cerr << "Shapefile fixture missing\n";
            return 1;
        }
        if (!looksLikeShapefile(fixtureBytes) || looksLikeShapefile(QByteArray("Place: 1,2,x"))) {
            std::cerr << "Shapefile magic sniff is wrong\n";
            return 1;
        }
        QString shpError;
        const auto doc = parseShapefile(fixtureBytes, &shpError);
        if (!doc.has_value() || doc->elements.size() != 5) {
            std::cerr << "Shapefile fixture failed: " << shpError.toStdString() << '\n';
            return 1;
        }
        for (const auto& element : doc->elements) {
            if (element.kind != PlacefileElementKind::Polygon || element.points.size() < 3) {
                std::cerr << "Shapefile fixture element is not a polygon\n";
                return 1;
            }
            for (const auto& point : element.points) {
                if (!std::isfinite(point.x()) || !std::isfinite(point.y()) ||
                    point.x() < -180.0 || point.x() > 180.0 || point.y() < -90.0 ||
                    point.y() > 90.0) {
                    std::cerr << "Shapefile fixture point out of range\n";
                    return 1;
                }
            }
        }
        const auto& first = doc->elements.front().points.front();
        if (std::abs(first.x() + 85.6598) > 1e-4 || std::abs(first.y() - 41.7592) > 1e-4) {
            std::cerr << "Shapefile fixture first point moved: " << first.x() << ',' << first.y()
                      << '\n';
            return 1;
        }
        // dBase names align to shape records: LaGrange, Linn, Kent, Furnas,
        // Putnam ride the five polygons as hover text.
        {
            const QByteArray dbfBytes = findFixture("us_counties_5.dbf");
            if (dbfBytes.isEmpty()) {
                std::cerr << "Shapefile dbf fixture missing\n";
                return 1;
            }
            const QStringList names = parseDbfNames(dbfBytes);
            const QStringList want{QStringLiteral("LaGrange"), QStringLiteral("Linn"),
                                   QStringLiteral("Kent"), QStringLiteral("Furnas"),
                                   QStringLiteral("Putnam")};
            if (names != want) {
                std::cerr << "Shapefile dbf names are wrong\n";
                return 1;
            }
            QString namedError;
            const auto named = parseShapefileData(fixtureBytes, names, &namedError);
            if (!named.has_value()) {
                std::cerr << "Named shapefile failed: " << namedError.toStdString() << '\n';
                return 1;
            }
            for (int i = 0; i < 5; ++i) {
                if (named->elements.at(i).hoverText != want.at(i)) {
                    std::cerr << "Shapefile hover name wrong at index " << i << '\n';
                    return 1;
                }
            }
            // Malformed companions degrade to unnamed, never fatal.
            const auto unnamed = parseShapefileData(fixtureBytes, parseDbfNames(QByteArray("junk")),
                                                    &namedError);
            if (!unnamed.has_value() || !unnamed->elements.front().hoverText.isEmpty()) {
                std::cerr << "Bad dbf should degrade to unnamed geometry\n";
                return 1;
            }
            // Deleted dBase rows keep index alignment with an empty name.
            {
                QByteArray dbf;
                QDataStream head(&dbf, QIODevice::WriteOnly);
                head.setByteOrder(QDataStream::LittleEndian);
                dbf.append(QByteArray(32, 0));
                head.device()->seek(0);
                head << static_cast<quint8>(0x03);
                head.device()->seek(4);
                head << static_cast<quint32>(2) << static_cast<quint16>(65)
                     << static_cast<quint16>(5);
                QByteArray field(32, 0);
                field[0] = 'N';
                field[1] = 'A';
                field[2] = 'M';
                field[3] = 'E';
                field[11] = 'C';
                field[16] = 4;
                dbf.append(field);
                dbf.append(static_cast<char>(0x0D));
                dbf.append(" A001");
                dbf.append("*B002");
                const QStringList deleted = parseDbfNames(dbf);
                if (deleted.size() != 2 || deleted.at(0) != QStringLiteral("A001") ||
                    !deleted.at(1).isEmpty()) {
                    std::cerr << "Deleted dbf rows should stay aligned but empty\n";
                    return 1;
                }
            }
        }
        // Source wiring: the placefile loader sniffs shapefile magic and
        // publishes polygons with no dialog involved.
        {
            const QString path = fixturePath("us_counties_5.shp");
            if (path.isEmpty()) {
                std::cerr << "Shapefile fixture missing for source test\n";
                return 1;
            }
            PlacefileSource source;
            QString loadError;
            if (!source.addLocalFile(path, &loadError)) {
                std::cerr << "Shapefile addLocalFile failed: " << loadError.toStdString() << '\n';
                return 1;
            }
            const auto documents = source.documents();
            if (documents.size() != 1 || documents.front().elements.size() != 5) {
                std::cerr << "Shapefile source published the wrong document\n";
                return 1;
            }
            // Sibling .dbf travels with the fixture, so the end-to-end load
            // names the first county.
            if (documents.front().elements.front().hoverText != QStringLiteral("LaGrange")) {
                std::cerr << "Shapefile source did not attach the county name\n";
                return 1;
            }
        }
        // Hand-built: one 2-part polyline, one null, one point record.
        QByteArray synthetic;
        {
            QDataStream head(&synthetic, QIODevice::WriteOnly);
            head.setByteOrder(QDataStream::BigEndian);
            head << static_cast<qint32>(9994) << static_cast<qint32>(0) << static_cast<qint32>(0)
                 << static_cast<qint32>(0) << static_cast<qint32>(0) << static_cast<qint32>(0);
            // File length patched below (16-bit words).
            head << static_cast<qint32>(0);
            head.setByteOrder(QDataStream::LittleEndian);
            head << static_cast<qint32>(1000) << static_cast<qint32>(3);
            head.writeRawData(QByteArray(64, 0).constData(), 64);
            const auto record = [&](qint32 number, qint32 type, const QByteArray& body) {
                QByteArray header;
                QDataStream rec(&header, QIODevice::WriteOnly);
                rec.setByteOrder(QDataStream::BigEndian);
                rec << number << static_cast<qint32>((4 + body.size()) / 2);
                rec.setByteOrder(QDataStream::LittleEndian);
                rec << type;
                synthetic.append(header);
                synthetic.append(body);
            };
            QByteArray line;
            QDataStream body(&line, QIODevice::WriteOnly);
            body.setByteOrder(QDataStream::LittleEndian);
            body.writeRawData(QByteArray(32, 0).constData(), 32);  // bbox
            body << static_cast<qint32>(2) << static_cast<qint32>(5);
            body << static_cast<qint32>(0) << static_cast<qint32>(3);
            const double pts[] = {-97.0, 35.0, -96.0, 35.5, -95.0, 36.0, -94.0, 36.0, -93.0, 36.5};
            for (const double v : pts) body << v;
            record(1, 3, line);
            record(2, 0, QByteArray());
            record(3, 1, QByteArray(16, 0));
            {
                QDataStream fix(&synthetic, QIODevice::ReadWrite);
                fix.setByteOrder(QDataStream::BigEndian);
                fix.skipRawData(24);
                fix << static_cast<qint32>(synthetic.size() / 2);
            }
        }
        const auto syn = parseShapefile(synthetic, &shpError);
        if (!syn.has_value() || syn->elements.size() != 2 ||
            syn->elements.at(0).kind != PlacefileElementKind::Line ||
            syn->elements.at(0).points.size() != 3 ||
            syn->elements.at(1).points.size() != 2 || syn->warnings.isEmpty()) {
            std::cerr << "Hand-built shapefile failed: " << shpError.toStdString() << '\n';
            return 1;
        }
        // Every malformed shape fails closed with an error.
        const auto bad = [&](QByteArray bytes) {
            return parseShapefile(bytes, &shpError).has_value() || shpError.isEmpty();
        };
        if (bad(QByteArray("Place: 35,-97,x")) || bad(QByteArray(100, 0)) ||
            bad(synthetic.left(64)) || bad(QByteArray("garbage")) ||
            [&] {
                // Projected (meter) coordinates on every ring: loud,
                // specific failure instead of silent emptiness.
                QByteArray proj = synthetic;
                QDataStream patch(&proj, QIODevice::ReadWrite);
                patch.setByteOrder(QDataStream::LittleEndian);
                for (int point = 0; point < 5; ++point) {
                    // Record at 100: 8 header + 4 type + 32 bbox + 4 + 4
                    // counts + 8 part table (2 parts); points follow.
                    patch.device()->seek(100 + 8 + 4 + 32 + 4 + 4 + 8 + point * 16);
                    patch << 500000.0 << 4000000.0;
                }
                QString projError;
                const auto parsed = parseShapefile(proj, &projError);
                return parsed.has_value() || !projError.contains(QStringLiteral("projected"));
            }()) {
            std::cerr << "Malformed shapefile should fail closed\n";
            return 1;
        }
    }
    // Update discovery: version compare plus release-metadata parse in
    // the live GitHub shape (tag, prerelease flag, page URL, timestamp).
    {
        const auto v = [](const char* text) { return parseAppVersion(QString::fromLatin1(text)); };
        if (!v("0.7.0").valid || v("0.7.0").major != 0 || v("0.7.0").minor != 7 ||
            v("0.7.0").patch != 0 || !v("v0.8.0").valid || !v("1.2").valid ||
            v("1.2").patch != 0 || v("").valid || v("v").valid || v("0.7.0.1").valid ||
            v("1.x").valid || v("-1.2").valid || v("  ").valid) {
            std::cerr << "Update version parse is wrong\n";
            return 1;
        }
        if (compareVersions(v("0.7.0"), v("0.8.0")) != -1 ||
            compareVersions(v("0.8.0"), v("0.7.0")) != 1 ||
            compareVersions(v("0.7.0"), v("v0.7.0")) != 0 ||
            compareVersions(v("1.10.0"), v("1.9.9")) != 1) {
            std::cerr << "Update version compare is wrong\n";
            return 1;
        }
        if (currentAppVersion().isEmpty()) {
            std::cerr << "Current app version is empty\n";
            return 1;
        }
        const QByteArray liveShaped =
            R"JSON({"tag_name":"v0.8.0","name":"0.8.0","prerelease":false,"draft":false,
                    "published_at":"2026-10-01T12:00:00Z",
                    "html_url":"https://github.com/Mikifrfr/CappedWeather/releases/tag/v0.8.0"})JSON";
        QString releaseError;
        const auto release = parseUpdateRelease(liveShaped, &releaseError);
        if (!release.has_value() || release->tag != QStringLiteral("v0.8.0") ||
            release->version.minor != 8 || release->prerelease ||
            release->pageUrl.toString() !=
                QStringLiteral("https://github.com/Mikifrfr/CappedWeather/releases/tag/v0.8.0") ||
            projectReleasesUrl().toString() !=
                QStringLiteral("https://github.com/Mikifrfr/CappedWeather/releases")) {
            std::cerr << "Update release parse failed: " << releaseError.toStdString() << '\n';
            return 1;
        }
        // Unverifiable page URLs drop the link, never the version; garbage
        // fails closed with an error.
        const QByteArray evilShaped =
            R"JSON({"tag_name":"v0.8.0","html_url":"https://evil.example/r"})JSON";
        const auto evil = parseUpdateRelease(evilShaped, &releaseError);
        if (!evil.has_value() || evil->pageUrl.isValid()) {
            std::cerr << "Update release should drop hostile page URLs\n";
            return 1;
        }
        if (parseUpdateRelease(QByteArray("not json"), &releaseError).has_value() ||
            parseUpdateRelease(QByteArray(R"JSON({"name":"x"})JSON"), &releaseError).has_value() ||
            parseUpdateRelease(QByteArray(R"JSON({"tag_name":"soon"})JSON"), &releaseError)
                .has_value() ||
            releaseError.isEmpty()) {
            std::cerr << "Malformed release metadata should fail closed\n";
            return 1;
        }
        // Weekly auto-check clock: never-checked is due, six days is not,
        // seven days is, undated now never fires.
        if (!updateCheckDue(0, 1791658800LL) || !updateCheckDue(-5, 1791658800LL) ||
            updateCheckDue(1791658800LL - 6 * 86400LL, 1791658800LL) ||
            !updateCheckDue(1791658800LL - 7 * 86400LL, 1791658800LL) ||
            !updateCheckDue(1791658800LL - 30 * 86400LL, 1791658800LL) ||
            updateCheckDue(1791658800LL, 0)) {
            std::cerr << "Update check clock is wrong\n";
            return 1;
        }
    }
    // Update discovery action is wired into the menus (the result dialog
    // itself stays out of headless runs: modal exec would hang the suite).
    {
        bool foundUpdate = false;
        for (auto* action : window.findChildren<QAction*>()) {
            if (action->text() == QStringLiteral("Check for updates…")) {
                foundUpdate = true;
                break;
            }
        }
        if (!foundUpdate) {
            std::cerr << "Update check action is missing from the menus\n";
            return 1;
        }
    }
    // IEM mosaic listing pick and slot clock: hermetic against hand-built
    // directory HTML in the live archive shape.
    {
        const auto url = mosaicListUrl(QDate(2026, 10, 10));
        if (!url.isValid() ||
            url.toString() !=
                QStringLiteral("https://mesonet.agron.iastate.edu/archive/data/2026/10/10/GIS/uscomp/")) {
            std::cerr << "Mosaic list URL is wrong: " << url.toString().toStdString() << '\n';
            return 1;
        }
        if (mosaicListUrl(QDate()).isValid()) {
            std::cerr << "Mosaic list URL should reject bad input\n";
            return 1;
        }
        const QByteArray listing =
            R"HTML(<html><body>
<a href="n0q_202610101720.png">n0q_202610101720.png</a>
<a href="n0q_202610101730.png">n0q_202610101730.png</a>
<a href="n0q_202610101730.wld">n0q_202610101730.wld</a>
<a href="n0r_202610101730.png">n0r_202610101730.png</a>
</body></html>)HTML";
        if (newestMosaicFile(listing) != QStringLiteral("n0q_202610101730.png") ||
            !newestMosaicFile(QByteArray("<html></html>")).isEmpty() ||
            !newestMosaicFile(QByteArray("garbage")).isEmpty()) {
            std::cerr << "Mosaic newest-file pick is wrong\n";
            return 1;
        }
        const auto at = [](int hour, int minute) {
            return QDateTime(QDate(2026, 10, 10), QTime(hour, minute), QTimeZone::UTC);
        };
        if (latestMosaicSlot(at(15, 37)) != at(15, 25) || latestMosaicSlot(at(15, 4)) != at(14, 50) ||
            latestMosaicSlot(QDateTime()).isValid()) {
            std::cerr << "Mosaic slot clock is wrong\n";
            return 1;
        }
        // Decode helper: committed fixture plus garbage through the exact
        // production path, including the scaled branch on a synthetic
        // CONUS-scale image.
        {
            QString decodeError;
            if (decodeMosaicFrame(QByteArray(), "0.005\n0.0\n0.0\n-0.005\n-126.0\n50.0\n",
                                  1791653400LL, &decodeError)
                    .has_value() ||
                decodeError.isEmpty() ||
                decodeMosaicFrame(QByteArray("not a png"),
                                  "0.005\n0.0\n0.0\n-0.005\n-126.0\n50.0\n", 1791653400LL,
                                  &decodeError)
                    .has_value()) {
                std::cerr << "Mosaic decode should fail closed on garbage\n";
                return 1;
            }
            QImage big(2000, 2000, QImage::Format_Indexed8);
            big.setColorCount(256);
            for (int i = 0; i < 256; ++i) big.setColor(i, qRgb(0, i, 0));
            big.fill(128);
            QByteArray bigPng;
            QBuffer bigBuffer(&bigPng);
            bigBuffer.open(QIODevice::WriteOnly);
            if (!big.save(&bigBuffer, "PNG")) {
                std::cerr << "Could not stage synthetic mosaic PNG\n";
                return 1;
            }
            const auto bigFrame = decodeMosaicFrame(
                bigPng, "0.005\n0.0\n0.0\n-0.005\n-126.0\n50.0\n", 1791653400LL, &decodeError);
            if (!bigFrame.has_value() || bigFrame->image.width() > 300 ||
                bigFrame->image.height() > 300 ||
                static_cast<std::size_t>(bigFrame->image.width()) != bigFrame->grid.nx ||
                !(bigFrame->grid.degreesPerPixelX > 0.03)) {
                std::cerr << "Mosaic scaled decode is wrong\n";
                return 1;
            }
        }
    }
    // Warning countdowns: clock-injected, minute/hour/day bands plus the
    // expired and undated fail-closed states.
    {
        const QDateTime now(QDate(2026, 10, 10), QTime(12, 0), QTimeZone::UTC);
        if (warningCountdownText(now.addSecs(18 * 60), now) != QStringLiteral("18m left") ||
            warningCountdownText(now.addSecs(2 * 3600 + 5 * 60), now) !=
                QStringLiteral("2h 5m left") ||
            warningCountdownText(now.addSecs(26 * 3600), now) != QStringLiteral("1d 2h left") ||
            warningCountdownText(now.addSecs(30), now) != QStringLiteral("<1m left") ||
            warningCountdownText(now.addSecs(-60), now) != QStringLiteral("expired") ||
            warningCountdownText({}, now) != QStringLiteral("no expiry listed")) {
            std::cerr << "Warning countdown formatting failed\n";
            return 1;
        }
    }
    // ECCC alerts (MSC GeoMet OGC API): same warning rows as NWS, mapped
    // from alert_name_en/type with title-cased events, no status filtering.
    {
        const QByteArray eccc = R"JSON({"type":"FeatureCollection","features":[
{"id":"batch1_fea1-2195","type":"Feature","geometry":{"type":"Polygon","coordinates":[[[-115.9,54.85],[-115.98,54.85],[-115.96,54.5],[-115.06,54.5],[-115.9,54.85]]]},"properties":{"alert_name_en":"special weather statement","alert_type":"statement","alert_text_en":"First snow.","feature_name_en":"Big Lakes County near Swan Hills","publication_datetime":"2026-10-09T11:19:59.506Z","expiration_datetime":"2026-10-10T03:19:59.506Z","status_en":"ended"}},
{"id":"batch1_fea1-2200","type":"Feature","geometry":{"type":"Polygon","coordinates":[[[-114.0,53.0],[-114.5,53.0],[-114.5,52.5],[-114.0,52.5],[-114.0,53.0]]]},"properties":{"alert_name_en":"snowfall warning","alert_type":"warning","alert_text_en":"Heavy snow.","feature_name_en":"Calgary","publication_datetime":"2026-10-09T12:00:00.000Z","expiration_datetime":"2026-10-10T12:00:00.000Z","status_en":"active"}},
{"id":"batch1_fea1-2201","type":"Feature","geometry":null,"properties":{"alert_name_en":"wind warning","alert_type":"warning","alert_text_en":"Strong wind.","feature_name_en":"Edmonton","publication_datetime":"2026-10-09T12:00:00.000Z","expiration_datetime":"2026-10-10T12:00:00.000Z"}}]})JSON";
        const auto parsed = WeatherOverlaySource::parseEcccAlerts(eccc, &parseError);
        if (!parseError.isEmpty() || parsed.size() != 2) {
            std::cerr << "ECCC alert parsing failed: " << parseError.toStdString() << '\n';
            return 1;
        }
        const auto statement = *std::find_if(parsed.cbegin(), parsed.cend(), [](const WeatherWarning& row) {
            return row.id == QStringLiteral("eccc:batch1_fea1-2195");
        });
        if (statement.event != QStringLiteral("Special Weather Statement") ||
            statement.severity != QStringLiteral("Minor") ||
            statement.senderName != QStringLiteral("Environment Canada") ||
            statement.headline !=
                QStringLiteral("Big Lakes County near Swan Hills: Special Weather Statement") ||
            statement.description != QStringLiteral("First snow.") ||
            statement.polygons.isEmpty() || statement.polygons.front().exterior.size() != 5 ||
            statement.alertColor != QColor(QStringLiteral("#FFE4B5"))) {
            std::cerr << "ECCC statement mapping is wrong\n";
            return 1;
        }
        const auto warning = *std::find_if(parsed.cbegin(), parsed.cend(), [](const WeatherWarning& row) {
            return row.id == QStringLiteral("eccc:batch1_fea1-2200");
        });
        if (warning.event != QStringLiteral("Snowfall Warning") ||
            warning.severity != QStringLiteral("Severe")) {
            std::cerr << "ECCC warning mapping is wrong\n";
            return 1;
        }
        if (!WeatherOverlaySource::parseEcccAlerts(QByteArray("not json"), &parseError).isEmpty() ||
            parseError.isEmpty()) {
            std::cerr << "Malformed ECCC payload should fail closed\n";
            return 1;
        }
    }
    // METAR station catalog: only ICAO stations with coordinates and a METAR
    // feed survive; NEXRAD sites and id-less rows are dropped, never fatal.
    {
        const QByteArray stations = R"JSON([{"id":"KOKC","icaoId":"KOKC","site":"Oklahoma City/Will Rogers Intl","lat":35.3843,"lon":-97.60033,"priority":1,"siteType":["METAR","TAF"]},
{"id":"KOUN","icaoId":"KOUN","site":"Norman/Univ Oklahoma Arpt","lat":35.24359,"lon":-97.47133,"priority":6,"siteType":["METAR"]},
{"id":"KTLX","icaoId":"KTLX","site":"Oklahoma City/NEXRAD","lat":35.333,"lon":-97.278,"priority":9,"siteType":[]},
{"id":"NONAV","site":"No ICAO here","lat":35.0,"lon":-97.0,"priority":5,"siteType":["METAR"]},
{"id":"BADLAT","icaoId":"KBAD","site":"Bad coords","lat":"north","lon":-97.0,"priority":5,"siteType":["METAR"]}])JSON";
        const auto parsed = parseMetarStations(stations, &parseError);
        if (!parseError.isEmpty() || parsed.size() != 2 || parsed.at(0).icaoId != QStringLiteral("KOKC") ||
            parsed.at(1).icaoId != QStringLiteral("KOUN") || !parsed.at(0).hasMetar ||
            !parsed.at(0).hasCoords || parsed.at(0).priority != 1) {
            std::cerr << "METAR station catalog parsing failed\n";
            return 1;
        }
        if (!parseMetarStations(QByteArray(R"JSON({"not":"an array"})JSON"), &parseError).isEmpty() ||
            parseError.isEmpty()) {
            std::cerr << "Non-array station catalog should fail closed with an error\n";
            return 1;
        }
    }
    // METAR observations: newest report per station wins, missing fields
    // stay NaN/-1 (never zero), id-less rows are dropped.
    {
        const QByteArray obs = R"JSON([{"icaoId":"KOKC","obsTime":1791561120,"temp":27.2,"dewp":13.9,"wdir":160,"wspd":6,"lat":35.3843,"lon":-97.6003,"name":"Oklahoma City/Will Rogers Intl, OK, US","fltCat":"VFR"},
{"icaoId":"KOKC","obsTime":1791557520,"temp":22.8,"dewp":15.0,"wdir":200,"wspd":4,"lat":35.3843,"lon":-97.6003,"name":"Oklahoma City/Will Rogers Intl, OK, US","fltCat":"VFR"},
{"icaoId":"KOUN","obsTime":1791561120,"wdir":180,"wspd":8,"wgst":14,"lat":35.24359,"lon":-97.47133,"name":"Norman/Univ Oklahoma Arpt, OK, US","fltCat":"MVFR"},
{"icaoId":"","obsTime":1791561120,"temp":20.0,"lat":35.0,"lon":-97.0,"fltCat":"VFR"}])JSON";
        const auto parsed = parseMetarObs(obs, &parseError);
        if (!parseError.isEmpty() || parsed.size() != 2) {
            std::cerr << "METAR observation parsing failed\n";
            return 1;
        }
        const auto kokc = *std::find_if(parsed.cbegin(), parsed.cend(), [](const MetarObs& row) {
            return row.icaoId == QStringLiteral("KOKC");
        });
        if (std::abs(kokc.tempC - 27.2) > 0.01 || std::abs(kokc.dewpointC - 13.9) > 0.01 ||
            kokc.windDirectionDegrees != 160 || kokc.windSpeedKnots != 6 ||
            kokc.flightCategory != QStringLiteral("VFR") ||
            kokc.obsTime != QDateTime::fromSecsSinceEpoch(1791561120, QTimeZone::UTC)) {
            std::cerr << "Newest KOKC report should win the dedupe\n";
            return 1;
        }
        const auto koun = *std::find_if(parsed.cbegin(), parsed.cend(), [](const MetarObs& row) {
            return row.icaoId == QStringLiteral("KOUN");
        });
        if (!std::isnan(koun.tempC) || koun.windGustKnots != 14 ||
            koun.flightCategory != QStringLiteral("MVFR")) {
            std::cerr << "Missing METAR fields should stay NaN, gusts should parse\n";
            return 1;
        }
        if (!parseMetarObs(QByteArray("not json"), &parseError).isEmpty() || parseError.isEmpty()) {
            std::cerr << "Malformed METAR payload should fail closed with an error\n";
            return 1;
        }
        // Flight-category colors: unknown strings paint gray, never VFR green.
        if (metarCategoryColor(QStringLiteral("VFR")) != QColor(QStringLiteral("#53d769")) ||
            metarCategoryColor(QStringLiteral("MVFR")) != QColor(QStringLiteral("#44a7ff")) ||
            metarCategoryColor(QStringLiteral("IFR")) != QColor(QStringLiteral("#ff9f43")) ||
            metarCategoryColor(QStringLiteral("LIFR")) != QColor(QStringLiteral("#ff3b4d")) ||
            metarCategoryColor(QStringLiteral("XYZ")) != QColor(QStringLiteral("#9aa0a6")) ||
            metarCategoryColor(QString{}).name() != QColor(QStringLiteral("#9aa0a6")).name()) {
            std::cerr << "METAR flight-category colors are wrong\n";
            return 1;
        }
        // Experimental precip type: strict melting-level gate, conservative
        // freezing thresholds, NaN-safe.
        if (precipTypeForStation(-2.0, true) != PrecipType::Snow ||
            precipTypeForStation(0.0, true) != PrecipType::Snow ||
            precipTypeForStation(1.5, true) != PrecipType::Mix ||
            precipTypeForStation(2.0, true) != PrecipType::Mix ||
            precipTypeForStation(2.5, true) != PrecipType::Rain ||
            precipTypeForStation(-5.0, false) != PrecipType::Unknown ||
            precipTypeForStation(15.0, false) != PrecipType::Unknown ||
            precipTypeForStation(std::numeric_limits<double>::quiet_NaN(), true) !=
                PrecipType::Unknown ||
            precipTypeLabel(PrecipType::Snow) != QStringLiteral("snow") ||
            precipTypeLabel(PrecipType::Unknown) != QStringLiteral("unknown")) {
            std::cerr << "Precip-type classification is wrong\n";
            return 1;
        }
    }
    // IEM sounding JSON: live-shaped KOUN excerpt (null winds stay NaN),
    // day-stamped URL, latest-00Z/12Z clock with the 2 h publish lag.
    {
        const QByteArray raob = R"JSON({"profiles":[{"station":"KOUN","valid":"2026-10-10T12:00:00Z","profile":[
          {"pres":975.0,"hght":357.0,"tmpc":14.0,"dwpc":12.6,"drct":0.0,"sknt":0.0},
          {"pres":946.04,"hght":610.0,"tmpc":24.9,"dwpc":9.2,"drct":200.0,"sknt":20.0},
          {"pres":895.0,"hght":1086.7,"tmpc":21.8,"dwpc":3.8,"drct":null,"sknt":null}
        ]}],"generated_at":"2026-10-10T15:26:40Z"})JSON";
        QString soundingError;
        const auto sounding = parseIemRaob(raob, &soundingError);
        if (!soundingError.isEmpty() || sounding.stationId != "KOUN" ||
            sounding.levels.size() != 3 ||
            std::abs(sounding.levels.at(1).heightMslMeters - 610.0) > 0.01 ||
            std::abs(sounding.levels.at(1).tempCelsius - 24.9) > 0.01 ||
            !std::isnan(sounding.levels.at(2).directionFromDegrees) ||
            !std::isnan(sounding.levels.at(2).speedKnots) ||
            sounding.validEpochSeconds !=
                QDateTime(QDate(2026, 10, 10), QTime(12, 0), QTimeZone::UTC).toSecsSinceEpoch()) {
            std::cerr << "Sounding parsing failed\n";
            return 1;
        }
        if (!parseIemRaob(QByteArray(R"JSON({"profiles":[]})JSON"), &soundingError).levels.empty() ||
            soundingError.isEmpty() ||
            !parseIemRaob(QByteArray("not json"), &soundingError).levels.empty()) {
            std::cerr << "Empty/malformed soundings should fail closed\n";
            return 1;
        }
        if (soundingUrl(QStringLiteral("koun"),
                        QDateTime(QDate(2026, 10, 10), QTime(12, 0), QTimeZone::UTC))
                .toString() !=
            QStringLiteral("https://mesonet.agron.iastate.edu/json/raob.py?station=KOUN&ts=202610101200")) {
            std::cerr << "Sounding URL builder is wrong\n";
            return 1;
        }
        if (soundingUrl(QString{}, QDateTime::currentDateTimeUtc()).isValid() ||
            soundingUrl(QStringLiteral("KOUN"), QDateTime()).isValid()) {
            std::cerr << "Sounding URL builder should reject bad input\n";
            return 1;
        }
        const auto at = [](int hour, int minute) {
            return QDateTime(QDate(2026, 10, 10), QTime(hour, minute), QTimeZone::UTC);
        };
        if (latestSoundingTime(at(15, 30)) != at(12, 0) || latestSoundingTime(at(13, 30)) != at(0, 0) ||
            latestSoundingTime(at(1, 30)) !=
                QDateTime(QDate(2026, 10, 9), QTime(12, 0), QTimeZone::UTC) ||
            latestSoundingTime(at(3, 30)) != at(0, 0) ||
            latestSoundingTime(QDateTime()).isValid()) {
            std::cerr << "Sounding clock is wrong\n";
            return 1;
        }
    }
    // KML GroundOverlay: exact box values in the XML, XML-escaped names,
    // and fail-closed on flipped, wrapped, or non-finite bounds.
    {
        QString kmlError;
        const auto kml = MainWindow::frameGroundOverlayKml(
            QStringLiteral("KTLX & REF <live>"), QStringLiteral("frame.png"), 38.5, 32.5, -94.0,
            -100.0, &kmlError);
        if (kml.isEmpty() || !kmlError.isEmpty() ||
            !kml.contains(QStringLiteral("<north>38.500000</north>")) ||
            !kml.contains(QStringLiteral("<south>32.500000</south>")) ||
            !kml.contains(QStringLiteral("<east>-94.000000</east>")) ||
            !kml.contains(QStringLiteral("<west>-100.000000</west>")) ||
            !kml.contains(QStringLiteral("<href>frame.png</href>")) ||
            !kml.contains(QStringLiteral("KTLX &amp; REF &lt;live&gt;"))) {
            std::cerr << "KML GroundOverlay builder is wrong\n";
            return 1;
        }
        if (!MainWindow::frameGroundOverlayKml(QStringLiteral("n"), QStringLiteral("f.png"), 32.0,
                                               38.0, -94.0, -100.0, &kmlError)
                     .isEmpty() ||
            kmlError.isEmpty()) {
            std::cerr << "Flipped north/south should fail closed\n";
            return 1;
        }
        if (!MainWindow::frameGroundOverlayKml(QStringLiteral("n"), QStringLiteral("f.png"), 38.0,
                                               32.0, -100.0, -94.0, &kmlError)
                     .isEmpty() ||
            kmlError.isEmpty()) {
            std::cerr << "Antimeridian wrap should fail closed, never mirror\n";
            return 1;
        }
        const auto nan = std::numeric_limits<double>::quiet_NaN();
        if (!MainWindow::frameGroundOverlayKml(QStringLiteral("n"), QStringLiteral("f.png"), nan,
                                               32.0, -94.0, -100.0, &kmlError)
                     .isEmpty() ||
            kmlError.isEmpty()) {
            std::cerr << "Non-finite bounds should fail closed\n";
            return 1;
        }
        if (!MainWindow::frameGroundOverlayKml(QString{}, QStringLiteral("f.png"), 38.0, 32.0,
                                               -94.0, -100.0, &kmlError)
                     .isEmpty() ||
            kmlError.isEmpty()) {
            std::cerr << "Empty overlay name should fail closed\n";
            return 1;
        }
    }
    // World file: exact six lines for a known box, fail-closed otherwise.
    {
        QString worldError;
        const auto world = MainWindow::frameWorldFile(40.0, 30.0, -90.0, -100.0, 1000, 500,
                                                      &worldError);
        const QString expected = QStringLiteral("0.0100000000\n0\n0\n-0.0200000000\n"
                                                "-99.9950000000\n39.9900000000\n");
        if (world != expected || !worldError.isEmpty()) {
            std::cerr << "World file math is wrong: " << world.toStdString() << '\n';
            return 1;
        }
        const auto nan = std::numeric_limits<double>::quiet_NaN();
        if (!MainWindow::frameWorldFile(nan, 30.0, -90.0, -100.0, 1000, 500, &worldError)
                 .isEmpty() ||
            worldError.isEmpty()) {
            std::cerr << "Non-finite bounds should fail the world file\n";
            return 1;
        }
        if (!MainWindow::frameWorldFile(40.0, 30.0, -90.0, -100.0, 0, 500, &worldError)
                 .isEmpty() ||
            worldError.isEmpty()) {
            std::cerr << "Zero-width image should fail the world file\n";
            return 1;
        }
        if (!MainWindow::frameWorldFile(30.0, 40.0, -90.0, -100.0, 1000, 500, &worldError)
                 .isEmpty() ||
            worldError.isEmpty()) {
            std::cerr << "Flipped bounds should fail the world file\n";
            return 1;
        }
    }
    // Hail-swath KML: one tier-tinted Placemark per drawable hull, empty when
    // there is nothing to draw (never an error).
    {
        QString swathError;
        HailSwath swath;
        swath.trackId = 7;
        swath.maxTier = 2;
        swath.fixes = {{35.0, -97.0, 1, 1000.0}, {35.02, -96.99, 2, 1300.0}, {35.0, -96.98, 1, 1600.0}};
        const auto placemarks = MainWindow::swathPlacemarksKml({swath}, &swathError);
        if (!swathError.isEmpty() || !placemarks.contains(QStringLiteral("<Placemark>")) ||
            !placemarks.contains(QStringLiteral("HAIL SWATH T2")) ||
            !placemarks.contains(QStringLiteral("<coordinates>")) ||
            !placemarks.contains(QStringLiteral("never a forecast"))) {
            std::cerr << "Swath placemarks should carry one guidance polygon\n";
            return 1;
        }
        HailSwath line;
        line.trackId = 9;
        line.fixes = {{35.0, -97.0, 1, 1000.0}, {35.01, -97.0, 1, 1300.0}};
        if (!MainWindow::swathPlacemarksKml({line}, &swathError).isEmpty() ||
            !MainWindow::swathPlacemarksKml({}, &swathError).isEmpty() || !swathError.isEmpty()) {
            std::cerr << "Undrawable swaths should export as empty, not error\n";
            return 1;
        }
    }
    // Case-pack zip: deflated round-trip with CRC verification, fail-closed
    // on truncation and non-zip input (half-extracted files are removed).
    {
        if (!zipAvailable()) {
            std::cerr << "note: zlib unavailable, skipping case-pack zip asserts\n";
        } else {
            QTemporaryDir packDir;
            if (!packDir.isValid()) {
                std::cerr << "Temporary dir for case-pack test failed\n";
                return 1;
            }
            const auto writeFile = [&packDir](const QString& name, const QByteArray& bytes) {
                QFile file(packDir.filePath(name));
                if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
                return file.write(bytes) == bytes.size();
            };
            QByteArray big;
            big.reserve(262144);
            for (int i = 0; i < 65536; ++i) big.append("ABCD"[i % 4]);
            if (!writeFile(QStringLiteral("a.txt"), QByteArray("hello case-pack")) ||
                !writeFile(QStringLiteral("empty.bin"), QByteArray()) ||
                !writeFile(QStringLiteral("big.bin"), big)) {
                std::cerr << "Case-pack test fixture write failed\n";
                return 1;
            }
            const QString zipPath = packDir.filePath(QStringLiteral("case.cwcase"));
            const QVector<QPair<QString, QString>> entries = {
                {QStringLiteral("manifest.json"), packDir.filePath(QStringLiteral("a.txt"))},
                {QStringLiteral("volumes/empty.cwdv"), packDir.filePath(QStringLiteral("empty.bin"))},
                {QStringLiteral("volumes/big.cwdv"), packDir.filePath(QStringLiteral("big.bin"))},
            };
            QString zipError;
            if (!zipPackFiles(entries, zipPath, &zipError)) {
                std::cerr << "zipPackFiles failed: " << zipError.toStdString() << '\n';
                return 1;
            }
            const QString outDir = packDir.filePath(QStringLiteral("out"));
            const auto names = zipExtractFiles(zipPath, outDir, &zipError);
            if (names.size() != 3 || !zipError.isEmpty()) {
                std::cerr << "zipExtractFiles failed: " << zipError.toStdString() << '\n';
                return 1;
            }
            QFile check(outDir + QStringLiteral("/volumes/big.cwdv"));
            if (!check.open(QIODevice::ReadOnly) || check.readAll() != big) {
                std::cerr << "Zip round-trip bytes differ\n";
                return 1;
            }
            // Truncation and garbage fail closed with nothing trusted.
            QFile zip(zipPath);
            if (!zip.open(QIODevice::ReadOnly)) {
                std::cerr << "Case-pack zip unreadable\n";
                return 1;
            }
            const QByteArray full = zip.readAll();
            zip.close();
            QFile shortZip(packDir.filePath(QStringLiteral("short.cwcase")));
            if (!shortZip.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
                shortZip.write(full.constData(), full.size() / 2) != full.size() / 2) {
                std::cerr << "Truncated zip fixture write failed\n";
                return 1;
            }
            shortZip.close();
            if (!zipExtractFiles(shortZip.fileName(), packDir.filePath(QStringLiteral("out2")),
                                 &zipError)
                     .isEmpty() ||
                zipError.isEmpty()) {
                std::cerr << "Truncated zip should fail closed\n";
                return 1;
            }
            if (!QDir(packDir.filePath(QStringLiteral("out2/volumes"))).exists()) {
                // out2/volumes must not exist: fail() removes partial extracts.
            } else if (!QDir(packDir.filePath(QStringLiteral("out2/volumes")))
                                  .entryList(QDir::Files)
                                  .isEmpty()) {
                std::cerr << "Truncated zip left trusted files behind\n";
                return 1;
            }
            QFile garbage(packDir.filePath(QStringLiteral("garbage.cwcase")));
            if (!garbage.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
                garbage.write("definitely not a zip file....................") != 45) {
                std::cerr << "Garbage zip fixture write failed\n";
                return 1;
            }
            garbage.close();
            if (!zipExtractFiles(garbage.fileName(), packDir.filePath(QStringLiteral("out3")),
                                 &zipError)
                     .isEmpty() ||
                zipError.isEmpty()) {
                std::cerr << "Non-zip input should fail closed\n";
                return 1;
            }
        }
    }
    // Case manifest: write/parse identity plus fail-closed validation.
    {
        CaseManifest manifest;
        manifest.radarId = QStringLiteral("KTLX");
        manifest.frameIndex = 1;
        manifest.exportedAt = QDateTime(QDate(2026, 10, 9), QTime(18, 0), QTimeZone::UTC);
        for (int i = 0; i < 2; ++i) {
            CaseFrame frame;
            frame.volumeId = QStringLiteral("v%1").arg(i);
            frame.dataTime = QDateTime(QDate(2026, 10, 9), QTime(18, i), QTimeZone::UTC);
            frame.chunkCount = 10;
            frame.failedChunks = i;
            frame.file = QStringLiteral("volumes/v%1.cwdv").arg(i);
            manifest.frames.push_back(frame);
        }
        CasePanelState panel;
        panel.radarId = QStringLiteral("KTLX");
        panel.product = 0;
        panel.tilt = -1;
        manifest.panels.push_back(panel);
        manifest.placefileUrls.push_back(QStringLiteral("https://example.com/spots.txt"));
        manifest.placefileFiles.push_back(QStringLiteral("placefiles/0.txt"));
        QString manifestError;
        const auto parsed = parseCaseManifest(writeCaseManifest(manifest), &manifestError);
        if (!parsed || !manifestError.isEmpty() || parsed->radarId != QStringLiteral("KTLX") ||
            parsed->frames.size() != 2 || parsed->frameIndex != 1 ||
            parsed->frames.at(0).volumeId != QStringLiteral("v0") ||
            parsed->frames.at(1).failedChunks != 1 || parsed->panels.size() != 1 ||
            parsed->placefileUrls.size() != 1 || parsed->placefileFiles.size() != 1 ||
            parsed->exportedAt != manifest.exportedAt) {
            std::cerr << "Case manifest write/parse identity failed\n";
            return 1;
        }
        const auto mutate = [](QByteArray bytes, const QByteArray& from, const QByteArray& to) {
            return QByteArray(bytes.replace(from, to));
        };
        const auto bytes = writeCaseManifest(manifest);
        if (parseCaseManifest(mutate(bytes, "\"caseFormat\":1", "\"caseFormat\":2"), &manifestError) ||
            manifestError.isEmpty()) {
            std::cerr << "Wrong case format version should fail closed\n";
            return 1;
        }
        if (parseCaseManifest(mutate(bytes, "\"frameIndex\":1", "\"frameIndex\":7"), &manifestError) ||
            manifestError.isEmpty()) {
            std::cerr << "Out-of-range frame index should fail closed\n";
            return 1;
        }
        if (parseCaseManifest(QByteArray("not json"), &manifestError) || manifestError.isEmpty()) {
            std::cerr << "Malformed manifest should fail closed\n";
            return 1;
        }
        if (parseCaseManifest(mutate(bytes, "\"file\":\"volumes/v0.cwdv\"", "\"file\":\"../evil.cwdv\""),
                              &manifestError) ||
            manifestError.isEmpty()) {
            std::cerr << "Escaping frame path should fail closed\n";
            return 1;
        }
    }
    // Full volume cycle: spill, pack, extract, fault back in, same gates.
    {
        if (zipAvailable()) {
            QTemporaryDir cycleDir;
            if (!cycleDir.isValid()) {
                std::cerr << "Temporary dir for case cycle test failed\n";
                return 1;
            }
            DecodedChunk chunk;
            chunk.radarId = "KTLX";
            for (int ray = 0; ray < 3; ++ray) {
                DecodedRay decoded;
                decoded.azimuthDegrees = static_cast<float>(ray * 10);
                decoded.elevationNumber = 1;
                decoded.elevationDegrees = 0.5F;
                const auto index = static_cast<std::size_t>(ProductKind::Reflectivity);
                decoded.fields.at(index).assign(16, static_cast<std::uint16_t>(100 + ray));
                decoded.firstGateByProduct.at(index) = 0;
                decoded.gateSpacingByProduct.at(index) = 1000;
                decoded.scaleByProduct.at(index) = 2.0F;
                decoded.offsetByProduct.at(index) = 66.0F;
                chunk.rays.push_back(std::move(decoded));
            }
            const QString spilled = cycleDir.filePath(QStringLiteral("vol.cwdv"));
            if (!spillVolume(chunk, spilled.toStdString())) {
                std::cerr << "Case cycle spill failed\n";
                return 1;
            }
            QString zipError;
            if (!zipPackFiles({{QStringLiteral("volumes/vol.cwdv"), spilled}},
                              cycleDir.filePath(QStringLiteral("cycle.cwcase")), &zipError)) {
                std::cerr << "Case cycle pack failed: " << zipError.toStdString() << '\n';
                return 1;
            }
            const auto names = zipExtractFiles(cycleDir.filePath(QStringLiteral("cycle.cwcase")),
                                               cycleDir.filePath(QStringLiteral("unpacked")), &zipError);
            if (names.size() != 1 || !zipError.isEmpty()) {
                std::cerr << "Case cycle unpack failed: " << zipError.toStdString() << '\n';
                return 1;
            }
            const auto loaded = loadSpilledVolume(
                cycleDir.filePath(QStringLiteral("unpacked/volumes/vol.cwdv")).toStdString());
            const auto refIndex = static_cast<std::size_t>(ProductKind::Reflectivity);
            if (!loaded || loaded->rays.size() != 3 ||
                loaded->rays.at(2).fields.at(refIndex) != chunk.rays.at(2).fields.at(refIndex)) {
                std::cerr << "Case cycle volume differs after pack/unpack\n";
                return 1;
            }
            // Session rebuild from the unpacked pack: history, index,
            // follow flag, plus a scrub fault-in through the live machinery.
            {
                CaseManifest importManifest;
                importManifest.radarId = QStringLiteral("KTLX");
                importManifest.frameIndex = 0;
                CaseFrame importFrame;
                importFrame.volumeId = QStringLiteral("cycle-vol");
                importFrame.dataTime = QDateTime(QDate(2026, 10, 9), QTime(16, 37),
                                                 QTimeZone::UTC);
                importFrame.chunkCount = 53;
                importFrame.file = QStringLiteral("volumes/vol.cwdv");
                importManifest.frames.push_back(importFrame);
                RadarSession importSession;
                importSession.radarId = QStringLiteral("KTLX");
                QString populateError;
                if (!populateImportedSession(
                        importSession, importManifest,
                        cycleDir.filePath(QStringLiteral("unpacked")), &populateError)) {
                    std::cerr << "populateImportedSession failed: "
                              << populateError.toStdString() << '\n';
                    return 1;
                }
                if (importSession.history.size() != 1 || importSession.frameIndex != 0 ||
                    !importSession.followLatest ||
                    importSession.history.front().volumeId != QStringLiteral("cycle-vol")) {
                    std::cerr << "Imported session state is wrong\n";
                    return 1;
                }
                const auto faulted = sessionFrameChunk(importSession, 0, 12);
                if (!faulted || faulted->rays.size() != 3) {
                    std::cerr << "Imported frame should fault back in\n";
                    return 1;
                }
                // Wrong radar and missing files fail closed with the
                // pre-existing session left alone.
                RadarSession wrongRadar;
                wrongRadar.radarId = QStringLiteral("KAMA");
                if (populateImportedSession(wrongRadar, importManifest,
                                            cycleDir.filePath(QStringLiteral("unpacked")),
                                            &populateError) ||
                    populateError.isEmpty() || !wrongRadar.history.empty()) {
                    std::cerr << "Radar mismatch should fail closed\n";
                    return 1;
                }
                CaseManifest missingFile = importManifest;
                missingFile.frames.front().file = QStringLiteral("volumes/nope.cwdv");
                RadarSession missingSession;
                missingSession.radarId = QStringLiteral("KTLX");
                if (populateImportedSession(missingSession, missingFile,
                                            cycleDir.filePath(QStringLiteral("unpacked")),
                                            &populateError) ||
                    populateError.isEmpty() || !missingSession.history.empty()) {
                    std::cerr << "Missing pack file should fail closed\n";
                    return 1;
                }
            }
        }
    }
    // Offline pack plan: pure tile math, fail-closed on bad regions.
    {
        // Equatorial z1 in a +-20 degree box is exactly 2x2 tiles.
        const auto world = planOfflinePack(0.0, 0.0, 20.0, 1, 1);
        if (!world.error.isEmpty() || world.tiles.size() != 4) {
            std::cerr << "z1 pack of +-20 degrees should be 4 tiles: "
                      << world.error.toStdString() << '\n';
            return 1;
        }
        for (const auto& tile : world.tiles) {
            if (tile.zoom != 1 || tile.x < 0 || tile.x > 1 || tile.y < 0 || tile.y > 1) {
                std::cerr << "Pack tile out of its zoom bounds\n";
                return 1;
            }
        }
        if (world.urlsPerTile < 1 || world.estimatedBytes <= 0) {
            std::cerr << "Pack estimate should be positive\n";
            return 1;
        }
        // A single zoom-0 tile for a small region.
        const auto single = planOfflinePack(35.5, -97.5, 0.5, 0, 0);
        if (!single.error.isEmpty() || single.tiles.size() != 1) {
            std::cerr << "z0 pack should be 1 tile\n";
            return 1;
        }
        const auto badRange = planOfflinePack(0.0, 0.0, 1.0, 8, 4);
        const auto badLat =
            planOfflinePack(std::numeric_limits<double>::quiet_NaN(), 0.0, 1.0, 4, 6);
        const auto badHalf = planOfflinePack(0.0, 0.0, 0.0, 4, 6);
        const auto badWrap = planOfflinePack(0.0, 179.0, 5.0, 4, 6);
        const auto badCap = planOfflinePack(0.0, 0.0, 20.0, 0, 12);
        for (const auto* bad : {&badRange, &badLat, &badHalf, &badWrap, &badCap}) {
            if (bad->error.isEmpty() || !bad->tiles.isEmpty()) {
                std::cerr << "Bad pack region should fail closed with no tiles\n";
                return 1;
            }
        }
    }
    // Font-scale chrome: dock titles, badges, and tabs follow ui/fontScale
    // through late override rules instead of staying pinned at 10 px.
    {
        const auto base = appStylesheet(AppTheme::Classic);
        if (!base.contains(QStringLiteral("QDockWidget::title"))) {
            std::cerr << "Theme sheet lost its dock title rule\n";
            return 1;
        }
        const auto scaled = appStylesheet(AppTheme::Classic, 1.5);
        if (!scaled.contains(QStringLiteral("QTabBar::tab{font-size:15px}")) ||
            !scaled.contains(QStringLiteral("QLabel[layerBadge=\"true\"]{font-size:15px}")) ||
            !scaled.contains(QStringLiteral("QListWidget#warningList{font-size:17px}")) ||
            !scaled.contains(QStringLiteral("QLabel#activeProductLabel{font-size:18px}"))) {
            std::cerr << "Font-scale chrome overrides are wrong\n";
            return 1;
        }
        // Overrides come last so they win the tie with the pinned sizes.
        const auto titlePos = scaled.lastIndexOf(QStringLiteral("QDockWidget::title{font-size:15px}"));
        const auto pinnedPos = scaled.indexOf(QStringLiteral("QDockWidget::title {"));
        if (titlePos < 0 || pinnedPos < 0 || titlePos < pinnedPos) {
            std::cerr << "Font-scale overrides must follow the pinned rules\n";
            return 1;
        }
        const auto squall = appStylesheet(AppTheme::Prototype, 0.8);
        if (!squall.contains(QStringLiteral("QDockWidget::title{font-size:8px}"))) {
            std::cerr << "Squall theme should scale chrome too\n";
            return 1;
        }
    }
    // Spill budget tail-drop must carry the timeline index along: erasing
    // the front shifts every volume down, and a stale frameIndex would
    // point at the next volume over. Null chunks keep this disk-free.
    {
        RadarSession session;
        session.radarId = QStringLiteral("KTLX");
        for (int i = 0; i < 14; ++i) {
            RadarFrame frame;
            frame.volumeId = QStringLiteral("v%1").arg(i);
            session.history.push_back(std::move(frame));
        }
        session.frameIndex = 13;
        enforceSessionSpillBudget(session, 2);
        if (session.history.size() != 10 || session.frameIndex != 9 ||
            session.history.at(static_cast<std::size_t>(session.frameIndex)).volumeId !=
                QStringLiteral("v13")) {
            std::cerr << "Tail-drop should shift frameIndex with the erased front\n";
            return 1;
        }
        // Index pinned at the front clamps instead of underflowing.
        for (int i = 14; i < 16; ++i) {
            RadarFrame frame;
            frame.volumeId = QStringLiteral("v%1").arg(i);
            session.history.push_back(std::move(frame));
        }
        session.frameIndex = 0;
        enforceSessionSpillBudget(session, 2);
        if (session.history.size() != 10 || session.frameIndex != 0 ||
            session.history.front().volumeId != QStringLiteral("v6")) {
            std::cerr << "Tail-drop at index 0 should clamp, not underflow\n";
            return 1;
        }
    }
    // Spill prefetch pick-list: forward-biased proximity (+1,-1,+2,-2),
    // spilled frames only (resident and dropped frames skipped).
    {
        if (nextSpillIndexes(0, 0) != std::vector<int>{} ||
            nextSpillIndexes(5, -1) != std::vector<int>{} ||
            nextSpillIndexes(5, 5) != std::vector<int>{} ||
            nextSpillIndexes(5, 2, 0) != std::vector<int>{}) {
            std::cerr << "nextSpillIndexes should be empty for bad inputs\n";
            return 1;
        }
        if (nextSpillIndexes(5, 2) != std::vector<int>({3, 1, 4, 0})) {
            std::cerr << "nextSpillIndexes(5,2) should be +1,-1,+2,-2\n";
            return 1;
        }
        if (nextSpillIndexes(5, 0) != std::vector<int>({1, 2})) {
            std::cerr << "nextSpillIndexes should skip out-of-range, never clamp\n";
            return 1;
        }
        if (nextSpillIndexes(5, 2, 2) != std::vector<int>({3, 1})) {
            std::cerr << "nextSpillIndexes should honor count\n";
            return 1;
        }
        RadarSession session;
        session.radarId = QStringLiteral("KTLX");
        for (int i = 0; i < 5; ++i) {
            RadarFrame frame;
            frame.volumeId = QStringLiteral("p%1").arg(i);
            if (i == 1 || i == 3) {
                frame.spilledPath = QStringLiteral("/tmp/p%1.cwdv").arg(i);  // spilled
            } else if (i == 2) {
                frame.chunk = std::make_shared<DecodedChunk>();  // resident: skip
            }  // i==0,4 dropped (both empty): skip
            session.history.push_back(std::move(frame));
        }
        session.frameIndex = 2;
        if (SessionPrefetcher::pickSpillIndexes(session) != std::vector<int>({3, 1})) {
            std::cerr << "pickSpillIndexes should return spilled neighbours only, +1 first\n";
            return 1;
        }
    }
    // Prefetch install matches by volumeId so a tail-drop shift mid-flight
    // still lands on the right frame; never resurrects dropped frames.
    {
        RadarSession session;
        session.radarId = QStringLiteral("KTLX");
        for (int i = 0; i < 4; ++i) {
            RadarFrame frame;
            frame.volumeId = QStringLiteral("q%1").arg(i);
            frame.spilledPath = QStringLiteral("/tmp/q%1.cwdv").arg(i);
            session.history.push_back(std::move(frame));
        }
        session.frameIndex = 0;
        auto loaded = std::make_shared<DecodedChunk>();
        // Simulate the front dropping while the disk read is in flight.
        session.history.erase(session.history.begin());
        if (!installPrefetchedChunk(session, QStringLiteral("q2"), loaded, 12) ||
            session.history.at(1).volumeId != QStringLiteral("q2") ||
            session.history.at(1).chunk != loaded) {
            std::cerr << "installPrefetchedChunk should match by volumeId after a shift\n";
            return 1;
        }
        if (installPrefetchedChunk(session, QStringLiteral("q9"), loaded, 12)) {
            std::cerr << "installPrefetchedChunk should fail for an evicted volume\n";
            return 1;
        }
        RadarFrame dropped;
        dropped.volumeId = QStringLiteral("qD");
        session.history.push_back(std::move(dropped));
        if (installPrefetchedChunk(session, QStringLiteral("qD"), loaded, 12)) {
            std::cerr << "installPrefetchedChunk should never resurrect a dropped frame\n";
            return 1;
        }
        if (installPrefetchedChunk(session, QStringLiteral("q2"), loaded, 12)) {
            std::cerr << "installPrefetchedChunk should fail when already resident\n";
            return 1;
        }
        if (installPrefetchedChunk(session, QStringLiteral("q1"), nullptr, 12)) {
            std::cerr << "installPrefetchedChunk should fail for a null chunk\n";
            return 1;
        }
    }
    const QByteArray meteoEmpty = R"XML(<?xml version="1.0" encoding="UTF-8"?>
<feed xmlns="http://www.w3.org/2005/Atom"><id>tag:meteoalarm.org,2021-02-19:NL</id></feed>)XML";
    // The legacy Atom path is retired (visservice below carries geometry);
    // an empty feed simply parses to no NWS warnings and is ignored.
    const auto meteoNone =
        WeatherOverlaySource::parseAlerts(meteoEmpty, &parseError);
    if (!meteoNone.isEmpty()) {
        std::cerr << "Non-GeoJSON input should not produce NWS warnings\n";
        return 1;
    }
    // Meteoalarm visservice helpers: reference hour, level scale, tile math.
    {
        const QDateTime now(QDate(2026, 9, 6), QTime(15, 53, 11), QTimeZone::UTC);
        const auto ref = WeatherOverlaySource::meteoReferenceDate(now);
        if (ref != QDateTime(QDate(2026, 9, 6), QTime(15, 0, 0), QTimeZone::UTC)) {
            std::cerr << "Meteoalarm reference date should truncate to the hour\n";
            return 1;
        }
        if (WeatherOverlaySource::meteoSeverityForLevel(1) != QStringLiteral("Extreme") ||
            WeatherOverlaySource::meteoSeverityForLevel(2) != QStringLiteral("Severe") ||
            WeatherOverlaySource::meteoSeverityForLevel(3) != QStringLiteral("Moderate") ||
            WeatherOverlaySource::meteoSeverityForLevel(9) != QStringLiteral("Unknown")) {
            std::cerr << "Meteoalarm level-to-severity mapping is wrong\n";
            return 1;
        }
        const auto levelStyle = WeatherOverlaySource::meteoalarmLevelStyle(2);
        if (levelStyle.priority != 4 ||
            levelStyle.color.name(QColor::HexRgb).compare(QStringLiteral("#ff9300"),
                                                          Qt::CaseInsensitive) != 0) {
            std::cerr << "Meteoalarm level style should use official colors\n";
            return 1;
        }
        // France bbox covers tile 16/11 at z5.
        const auto tiles = WeatherOverlaySource::meteoTilesForBounds(-5.0, 41.0, 10.0, 51.5);
        bool hasFrance = false;
        for (const auto& tile : tiles) {
            if (tile.first == 16 && tile.second == 11) {
                hasFrance = true;
                break;
            }
        }
        if (!hasFrance || tiles.isEmpty()) {
            std::cerr << "Meteoalarm tile coverage misses France\n";
            return 1;
        }
        const auto tileUrl = WeatherOverlaySource::meteoTileUrl(5, 16, 11, ref);
        if (!tileUrl.toString().contains(QStringLiteral("/stream-buffers/live/tiles/5/16/11.mvt")) ||
            !tileUrl.toString().contains(QStringLiteral("referenceDate=2026-09-06T15")) ||
            !tileUrl.toString().contains(QStringLiteral("preset=now"))) {
            std::cerr << "Meteoalarm tile URL is malformed: " << tileUrl.toString().toStdString()
                      << '\n';
            return 1;
        }
    }
    // MVT areas tile: hand-built gzip PBF (layer "areas", one triangle with a
    // hole, uuid "w1"). Tile z=5 x=16 y=11.
    {
        static const unsigned char kMvtGzip[] = {
            31,  139, 8,   0,   181, 143, 157, 106, 2,   255, 147, 114, 168, 96,  228, 98,
            77,  44,  74,  77,  44,  22,  82,  227, 96,  23,  98,  98,  96,  144, 96,  86,
            146, 225, 60,  193, 120, 130, 81,  104, 1,   27,  195, 124, 182, 5,   108, 252,
            64,  222, 117, 22,  33,  6,   144, 216, 113, 70,  126, 41,  150, 210, 210, 204,
            20,  37,  22,  46,  166, 114, 67,  141, 6,   5,   0,   213, 55,  30,  92,  66,
            0,   0,   0};
        const QByteArray gzip(reinterpret_cast<const char*>(kMvtGzip), sizeof(kMvtGzip));
        const QSet<QString> wanted{QStringLiteral("w1")};
        QString tileError;
        const auto polygons =
            WeatherOverlaySource::parseMvtAreas(gzip, 5, 16, 11, wanted, &tileError);
        if (!meteoTilesAvailable()) {
            if (tileError.isEmpty() || !polygons.isEmpty()) {
                std::cerr << "MVT without zlib should fail closed\n";
                return 1;
            }
        } else {
            if (!tileError.isEmpty() || !polygons.contains(QStringLiteral("w1")) ||
                polygons.value(QStringLiteral("w1")).size() != 1) {
                std::cerr << "MVT areas parsing failed: " << tileError.toStdString() << '\n';
                return 1;
            }
            const auto& polygon = polygons.value(QStringLiteral("w1")).front();
            if (polygon.exterior.size() != 4 || polygon.holes.size() != 1 ||
                polygon.holes.front().size() != 4) {
                std::cerr << "MVT exterior/hole split is wrong\n";
                return 1;
            }
            const auto& first = polygon.exterior.front();
            if (std::abs(first.x() - 0.27466) > 0.001 || std::abs(first.y() - 48.74170) > 0.001) {
                std::cerr << "MVT tile-to-lonlat projection is wrong\n";
                return 1;
            }
            // Unwanted UUIDs are skipped without decoding geometry.
            const QSet<QString> nobody{QStringLiteral("nope")};
            const auto skipped =
                WeatherOverlaySource::parseMvtAreas(gzip, 5, 16, 11, nobody, &tileError);
            if (!tileError.isEmpty() || !skipped.isEmpty()) {
                std::cerr << "MVT should skip unrequested areas\n";
                return 1;
            }
            // Corrupt input fails closed, never crashes.
            const auto garbage = WeatherOverlaySource::parseMvtAreas(
                QByteArray("definitely not gzip data"), 5, 16, 11, wanted, &tileError);
            if (tileError.isEmpty() || !garbage.isEmpty()) {
                std::cerr << "MVT garbage input should fail closed\n";
                return 1;
            }
        }
    }
    map->setWeatherOverlays(warnings, stormReports);
    if (map->warningCount() != 1 || map->stormReportCount() != 1) {
        std::cerr << "Weather overlays did not reach the map\n";
        return 1;
    }
    map->setSites({{"OUTSIDE", "Outside", "WSR-88D", 0.0, 0.0, 0.0}});
    map->setView({-97.2, 35.2}, 6);
    // Background layers fetch live data on the overlay timer: hide them so
    // the card pin/drag/dismiss choreography below cannot snag a live
    // outlook, survey path, or smoke polygon instead of the test warning.
    map->setSpcOutlookVisible(false);
    map->setSpcFireVisible(false);
    map->setWpcVisible(false);
    map->setMcdVisible(false);
    map->setNhcVisible(false);
    map->setHmsVisible(false);
    map->setDatVisible(false);
    map->setHrrrVisible(false);
    map->setGoesVisible(false);
    map->setMosaicVisible(false);
    application.processEvents();
    QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, map->rect().center());
    application.processEvents();
    if (!map->probeText().contains(QLatin1Char('\n')) ||
        !map->probeText().contains(QStringLiteral("Instructions: Take shelter now"))) {
        std::cerr << "Warning detail card lost structured alert text\n";
        return 1;
    }
    const auto initialCardRect = map->infoCardRect();
    const auto dragStart = (initialCardRect.topLeft() + QPointF(90.0, 18.0)).toPoint();
    const auto dragEnd = dragStart + QPoint(-70, 45);
    QTest::mousePress(map, Qt::LeftButton, Qt::NoModifier, dragStart);
    QTest::mouseMove(map, dragEnd);
    QTest::mouseRelease(map, Qt::LeftButton, Qt::NoModifier, dragEnd);
    application.processEvents();
    if (map->infoCardRect().topLeft() == initialCardRect.topLeft()) {
        std::cerr << "Warning detail card could not be dragged by its header\n";
        return 1;
    }
    const auto scrollPosition = map->infoCardRect().center();
    QWheelEvent partialWheel(
        scrollPosition, map->mapToGlobal(scrollPosition.toPoint()),
        QPoint{}, QPoint(0, -30), Qt::NoButton, Qt::NoModifier,
        Qt::ScrollUpdate, false);
    QCoreApplication::sendEvent(map, &partialWheel);
    // The card scrolls on a 16 ms animation timer; give loaded CI runners
    // ample ticks, then pump once more before reading the offset.
    QTest::qWait(500);
    QCoreApplication::processEvents();
    // Card content height is font-metric dependent: only assert scrolling
    // when the card actually overflows. Otherwise scroll is a correct no-op.
    if (map->infoCardScrollable() && map->infoCardScrollOffset() <= 0.0) {
        std::cerr << "Warning detail card discarded high-resolution wheel input\n";
        return 1;
    }
    QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, QPoint(8, map->height() - 8));
    application.processEvents();
    if (!map->probeText().isEmpty()) {
        std::cerr << "Warning detail card did not dismiss after clicking outside it\n";
        return 1;
    }
    map->setCustomPalette({{-10.0, QColor("#001122")}, {40.0, QColor("#ffcc00")}});
    map->setPalette(RadarMapCanvas::PaletteKind::Custom);
    if (map->customPalette().size() != 2 ||
        RadarMapCanvas::colorForValue(ProductKind::Reflectivity, 45.0,
                                      RadarMapCanvas::PaletteKind::Custom, 100,
                                      map->customPalette()).name() != QStringLiteral("#ffcc00")) {
        std::cerr << "Custom palette application failed\n";
        return 1;
    }
    QTemporaryDir animationDirectory;
    const auto animationPath = animationDirectory.filePath(QStringLiteral("timeline.gif"));
    QImage firstFrame(8, 6, QImage::Format_RGB32);
    QImage secondFrame(8, 6, QImage::Format_RGB32);
    firstFrame.fill(QColor("#ff0000"));
    secondFrame.fill(QColor("#0033ff"));
    if (!writeAnimatedGif(animationPath, {firstFrame, secondFrame}, 10, &parseError)) {
        std::cerr << "Animated GIF export failed\n";
        return 1;
    }
    QFile animation(animationPath);
    if (!animation.open(QIODevice::ReadOnly)) return 1;
    const auto animationBytes = animation.readAll();
    if (!animationBytes.startsWith("GIF89a") || !animationBytes.contains("NETSCAPE2.0") ||
        animationBytes.back() != ';' || QImage(animationPath).isNull()) {
        std::cerr << "Animated GIF output is invalid\n";
        return 1;
    }

    DecodedRay ray;
    ray.azimuthDegrees = 90.0F;
    ray.azimuthResolutionDegrees = 1.0F;
    ray.elevationNumber = 1;
    ray.elevationDegrees = 0.5F;
    const auto reflectivityIndex = static_cast<std::size_t>(ProductKind::Reflectivity);
    ray.fields.at(reflectivityIndex).assign(1000, 86);
    ray.firstGateByProduct.at(reflectivityIndex) = 0;
    ray.gateSpacingByProduct.at(reflectivityIndex) = 250;
    ray.scaleByProduct.at(reflectivityIndex) = 2.0F;
    ray.offsetByProduct.at(reflectivityIndex) = 66.0F;
    map->setSites({{"TATL", "Atlanta", "TDWR", 33.6470, -84.2620, 327.7}});
    map->setSelectedRadar(QStringLiteral("TATL"));
    map->setProduct(ProductKind::Reflectivity);
    map->setElevationFilter(1);
    map->setDecodedChunk({"TATL", {ray}, 1}, QDateTime::currentDateTimeUtc());
    LightningCellSnapshot lightning;
    lightning.kind = JumpEventKind::JumpUpdate;
    lightning.eventTimeEpochSec = QDateTime::currentSecsSinceEpoch() - 45;
    lightning.latitudeCenterDeg = 33.65;
    lightning.longitudeCenterDeg = -83.85;
    lightning.sigmaLevel = 3.2;
    lightning.flashRatePerMin = 28.0;
    lightning.cellX = -336;
    lightning.cellY = 134;
    lightning.satelliteNumber = 19;
    lightning.latestDataEpochSec = QDateTime::currentSecsSinceEpoch() - 12;
    map->setLightningCells({lightning});
    if (map->lightningCellCount() != 1) {
        std::cerr << "GLM lightning overlay did not reach the map\n";
        return 1;
    }
    lightningCheck->setChecked(false);
    application.processEvents();
    if (lightningFusionCheck->isEnabled()) {
        std::cerr << "Fusion control remained enabled while GLM overlay was disabled\n";
        return 1;
    }
    lightningCheck->setChecked(true);
    map->recenterSelected();
    map->setView({-84.2620, 33.6470}, 8);
    map->setVolumeQuality(9, 10, 1);
    map->setPalette(RadarMapCanvas::PaletteKind::Operational);
    map->setRadarOpacity(65);
    map->setBelowThresholdVisible(true);
    // Background-context layers fetch live data on the overlay timer, and
    // processEvents pumps those replies into this panel. Hide them so the
    // echo probe below samples radar gates deterministically instead of
    // pinning whatever outlook/MCD/cyclone polygon covers Atlanta today.
    map->setSpcOutlookVisible(false);
    map->setSpcFireVisible(false);
    map->setWpcVisible(false);
    map->setMcdVisible(false);
    map->setNhcVisible(false);
    map->setHmsVisible(false);
    map->setDatVisible(false);
    map->setHrrrVisible(false);
    map->setGoesVisible(false);
    map->setMosaicVisible(false);
    const auto probePoint = map->radarScreenPoint(QStringLiteral("TATL")) + QPoint(20, 0);
    map->inspectAt(QPointF(probePoint));
    if (map->probeText().isEmpty() || !map->probeText().contains(QStringLiteral("dBZ"))) {
        const auto radarPoint = map->radarScreenPoint(QStringLiteral("TATL"));
        std::cerr << "Radar cursor probe failed: " << map->probeText().toStdString()
                  << " radar=" << map->radarId().toStdString()
                  << " point=" << radarPoint.x() << ',' << radarPoint.y()
                  << " size=" << map->width() << 'x' << map->height() << "\n";
        return 1;
    }
    QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier,
                      map->lightningCellScreenPoint(0));
    application.processEvents();
    const auto lightningInfo = map->probeText();
    if (!lightningInfo.contains(QStringLiteral("GLM LIGHTNING JUMP")) ||
        !lightningInfo.contains(QStringLiteral("ACTIVE UPDATE")) ||
        !lightningInfo.contains(QStringLiteral("GOES-19 East")) ||
        !lightningInfo.contains(QStringLiteral("Feed: LIVE")) ||
        !lightningInfo.contains(QStringLiteral("Radar fusion:"))) {
        std::cerr << "GLM intelligence inspector incomplete: "
                  << lightningInfo.toStdString() << '\n';
        return 1;
    }

    // --- Hermetic P1 coverage for shipped-but-untested features (no network) ---
    {
        QString failure;
        if (!testAlertCenterDedupe(&failure)) {
            std::cerr << "AlertCenter dedupe/suppression failed: " << failure.toStdString() << '\n';
            return 1;
        }
        if (!testAlertCenterSeverity(&failure)) {
            std::cerr << "AlertCenter minSeverity failed: " << failure.toStdString() << '\n';
            return 1;
        }
        if (!testAlertCenterLightning(&failure)) {
            std::cerr << "AlertCenter lightning sigma failed: " << failure.toStdString() << '\n';
            return 1;
        }
        if (!testAlertCenterPrimingAndAge(&failure)) {
            std::cerr << "AlertCenter priming/age gate failed: " << failure.toStdString() << '\n';
            return 1;
        }
        if (!testAlertCenterCirculations(&failure)) {
            std::cerr << "AlertCenter circulations failed: " << failure.toStdString() << '\n';
            return 1;
        }
        if (!testHodographPaint(&failure)) {
            std::cerr << "Hodograph paint failed: " << failure.toStdString() << '\n';
            return 1;
        }
        if (!testBasemapTileUrls(&failure)) {
            std::cerr << "Basemap tile URLs failed: " << failure.toStdString() << '\n';
            return 1;
        }
        if (!testOperaKeys(&failure)) {
            std::cerr << "OPERA key helpers failed: " << failure.toStdString() << '\n';
            return 1;
        }
        if (!testThemeSwitcher(&window, &failure)) {
            std::cerr << "Theme switcher failed: " << failure.toStdString() << '\n';
            return 1;
        }
        if (!testSidebarRedesign(&window, &failure)) {
            std::cerr << "Sidebar redesign failed: " << failure.toStdString() << '\n';
            return 1;
        }
        if (!testProximityFilter(&failure)) {
            std::cerr << "Proximity filter failed: " << failure.toStdString() << '\n';
            return 1;
        }
        if (!testOverlayVisibility(&window, &failure)) {
            std::cerr << "OverlayVisibility override failed: " << failure.toStdString() << '\n';
            return 1;
        }
        if (!testPresets(&window, &failure)) {
            std::cerr << "Layout presets failed: " << failure.toStdString() << '\n';
            return 1;
        }
        if (!testViewHistory(&failure)) {
            std::cerr << "View history failed: " << failure.toStdString() << '\n';
            return 1;
        }
        if (!testPinnedLock(&failure)) {
            std::cerr << "Pinned lock failed: " << failure.toStdString() << '\n';
            return 1;
        }
        if (!testPinnedCardKeyboard(&failure)) {
            std::cerr << "Pinned card keyboard failed: " << failure.toStdString() << '\n';
            return 1;
        }
        if (!testProbeClearsOnLeave(&failure)) {
            std::cerr << "Probe leave-clear failed: " << failure.toStdString() << '\n';
            return 1;
        }
        if (!testPlaybackSpeeds(&window, &failure)) {
            std::cerr << "Playback speeds failed: " << failure.toStdString() << '\n';
            return 1;
        }
        if (!testHealthIndicator(&window, &failure)) {
            std::cerr << "Health indicator failed: " << failure.toStdString() << '\n';
            return 1;
        }
        if (!testContextMenuToolModes(&failure)) {
            std::cerr << "Context-menu tool modes failed: " << failure.toStdString() << '\n';
            return 1;
        }
        // Restore main-map view that preset/speed probes may have touched.
        map->activated();
        application.processEvents();
        // Raster-context paint: committed GOES/HRRR fixtures through the
        // production frame path (set + repaint + grab). Deterministic proof
        // the painters survive real grid shapes, row orders, and value
        // ranges; live frames share this exact path.
        {
            const std::string roots[] = {
                "tests/fixtures/",
                "../tests/fixtures/",
                "../../tests/fixtures/",
            };
            const auto findFixture = [&](const char* name) {
                for (const auto& root : roots) {
                    const std::string path = root + name;
                    std::ifstream file(path, std::ios::binary);
                    if (file) return path;
                }
                return std::string{};
            };
            if (goesReaderAvailable()) {
                const auto goesPath = findFixture("goes13_ok.nc");
                if (goesPath.empty()) {
                    std::cerr << "GOES fixture missing\n";
                    return 1;
                }
                const auto frame = readGoesRad(goesPath);
                if (!frame.has_value()) {
                    std::cerr << "GOES fixture did not parse\n";
                    return 1;
                }
                map->setGoesVisible(true);
                map->setGoesFrame(frame);
                map->setView({-137.0, 30.0}, 4);
                application.processEvents();
                const QPixmap goesShot = map->grab();
                if (goesShot.isNull()) {
                    std::cerr << "GOES frame paint produced nothing\n";
                    return 1;
                }
                map->setGoesFrame(std::nullopt);
            }
            if (hrrrReaderAvailable()) {
                const auto hrrrPath = findFixture("hrrr_refc_ok.nc");
                if (hrrrPath.empty()) {
                    std::cerr << "HRRR fixture missing\n";
                    return 1;
                }
                const auto frame = readHrrrRefc(hrrrPath);
                if (!frame.has_value()) {
                    std::cerr << "HRRR fixture did not parse\n";
                    return 1;
                }
                map->setHrrrVisible(true);
                map->setHrrrFrame(frame);
                map->setView({-97.28, 35.33}, 6);
                application.processEvents();
                const QPixmap hrrrShot = map->grab();
                if (hrrrShot.isNull()) {
                    std::cerr << "HRRR frame paint produced nothing\n";
                    return 1;
                }
                map->setHrrrFrame(std::nullopt);
            }
            if (true) {
                // IEM mosaic fixture through the same production path.
                const auto pngPath = findFixture("mosaic_ok.png");
                const auto wldPath = findFixture("mosaic_ok.wld");
                if (pngPath.empty() || wldPath.empty()) {
                    std::cerr << "Mosaic fixture missing\n";
                    return 1;
                }
                QImage image(QString::fromStdString(pngPath));
                if (image.isNull()) {
                    std::cerr << "Mosaic fixture did not decode\n";
                    return 1;
                }
                image = image.convertToFormat(QImage::Format_ARGB32);
                QFile world(QString::fromStdString(wldPath));
                if (!world.open(QIODevice::ReadOnly)) {
                    std::cerr << "Mosaic world fixture missing\n";
                    return 1;
                }
                const auto grid = parseWorldFile(
                    world.readAll().toStdString(),
                    static_cast<std::size_t>(image.width()),
                    static_cast<std::size_t>(image.height()));
                if (!grid.has_value()) {
                    std::cerr << "Mosaic world fixture did not parse\n";
                    return 1;
                }
                MosaicFrame mosaic;
                mosaic.validEpochSeconds = 1791653400LL;
                mosaic.image = image;
                mosaic.grid = *grid;
                map->setMosaicVisible(true);
                map->setMosaicFrame(mosaic);
                map->setView({-95.5, 36.5}, 5);
                application.processEvents();
                const QPixmap mosaicShot = map->grab();
                if (mosaicShot.isNull()) {
                    std::cerr << "Mosaic frame paint produced nothing\n";
                    return 1;
                }
                map->setMosaicFrame(std::nullopt);
            }
        }
        std::cout << "P1 hermetic UI checks passed\n";
    }

    window.resizeDocks({controlDock}, {386}, Qt::Horizontal);
    application.processEvents();
    const auto savedDockWidth = controlDock->width();
    window.close();
    application.processEvents();
    MainWindow restoredWindow;
    restoredWindow.show();
    application.processEvents();
    const auto* restoredDock = restoredWindow.findChild<QDockWidget*>(QStringLiteral("controlDock"));
    if (!restoredDock || std::abs(restoredDock->width() - savedDockWidth) > 12) {
        std::cerr << "Radar sidebar width was not restored: saved=" << savedDockWidth
                  << " restored=" << (restoredDock ? restoredDock->width() : -1) << '\n';
        return 1;
    }
    restoredWindow.close();

    ShortcutHelpDialog helpDialog;
    if (helpDialog.windowTitle() != QStringLiteral("Keyboard shortcuts — CappedWeather")) {
        std::cerr << "Shortcut help dialog title mismatch\n";
        return 1;
    }

    std::cout << "UI smoke passed\n";
    return 0;
}
