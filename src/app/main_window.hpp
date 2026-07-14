#pragma once

#include "core/level2_decoder.hpp"
#include "app/weather_overlay_source.hpp"

#include <QMainWindow>
#include <QSet>
#include <QStringList>

#include <vector>

class QByteArray;
class QDateTime;
class QEvent;
class QModelIndex;
class QComboBox;
class QCheckBox;
class QProgressBar;
class QLabel;
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

private slots:
    void radarSelected(const QModelIndex& index);
    void requestSelectedRadar();
    void sourceStatusChanged(const QString& radarId, const QString& status);
    void sourceFailed(const QString& radarId, const QString& message);
    void catalogReceived(const QByteArray& data);
    void volumeStarted(const QString& radarId, const QString& volumeId, int chunkCount);
    void volumeProgress(const QString& radarId, const QString& volumeId, int completed,
                        int total, int decoded, int failed);
    void chunkDecompressed(const QString& radarId, const QString& volumeId, const QString& key,
                           const QByteArray& data, const QDateTime& receivedAt);
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
    QString selectedRadarId() const;

    struct RadarFrame {
        QString volumeId;
        DecodedChunk chunk;
        QDateTime dataTime;
        int chunkCount = 0;
        int failedChunks = 0;
    };

    RadarListModel* model_ = nullptr;
    RadarMapCanvas* map_ = nullptr;
    QComboBox* productCombo_ = nullptr;
    QComboBox* tiltCombo_ = nullptr;
    QComboBox* stationCombo_ = nullptr;
    QComboBox* warningFilterCombo_ = nullptr;
    QComboBox* paletteCombo_ = nullptr;
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
    QString pendingVolumeId_;
    QString radarStatus_ = QStringLiteral("Waiting for NOAA radar data");
    QString overlayStatus_ = QStringLiteral("Waiting for NWS/SPC overlays");
    QStringList recentRadarIds_;
    QSet<QString> favoriteRadarIds_;
    DecodedChunk pendingVolume_;
    int pendingChunkCount_ = 0;
    int pendingFailedChunks_ = 0;
    std::vector<RadarFrame> history_;
    int frameIndex_ = -1;
    bool followLatest_ = true;
    bool radarRequestActive_ = false;
    bool historyRequested_ = false;
    bool historyLoadStarted_ = false;
    bool playWhenHistoryReady_ = false;
};

} // namespace cappedweather
