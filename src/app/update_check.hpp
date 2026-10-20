#pragma once

// Update discovery for every desktop platform (the bounded half of the
// Flatpak-plus-updates item): an async check against this project's public
// stable-release metadata, version-compared, with the verified project
// release page one click away when a newer build exists. Discovery only: no
// downloading installers, no installs, no new dependency.
//
// Platform assets are deliberately ignored: only tag_name / prerelease /
// html_url / published_at are read from /releases/latest. Adding a macOS
// (or Flatpak) asset therefore needs no platform token, no asset selector
// and no change here — the newer-version notice links to the release page
// and the user picks their own artifact.
//
// States stay explicit: loading, current, newer, unknown-current
// (development builds), no-release-info (a missing release is never proof
// the app is current), network-error, malformed. The releases page URL is
// host-validated before anything opens it. Pure parse/compare hooks below
// are hermetic; fetching lives in UpdateChecker.

#include <QDateTime>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QUrl>

#include <optional>

namespace cappedweather {

// This build's version from the CMake project version ("0.7.0"); "unknown"
// when the macro is absent (development builds).
QString currentAppVersion();

struct UpdateVersion {
    int major = 0;
    int minor = 0;
    int patch = 0;
    bool valid = false;
};

// "v0.7.0" / "0.7.0" / "0.7" (missing parts are zero). Anything else is
// invalid, never coerced.
UpdateVersion parseAppVersion(const QString& text);
// -1/0/+1 on two valid versions; never called with invalid ones.
int compareVersions(const UpdateVersion& a, const UpdateVersion& b);

struct UpdateRelease {
    QString tag;
    UpdateVersion version;
    bool prerelease = false;
    QUrl pageUrl;  // empty unless verified github.com project URL
    QString publishedAt;
};

// GitHub /releases/latest payload. Nullopt with an error on malformed
// JSON, missing tag_name, or an unusable page URL (the URL check only
// drops the link; the version still parses).
std::optional<UpdateRelease> parseUpdateRelease(const QByteArray& json, QString* error = nullptr);

// Verified project release page (hardcoded constant for the
// no-release-info and error paths so the user can always look manually).
QUrl projectReleasesUrl();

// Weekly auto-check scheduling: due when never checked or the last check
// predates now by seven days. Pure, clock-injected for hermetic tests.
bool updateCheckDue(long long lastCheckEpochSeconds, long long nowEpochSeconds);

enum class UpdateState {
    Current,
    Newer,
    UnknownCurrent,
    NoReleaseInfo,
    NetworkError,
    Malformed,
};

struct UpdateResult {
    UpdateState state = UpdateState::NetworkError;
    QString currentText;
    QString latestText;
    QUrl pageUrl;  // verified or empty
    QString detail;
};

class UpdateChecker : public QObject {
    Q_OBJECT
public:
    explicit UpdateChecker(QObject* parent = nullptr);

    // One async check; concurrent calls while pending are ignored.
    void check();

signals:
    void finished(const UpdateResult& result);

private:
    bool pending_ = false;
    QNetworkAccessManager network_;
};

}  // namespace cappedweather
