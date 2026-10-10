#pragma once

// IEM raob.py soundings (verified live 2026-10-10): anonymous JSON for one
// RAOB site and timestamp,
//   https://mesonet.agron.iastate.edu/json/raob.py?station=KOUN&ts=202610101200
// -> {profiles: [{station, valid, profile: [{pres, hght, tmpc, dwpc, drct,
//    sknt}]}]}, nulls for missing winds. Timestamps are 00Z/12Z; same-day
// analyses appear a couple of hours after valid time.
// Anything malformed fails closed to an empty sounding.

#include "core/sounding.hpp"

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QUrl>

namespace cappedweather {

// Latest of the two daily soundings old enough to be published (analyses
// land ~2 h after valid time). Pure, clock-injected for hermetic tests.
QDateTime latestSoundingTime(const QDateTime& nowUtc);
// Fetch URL for one station + sounding time (ts as YYYYMMDDHHmm).
QUrl soundingUrl(const QString& station, const QDateTime& soundingTimeUtc);
// One profile payload -> Sounding (first profile wins; empty profile is an
// error, null winds stay NaN via the core contract).
Sounding parseIemRaob(const QByteArray& data, QString* error);

}  // namespace cappedweather
