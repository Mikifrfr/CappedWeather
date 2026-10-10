#include "app/placefile_source.hpp"
#include "app/http_client.hpp"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>

namespace cappedweather {
namespace {
constexpr qint64 kMaximumResponseBytes = 2 * 1024 * 1024;

QString displaySource(const QUrl& url) {
    if (url.isLocalFile()) return QDir::toNativeSeparators(url.toLocalFile());
    auto scrubbed = url;
    scrubbed.setUserInfo(QString{});
    QUrlQuery query(scrubbed);
    const auto items = query.queryItems(QUrl::FullyDecoded);
    query.clear();
    for (const auto& [key, value] : items) {
        const auto lowered = key.toLower();
        const auto sensitive = lowered.contains(QStringLiteral("token")) ||
            lowered.contains(QStringLiteral("secret")) ||
            lowered.contains(QStringLiteral("password")) || lowered == QStringLiteral("key") ||
            lowered.contains(QStringLiteral("api_key")) || lowered.contains(QStringLiteral("apikey")) ||
            lowered.contains(QStringLiteral("auth"));
        query.addQueryItem(key, sensitive ? QStringLiteral("[redacted]") : value);
    }
    scrubbed.setQuery(query);
    return scrubbed.toDisplayString(QUrl::RemovePassword);
}

} // namespace

PlacefileSource::PlacefileSource(QObject* parent)
    : QObject(parent) {
    connect(&network_, &QNetworkAccessManager::finished, this, &PlacefileSource::finish);
}

bool PlacefileSource::addLocalFile(const QString& path, QString* error) {
    if (error) error->clear();
    if (QFileInfo(path).size() > kMaximumResponseBytes) {
        if (error) *error = QStringLiteral("placefile exceeds the 2 MiB safety limit");
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    if (file.size() > kMaximumResponseBytes) {
        if (error) *error = QStringLiteral("placefile exceeds the 2 MB safety limit");
        return false;
    }
    QString parseError;
    const QByteArray payload = file.readAll();
    std::optional<PlacefileDocument> document;
    bool dbfCompanionBad = false;
    if (looksLikeShapefile(payload)) {
        // Sibling .dbf names the shapes (record-aligned hover text); a
        // missing or malformed companion only costs the names, never the
        // geometry. Remote shapefiles skip this (one fetch, geometry only).
        QStringList recordNames;
        if (path.endsWith(QStringLiteral(".shp"), Qt::CaseInsensitive)) {
            QFile dbfFile(path.left(path.size() - 4) + QStringLiteral(".dbf"));
            if (dbfFile.exists()) {
                if (dbfFile.size() <= kMaximumResponseBytes && dbfFile.open(QIODevice::ReadOnly)) {
                    recordNames = parseDbfNames(dbfFile.readAll());
                    dbfCompanionBad = recordNames.isEmpty();
                } else {
                    dbfCompanionBad = true;
                }
            }
        }
        document = parseShapefileData(payload, recordNames, &parseError);
        if (document && dbfCompanionBad) {
            document->warnings.push_back(QStringLiteral("dbf companion unreadable; shapes unnamed"));
        }
    } else {
        document = parsePlacefile(payload, &parseError);
    }
    if (!document) {
        if (error) *error = parseError;
        return false;
    }
    const auto absolutePath = QFileInfo(path).absoluteFilePath();
    document->source = QDir::toNativeSeparators(absolutePath);
    if (document->title.isEmpty()) document->title = QFileInfo(path).completeBaseName();
    auto entry = std::make_unique<Entry>();
    entry->key = QUrl::fromLocalFile(absolutePath).toString();
    entry->url = QUrl::fromLocalFile(absolutePath);
    entry->document = std::move(document);
    entries_[entry->key] = std::move(entry);
    publish();
    return true;
}

bool PlacefileSource::addUrl(const QUrl& sourceUrl, QString* error) {
    if (error) error->clear();
    const auto url = sourceUrl.adjusted(QUrl::NormalizePathSegments | QUrl::RemoveFragment);
    if (!url.isValid() || (url.scheme() != QStringLiteral("http") &&
                           url.scheme() != QStringLiteral("https")) || url.host().isEmpty()) {
        if (error) *error = QStringLiteral("placefile URL must use http or https");
        return false;
    }
    const auto key = url.toString(QUrl::RemovePassword);
    auto found = entries_.find(key);
    if (found == entries_.end()) {
        auto entry = std::make_unique<Entry>();
        entry->key = key;
        entry->url = url;
        entry->remote = true;
        entry->refreshTimer = new QTimer(this);
        entry->refreshTimer->setSingleShot(true);
        const auto entryKey = entry->key;
        connect(entry->refreshTimer, &QTimer::timeout, this, [this, entryKey] {
            const auto current = entries_.find(entryKey);
            if (current != entries_.end()) request(*current->second);
        });
        found = entries_.emplace(key, std::move(entry)).first;
    }
    request(*found->second);
    return true;
}

void PlacefileSource::setContext(const double latitude, const double longitude) {
    bool changed = false;
    if (std::isfinite(latitude) && latitude >= -90.0 && latitude <= 90.0 &&
        std::abs(contextLatitude_ - latitude) > 0.0001) {
        contextLatitude_ = latitude;
        changed = true;
    }
    if (std::isfinite(longitude) && longitude >= -180.0 && longitude <= 180.0 &&
        std::abs(contextLongitude_ - longitude) > 0.0001) {
        contextLongitude_ = longitude;
        changed = true;
    }
    if (!changed) return;
    for (auto& [key, entry] : entries_) {
        Q_UNUSED(key);
        if (entry->remote && !entry->reply && entry->refreshTimer) entry->refreshTimer->start(0);
    }
}

void PlacefileSource::clear() {
    // No leak: abort() still emits finished, which finish() always ends with
    // deleteLater (missing entries hit its early-out branch, like archive_source).
    for (auto& [key, entry] : entries_) {
        Q_UNUSED(key);
        if (entry->reply) entry->reply->abort();
        if (entry->refreshTimer) {
            entry->refreshTimer->stop();
            entry->refreshTimer->deleteLater();
        }
    }
    replies_.clear();
    entries_.clear();
    publish();
    emit statusChanged(QStringLiteral("Placefiles cleared"));
}

QVector<PlacefileDocument> PlacefileSource::documents() const {
    QVector<PlacefileDocument> result;
    result.reserve(static_cast<qsizetype>(entries_.size()));
    for (const auto& [key, entry] : entries_) {
        Q_UNUSED(key);
        if (entry->document) result.push_back(*entry->document);
    }
    return result;
}

int PlacefileSource::sourceCount() const {
    return static_cast<int>(entries_.size());
}

QStringList PlacefileSource::sourceUrls() const {
    QStringList urls;
    urls.reserve(static_cast<qsizetype>(entries_.size()));
    for (const auto& [key, entry] : entries_) {
        Q_UNUSED(key);
        urls.push_back(entry->url.toString());
    }
    return urls;
}

void PlacefileSource::request(Entry& entry) {
    if (entry.reply) return;
    auto url = entry.url;
    QUrlQuery query(url);
    if (!query.hasQueryItem(QStringLiteral("lat")))
        query.addQueryItem(QStringLiteral("lat"), QString::number(contextLatitude_, 'f', 4));
    if (!query.hasQueryItem(QStringLiteral("lon")))
        query.addQueryItem(QStringLiteral("lon"), QString::number(contextLongitude_, 'f', 4));
    if (!query.hasQueryItem(QStringLiteral("version")))
        query.addQueryItem(QStringLiteral("version"), QStringLiteral("1.2"));
    url.setQuery(query);
    auto request = makeRequest(url, {}, QStringLiteral(" GR placefile client"));
    auto* reply = network_.get(request);
    reply->setReadBufferSize(kMaximumResponseBytes + 1);
    connect(reply, &QNetworkReply::readyRead, this, [reply] {
        if (reply->bytesAvailable() <= kMaximumResponseBytes) return;
        reply->setProperty("cappedweatherTooLarge", true);
        reply->abort();
    });
    entry.reply = reply;
    replies_.insert(reply, entry.key);
    emit statusChanged(QStringLiteral("Fetching placefile %1").arg(displaySource(entry.url)));
}

void PlacefileSource::finish(QNetworkReply* reply) {
    const auto key = replies_.take(reply);
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        reply->deleteLater();
        return;
    }
    auto& entry = *found->second;
    entry.reply.clear();
    const auto response = reply->readAll();
    const auto networkError = reply->error();
    const auto networkMessage = reply->errorString();
    const auto tooLarge = reply->property("cappedweatherTooLarge").toBool();
    reply->deleteLater();

    QString failure;
    std::optional<PlacefileDocument> document;
    if (tooLarge) {
        failure = QStringLiteral("response exceeded the 2 MiB safety limit");
    } else if (networkError != QNetworkReply::NoError) {
        failure = networkMessage;
    } else if (response.size() > kMaximumResponseBytes) {
        failure = QStringLiteral("response exceeds the 2 MB safety limit");
    } else {
        document = looksLikeShapefile(response) ? parseShapefile(response, &failure)
                                                : parsePlacefile(response, &failure);
    }
    if (document) {
        document->source = displaySource(entry.url);
        if (document->title.isEmpty()) document->title = entry.url.host();
        entry.document = std::move(document);
        publish();
        emit statusChanged(QStringLiteral("Loaded %1 placefile element%2 from %3")
                               .arg(entry.document->elements.size())
                               .arg(entry.document->elements.size() == 1 ? QString{} : QStringLiteral("s"))
                               .arg(displaySource(entry.url)));
    } else {
        emit statusChanged(QStringLiteral("Placefile failed: %1").arg(failure));
    }

    if (entry.refreshTimer) {
        const auto requested = entry.document ? entry.document->refreshSeconds : 60;
        entry.refreshTimer->start(std::clamp(requested > 0 ? requested : 60, 15, 3600) * 1000);
    }
}

void PlacefileSource::publish() {
    emit documentsChanged(documents());
}

} // namespace cappedweather
