#include "app/update_check.hpp"

#include "app/http_client.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>

namespace cappedweather {
namespace {

constexpr char kReleasesLatest[] =
    "https://api.github.com/repos/Mikifrfr/CappedWeather/releases/latest";
constexpr char kProjectReleases[] = "https://github.com/Mikifrfr/CappedWeather/releases";

// Verified-or-empty: only the project's own releases page opens.
QUrl verifiedPageUrl(const QString& text) {
    const QUrl url(text.trimmed());
    if (!url.isValid() || url.scheme() != QStringLiteral("https") ||
        url.host().compare(QStringLiteral("github.com"), Qt::CaseInsensitive) != 0 ||
        !url.path().startsWith(QStringLiteral("/Mikifrfr/CappedWeather/releases"))) {
        return {};
    }
    return url;
}

}  // namespace

QString currentAppVersion() {
#ifdef CAPPEDWEATHER_VERSION
    return QStringLiteral(CAPPEDWEATHER_VERSION);
#else
    return QStringLiteral("unknown");
#endif
}

QUrl projectReleasesUrl() {
    return QUrl(QString::fromLatin1(kProjectReleases));
}

UpdateVersion parseAppVersion(const QString& text) {
    UpdateVersion version;
    QString stripped = text.trimmed();
    if (stripped.startsWith(QLatin1Char('v')) || stripped.startsWith(QLatin1Char('V'))) {
        stripped = stripped.mid(1).trimmed();
    }
    if (stripped.isEmpty()) return version;
    const QStringList parts = stripped.split(QLatin1Char('.'));
    if (parts.size() > 3) return version;
    int numbers[3] = {0, 0, 0};
    for (int i = 0; i < parts.size(); ++i) {
        bool ok = false;
        // No sign, no whitespace, no suffix: pure digits only.
        const QString& part = parts.at(i);
        if (part.isEmpty()) return version;
        for (const QChar ch : part) {
            if (!ch.isDigit()) return version;
        }
        numbers[i] = part.toInt(&ok);
        if (!ok || numbers[i] < 0 || numbers[i] > 9999) return version;
    }
    version.major = numbers[0];
    version.minor = numbers[1];
    version.patch = numbers[2];
    version.valid = true;
    return version;
}

int compareVersions(const UpdateVersion& a, const UpdateVersion& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

std::optional<UpdateRelease> parseUpdateRelease(const QByteArray& json, QString* error) {
    const auto fail = [&](const QString& message) {
        if (error) *error = message;
        return std::optional<UpdateRelease>{};
    };
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return fail(QStringLiteral("release metadata is not JSON"));
    }
    const auto object = document.object();
    const QString tag = object.value(QStringLiteral("tag_name")).toString().trimmed();
    if (tag.isEmpty()) return fail(QStringLiteral("release metadata has no tag"));
    const auto version = parseAppVersion(tag);
    if (!version.valid) return fail(QStringLiteral("release tag is not a version"));
    UpdateRelease release;
    release.tag = tag;
    release.version = version;
    release.prerelease = object.value(QStringLiteral("prerelease")).toBool(false);
    release.pageUrl =
        verifiedPageUrl(object.value(QStringLiteral("html_url")).toString());
    release.publishedAt = object.value(QStringLiteral("published_at")).toString();
    return release;
}

UpdateChecker::UpdateChecker(QObject* parent) : QObject(parent) {
}

void UpdateChecker::check() {
    if (pending_) return;
    pending_ = true;
    const QUrl url(QString::fromLatin1(kReleasesLatest));
    auto* reply = network_.get(makeRequest(url, QStringLiteral("application/vnd.github+json"),
                                           QStringLiteral(" update check client"),
                                           CacheMode::AlwaysNetwork));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        pending_ = false;
        UpdateResult result;
        result.currentText = currentAppVersion();
        const bool transportOk = reply->error() == QNetworkReply::NoError;
        const int code =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool httpOk = transportOk && (code == 0 || (code >= 200 && code < 300));
        if (!httpOk) {
            if (transportOk && code == 404) {
                // Missing release metadata is never proof of currency: the
                // releases page may simply require access right now.
                result.state = UpdateState::NoReleaseInfo;
                result.detail = QStringLiteral(
                    "No public release metadata was found. The project releases page may "
                    "require access; check it manually for newer builds.");
                result.pageUrl = projectReleasesUrl();
            } else {
                result.state = UpdateState::NetworkError;
                result.detail = transportOk ? QStringLiteral("Release query returned HTTP %1.")
                                                  .arg(code)
                                            : reply->errorString();
                result.pageUrl = projectReleasesUrl();
            }
            emit finished(result);
            return;
        }
        QString parseError;
        const auto release = parseUpdateRelease(reply->readAll(), &parseError);
        if (!release.has_value()) {
            result.state = UpdateState::Malformed;
            result.detail = parseError;
            result.pageUrl = projectReleasesUrl();
            emit finished(result);
            return;
        }
        result.latestText = release->tag;
        result.pageUrl = release->pageUrl;
        const auto current = parseAppVersion(result.currentText);
        if (!current.valid) {
            result.state = UpdateState::UnknownCurrent;
            result.detail = QStringLiteral(
                "This build does not report a release version. The latest public "
                "release is %1.")
                                .arg(release->tag);
            emit finished(result);
            return;
        }
        const int order = compareVersions(current, release->version);
        if (order < 0) {
            result.state = UpdateState::Newer;
            result.detail = QStringLiteral("CappedWeather %1 is available (this build: %2).")
                                .arg(release->tag, result.currentText);
        } else {
            result.state = UpdateState::Current;
            result.detail = order == 0
                                ? QStringLiteral("This build (%1) matches the latest public release.")
                                      .arg(result.currentText)
                                : QStringLiteral("This build (%1) is ahead of the latest public "
                                                 "release (%2).")
                                      .arg(result.currentText, release->tag);
        }
        emit finished(result);
    });
}

}  // namespace cappedweather
