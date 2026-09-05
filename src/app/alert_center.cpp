#include "app/alert_center.hpp"

#include "core/radar_geometry.hpp"

#include <QApplication>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QSettings>
#include <QSystemTrayIcon>

#ifdef CAPPEDWEATHER_HAS_MULTIMEDIA
#include <QSoundEffect>
#endif

#include <algorithm>
#include <cmath>
#include <utility>

namespace cappedweather {

namespace {

constexpr int kRecentLimit = 50;

// Generates a tiny 16x16 PNG used as a fallback tray icon. We avoid shipping an
// image asset so the build remains self-contained.
QIcon buildFallbackIcon() {
    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor("#ff5a4f"));
    painter.drawEllipse(1, 1, 14, 14);
    painter.setBrush(QColor("#ffd84d"));
    painter.drawEllipse(4, 4, 8, 8);
    return QIcon(pixmap);
}

QString lightningKey(const LightningCellSnapshot& cell) {
    // A "cell" in the LJA is identified by (satellite, cellX, cellY). Composite
    // those plus the kind so a JumpStart and its follow-up cessation are not
    // collapsed into a single dedupe bucket.
    return QStringLiteral("%1:%2:%3:%4")
        .arg(cell.satelliteNumber)
        .arg(cell.cellX)
        .arg(cell.cellY)
        .arg(static_cast<int>(cell.kind));
}

} // namespace

AlertCenter::AlertCenter(QObject* parent) : QObject(parent) {
    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        tray_ = new QSystemTrayIcon(this);
        tray_->setIcon(buildFallbackIcon());
        tray_->setToolTip(QStringLiteral("CappedWeather alerts"));
    }
#ifdef CAPPEDWEATHER_HAS_MULTIMEDIA
    sound_ = new QSoundEffect(this);
    sound_->setVolume(0.85);
#endif
}

AlertCenter::~AlertCenter() {
    if (tray_) tray_->hide();
}

void AlertCenter::setConfig(AlertConfig config) {
    config_ = std::move(config);
#ifdef CAPPEDWEATHER_HAS_MULTIMEDIA
    if (sound_) sound_->setVolume(0.85);
#endif
    emit configChanged();
}

void AlertCenter::clearMemory() {
    seenWarnings_.clear();
    seenLightningCells_.clear();
    recent_.clear();
    // Next delivery re-primes silently instead of bursting for everything
    // that is still active (e.g. right after the user clears the log).
    warningsPrimed_ = false;
    cellsPrimed_ = false;
}

std::optional<GeographicCoordinate> AlertCenter::filterAnchor() const {
    if (chaseModeActive_) {
        return GeographicCoordinate{chaseLatitude_, chaseLongitude_};
    }
    if (selectedRadar_.valid) {
        return GeographicCoordinate{selectedRadar_.latitude, selectedRadar_.longitude};
    }
    return std::nullopt;
}

void AlertCenter::setSelectedRadar(GeographicCoordinate coordinate) {
    selectedRadar_.latitude = coordinate.latitude;
    selectedRadar_.longitude = coordinate.longitude;
    selectedRadar_.valid = true;
}

void AlertCenter::clearSelectedRadar() {
    selectedRadar_.valid = false;
}

void AlertCenter::setChaseMode(bool enabled, double latitude, double longitude) {
    chaseModeActive_ = enabled;
    chaseLatitude_ = latitude;
    chaseLongitude_ = longitude;
}

void AlertCenter::ingestWarnings(const QVector<WeatherWarning>& warnings) {
    if (!config_.enabled) return;
    const auto now = QDateTime::currentDateTimeUtc();
    const bool priming = !warningsPrimed_;
    const qint64 maxAgeMs = static_cast<qint64>(config_.maxWarningAgeMinutes) * 60 * 1000;
    for (const auto& warning : warnings) {
        if (!eventAllowed(warning.event)) continue;
        if (!severityAllowed(warning.severity)) continue;

        // Geographic filter. If the warning has no polygon (zone-only alert),
        // we cannot verify location and so we accept it through the filter only
        // when geographic filtering is disabled. (This mirrors how the rest of
        // the app already gracefully handles zone-only geometry.)
        if (config_.geographicFilterEnabled) {
            const auto anchor = filterAnchor();
            if (!anchor.has_value()) continue;
            bool inRange = false;
            for (const auto& polygon : warning.polygons) {
                for (const auto& point : polygon.exterior) {
                    if (haversineMeters(anchor->latitude, anchor->longitude,
                                        point.y(), point.x()) <=
                        config_.geographicFilterRadiusKm * 1000.0) {
                        inRange = true;
                        break;
                    }
                }
                if (inRange) break;
            }
            if (!inRange) continue;
        }

        // Dedup: prefer NWS id; fall back to a fingerprint so alerts without
        // ids still avoid spamming.
        const auto key = !warning.id.isEmpty()
                             ? warning.id
                             : QStringLiteral("%1|%2|%3|%4")
                                   .arg(warning.event, warning.onset.toString(Qt::ISODate),
                                        warning.expires.toString(Qt::ISODate),
                                        warning.headline);
        // Stale warnings never notify: record them silently so they cannot
        // fire on a later refresh either. Warnings without an onset always
        // pass (missing data must not suppress a real alert).
        if (!priming && config_.maxWarningAgeMinutes > 0 && warning.onset.isValid() &&
            warning.onset.msecsTo(now) > maxAgeMs) {
            seenWarnings_.insert(key, now);
            continue;
        }
        if (seenWarnings_.contains(key)) {
            const auto last = seenWarnings_.value(key);
            if (last.msecsTo(now) < config_.repeatSuppressionMinutes * 60 * 1000) continue;
        }
        seenWarnings_.insert(key, now);
        if (priming) continue;  // first sync only seeds the dedupe set

        QString detail;
        if (!warning.headline.isEmpty()) detail = warning.headline;
        else if (!warning.description.isEmpty()) detail = warning.description.left(160);
        if (!warning.senderName.isEmpty()) {
            detail = detail.isEmpty() ? warning.senderName
                                      : QStringLiteral("%1 — %2").arg(detail, warning.senderName);
        }
        raiseAlert(QStringLiteral("warning"), warning.event, detail);
    }
    warningsPrimed_ = true;
    pruneSeenEntries(seenWarnings_);
}

void AlertCenter::ingestLightningCells(const std::vector<LightningCellSnapshot>& cells) {
    if (!config_.enabled) return;
    const auto now = QDateTime::currentDateTimeUtc();
    const bool priming = !cellsPrimed_;
    for (const auto& cell : cells) {
        // Only JumpStart cells (not cessations) at or above the sigma threshold.
        if (cell.sigmaLevel < config_.lightningSigmaThreshold) continue;
        if (!cell.active()) continue;

        if (config_.geographicFilterEnabled) {
            const auto anchor = filterAnchor();
            if (!anchor.has_value()) continue;
            if (haversineMeters(anchor->latitude, anchor->longitude,
                                cell.latitudeCenterDeg, cell.longitudeCenterDeg) >
                config_.geographicFilterRadiusKm * 1000.0) {
                continue;
            }
        }

        const auto key = lightningKey(cell);
        if (seenLightningCells_.contains(key)) {
            const auto last = seenLightningCells_.value(key);
            if (last.msecsTo(now) < config_.repeatSuppressionMinutes * 60 * 1000) continue;
        }
        seenLightningCells_.insert(key, now);
        if (priming) continue;  // first sync only seeds the dedupe set

        const auto title = QStringLiteral("Lightning jump σ%1").arg(cell.sigmaLevel, 0, 'f', 1);
        const auto detail = QStringLiteral("%1 flashes/min at (%2°, %3°)")
                                .arg(cell.flashRatePerMin, 0, 'f', 1)
                                .arg(cell.latitudeCenterDeg, 0, 'f', 3)
                                .arg(cell.longitudeCenterDeg, 0, 'f', 3);
        raiseAlert(QStringLiteral("lightning-jump"), title, detail);
    }
    cellsPrimed_ = true;
    pruneSeenEntries(seenLightningCells_);
}

void AlertCenter::triggerTestAlert() {
    raiseAlert(QStringLiteral("test"),
               QStringLiteral("CappedWeather alert test"),
               QStringLiteral("Sound + tray notification round-trip OK"));
}

bool AlertCenter::eventAllowed(const QString& event) const {
    if (!config_.eventFilterEnabled) return true;
    return config_.eventFilter.contains(event);
}

bool AlertCenter::severityAllowed(const QString& severity) const {
    // NWS CAP severity values: "Extreme", "Severe", "Moderate", "Minor", "Unknown".
    // Map to the integer scale documented on AlertConfig::minSeverity.
    int level = 1;  // Unknown defaults to Moderate so unclassified warnings still alert.
    const QString trimmed = severity.trimmed();
    if (trimmed.compare(QLatin1String("Extreme"), Qt::CaseInsensitive) == 0) level = 3;
    else if (trimmed.compare(QLatin1String("Severe"), Qt::CaseInsensitive) == 0) level = 2;
    else if (trimmed.compare(QLatin1String("Moderate"), Qt::CaseInsensitive) == 0) level = 1;
    else if (trimmed.compare(QLatin1String("Minor"), Qt::CaseInsensitive) == 0) level = 0;
    return level >= config_.minSeverity;
}

void AlertCenter::raiseAlert(const QString& kind, const QString& title, const QString& detail) {
    AlertRecord record{kind, title, detail, QDateTime::currentDateTimeUtc()};
    recent_.prepend(record);
    if (recent_.size() > kRecentLimit) recent_.resize(kRecentLimit);
    emit alertRaised(record);
    if (config_.soundEnabled) playSound();
    if (config_.systemNotificationEnabled) showSystemNotification(title, detail);
    emit statusChanged(QStringLiteral("Alert: %1 — %2").arg(title, detail));
}

void AlertCenter::playSound() {
#ifdef CAPPEDWEATHER_HAS_MULTIMEDIA
    if (!sound_) return;
    if (!config_.customSoundPath.isEmpty()) {
        sound_->setSource(QUrl::fromLocalFile(config_.customSoundPath));
        sound_->play();
        return;
    }
#endif
    // Without a custom sound file (or when Qt6::Multimedia is unavailable),
    // fall back to the platform's standard alert tone. QApplication::beep is
    // a no-op on platforms without a console bell but the tray notification
    // remains the primary attention channel.
    if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) {
        QMetaObject::invokeMethod(app, "beep", Qt::DirectConnection);
    }
}

void AlertCenter::showSystemNotification(const QString& title, const QString& detail) {
    if (!tray_ || !tray_->isVisible()) {
        if (tray_) tray_->show();
        else return;
    }
    tray_->showMessage(title, detail, QSystemTrayIcon::Warning, 6000);
}

void AlertCenter::pruneSeenEntries(QHash<QString, QDateTime>& seen) const {
    // Drop entries older than the suppression window so memory stays bounded
    // across long uptimes. A weekly ceiling is more than enough.
    const auto cutoff = QDateTime::currentDateTimeUtc().addDays(-7);
    for (auto it = seen.begin(); it != seen.end();) {
        if (it.value() < cutoff) it = seen.erase(it);
        else ++it;
    }
}

} // namespace cappedweather
