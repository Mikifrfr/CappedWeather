#pragma once

// HRRR simulated composite reflectivity as model context: TDS NetCDF
// Subset Service over the Best CONUS 2.5 km time series (verified live
// 2026-10-10, anonymous, no key). One REFC field for the latest analysis
// over a radar-centered box (single time step, ~100-200 KB), refreshed on
// a 30-minute timer plus radar select and toggle-on. Full CONUS GRIB2
// files run 370-780 MB and are never fetched: the subset query is the
// entire feasibility story.
// Valid time rides the frame; the canvas labels it so model output never
// reads as observed echo. Anything malformed fails closed to empty.
//
// Pure data + parsing hooks here (testable without a window); fetching
// lives in HrrrSource below. NetCDF decode itself is core/hrrr_frame.

#include "core/hrrr_frame.hpp"

#include <QDateTime>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVector>

#include <optional>

class QTimer;

namespace cappedweather {

// Latest full UTC hour minus two (HRRR runs hourly, analyses land ~1 h
// late). Pure, clock-injected for hermetic tests.
QDateTime latestHrrrTime(const QDateTime& nowUtc);
// Analysis clock plus forecast lead (0-18 h, same cycle steps through the
// identical subset path). Pure, hermetic.
QDateTime hrrrValidTime(const QDateTime& nowUtc, int leadHours);
// REFC subset URL for a lat/lon box at one valid time. Box clamps to the
// CONUS coverage; empty URL on bad input.
QUrl hrrrSubsetUrl(double centerLatitude, double centerLongitude, double halfDegrees,
                   const QDateTime& validTimeUtc);

class HrrrSource : public QObject {
    Q_OBJECT
public:
    explicit HrrrSource(QObject* parent = nullptr);

    void refresh();
    void setEnabled(bool enabled);
    // Model-context region: follows the selected radar like METAR.
    void setRegion(double latitude, double longitude, double halfDegrees = 3.0);
    // Forecast lead in whole hours (0 = latest analysis). Refetches.
    void setLeadHours(int hours);

signals:
    void frameReady(std::optional<HrrrFrame> frame);
    void statusChanged(const QString& status);

private:
    void requestTime(const QDateTime& validTime, bool fallbackLeft);

    bool enabled_ = true;
    bool pending_ = false;
    int leadHours_ = 0;
    double regionLatitude_ = 38.0;
    double regionLongitude_ = -96.0;
    double regionHalfDegrees_ = 3.0;
    QNetworkAccessManager network_;
    QTimer* refreshTimer_ = nullptr;
};

}  // namespace cappedweather
