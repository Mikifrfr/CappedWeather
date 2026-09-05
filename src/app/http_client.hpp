#pragma once

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
//   placefiles use " GR placefile client", chase detection uses " (chase-position detection)".
QString userAgent(const QString& suffix = QStringLiteral(" operational radar viewer"));
int networkTimeoutMs();
QNetworkRequest makeRequest(const QUrl& url, const QString& accept = {},
                            const QString& uaSuffix = QStringLiteral(" operational radar viewer"));

} // namespace cappedweather
