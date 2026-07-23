#pragma once

#include "core/lightning_flash.hpp"

#include <cstdint>
#include <map>
#include <vector>

namespace cappedweather {

struct LightningJumpConfig {
    double rateResolutionSec = 60.0;    // flash-rate bin size
    double sigmaWindowSec = 600.0;      // trailing sigma window (10 min; M5 tuned)
    double jumpSigmaThreshold = 2.0;    // classic 2-sigma jump threshold
    double minFlashRatePerMin = 12.0;   // noise floor (M5 tuned); suppresses weak-cell noise
    int minSigmaSamples = 5;            // min trailing DFRDT samples for a valid sigma
};

// Lightning Jump Algorithm (LJA) detector for ONE flash-rate time series
// (a single storm region / grid cell).
//
// Feed flashes via addFlash(), advance time via poll(). Only completed rate
// bins are evaluated. Missing bins between polls count as zero-rate samples,
// so a data gap correctly decays into jump cessation.
class LightningJumpDetector {
public:
    explicit LightningJumpDetector(LightningJumpConfig config = {});

    void addFlash(double timeEpochSec);

    // Advances detection through all bins completed before `nowEpochSec`.
    // Returns events emitted by those bins, in time order.
    std::vector<JumpEvent> poll(std::int64_t nowEpochSec);

    [[nodiscard]] const LightningJumpConfig& config() const { return config_; }
    [[nodiscard]] bool jumpActive() const { return jumpActive_; }
    [[nodiscard]] double lastSigmaLevel() const { return lastSigmaLevel_; }
    [[nodiscard]] double lastFlashRatePerMin() const { return lastRatePerMin_; }

private:
    struct RateSample {
        std::int64_t binIndex = 0;
        double ratePerMin = 0.0;
        double dfrdt = 0.0;   // flashes min^-2 vs previous sample
    };

    [[nodiscard]] std::int64_t binIndex(double timeEpochSec) const;
    JumpEvent processBin(std::int64_t index, int flashCount, bool& emitted);

    LightningJumpConfig config_;
    std::map<std::int64_t, int> binCounts_;        // bin index -> flashes (pending bins)
    std::vector<RateSample> history_;              // processed samples, time ordered
    std::int64_t nextBinToProcess_ = 0;
    bool hasProcessedBin_ = false;
    bool jumpActive_ = false;
    double lastSigmaLevel_ = 0.0;
    double lastRatePerMin_ = 0.0;
};

} // namespace cappedweather
