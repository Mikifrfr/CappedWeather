#include "app/http_client.hpp"

#include <QNetworkDiskCache>
#include <QSettings>
#include <QStandardPaths>

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

QNetworkRequest makeRequest(const QUrl& url, const QString& accept, const QString& uaSuffix,
                             const CacheMode cache) {
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent(uaSuffix));
    request.setTransferTimeout(networkTimeoutMs());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    // S3 chunk fetches multiplex over HTTP/2 when the endpoint negotiates
    // it (Qt falls back to 1.1 transparently); one RTT saved per chunk on
    // connection setup, plus header compression on listing-heavy flows.
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);
    if (cache == CacheMode::AlwaysNetwork)
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                             QNetworkRequest::AlwaysNetwork);
    else if (cache == CacheMode::PreferCache)
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                             QNetworkRequest::PreferCache);
    if (!accept.isEmpty()) request.setRawHeader("Accept", accept.toUtf8());
    return request;
}

void attachChunkDiskCache(QNetworkAccessManager& manager, const QString& subdirectory) {
    auto* diskCache = new QNetworkDiskCache(&manager);
    diskCache->setCacheDirectory(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
                                 QStringLiteral("/radar-chunks/") + subdirectory);
    diskCache->setMaximumCacheSize(512LL * 1024 * 1024);
    manager.setCache(diskCache);
}

} // namespace cappedweather
