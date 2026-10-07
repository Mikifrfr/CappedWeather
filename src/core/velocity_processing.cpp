#include "core/velocity_processing.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace cappedweather {

namespace {
constexpr double kPi = 3.14159265358979323846;

double closestFold(double value, const double reference, const double interval) {
    if (!std::isfinite(value) || !std::isfinite(reference) || interval <= 0.0) return value;
    return value + std::round((reference - value) / interval) * interval;
}
}

ProcessedVelocity processVelocity(const DecodedChunk& chunk,
                                  const VelocityProcessingOptions options) {
    const auto velocityIndex = static_cast<std::size_t>(ProductKind::Velocity);
    const auto missing = std::numeric_limits<float>::quiet_NaN();
    ProcessedVelocity result(chunk.rays.size());
    for (std::size_t rayIndex = 0; rayIndex < chunk.rays.size(); ++rayIndex) {
        const auto& ray = chunk.rays[rayIndex];
        if (!ray.has(ProductKind::Velocity)) continue;
        const auto& encodedValues = ray.fields.at(velocityIndex);
        auto& values = result[rayIndex];
        values.assign(encodedValues.size(), missing);
        double previous = missing;
        const double interval = options.nyquistVelocityMetersPerSecond * 2.0;
        const double nyquist = options.nyquistVelocityMetersPerSecond;
        for (std::size_t gate = 0; gate < encodedValues.size(); ++gate) {
            const auto native = nativeValue(encodedValues[gate], ray.scaleByProduct.at(velocityIndex),
                                            ray.offsetByProduct.at(velocityIndex));
            if (!native) continue;
            const double raw = *native;
            double value = raw;
            if (options.mode != VelocityProcessingMode::Native && std::isfinite(previous) &&
                interval > 0.0) {
                // Conditional unfold: only snap when the gate-to-gate jump
                // exceeds Nyquist (a true fold jumps ~2*Nyquist). The old code
                // always snapped, so small noise accumulated into stacked
                // intervals (4x56 = 224 m/s monsters).
                if (std::abs(raw - previous) > nyquist) {
                    value = closestFold(raw, previous, interval);
                }
                if (options.maxPhysicalVelocityMetersPerSecond > 0.0 &&
                    std::abs(value) > options.maxPhysicalVelocityMetersPerSecond &&
                    std::abs(raw) <= nyquist) {
                    // Over-unfolded: raw was inside Nyquist but the unfold
                    // threw it past physical. Fail closed (NaN) so it can
                    // never become a couplet, and hold previous for the
                    // next gate instead of propagating the error.
                    values[gate] = missing;
                    continue;
                }
            }
            values[gate] = static_cast<float>(value);
            previous = value;
        }
    }

    if (options.mode != VelocityProcessingMode::Native) {
        std::vector<std::size_t> order(chunk.rays.size());
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&chunk](const auto left, const auto right) {
            const auto& a = chunk.rays[left];
            const auto& b = chunk.rays[right];
            return a.elevationNumber < b.elevationNumber ||
                   (a.elevationNumber == b.elevationNumber && a.azimuthDegrees < b.azimuthDegrees);
        });
        for (std::size_t orderedIndex = 1; orderedIndex < order.size(); ++orderedIndex) {
            const auto previousIndex = order[orderedIndex - 1];
            const auto currentIndex = order[orderedIndex];
            // Intentional: 0/360 wrap neighbours are not joined; this is display
            // unfolding only, not for nowcast, so the north seam stays unfolded.
            if (chunk.rays[previousIndex].elevationNumber != chunk.rays[currentIndex].elevationNumber) continue;
            auto& current = result[currentIndex];
            const auto& previous = result[previousIndex];
            const auto commonGates = std::min(current.size(), previous.size());
            const double interval = options.nyquistVelocityMetersPerSecond * 2.0;
            const double nyquist = options.nyquistVelocityMetersPerSecond;
            for (std::size_t gate = 0; gate < commonGates; ++gate) {
                if (!std::isfinite(current[gate]) || !std::isfinite(previous[gate])) continue;
                // Conditional azimuthal unfold: same Nyquist-gated rule as
                // radial. Prevents a folded ray from dragging its neighbour
                // into a stacked fold.
                if (std::abs(static_cast<double>(current[gate]) -
                             static_cast<double>(previous[gate])) <= nyquist)
                    continue;
                double unfolded = closestFold(current[gate], previous[gate], interval);
                if (options.maxPhysicalVelocityMetersPerSecond > 0.0 &&
                    std::abs(unfolded) > options.maxPhysicalVelocityMetersPerSecond &&
                    std::abs(static_cast<double>(current[gate])) <= nyquist) {
                    current[gate] = std::numeric_limits<float>::quiet_NaN();
                    continue;
                }
                current[gate] = static_cast<float>(unfolded);
            }
        }
    }

    if (options.mode == VelocityProcessingMode::StormRelative) {
        for (std::size_t rayIndex = 0; rayIndex < chunk.rays.size(); ++rayIndex) {
            const auto angle = (static_cast<double>(chunk.rays[rayIndex].azimuthDegrees) -
                                options.stormMotionDirectionDegrees) * kPi / 180.0;
            const auto radialStormMotion = options.stormMotionSpeedMetersPerSecond * std::cos(angle);
            for (auto& value : result[rayIndex]) {
                if (std::isfinite(value)) value = static_cast<float>(value - radialStormMotion);
            }
        }
    }
    return result;
}

} // namespace cappedweather
