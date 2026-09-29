#pragma once

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QString>
#include <QUrl>

namespace cappedweather {

// Shared HTTP defaults: QSettings network/contact + network/timeoutSeconds
// (clamped 5-120s) with a consistent NoLessSafeRedirectPolicy.
// The UA base is "CappedWeather/0.3<suffix>" plus " (<contact>)" when configured.
// Callers pass their distinctive suffix to keep historically equivalent strings:
//   "" not used directly; most clients use " operational radar viewer",
//   GLM uses " GLM live client", archive uses " NOAA public Level-II archive client",
//   placefiles use " GR placefile client", chase detection uses " (chase-position detection)",
//   OPERA uses " OPERA ODIM client".
QString userAgent(const QString& suffix = QStringLiteral(" operational radar viewer"));
int networkTimeoutMs();
// CacheMode::Default leaves Qt's PreferNetwork untouched (tiles, feeds).
// Listings use AlwaysNetwork so a cached volume listing can never hide the
// newest scan; immutable chunk objects use PreferCache with the on-disk
// chunk cache attached by attachChunkDiskCache.
enum class CacheMode { Default, AlwaysNetwork, PreferCache };
QNetworkRequest makeRequest(const QUrl& url, const QString& accept = {},
                            const QString& uaSuffix = QStringLiteral(" operational radar viewer"),
                            CacheMode cache = CacheMode::Default);
// 512 MB on-disk cache for immutable radar chunk objects (S3 keys never
// change meaning, so PreferCache is safe). Listings and catalogs always go
// to network: a cached volume listing would hide the newest scan.
void attachChunkDiskCache(QNetworkAccessManager& manager, const QString& subdirectory);

} // namespace cappedweather
