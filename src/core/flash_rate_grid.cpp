#include "core/flash_rate_grid.hpp"

#include <algorithm>
#include <cmath>

namespace cappedweather {

FlashRateGrid::FlashRateGrid(FlashRateGridConfig config)
    : config_(config) {}

std::int64_t FlashRateGrid::cellKey(const int cellX, const int cellY) {
    return (static_cast<std::int64_t>(cellX) << 32) ^ (static_cast<std::int64_t>(cellY) & 0xFFFFFFFFLL);
}

void FlashRateGrid::unpackKey(const std::int64_t key, int& cellX, int& cellY) {
    cellX = static_cast<int>(key >> 32);
    cellY = static_cast<int>(key & 0xFFFFFFFFLL);
}

void FlashRateGrid::addFlash(const double timeEpochSec, const double latitudeDeg, const double longitudeDeg) {
    const int cellX = static_cast<int>(std::floor(longitudeDeg / config_.cellSizeDeg));
    const int cellY = static_cast<int>(std::floor(latitudeDeg / config_.cellSizeDeg));
    auto [it, inserted] = cells_.try_emplace(cellKey(cellX, cellY), config_.jump);
    (void)inserted;
    it->second.detector.addFlash(timeEpochSec);
    it->second.lastFlashEpochSec = std::max(it->second.lastFlashEpochSec, timeEpochSec);
}

std::vector<JumpEvent> FlashRateGrid::poll(const std::int64_t nowEpochSec) {
    std::vector<JumpEvent> events;
    for (auto it = cells_.begin(); it != cells_.end();) {
        const auto key = it->first;
        auto& cell = it->second;
        int cellX = 0;
        int cellY = 0;
        unpackKey(key, cellX, cellY);
        for (JumpEvent& event : cell.detector.poll(nowEpochSec)) {
            event.cellX = cellX;
            event.cellY = cellY;
            events.push_back(std::move(event));
        }
        const bool expired = config_.inactiveCellTtlSec > 0.0 &&
                             static_cast<double>(nowEpochSec) - cell.lastFlashEpochSec >
                                 config_.inactiveCellTtlSec &&
                             !cell.detector.jumpActive();
        if (expired) {
            it = cells_.erase(it);
        } else {
            ++it;
        }
    }
    return events;
}

} // namespace cappedweather
