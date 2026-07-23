#pragma once

#include <cstdint>

namespace cappedweather {

// One GLM flash. Flashes arrive pre-clustered by the upstream LCFA algorithm;
// we trust flash_id and never re-cluster events into flashes ourselves.
struct LightningFlash {
    std::int64_t id = 0;
    double timeEpochSec = 0.0;   // UTC unix seconds
    double latitudeDeg = 0.0;
    double longitudeDeg = 0.0;
    double energyJ = 0.0;
    double areaKm2 = 0.0;
};

enum class JumpEventKind {
    JumpStart,      // DFRDT crossed >= jumpSigmaThreshold * sigma
    JumpUpdate,     // jump already active, new 2-sigma surge (intensity refresh)
    JumpCessation   // DFRDT < 0 while a jump was active (documented tornado-time signal)
};

struct JumpEvent {
    JumpEventKind kind = JumpEventKind::JumpStart;
    std::int64_t timeEpochSec = 0;
    double sigmaLevel = 0.0;      // DFRDT / sigma at detection (2, 3, 4, 5+ ...)
    double flashRatePerMin = 0.0;
    int cellX = 0;                // grid cell indices; 0 for a scalar detector
    int cellY = 0;
};

} // namespace cappedweather
