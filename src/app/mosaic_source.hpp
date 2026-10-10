#pragma once

// IEM NEXRAD N0Q national mosaic as observed context: 5-minute CONUS
// composites from the anonymous IEM archive (verified live 2026-10-10),
// discovered by day-directory listing (filenames carry valid times; the
// newest n0q_YYYYmmddHHMM.png wins, yesterday as fallback). Native IEM
// colors paint under live echo at reduced alpha with the scan time on
// display; near-black pixels (NODATA plus sub-zero echo) stay out so the
// map never washes out. Anything malformed fails closed to empty.
//
// Pure data + parsing hooks here (testable without a window); fetching
// lives in MosaicSource below. Grid math itself is core/mosaic_frame.

#include "core/mosaic_frame.hpp"

#include <QDateTime>
#include <QImage>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QUrl>

#include <optional>

class QTimer;

namespace cappedweather {

struct MosaicFrame {
    long long validEpochSeconds = 0;
    QImage image;  // Decimated ARGB32 mosaic.
    MosaicGrid grid;  // Matches image pixels (spacing already scaled).
};

// Day-directory listing URL for a UTC date on the IEM archive.
QUrl mosaicListUrl(const QDate& dateUtc);
// Newest n0q PNG name in a directory listing, empty when none match.
QString newestMosaicFile(const QByteArray& listingHtml);
// Latest full 5-minute slot minus one (composites publish ~5 min late).
QDateTime latestMosaicSlot(const QDateTime& nowUtc);
// Decodes one PNG plus its world file into a paint-ready frame (scaled to
// the paint budget). Pure apart from Qt image IO: hermetic tests feed the
// committed fixture and garbage through it directly.
std::optional<MosaicFrame> decodeMosaicFrame(const QByteArray& png, const std::string& world,
                                             long long validEpochSeconds, QString* error);

class MosaicSource : public QObject {
    Q_OBJECT
public:
    explicit MosaicSource(QObject* parent = nullptr);

    void refresh();
    void setEnabled(bool enabled);

signals:
    void frameReady(std::optional<MosaicFrame> frame);
    void statusChanged(const QString& status);

private:
    void requestDay(const QDate& dateUtc, bool fallbackLeft);

    bool enabled_ = true;
    bool pending_ = false;
    QNetworkAccessManager network_;
    QTimer* refreshTimer_ = nullptr;
};

}  // namespace cappedweather
