#include "app/sounding_source.hpp"

#include "app/http_client.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimeZone>

namespace cappedweather {

QDateTime latestSoundingTime(const QDateTime& nowUtc) {
    if (!nowUtc.isValid()) return {};
    const QDate date = nowUtc.date();
    const QDateTime noon(date, QTime(12, 0), QTimeZone::UTC);
    const QDateTime midnight(date, QTime(0, 0), QTimeZone::UTC);
    // Analyses publish ~2 h late: after 14Z the 12Z run exists, after 02Z
    // the 00Z run exists, otherwise fall back to yesterday's 12Z.
    if (nowUtc >= noon.addSecs(2 * 3600)) return noon;
    if (nowUtc >= midnight.addSecs(2 * 3600)) return midnight;
    return QDateTime(date.addDays(-1), QTime(12, 0), QTimeZone::UTC);
}

QUrl soundingUrl(const QString& station, const QDateTime& soundingTimeUtc) {
    const auto clean = station.trimmed().toUpper();
    if (clean.isEmpty() || !soundingTimeUtc.isValid()) return {};
    return QUrl(QStringLiteral("https://mesonet.agron.iastate.edu/json/raob.py?station=%1&ts=%2")
                    .arg(clean, soundingTimeUtc.toString(QStringLiteral("yyyyMMddhhmm"))));
}

Sounding parseIemRaob(const QByteArray& data, QString* error) {
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error)
            *error = QStringLiteral("Invalid sounding JSON: %1").arg(parseError.errorString());
        return {};
    }
    const auto profiles = document.object().value(QStringLiteral("profiles")).toArray();
    if (profiles.isEmpty()) {
        if (error) *error = QStringLiteral("Sounding response holds no profiles");
        return {};
    }
    const auto head = profiles.at(0).toObject();
    const auto rows = head.value(QStringLiteral("profile")).toArray();
    if (rows.isEmpty()) {
        if (error) *error = QStringLiteral("Sounding profile is empty");
        return {};
    }
    Sounding sounding;
    sounding.stationId = head.value(QStringLiteral("station")).toString().toStdString();
    const auto valid = QDateTime::fromString(head.value(QStringLiteral("valid")).toString(),
                                             Qt::ISODate);
    if (valid.isValid()) sounding.validEpochSeconds = valid.toSecsSinceEpoch();
    for (const auto& row : rows) {
        const auto point = row.toObject();
        const auto number = [&](const char* key) {
            const auto value = point.value(QString::fromLatin1(key));
            return value.isNull() ? std::numeric_limits<double>::quiet_NaN() : value.toDouble();
        };
        SoundingLevel level;
        level.pressureHpa = number("pres");
        level.heightMslMeters = number("hght");
        level.tempCelsius = number("tmpc");
        level.dewpointCelsius = number("dwpc");
        level.directionFromDegrees = number("drct");
        level.speedKnots = number("sknt");
        sounding.levels.push_back(level);
    }
    if (error) error->clear();
    return sounding;
}

}  // namespace cappedweather
