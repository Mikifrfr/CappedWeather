#pragma once

// SPC convective context: Day 1/2 categorical outlook polygons plus active
// mesoscale discussions. Outlooks arrive as SPC GeoJSON (LABEL2 + official
// fill/stroke colors, VALID/EXPIRE/ISSUE timestamps); MCDs are discovered
// through the SPC discussions RSS feed and parsed from the discussion HTML
// (areas/concerning/valid/probability/summary + LAT...LON ring, longitudes
// positive-west per SPC convention). Watches need no new code: NWS alerts
// already carry Tornado/Severe-Thunderstorm Watch polygons through the
// warnings path with their own styles.
//
// Pure data + parsing here (testable without a window); fetching lives in
// SpcConvectiveSource below. Anything malformed fails closed to empty.

#include <QDateTime>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointF>
#include <QString>
#include <QVector>

#include <utility>
#include <vector>

namespace cappedweather {

struct SpcOutlookRing {
    // Exterior ring only (holes are vanishingly rare in outlooks and add
    // nothing at map scale); points are lon/lat degrees.
    QVector<QPointF> exterior;
};

struct SpcOutlookFeature {
    int day = 1;  // 1 or 2 (categorical or fire)
    QString label;  // "MDT" / "ELEV"
    QString label2;  // "Moderate Risk" / "Elevated Fire Risk"
    int dn = 0;  // SPC severity code (2 TSTM .. 8 HIGH; fire 5/8/10)
    QDateTime valid;
    QDateTime expire;
    QDateTime issue;
    QString forecaster;
    QString stroke;  // "#..." official outline, may be empty
    QString fill;  // "#..." official fill, may be empty
    QString fireKind;  // empty = categorical, else "DryT" or "WindRH"
    std::vector<SpcOutlookRing> rings;
};

struct SpcMcdCandidate {
    int number = 0;
    QString link;  // discussion page URL (relative or absolute)
    QString title;
};

struct SpcMcd {
    int number = 0;
    QString areas;  // "west-central Florida Peninsula"
    QString concerning;  // raw "Severe potential...Watch unlikely" line
    QDateTime validFrom;
    QDateTime validTo;
    int watchProbabilityPercent = -1;  // -1 when unstated
    QString summary;
    QString discussion;
    QVector<QPointF> exterior;  // lon/lat degrees, closed implicitly
};

// SPC outlook GeoJSON (day*N*otlk_cat.nolyr.geojson): nullopt entries are
// dropped, never fatal. `day` tags the source file (1 or 2).
QVector<SpcOutlookFeature> parseSpcOutlookGeoJson(const QByteArray& data, int day,
                                                  QString* error);
// MCD RSS (spcmdrss.xml): active discussion links, or empty when the feed
// says no MDs are in effect (normal midday state, not an error).
QVector<SpcMcdCandidate> parseMcdRss(const QByteArray& data, QString* error);
// One discussion page (mdNNNN.html): full field parse, polygon included.
SpcMcd parseMcdPage(const QByteArray& data, QString* error);

class SpcConvectiveSource : public QObject {
    Q_OBJECT
public:
    explicit SpcConvectiveSource(QObject* parent = nullptr);

    void refresh();
    void setEnabled(bool enabled);

signals:
    void outlooksReady(QVector<SpcOutlookFeature> outlooks);
    void fireOutlooksReady(QVector<SpcOutlookFeature> outlooks);
    void mcdsReady(QVector<SpcMcd> mcds);
    void statusChanged(const QString& status);

private:
    void fetchOutlooks();
    void fetchFireOutlooks();
    void fetchMcds();
    void requestMcdPages(QVector<SpcMcdCandidate> candidates);

    bool enabled_ = true;
    bool outlooksPending_ = false;
    bool firePending_ = false;
    bool mcdsPending_ = false;
    QVector<SpcOutlookFeature> outlooks_;
    QVector<SpcOutlookFeature> fireOutlooks_;
    QVector<SpcMcd> mcds_;
    QString outlooksError_;
    QString fireError_;
    QNetworkAccessManager network_;
};

}  // namespace cappedweather
