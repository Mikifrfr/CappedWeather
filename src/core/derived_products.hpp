#pragma once

#include "core/level2_decoder.hpp"

#include <cstddef>
#include <optional>

namespace cappedweather {

struct DerivedProductSummary {
    int sourceTiltCount = 0;
    std::size_t outputRayCount = 0;
    std::size_t echoTopGateCount = 0;
    std::size_t vilGateCount = 0;
    std::size_t echoTopLowerBoundGateCount = 0;
    // Gates where both VIL and echo top were valid enough to form VILD.
    std::size_t vilDensityGateCount = 0;
    // Maximum VILD seen in this pass (kg/m^3). 0 when no gate qualified.
    double maxVilDensityKgPerCubicMeter = 0.0;
    // Column-maximum reflectivity gates (CREF) and constant-altitude
    // interpolated gates (CAPPI at the requested MSL height).
    std::size_t crefGateCount = 0;
    std::size_t cappiGateCount = 0;
};

// Vertically integrated liquid density (VILD): VIL spread over the echo-top
// column thickness. Pure calculation, no ProductKind involved.
// Returns nullopt for non-finite inputs or non-positive thickness.
[[nodiscard]] std::optional<double> vilDensityKgPerCubicMeter(
    double vilKgPerSquareMeter, double layerThicknessMeters);
// Convenience form using absolute heights (MSL meters).
[[nodiscard]] std::optional<double> vilDensityKgPerCubicMeter(
    double vilKgPerSquareMeter, double echoTopMeters, double baseMeters);

// Materializes volume-derived 2D fields onto the lowest reflectivity sweep.
// Echo tops use the 18 dBZ threshold and are encoded in kft MSL. VIL uses
// the standard 3.44e-6 * integral(Z^(4/7) dh) approximation with reflectivity
// capped at 56 dBZ. CREF is the column-maximum reflectivity (dBZ, REF
// encoding so palettes and probes match the base moment). CAPPI linearly
// interpolates dBZ between adjacent tilts at cappiHeightMslMeters (MSL);
// below the lowest sample it clamps to the lowest value, above the highest
// sample the gate is missing (never invents echo above the scan top).
// At least two distinct reflectivity tilts are required.
DerivedProductSummary appendDerivedProducts(DecodedChunk& chunk,
                                            double radarElevationMeters = 0.0,
                                            double cappiHeightMslMeters = 3000.0);
// O(rays) pre-check before paying for a deep copy + derive: true when the
// chunk holds reflectivity on at least two distinct elevation numbers.
[[nodiscard]] bool chunkSupportsDerivedProducts(const DecodedChunk& chunk);

} // namespace cappedweather
