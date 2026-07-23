#include "core/lightning_jump.hpp"

#include <cmath>

namespace cappedweather {

LightningJumpDetector::LightningJumpDetector(LightningJumpConfig config)
    : config_(config) {}

std::int64_t LightningJumpDetector::binIndex(const double timeEpochSec) const {
    return static_cast<std::int64_t>(std::floor(timeEpochSec / config_.rateResolutionSec));
}

void LightningJumpDetector::addFlash(const double timeEpochSec) {
    binCounts_[binIndex(timeEpochSec)] += 1;
}

JumpEvent LightningJumpDetector::processBin(const std::int64_t index, const int flashCount, bool& emitted) {
    emitted = false;
    JumpEvent event{};

    const double minutesPerBin = config_.rateResolutionSec / 60.0;
    const double ratePerMin = static_cast<double>(flashCount) / minutesPerBin;
    const double prevRate = history_.empty() ? 0.0 : history_.back().ratePerMin;
    const double dfrdt = (ratePerMin - prevRate) / minutesPerBin;
    const double binStartSec = static_cast<double>(index) * config_.rateResolutionSec;

    // Trailing sigma over DFRDT inside the window, excluding the current sample
    // so the jump itself cannot inflate the baseline it is measured against.
    double sum = 0.0;
    int n = 0;
    for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
        const double sampleTimeSec = static_cast<double>(it->binIndex) * config_.rateResolutionSec;
        if (binStartSec - sampleTimeSec > config_.sigmaWindowSec) {
            break;
        }
        sum += it->dfrdt;
        ++n;
    }

    double sigma = 0.0;
    bool sigmaValid = false;
    if (n >= config_.minSigmaSamples) {
        const double mean = sum / static_cast<double>(n);
        double sq = 0.0;
        for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
            const double sampleTimeSec = static_cast<double>(it->binIndex) * config_.rateResolutionSec;
            if (binStartSec - sampleTimeSec > config_.sigmaWindowSec) {
                break;
            }
            const double d = it->dfrdt - mean;
            sq += d * d;
        }
        sigma = std::sqrt(sq / static_cast<double>(n - 1)); // sample stddev
        sigmaValid = true;
    }

    const bool isJump = sigmaValid && sigma > 1e-9 &&
                        dfrdt >= config_.jumpSigmaThreshold * sigma &&
                        ratePerMin >= config_.minFlashRatePerMin;

    if (isJump) {
        lastSigmaLevel_ = dfrdt / sigma;
        event.kind = jumpActive_ ? JumpEventKind::JumpUpdate : JumpEventKind::JumpStart;
        event.sigmaLevel = lastSigmaLevel_;
        jumpActive_ = true;
        emitted = true;
    } else if (jumpActive_ && dfrdt < 0.0) {
        event.kind = JumpEventKind::JumpCessation;
        event.sigmaLevel = 0.0;
        jumpActive_ = false;
        emitted = true;
    }

    event.timeEpochSec = static_cast<std::int64_t>(binStartSec);
    event.flashRatePerMin = ratePerMin;

    history_.push_back(RateSample{index, ratePerMin, dfrdt});
    lastRatePerMin_ = ratePerMin;

    // Prune samples that can never fall inside a future sigma window.
    while (!history_.empty()) {
        const double oldestSec = static_cast<double>(history_.front().binIndex) * config_.rateResolutionSec;
        if (binStartSec - oldestSec <= config_.sigmaWindowSec) {
            break;
        }
        history_.erase(history_.begin());
    }

    return event;
}

std::vector<JumpEvent> LightningJumpDetector::poll(const std::int64_t nowEpochSec) {
    std::vector<JumpEvent> events;
    const std::int64_t currentBin = binIndex(static_cast<double>(nowEpochSec));

    if (!hasProcessedBin_) {
        // Start at the first bin that actually contains a flash; earlier
        // all-zero history cannot produce a jump and only wastes samples.
        nextBinToProcess_ = binCounts_.empty() ? currentBin : std::min(binCounts_.begin()->first, currentBin);
        hasProcessedBin_ = true;
    }

    while (nextBinToProcess_ < currentBin) {
        int count = 0;
        if (const auto it = binCounts_.find(nextBinToProcess_); it != binCounts_.end()) {
            count = it->second;
            binCounts_.erase(it);
        }
        bool emitted = false;
        JumpEvent event = processBin(nextBinToProcess_, count, emitted);
        if (emitted) {
            events.push_back(event);
        }
        ++nextBinToProcess_;
    }

    // Discard flashes that arrived after their bin was already processed
    // (late data); they would corrupt an already-evaluated bin.
    while (!binCounts_.empty() && binCounts_.begin()->first < nextBinToProcess_) {
        binCounts_.erase(binCounts_.begin());
    }

    return events;
}

} // namespace cappedweather
