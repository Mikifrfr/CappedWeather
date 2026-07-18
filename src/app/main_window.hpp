#pragma once

#include "core/level2_decoder.hpp"
#include "app/weather_overlay_source.hpp"

#include <QMainWindow>
#include <QSet>
#include <QHash>
#include <QStringList>
#include <QVector>

#include <vector>
#include <memory>

class QByteArray;
class QDateTime;
class QEvent;
class QModelIndex;
class QComboBox;
class QCheckBox;
class QDoubleSpinBox;
class QCloseEvent;
class QProgressBar;
class QLabel;
class QGridLayout;
class QSlider;
class QTimer;
class QToolButton;

namespace cappedweather {

class NexradSource;
class RadarMapCanvas;
class RadarListModel;
class WeatherOverlaySource;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private slots:
    void radarSelected(const QModelIndex& index);
    void requestSelectedRadar();
    void sourceStatusChanged(const QString& radarId, const QString& status);
    void sourceFailed(const QString& radarId, const QString& message);
    void catalogReceived(const QByteArray& data);
    void volumeStarted(const QString& radarId, const QString& volumeId, int chunkCount);
    void volumeProgress(const QString& radarId, const QString& volumeId, int completed,
                        int total, int decoded, int failed);
    void chunkDecoded(const QString& radarId, const QString& volumeId, const QString& key,
                      DecodedChunk decoded, const QDateTime& receivedAt);
    void volumeReady(const QString& radarId, const QString& volumeId, int chunkCount,
                     int failedChunks, const QDateTime& receivedAt);
    void showFrame(int index);
    void advanceFrame();

private:
    void buildUi();
    void rebuildStationPicker();
    void updateStationSummary();
    void updateUnifiedStatus();
    void rebuildTiltCombo();
    void updateProductAvailability();
    void updatePlaybackControls();
    void ensureHistoryLoaded(bool playWhenReady = false);
    void stopAnimation();
    void acceptDecodedChunk(const QString& radarId, const QString& volumeId, DecodedChunk decoded);
    void restoreSettings();
    void saveSettings() const;
    void showSettingsDialog();
    void selectNearestChaseRadar();
    void setPanelLayout(int layoutIndex);
    void activateMap(RadarMapCanvas* map);
    void synchronizeMapView(RadarMapCanvas* source);
    void updateVelocityControls();
    void showCrossSection(const QPointF& start, const QPointF& end);
    void importCustomPalette();
    void exportSnapshot();
    void exportTimelineAnimation();
    struct RadarSession;
    std::shared_ptr<RadarSession> sessionFor(const QString& radarId, bool create = true);
    std::shared_ptr<RadarSession> activeSession() const;
    int panelIndex(const RadarMapCanvas* map) const;
    QString selectedRadarId() const;

    struct RadarFrame {
        QString volumeId;
        std::shared_ptr<DecodedChunk> chunk;
        QDateTime dataTime;
        int chunkCount = 0;
        int failedChunks = 0;
    };

    struct RadarSession {
        QString radarId;
        QString pendingVolumeId;
        DecodedChunk pendingVolume;
        int pendingChunkCount = 0;
        int pendingFailedChunks = 0;
        int pendingDecodeTasks = 0;
        bool pendingVolumeNetworkReady = false;
        QString pendingReadyVolumeId;
        int pendingReadyChunkCount = 0;
        int pendingReadyFailedChunks = 0;
        QDateTime pendingReadyReceivedAt;
        std::vector<RadarFrame> history;
        int frameIndex = -1;
        bool followLatest = true;
        bool requestActive = false;
        bool historyRequested = false;
        bool historyLoadStarted = false;
        bool playWhenHistoryReady = false;
    };

    RadarListModel* model_ = nullptr;
    RadarMapCanvas* map_ = nullptr;
    QVector<RadarMapCanvas*> maps_;
    QGridLayout* mapGrid_ = nullptr;
    QComboBox* productCombo_ = nullptr;
    QComboBox* tiltCombo_ = nullptr;
    QComboBox* stationCombo_ = nullptr;
    QComboBox* warningFilterCombo_ = nullptr;
    QComboBox* paletteCombo_ = nullptr;
    QComboBox* panelLayoutCombo_ = nullptr;
    QComboBox* velocityModeCombo_ = nullptr;
    QDoubleSpinBox* nyquistSpin_ = nullptr;
    QDoubleSpinBox* stormDirectionSpin_ = nullptr;
    QDoubleSpinBox* stormSpeedSpin_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* stationSummaryLabel_ = nullptr;
    QLabel* frameLabel_ = nullptr;
    QLabel* overlayStatusLabel_ = nullptr;
    QLabel* unifiedStatusLabel_ = nullptr;
    QProgressBar* loadingProgress_ = nullptr;
    QSlider* frameSlider_ = nullptr;
    QSlider* radarOpacitySlider_ = nullptr;
    QToolButton* previousButton_ = nullptr;
    QToolButton* playButton_ = nullptr;
    QToolButton* nextButton_ = nullptr;
    QToolButton* liveButton_ = nullptr;
    QToolButton* favoriteButton_ = nullptr;
    QToolButton* retryButton_ = nullptr;
    QToolButton* cancelButton_ = nullptr;
    QToolButton* measureButton_ = nullptr;
    QToolButton* crossSectionButton_ = nullptr;
    QComboBox* speedCombo_ = nullptr;
    QCheckBox* warningsCheck_ = nullptr;
    QCheckBox* reportsCheck_ = nullptr;
    QCheckBox* tornadoCheck_ = nullptr;
    QCheckBox* hailCheck_ = nullptr;
    QCheckBox* windCheck_ = nullptr;
    QCheckBox* belowThresholdCheck_ = nullptr;
    NexradSource* source_ = nullptr;
    WeatherOverlaySource* overlaySource_ = nullptr;
    QTimer* refreshTimer_ = nullptr;
    QTimer* overlayRefreshTimer_ = nullptr;
    QTimer* animationTimer_ = nullptr;
    QString selectedRadarId_;
    QString radarStatus_ = QStringLiteral("Waiting for NOAA radar data");
    QString overlayStatus_ = QStringLiteral("Waiting for NWS/SPC overlays");
    QStringList recentRadarIds_;
    QSet<QString> favoriteRadarIds_;
    QHash<QString, std::shared_ptr<RadarSession>> radarSessions_;
    QStringList panelRadarIds_;
    bool chaseMode_ = false;
    double chaseLatitude_ = 35.0;
    double chaseLongitude_ = -97.0;
    int maximumHistoryFrames_ = 12;
    int historyVolumeCount_ = 8;
    bool synchronizePanels_ = true;
    bool synchronizingMapView_ = false;
};

} // namespace cappedweather
