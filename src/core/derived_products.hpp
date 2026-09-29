#pragma once

#include "core/level2_decoder.hpp"

#include <cstddef>

namespace cappedweather {

struct DerivedProductSummary {
    int sourceTiltCount = 0;
    std::size_t outputRayCount = 0;
    std::size_t echoTopGateCount = 0;
    std::size_t vilGateCount = 0;
    std::size_t echoTopLowerBoundGateCount = 0;
};

// Materializes volume-derived 2D fields onto the lowest reflectivity sweep.
// Echo tops use the 18 dBZ threshold and are encoded in kft MSL. VIL uses
// the standard 3.44e-6 * integral(Z^(4/7) dh) approximation with reflectivity
// capped at 56 dBZ. At least two distinct reflectivity tilts are required.
DerivedProductSummary appendDerivedProducts(DecodedChunk& chunk,
                                            double radarElevationMeters = 0.0);
// O(rays) pre-check before paying for a deep copy + derive: true when the
// chunk holds reflectivity on at least two distinct elevation numbers.
[[nodiscard]] bool chunkSupportsDerivedProducts(const DecodedChunk& chunk);

} // namespace cappedweather
