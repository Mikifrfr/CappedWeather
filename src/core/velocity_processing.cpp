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
        for (std::size_t gate = 0; gate < encodedValues.size(); ++gate) {
            const auto native = nativeValue(encodedValues[gate], ray.scaleByProduct.at(velocityIndex),
                                            ray.offsetByProduct.at(velocityIndex));
            if (!native) continue;
            auto value = *native;
            if (options.mode != VelocityProcessingMode::Native && std::isfinite(previous)) {
                value = closestFold(value, previous, options.nyquistVelocityMetersPerSecond * 2.0);
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
            for (std::size_t gate = 0; gate < commonGates; ++gate) {
                current[gate] = static_cast<float>(closestFold(
                    current[gate], previous[gate], options.nyquistVelocityMetersPerSecond * 2.0));
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
