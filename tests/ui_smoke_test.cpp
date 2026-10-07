#include "app/main_window.hpp"
#include "app/radar_map_canvas.hpp"
#include "app/radar_list_model.hpp"
#include "app/weather_overlay_source.hpp"
#include "app/gif_writer.hpp"
#include "app/placefile.hpp"
#include "app/placefile_source.hpp"
#include "app/archive_level2.hpp"
#include "app/archive_source.hpp"
#include "app/glm_source.hpp"
#include "app/nexrad_source.hpp"
#include "app/opera_source.hpp"
#include "app/shortcut_help_dialog.hpp"
#include "core/product.hpp"

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
#include <QTimeZone>
#include <QToolButton>

#include <iostream>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
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
    if (chips.size() != 8 || !products) {
        *error = QStringLiteral("product chip grid should have 8 chips, got %1").arg(chips.size());
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
    if (!map || !secondMap || !products || products->count() != 8 ||
        products->findData(static_cast<int>(ProductKind::EchoTop)) < 0 ||
        products->findData(static_cast<int>(ProductKind::VerticallyIntegratedLiquid)) < 0 ||
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
