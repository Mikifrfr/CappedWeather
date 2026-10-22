#pragma once

#include "core/level2_decoder.hpp"
#include "core/bunkers.hpp"
#include "core/srh.hpp"
#include "core/hail_swath.hpp"
#include "app/alert_center.hpp"
#include "app/scan_age.hpp"
#include "app/session_store.hpp"
#include "app/theme.hpp"
#include "app/weather_overlay_source.hpp"

#include <QMainWindow>
#include <QMap>
#include <QDateTime>
#include <QPointer>
#include <QPointF>
#include <QSet>
#include <QHash>
#include <QStringList>
#include <QVector>

#include <vector>
#include <memory>

class QByteArray;
class QCompleter;
class QDateTime;
class QDateTimeEdit;
class QDialog;
class QDockWidget;
class QEvent;
class QModelIndex;
class QComboBox;
class QCheckBox;
class QDoubleSpinBox;
class QCloseEvent;
class QProgressBar;
class QResizeEvent;
class QLabel;
class QGridLayout;
class QVBoxLayout;
class QListWidget;
class QListWidgetItem;
class QSlider;
class QSpinBox;
class QTabBar;
class QTabWidget;
class QTableWidget;
class QTimer;
class QToolButton;

namespace cappedweather {

class NexradSource;
class OperaSource;
class GlmSource;
class RadarMapCanvas;
class CrossSectionWidget;
class RadarListModel;
class WeatherOverlaySource;
class SpcConvectiveSource;
class NhcCycloneSource;
class WpcEroSource;
class HmsSmokeSource;
class DatPathSource;
class HrrrSource;
class GoesSource;
class MosaicSource;
class UpdateChecker;
class MetarSource;
class PlacefileSource;
class ArchiveSource;
class AlertCenter;
class TimelineStrip;
class CommandPalette;

enum class RadarHealthState { Unknown, Online, Stale, Offline };

struct LayoutPreset {
    QString name;
    int layoutIndex = 0; // 0=1x1, 1=1x2, 2=2x1, 3=2x2
    int product[4] = {0, 0, 0, 0};
    int tilt[4] = {-1, -1, -1, -1}; // -1 = any
    bool builtIn = false;
};

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

    // Switchable identity (Classic / Squall prototype). Applies the palette,
    // stylesheet, panel spacing, and canvas accent colors live.
    void applyTheme(AppTheme theme);
    AppTheme theme() const { return theme_; }
    void applyFontScale(double scale);

    // GIS export for one frame: PNG snapshot plus a KML GroundOverlay with
    // the panel's current lon/lat box (current georeferencing, north-up, no
    // reprojection). Public and static so the KML builder is unit-testable.
    static QString frameGroundOverlayKml(const QString& name, const QString& pngHref, double north,
                                         double south, double east, double west, QString* error);
    // Hail-swath Placemark polygons for the frame KML: one outer ring per
    // swath hull, tier-tinted, with a guidance disclaimer in the description.
    // Empty (not an error) when there is nothing drawable. Unit-testable.
    static QString swathPlacemarksKml(const std::vector<HailSwath>& swaths, QString* error);
    // World file (.pgw) sidecar for PNG snapshots: six plain-text lines
    // mapping image pixels to lon/lat so GIS tools load the grab in place.
    static QString frameWorldFile(double north, double south, double east, double west,
                                  int imageWidth, int imageHeight, QString* error);
    // SCAN age block: "SCAN 14:22:10Z · VCP 212 · 3 min old · 1080 rays".
    // The wording, the clamp and the VCP-token rule live in app/scan_age.hpp;
    // this symbol stays because tests/ui_smoke_test.cpp pins it. Public and
    // static so the label format is unit-testable.
    static QString scanAgeLabelText(const QDateTime& scanTimeUtc, int rayCount, qint64 ageSeconds,
                                    int volumeCoveragePattern = 0);
    // SCAN age block contents for one history frame: what the console actually
    // shows. Built from the frame's scan time/id and the chunk rendered for it,
    // so the frame -> VCP -> label wiring has one pure, testable source. The
    // 30 s age tick re-renders this same state; it holds no chunk reference
    // (a retained shared_ptr would keep a spilled volume resident).
    struct ScanAgeState {
        QDateTime scanTimeUtc;
        QString volumeId;
        int rayCount = -1;              // <0 = chunk not resident ("— rays")
        int volumeCoveragePattern = 0;  // 0 = no vouchable pattern (no VCP token)
        static ScanAgeState fromFrame(const cappedweather::RadarFrame& frame,
                                      const DecodedChunk* shownChunk);
    };
    // Renders ScanAgeState into the SCAN label: text plus the stale tint.
    // showFrame() snapshots through fromFrame() and refreshScanAgeLabel()
    // re-renders through here, so this is the one path the console renders.
    // Public and static so the UI harness can assert frame -> label against
    // the real widget (no session, no archive lane, no network).
    static void renderScanAgeBlock(QLabel* label, const ScanAgeState& state, qint64 ageSeconds);
    // Whole-minute age at which the SCAN block turns amber. Forwards
    // cappedweather::kScanStaleSeconds so the threshold has one home and this
    // symbol still exists for the suite that compiles against it.
    static constexpr qint64 kScanStaleSeconds = cappedweather::kScanStaleSeconds;
    // Z-R rainfall readout (Display tab). A lower-bound estimate over the
    // volume history the session actually holds — never a measurement, never a
    // bare number. Line 1 names the estimate and its peak, line 2 its window
    // and coverage, line 3 the quality flag and the dominant caveat. Public and
    // static because the wording IS the honesty contract: unit-tested exactly.
    static QString rainfallEstimateLine(const RainfallSummary& summary);
    static QString rainfallCoverageLine(const RainfallSummary& summary);
    static QString rainfallQualityLine(const RainfallSummary& summary);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

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
                      std::shared_ptr<DecodedChunk> decoded, const QDateTime& receivedAt);
    void volumeReady(const QString& radarId, const QString& volumeId, int chunkCount,
                     int failedChunks, const QDateTime& receivedAt, bool live = true);
    void showFrame(int index);
    void advanceFrame();
    // Scan diagnostics pop-out: a real slot (not just a method) so tests,
    // the command palette, and the Details button share one entry point.
    void showScanDiagnostics();
    // Z-R rainfall pop-out: the dock block's numbers plus the per-flag
    // provenance table and the unvalidated warning. Real slot for the same
    // reason (tests, palette, Details button share it).
    void showRainfallDetails();
    // RAOB sounding pop-out: hodograph plus sounding SRH ground truth and
    // freezing level for the current Bunkers motion. Real slot like above.
    void showSoundingDialog();
    // Async update discovery: Help action plus completion dialog. Real slot
    // so tests and the command palette share it with the menu.
    void checkForUpdates();

private:
    // Silent variant for the weekly auto-check (surfaces newer builds on
    // the status bar only). Shared fetch path with the dialog above.
    void runUpdateCheck(bool silent);
    void buildUi();
    void rebuildStationPicker();
    void updateStationSummary();
    void updateUnifiedStatus();
    void applyUnifiedStatusElision();
    void rebuildTiltCombo();
    void updateProductAvailability();
    void updatePlaybackControls();
    // Re-renders the SCAN age block from the showFrame() snapshot so the
    // displayed age counts up between frames. No-op without a shown scan.
    void refreshScanAgeLabel();
    void ensureHistoryLoaded(bool playWhenReady = false);
    void stopAnimation();
    void acceptDecodedChunk(const QString& radarId, const QString& volumeId,
                            std::shared_ptr<DecodedChunk> decoded);
    void finalizeVolumeReady(const QString& radarId, const QString& volumeId, int chunkCount,
                             int failedChunks, const QDateTime& receivedAt,
                             std::shared_ptr<DecodedChunk> volume, bool live = true);
    // Background derived-product swap: replaces a published base frame's
    // chunk once ET18/VIL finish off the critical path.
    void swapDerivedVolume(const QString& radarId, const QString& volumeId,
                           std::shared_ptr<DecodedChunk> derived, double cappiHeightM);
    // CAPPI altitude (global MSL setting): re-derives resident histories in
    // the background; spilled frames re-derive lazily on display.
    void setCappiHeightM(double heightM);
    void refreshCappiBox();
    void rederiveAllForCappiHeight();
    void launchFrameRederive(const QString& radarId, const QString& volumeId,
                             std::shared_ptr<DecodedChunk> base, double radarElevationMeters,
                             double cappiHeightM);
    void updateCrossSectionAvailability();
    void restoreSettings();
    void saveSettings() const;
    void showSettingsDialog();
    void selectNearestChaseRadar();
    void setPanelLayout(int layoutIndex);
    void applyLayoutPreset(const QString& name);
    void saveCurrentAsPreset();
    void rebuildPresetsCombo();
    QVector<LayoutPreset> builtInPresets() const;
    LayoutPreset presetByName(const QString& name) const;
    void activateMap(RadarMapCanvas* map);
    void synchronizeMapView(RadarMapCanvas* source);
    void refreshActivePanelOverlayUi();
    void refreshLayerBadges();
    void refreshWarningList();
    void refreshTimelineStrip();
    void toggleFrameBookmark();
    void refreshScopeVisibility();
    void syncProductChips();
    void rebuildTiltGrid();
    void pushAlertDockToCenter();
    void refreshAlertDockFromCenter();
    void pushProximityFilterToCanvases();
    // OPERA (European ODIM) radars flow through OperaSource instead of the
    // NEXRAD chunk pipeline; the volume signals are identical downstream.
    bool isOperaRadar(const QString& radarId) const;
    void requestLatestFor(const QString& radarId);
    void requestRecentFor(const QString& radarId, int volumeCount);
    void cancelFor(const QString& radarId);
    void clearDownloadedFor(const QString& radarId);
    void updatePanelAttribution();
    void setRadarHealth(RadarHealthState state, const QString& tooltip = {});
    void recomputeRadarHealth();
    RadarHealthState classifyHealth() const;
    void updateVelocityControls();
    void showCrossSection(const QPointF& start, const QPointF& end);
    void refreshCrossSection();
    void refreshScanDialog();
    void populateScanDialog();
    // Rainfall readout: snapshot the active session's window on the GUI thread
    // (no I/O), integrate it on a worker, and install the result back on the UI
    // thread. scheduleRainfallEstimate() no-ops when the window signature has
    // not changed, so it is safe to call from every frame/volume/selection path.
    void scheduleRainfallEstimate();
    void startRainfallCompute();
    void applyRainfallEstimate(RainfallHistoryResult result);
    void refreshRainfallLabels();
    void populateRainfallDialog();
    void togglePanelMaximized();
    // Moves the single product tab bar directly above the active panel cell
    // (1x1 / maximized: top of the column; multi-panel: grid tab row over
    // the active column/row). Tab edits still drive the active panel only.
    void repositionProductTabs();
    void updateOfflineBanner();
    void applyDataSaverPreset();
    void setupMenus();
    void showCommandPalette();
    void showAboutDialog();
    void importCustomPalette();
    void refreshPalettePreview();
    void exportSnapshot();
    void exportTimelineAnimation();
    void exportTimelineMp4();
    // Frame-exact VP9 through an ffmpeg image pipe (GIF path's frames,
    // sharing-friendly container). Fail-closed when ffmpeg is missing.
    void exportTimelineWebM();
    void exportFrameKml();
    // Case-pack: export the active radar session (volumes, panels,
    // placefiles, timeline position) to one .cwcase zip; import rebuilds
    // the session from a pack so a shared case reproduces exactly.
    void exportCasePack();
    void importCasePack();
    // Offline basemap pack for field chasing: pre-downloads tiles around
    // the selected radar into the shared tile cache.
    void downloadOfflinePack();
    // Spill prefetch, one session at a time (scheduleSpillPrefetch fans out
    // to every panel session; job keys dedupe shared sessions).
    void prefetchSessionSpills(const std::shared_ptr<RadarSession>& session);
    void openLocalPlacefile();
    void addPlacefileUrl();
    void clearPlacefiles();
    void openArchiveFiles();
    void openArchiveFolder();
    void openAwsArchive();
    void prepareArchiveCase(int requestedFrames);
    // Session types live in session_store.hpp so history/spill logic is
    // testable without a QMainWindow. Aliases keep MainWindow::RadarFrame /
    // MainWindow::RadarSession valid at all existing call sites.
    using RadarSession = cappedweather::RadarSession;
    using RadarFrame = cappedweather::RadarFrame;
    std::shared_ptr<RadarSession> sessionFor(const QString& radarId, bool create = true);
    std::shared_ptr<RadarSession> activeSession() const;
    int panelIndex(const RadarMapCanvas* map) const;
    QString selectedRadarId() const;
    // Spilled-frame access: returns the resident chunk, faulting back from
    // disk on a scrub hit (and re-enforcing the RAM budget afterwards).
    // Null only when the frame is missing or its spill is unreadable.
    // Delegates to session_store::{sessionFrameChunk,enforceSessionSpillBudget}.
    std::shared_ptr<DecodedChunk> frameChunk(RadarSession& session, std::size_t index);
    // Keeps the newest maximumHistoryFrames_ volumes resident, spills older
    // ones to disk (up to kSpilledKeepVolumes), drops the rest entirely.
    void enforceSpillBudget(RadarSession& session, int excludeIndex = -1);
    // Background spill warmer: faults the +1/-1 (+2) spilled neighbours back
    // into RAM off the UI thread so the next scrub step hits memory. Queued
    // from showFrame(); installs by volumeId via installPrefetchedChunk().
    void scheduleSpillPrefetch();

    RadarListModel* model_ = nullptr;
    RadarMapCanvas* map_ = nullptr;
    QVector<RadarMapCanvas*> maps_;
    QGridLayout* mapGrid_ = nullptr;
    // Focus-following product tabs: the single tab bar is reparented between
    // the top column (1x1 / maximized) and a tab row directly above the
    // active panel cell (1x2 / 2x1 / 2x2). Members pin the layouts so
    // repositionProductTabs() can move the bar without rebuilding the UI.
    QWidget* mapHost_ = nullptr;
    QVBoxLayout* mapColumn_ = nullptr;
    QWidget* mapGridHost_ = nullptr;
    int panelLayoutIndex_ = 0;
    QDockWidget* controlDock_ = nullptr;
    QDockWidget* playbackDock_ = nullptr;
    QTabWidget* sidebarTabs_ = nullptr;
    QListWidget* warningList_ = nullptr;
    // Last overlaysReady snapshot (same order the canvases hold): feeds the
    // warning inbox. Indexes double as canvas warning indexes.
    QVector<WeatherWarning> latestWarnings_;
    QComboBox* productCombo_ = nullptr;
    QTabBar* productTabBar_ = nullptr;
    QVector<QToolButton*> productChips_;
    // Spells out the active product; the chips themselves only carry the short
    // operational abbreviation so a two-column grid fits a narrow dock.
    QLabel* activeProductLabel_ = nullptr;
    QComboBox* tiltCombo_ = nullptr;
    QToolButton* tiltMinus_ = nullptr;
    QToolButton* tiltPlus_ = nullptr;
    QToolButton* tiltValueButton_ = nullptr;
    QWidget* tiltGridBox_ = nullptr;
    QVector<QToolButton*> tiltGridButtons_;
    QWidget* velocityBox_ = nullptr;
    // CAPPI altitude selector (global MSL height for the derived CAPPI
    // field). Shown only while the active panel views CAPPI, like the
    // velocity box shows only for VEL.
    QWidget* cappiBox_ = nullptr;
    QComboBox* cappiHeightCombo_ = nullptr;
    double cappiHeightM_ = 3000.0;
    QComboBox* stationCombo_ = nullptr;
    QComboBox* warningFilterCombo_ = nullptr;
    QComboBox* paletteCombo_ = nullptr;
    QWidget* palettePreview_ = nullptr;
    QComboBox* panelLayoutCombo_ = nullptr;
    QComboBox* presetsCombo_ = nullptr;
    QToolButton* savePresetButton_ = nullptr;
    QComboBox* velocityModeCombo_ = nullptr;
    QDoubleSpinBox* nyquistSpin_ = nullptr;
    QDoubleSpinBox* stormDirectionSpin_ = nullptr;
    QDoubleSpinBox* stormSpeedSpin_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* stationSummaryLabel_ = nullptr;
    QLabel* frameLabel_ = nullptr;
    // SCAN age block cache: showFrame() snapshots the shown frame through
    // ScanAgeState::fromFrame(); refreshScanAgeLabel() re-renders the
    // whole-minute age from the snapshot so the 30 s health tick counts
    // without faulting spilled volumes back from disk.
    ScanAgeState scanAgeState_;
    QLabel* overlayStatusLabel_ = nullptr;
    QLabel* unifiedStatusLabel_ = nullptr;
    QString unifiedStatusFullText_;
    QLabel* glmStatusNote_ = nullptr;
    QLabel* emptyStateHint_ = nullptr;
    QLabel* offlineBanner_ = nullptr;
    QToolButton* emptyStateGoButton_ = nullptr;
    QProgressBar* loadingProgress_ = nullptr;
    QSlider* frameSlider_ = nullptr;
    TimelineStrip* timelineStrip_ = nullptr;
    QDateTimeEdit* timeJumpEdit_ = nullptr;
    QSlider* radarOpacitySlider_ = nullptr;
    QSlider* warningOpacitySlider_ = nullptr;
    QCheckBox* warningsOnTopCheck_ = nullptr;
    QToolButton* maximizePanelButton_ = nullptr;
    QToolButton* dataSaverButton_ = nullptr;
    QCheckBox* proximityFilterCheck_ = nullptr;
    QDoubleSpinBox* proximityFilterRadius_ = nullptr;
    QToolButton* previousButton_ = nullptr;
    QToolButton* playButton_ = nullptr;
    QToolButton* nextButton_ = nullptr;
    // Follow-latest toggle (session->followLatest UI; the session logic is
    // unchanged, only the LIVE text/dot is gone).
    QToolButton* followLatestButton_ = nullptr;
    QToolButton* favoriteButton_ = nullptr;
    QToolButton* retryButton_ = nullptr;
    QToolButton* cancelButton_ = nullptr;
    QToolButton* measureButton_ = nullptr;
    QToolButton* crossSectionButton_ = nullptr;
    QComboBox* speedCombo_ = nullptr;
    QCheckBox* warningsCheck_ = nullptr;
    QLabel* warningsBadge_ = nullptr;
    QCheckBox* reportsCheck_ = nullptr;
    QLabel* reportsBadge_ = nullptr;
    QCheckBox* tornadoCheck_ = nullptr;
    QCheckBox* hailCheck_ = nullptr;
    QCheckBox* windCheck_ = nullptr;
    QCheckBox* belowThresholdCheck_ = nullptr;
    QCheckBox* lightningCheck_ = nullptr;
    QLabel* lightningBadge_ = nullptr;
    QCheckBox* lightningFusionCheck_ = nullptr;
    QLabel* fusionBadge_ = nullptr;
    QCheckBox* shearCheck_ = nullptr;
    QLabel* shearBadge_ = nullptr;
    QCheckBox* tvsCheck_ = nullptr;
    QLabel* tvsBadge_ = nullptr;
    QCheckBox* hailCoreCheck_ = nullptr;
    QLabel* hailCoreBadge_ = nullptr;
    QCheckBox* zdrColumnCheck_ = nullptr;
    QLabel* zdrColumnBadge_ = nullptr;
    QCheckBox* tbssCheck_ = nullptr;
    QLabel* tbssBadge_ = nullptr;
    QCheckBox* tdsCheck_ = nullptr;
    QLabel* tdsBadge_ = nullptr;
    QCheckBox* spcOutlookCheck_ = nullptr;
    QLabel* spcOutlookBadge_ = nullptr;
    QCheckBox* spcFireCheck_ = nullptr;
    QLabel* spcFireBadge_ = nullptr;
    QCheckBox* nhcCheck_ = nullptr;
    QLabel* nhcBadge_ = nullptr;
    QCheckBox* wpcCheck_ = nullptr;
    QLabel* wpcBadge_ = nullptr;
    QCheckBox* hmsCheck_ = nullptr;
    QLabel* hmsBadge_ = nullptr;
    QCheckBox* datCheck_ = nullptr;
    QLabel* datBadge_ = nullptr;
    QCheckBox* hrrrCheck_ = nullptr;
    QLabel* hrrrBadge_ = nullptr;
    QComboBox* hrrrLeadCombo_ = nullptr;
    QCheckBox* goesCheck_ = nullptr;
    QLabel* goesBadge_ = nullptr;
    QComboBox* goesChannelCombo_ = nullptr;
    QCheckBox* mosaicCheck_ = nullptr;
    QLabel* mosaicBadge_ = nullptr;
    QCheckBox* mcdCheck_ = nullptr;
    QLabel* mcdBadge_ = nullptr;
    QCheckBox* metarCheck_ = nullptr;
    QLabel* metarBadge_ = nullptr;
    QCheckBox* trailCheck_ = nullptr;
    QCheckBox* smoothingCheck_ = nullptr;
    QLabel* scanDiagnosticsLabel_ = nullptr;
    // Rainfall (Z-R estimate) block: three wrapped lines in the Display tab
    // plus the pop-out that carries per-flag provenance. The grid is kept so
    // the pop-out can name the peak gate and a future paint can reuse it.
    QLabel* rainfallEstimateLabel_ = nullptr;
    QLabel* rainfallCoverageLabel_ = nullptr;
    QLabel* rainfallQualityLabel_ = nullptr;
    QToolButton* rainfallDetailsButton_ = nullptr;
    QPointer<QDialog> rainfallDialog_;
    QLabel* rainfallDialogSummary_ = nullptr;
    QTableWidget* rainfallFlagTable_ = nullptr;
    QLabel* rainfallDialogNotes_ = nullptr;
    RainfallHistoryResult rainfallLatest_;
    QString rainfallSignature_;   // radar|volumes|newest volume the readout shows
    QString rainfallJobRadarId_;  // radar the in-flight job was started for
    bool rainfallInFlight_ = false;
    bool rainfallDirty_ = false;
    QCheckBox* placefilesCheck_ = nullptr;
    QLabel* placefilesBadge_ = nullptr;
    QComboBox* scopeCombo_ = nullptr;
    QWidget* perPanelBox_ = nullptr;
    QComboBox* severityCombo_ = nullptr;
    QSpinBox* ageSpin_ = nullptr;
    QCheckBox* soundCheck_ = nullptr;
    QCheckBox* trayCheck_ = nullptr;
    QToolButton* addPlacefileButton_ = nullptr;
    QToolButton* addPlacefileUrlButton_ = nullptr;
    QToolButton* clearPlacefilesButton_ = nullptr;
    QToolButton* openArchiveFilesButton_ = nullptr;
    QToolButton* openArchiveFolderButton_ = nullptr;
    QToolButton* awsArchiveButton_ = nullptr;
    // Per-panel overlay overrides (active panel only).
    QLabel* activePanelOverlayLabel_ = nullptr;
    QCheckBox* activePanelWarnings_ = nullptr;
    QCheckBox* activePanelReports_ = nullptr;
    QCheckBox* activePanelLightning_ = nullptr;
    QCheckBox* activePanelFusion_ = nullptr;
    QToolButton* useGlobalButton_ = nullptr;
    // Radar health indicator: a small colored dot next to the station
    // summary label that reflects Online / Stale / Offline for the active
    // radar session.
    QLabel* healthDot_ = nullptr;
    QLabel* healthLabel_ = nullptr;
    QTimer* healthTimer_ = nullptr;
    RadarHealthState radarHealthState_ = RadarHealthState::Unknown;
    QDateTime lastVolumeTime_;  // data time of the newest live volume seen
    QDateTime lastArrivalTime_; // wall-clock arrival of that volume
    NexradSource* source_ = nullptr;
    OperaSource* operaSource_ = nullptr;
    GlmSource* glmSource_ = nullptr;
    WeatherOverlaySource* overlaySource_ = nullptr;
    SpcConvectiveSource* spcSource_ = nullptr;
    NhcCycloneSource* nhcSource_ = nullptr;
    WpcEroSource* wpcSource_ = nullptr;
    HmsSmokeSource* hmsSource_ = nullptr;
    DatPathSource* datSource_ = nullptr;
    HrrrSource* hrrrSource_ = nullptr;
    GoesSource* goesSource_ = nullptr;
    MosaicSource* mosaicSource_ = nullptr;
    UpdateChecker* updateChecker_ = nullptr;
    bool updateCheckPending_ = false;
    MetarSource* metarSource_ = nullptr;
    PlacefileSource* placefileSource_ = nullptr;
    ArchiveSource* archiveSource_ = nullptr;
    AlertCenter* alertCenter_ = nullptr;
    QTimer* refreshTimer_ = nullptr;
    QTimer* overlayRefreshTimer_ = nullptr;
    QTimer* animationTimer_ = nullptr;
    QString selectedRadarId_;
    QString radarStatus_ = QStringLiteral("Pick a station → latest scan → timeline → overlays");
    QString overlayStatus_ = QStringLiteral("Waiting for NWS/SPC overlays");
    QString spcStatus_;
    QString lightningStatus_ = QStringLiteral("Waiting for GOES GLM");
    QStringList recentRadarIds_;
    QStringList remotePlacefileUrls_;
    QSet<QString> favoriteRadarIds_;
    QHash<QString, std::shared_ptr<RadarSession>> radarSessions_;
    QStringList panelRadarIds_;
    bool chaseMode_ = false;
    double chaseLatitude_ = 35.0;
    double chaseLongitude_ = -97.0;
    int maximumHistoryFrames_ = 12;
    int historyVolumeCount_ = 8;
    // Spill-prefetch jobs in flight, keyed radarId/volumeId so fast scrubbing
    // never stacks duplicate disk reads for the same volume.
    QSet<QString> spillPrefetchInFlight_;
    bool synchronizePanels_ = true;
    bool synchronizingMapView_ = false;
    AppTheme theme_ = AppTheme::Prototype;
    double fontScale_ = 1.0;
    double uiBasePointSize_ = 9.0;
    // Modeless cross-section: stays usable beside live radar. Updated on
    // showFrame() via refreshCrossSection().
    QPointer<QDialog> crossSectionDialog_;
    class CrossSectionWidget* crossSectionWidget_ = nullptr;
    // Modeless scan diagnostics (per-tilt table plus VAD wind profile).
    // Same live-update contract: showFrame() repopulates while open.
    QPointer<QDialog> scanDialog_;
    QTableWidget* scanTable_ = nullptr;
    QLabel* scanSummary_ = nullptr;
    // Modeless sounding pop-out (RAOB hodograph plus SRH ground truth).
    // Rebuilt on each open like Settings: soundings are 12-hourly, so no
    // live-update contract is needed.
    QPointer<QDialog> soundingDialog_;
    // Last volume's storm motion + SRH (populated with the scan dialog):
    // the sounding pop-out reuses the same motion for ground-truth SRH.
    BunkersMotion lastBunkersMotion_;
    SrhEstimate lastSrh_;
    QPointF crossSectionStart_;
    QPointF crossSectionEnd_;
    QString crossSectionRadarId_;
    // Temporary 1-panel focus without destroying the layout preset.
    bool panelMaximized_ = false;
    bool settingsRestoreComplete_ = false;
    int pendingDockWidth_ = -1;
    int maximizedPanelIndex_ = 0;
    int layoutBeforeMaximize_ = 0;
    int consecutiveFailures_ = 0;
};

} // namespace cappedweather
