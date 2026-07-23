#pragma once

#include "core/lightning_flash.hpp"

#include <cstdint>

namespace cappedweather {

// A renderable, time-bounded view of one grid cell emitted by the live LJA.
// The app keeps active jumps until a cessation arrives and retains cessations
// briefly because cessation clustering is itself operationally meaningful.
struct LightningCellSnapshot {
    JumpEventKind kind = JumpEventKind::JumpStart;
    std::int64_t eventTimeEpochSec = 0;
    std::int64_t expiresEpochSec = 0;
    double latitudeCenterDeg = 0.0;
    double longitudeCenterDeg = 0.0;
    double cellSizeDeg = 0.25;
    double sigmaLevel = 0.0;
    double flashRatePerMin = 0.0;
    int cellX = 0;
    int cellY = 0;

    [[nodiscard]] bool active() const { return kind != JumpEventKind::JumpCessation; }
};

} // namespace cappedweather
