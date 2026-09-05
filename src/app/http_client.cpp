#include "app/http_client.hpp"

#include <QSettings>

#include <algorithm>

namespace cappedweather {

QString userAgent(const QString& suffix) {
    const auto contact = QSettings().value(QStringLiteral("network/contact")).toString().trimmed();
    const auto base = QStringLiteral("CappedWeather/0.3%1").arg(suffix);
    return contact.isEmpty() ? base : QStringLiteral("%1 (%2)").arg(base, contact);
}

int networkTimeoutMs() {
    return std::clamp(QSettings().value(QStringLiteral("network/timeoutSeconds"), 30).toInt(), 5,
                      120) *
           1000;
}

QNetworkRequest makeRequest(const QUrl& url, const QString& accept, const QString& uaSuffix) {
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent(uaSuffix));
    request.setTransferTimeout(networkTimeoutMs());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    if (!accept.isEmpty()) request.setRawHeader("Accept", accept.toUtf8());
    return request;
}

} // namespace cappedweather
