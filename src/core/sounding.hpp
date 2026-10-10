#pragma once

// Observed soundings (IEM raob.py JSON, verified live 2026-10-10): level
// data as plain structs plus the two derivations the app needs:
//   - wind limbs as SrhWindPoint vectors (heights relative to the
//     sounding's lowest level, stated approximation: RAOB heights are MSL
//     and station elevation is not in the payload),
//   - freezing-level height by linear interpolation of the first upward
//     0 C crossing (NaN when the column never crosses).
// Winds arrive as meteorological FROM degrees + knots; nulls stay NaN and
// never steer. Pure core types, no Qt, deterministic.

#include "core/srh.hpp"

#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

namespace cappedweather {

struct SoundingLevel {
    double pressureHpa = std::numeric_limits<double>::quiet_NaN();
    double heightMslMeters = std::numeric_limits<double>::quiet_NaN();
    double tempCelsius = std::numeric_limits<double>::quiet_NaN();
    double dewpointCelsius = std::numeric_limits<double>::quiet_NaN();
    double directionFromDegrees = std::numeric_limits<double>::quiet_NaN();
    double speedKnots = std::numeric_limits<double>::quiet_NaN();
};

struct Sounding {
    std::string stationId;
    long long validEpochSeconds = 0;
    std::vector<SoundingLevel> levels;
};

// Wind limbs for the shared SRH integrator (knots -> m/s). Levels without
// a usable wind triple are skipped, never zero-filled.
std::vector<SrhWindPoint> soundingWindPoints(const Sounding& sounding);

// First upward freezing-level crossing in MSL meters, NaN when absent.
double soundingFreezingLevelMsl(const Sounding& sounding);

}  // namespace cappedweather
