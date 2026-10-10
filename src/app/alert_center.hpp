#pragma once

#include "core/radar_geometry.hpp"
#include "app/weather_overlay_source.hpp"
#include "core/circulation_detector.hpp"
#include "core/lightning_overlay.hpp"

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>
#include <utility>

class QSoundEffect;
class QSystemTrayIcon;

namespace cappedweather {

// Persistent settings shared with the Settings dialog.
struct AlertConfig {
    // Master enable. When false, all alerts are silently dropped.
    bool enabled = true;
    // Audio side (system beep or sound file).
    bool soundEnabled = true;
    // System tray balloon / OS notification.
    bool systemNotificationEnabled = true;
    // Restrict to the selected radar's coverage (a fixed radius around the radar)
    // or, when chase mode is on, around the chase point.
    bool geographicFilterEnabled = false;
    // Radius used by the geographic filter, in km.
    double geographicFilterRadiusKm = 200.0;
    // When true, only warnings matching one of the eventFilter events trigger alerts.
    // When false (default), every NWS event is considered.
    bool eventFilterEnabled = false;
    QStringList eventFilter;
    // Trigger lightning alerts on cells whose sigmaLevel >= this threshold.
    double lightningSigmaThreshold = 2.0;
    // Opt-in TVS/mesocyclone marker alerts. Off by default: rotation markers
    // are guidance (algorithm output, not NWS warnings), so they must never
    // page the operator unless explicitly asked. When on, TVS notifies at
    // Severe+ and mesocyclones at Moderate+, honoring the severity knob.
    bool circulationAlertsEnabled = false;
    // Minimum NWS CAP severity to alert on. The NWS severity taxonomy is
    // {Extreme, Severe, Moderate, Minor, Unknown}; the default (Severe) means
    // Tornado/Severe Thunderstorm warnings and up. Anything tagged Moderate
    // or Minor (Dense Fog Advisory, Air Quality Alert, Special Weather
    // Statement, etc.) is dropped by default. Unknown is treated as Moderate
    // so that the rare unclassified event still notifies the operator.
    //   0 = Minor, 1 = Moderate, 2 = Severe, 3 = Extreme
    int minSeverity = 2;
    // Suppress repeat alerts for the same cell/warning for this many minutes.
    int repeatSuppressionMinutes = 30;
    // Maximum age of a warning's onset for it to trigger a notification,
    // in minutes. Older warnings are recorded silently so they never fire
    // later, but they make no sound and show no banner. This stops the
    // classic spam: opening the app (or enabling the geo filter) while
    // hours-old warnings are still active on the NWS feed. 0 = no age
    // limit; every new warning notifies regardless of onset. Default 60.
    int maxWarningAgeMinutes = 60;
    // Custom sound file path. When empty, falls back to a generated beep.
    QString customSoundPath;
};

// One row in the alert history (used by the Settings dialog "Test" + future log).
struct AlertRecord {
    QString kind;        // "warning", "lightning-jump", or "rotation"
    QString title;
    QString detail;
    QDateTime at;
};

// AlertCenter is the single sink for warning + lightning jump events that
// warrants operator attention. It deduplicates alerts (per NWS id, per cell id),
// applies event + geographic filters, and surfaces results through:
//   - a QSystemTrayIcon (when the platform supports one)
//   - a QSoundEffect (or QApplication::beep fallback)
//   - an in-memory log of recent alerts (AlertRecord::recent)
//   - a statusChanged signal so MainWindow can update the status row
class AlertCenter final : public QObject {
    Q_OBJECT

public:
    explicit AlertCenter(QObject* parent = nullptr);
    ~AlertCenter() override;

    // Push hooks called from MainWindow. Each call should be made once per
    // overlaysReady / cellsUpdated delivery. They are non-blocking and safe to
    // call from the main thread.
    void ingestWarnings(const QVector<WeatherWarning>& warnings);
    void ingestLightningCells(const std::vector<LightningCellSnapshot>& cells);
    // TVS/mesocyclone markers from the canvases (persisted 2+-volume tracks
    // only: callers pass what they draw). Opt-in via circulationAlertsEnabled;
    // every record is labeled guidance. radarId scopes the dedup key so two
    // panels on different radars never suppress each other.
    void ingestCirculations(const std::vector<CirculationDetection>& detections,
                            const QString& radarId);

    // Resets the dedup memory (e.g. when the user explicitly clears the log).
    void clearMemory();

    AlertConfig config() const { return config_; }
    void setConfig(AlertConfig config);

    // Returns the most recent alerts (newest first). Bounded to a small ring.
    const QVector<AlertRecord>& recent() const { return recent_; }

    // Triggers a synthetic alert end-to-end (sound + tray + log). Useful for the
    // Settings dialog's "Test" button and the Help menu.
    void triggerTestAlert();

    // Geographic filter helpers. Both return std::nullopt when the source is
    // not currently meaningful (e.g. no radar selected + chase mode off).
    std::optional<GeographicCoordinate> filterAnchor() const;
    void setSelectedRadar(GeographicCoordinate coordinate);
    void clearSelectedRadar();
    void setChaseMode(bool enabled, double latitude, double longitude);

signals:
    void statusChanged(QString text);
    void alertRaised(AlertRecord record);
    void configChanged();

private:
    struct RadarCoord {
        double latitude = 0.0;
        double longitude = 0.0;
        bool valid = false;
    };

    bool eventAllowed(const QString& event) const;
    // Returns true when the warning's CAP severity meets or exceeds
    // config_.minSeverity. Unknown severity is conservatively treated as
    // Moderate so unclassified warnings still reach the operator when the
    // threshold is at or below Moderate.
    bool severityAllowed(const QString& severity) const;
    void raiseAlert(const QString& kind, const QString& title, const QString& detail);
    void playSound();
    void showSystemNotification(const QString& title, const QString& detail);
    void pruneSeenEntries(QHash<QString, QDateTime>& seen) const;

    AlertConfig config_;
    QSystemTrayIcon* tray_ = nullptr;
    QSoundEffect* sound_ = nullptr;
    // Deduplication sets: warningId (NWS @id) and lightning cell key.
    QHash<QString, QDateTime> seenWarnings_;
    QHash<QString, QDateTime> seenLightningCells_;
    QHash<QString, QDateTime> seenCirculations_;
    // First-sync priming: the first delivery after construction (or after
    // clearMemory) only seeds the dedupe sets without notifying, so alerts
    // that were already active before the operator started watching do not
    // all fire at once. Only genuinely new arrivals notify afterwards.
    bool warningsPrimed_ = false;
    bool cellsPrimed_ = false;
    bool circulationsPrimed_ = false;
    // Bounded ring of recent alerts.
    QVector<AlertRecord> recent_;
    RadarCoord selectedRadar_;
    bool chaseModeActive_ = false;
    double chaseLatitude_ = 0.0;
    double chaseLongitude_ = 0.0;
};

} // namespace cappedweather
