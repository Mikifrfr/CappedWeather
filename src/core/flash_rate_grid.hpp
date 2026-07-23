#pragma once

#include "core/lightning_jump.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace cappedweather {

struct FlashRateGridConfig {
    double cellSizeDeg = 0.25;          // lat/lon bin size
    double inactiveCellTtlSec = 7200.0; // bound a long-running live grid
    LightningJumpConfig jump = {};
};

// Phase-1 spatial binning: one LightningJumpDetector per lat/lon cell.
// Storm-cell tracking (radar-feature association) replaces this in M4.
class FlashRateGrid {
public:
    explicit FlashRateGrid(FlashRateGridConfig config = {});

    void addFlash(double timeEpochSec, double latitudeDeg, double longitudeDeg);

    // Advances all cells; returns events tagged with their grid cell indices.
    std::vector<JumpEvent> poll(std::int64_t nowEpochSec);

    [[nodiscard]] std::size_t activeCellCount() const { return cells_.size(); }
    [[nodiscard]] const FlashRateGridConfig& config() const { return config_; }

private:
    struct CellState {
        explicit CellState(LightningJumpConfig config)
            : detector(config) {}

        LightningJumpDetector detector;
        double lastFlashEpochSec = 0.0;
    };

    static std::int64_t cellKey(int cellX, int cellY);
    static void unpackKey(std::int64_t key, int& cellX, int& cellY);

    FlashRateGridConfig config_;
    std::unordered_map<std::int64_t, CellState> cells_;
};

} // namespace cappedweather
