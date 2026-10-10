#pragma once

// GOES-R ABI CONUS infrared as observed context: per-channel
// L1b Rad files from the anonymous NOAA CLASS buckets (verified live
// 2026-10-10; GOES-19/East is the default because its RadC fixed-grid
// scene covers CONUS chase country, while GOES-18/West looks at the
// Pacific plus the western states), discovered by hour-directory listing (filenames carry scan
// times; the newest C13/C07 key wins, one hour of fallback). v1 renders
// clean-window IR (C13) and shortwave IR (C07) brightness temperature;
// visible channels stay out (C02 runs ~52 MB per 5 minutes, and
// reflectance needs a solar path the IR painter does not).
// Painted with its scan time on display, under live echo like HRRR.
// Anything malformed fails closed to empty.
//
// Pure data + parsing hooks here (testable without a window); fetching
// lives in GoesSource below. NetCDF decode itself is core/goes_frame.

#include "core/goes_frame.hpp"

#include <QDateTime>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QUrl>

#include <optional>

class QTimer;

namespace cappedweather {

// Supported IR channels (CONUS 2 km). Others are refused, never coerced.
bool goesChannelSupported(int channel);
// Hour-directory listing URL for a UTC day/hour on the given bucket.
QUrl goesListUrl(const QString& bucket, const QDateTime& hourUtc);
// Newest key in a listing whose name carries the channel tag ("M6C13"),
// empty when none match.
QString newestGoesKey(const QByteArray& listingXml, int channel);
// Latest full 5-minute slot minus one (scans publish ~5 min late).
QDateTime latestGoesSlot(const QDateTime& nowUtc);

class GoesSource : public QObject {
    Q_OBJECT
public:
    explicit GoesSource(QObject* parent = nullptr);

    void refresh();
    void setEnabled(bool enabled);
    void setChannel(int channel);

signals:
    void frameReady(std::optional<GoesFrame> frame);
    void statusChanged(const QString& status);

private:
    void requestHour(const QDateTime& hourUtc, bool fallbackLeft);
    void requestFile(const QUrl& fileUrl);

    bool enabled_ = true;
    bool pending_ = false;
    int channel_ = 13;
    // GOES-19 flies East (subpoint 75 W); its RadC scene sits over CONUS.
    QString bucket_ = QStringLiteral("noaa-goes19");
    QNetworkAccessManager network_;
    QTimer* refreshTimer_ = nullptr;
};

}  // namespace cappedweather
