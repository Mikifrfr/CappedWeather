#pragma once

#include "core/level2_decoder.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

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

// ---------------------------------------------------------------------------
// Z-R rainfall accumulation over a caller-supplied volume history
// ---------------------------------------------------------------------------
//
// Guidance only, never warnings. A single Z-R relation (default Marshall-Palmer
// Z = 300 R^1.4, i.e. R = (Z/300)^(1/1.4) mm/h) applied to the lowest
// reflectivity tilt of every volume in the window, left-hold integrated between
// volume times. Nothing here claims gauge accuracy: the caller has to hand in a
// window it actually holds, and the summary reports how much of that window was
// covered, so a short history cannot masquerade as a storm total.
//
// Guard (part of the product, not an add-on):
//   * geometry  - gates beyond maximumRangeMeters or whose beam is above
//                 maximumBeamHeightMeters contribute nothing and are flagged;
//                 beams above beamHeightFlagMeters count but stay flagged.
//   * dual-pol  - when a sample volume carries correlation coefficient at the
//                 gate, CC below minimumCorrelationCoefficient is
//                 non-meteorological and contributes nothing.
//   * vertical  - a strong low-level gate (>= clutterReflectivityDbz) whose
//                 column echo top stays below clutterTopMeters is clutter or
//                 anomalous propagation and contributes nothing. When the
//                 volume never observed that range above the height threshold
//                 (single-tilt volume or short range), the guard cannot run:
//                 the gate keeps its depth but is flagged, never silently
//                 trusted.
//   * hail      - reflectivity above hailCapDbz is capped before the Z-R
//                 relation, so a hail core cannot mint hundreds of mm.

enum class RainfallQuality {
    Good,      // full window, low contamination, guard ran everywhere it mattered
    Marginal,  // usable with a named caveat (short window, gaps, guard blind spots)
    Poor,      // coverage too thin or exclusions too common to read as a total
    Invalid,   // fewer than two usable volumes / no reference geometry / no time span
};

// Per-gate provenance bits. OR-ed together; a gate with no depth still carries
// the flags that explain why.
enum RainfallGateFlag : std::uint16_t {
    RainfallGateClutterSuspect = 1u << 0,    // strong low echo, no column above the melt-level floor
    RainfallGateNonMeteorological = 1u << 1, // dual-pol CC says not precipitation
    RainfallGateHailCapped = 1u << 2,        // reflectivity above the hail cap, rate clamped
    RainfallGateBeamHigh = 1u << 3,          // contributing beam above the trust height
    RainfallGateRangeLimited = 1u << 4,      // beyond the QPE range cap
    RainfallGateIncomplete = 1u << 5,        // at least one interval had no measurement here
    RainfallGateNoVerticalCheck = 1u << 6,   // strong echo but the column guard could not run
    RainfallGateGapClamped = 1u << 7,        // a volume gap shortened an interval
    RainfallGateDepthSaturated = 1u << 8,    // depth hit the storage cap
};

struct RainfallSample {
    // Borrowed, not owned: the caller keeps the volume alive for the call.
    const DecodedChunk* chunk = nullptr;
    // Volume time (scan completion) in seconds since the epoch. Non-finite
    // samples are rejected outright.
    double secondsSinceEpoch = std::numeric_limits<double>::quiet_NaN();
};

struct RainfallConfig {
    double zrA = 300.0;  // Marshall-Palmer maritime default
    double zrB = 1.4;
    double minimumReflectivityDbz = 5.0;  // below this the gate contributes 0
    double hailCapDbz = 53.0;             // ~104 mm/h ceiling before the cap
    double maximumRangeMeters = 230000.0;
    double beamHeightFlagMeters = 2000.0;   // weakest contributing beam gets flagged above this
    double maximumBeamHeightMeters = 4000.0; // above this no depth is credited at all
    double clutterReflectivityDbz = 40.0;    // strong-echo floor for the vertical guard
    double clutterTopMeters = 3000.0;        // a strong echo below this top is suspect
    double minimumCorrelationCoefficient = 0.80;
    // An interval longer than this means volumes are missing. Credit only this
    // much of it instead of pouring a multi-hour gap through one rate.
    double maximumIntervalSeconds = 600.0;
};

struct RainfallSummary {
    bool valid = false;
    RainfallQuality quality = RainfallQuality::Invalid;
    int sampleCount = 0;          // usable samples inside the window
    int rejectedSampleCount = 0;  // null chunk, non-finite time, or duplicate time
    int intervalCount = 0;        // intervals with a positive length
    int gapClampedIntervalCount = 0;
    double windowSeconds = 0.0;   // newest sample time - oldest sample time
    double coveredSeconds = 0.0;  // credited integration time after gap clamping
    double maximumMillimeters = 0.0;
    double meanMillimeters = 0.0;  // over gates that carry depth
    std::size_t depthGateCount = 0;
    std::size_t clutterSuspectGateCount = 0;
    std::size_t nonMeteorologicalGateCount = 0;
    std::size_t hailCappedGateCount = 0;
    std::size_t beamHighGateCount = 0;
    std::size_t rangeLimitedGateCount = 0;
    std::size_t incompleteGateCount = 0;
    std::size_t noVerticalCheckGateCount = 0;
    std::size_t gapClampedGateCount = 0;
    std::size_t depthSaturatedGateCount = 0;
    // Gate where the deepest total sits, so the UI can say where the number is
    // from instead of implying a basin average.
    bool haveMaximum = false;
    int maximumRayIndex = -1;  // index into the reference chunk's rays
    int maximumGateIndex = -1;
};

struct RainfallGate {
    // Tenths of a millimetre: 0.1 mm matches a tipping bucket and keeps the
    // storage honest about how much precision Z-R can actually justify.
    std::uint16_t depthTenthsMillimeters = 0;
    std::uint16_t flags = 0;
};

struct RainfallRay {
    int referenceRayIndex = -1;  // index into the newest chunk's rays
    float azimuthDegrees = 0.0F;
    std::vector<RainfallGate> gates;
};

struct RainfallGrid {
    std::string radarId;
    // Polar geometry of the reference grid (the newest sample's lowest tilt).
    int firstGateMeters = 0;
    int gateSpacingMeters = 0;
    double referenceElevationDegrees = std::numeric_limits<double>::quiet_NaN();
    std::vector<RainfallRay> rays;
    RainfallSummary summary;
};

// R = (Z / zrA)^(1 / zrB) with Z = 10^(dBZ/10) capped at hailCapDbz by the
// caller. Nullopt for non-finite input, non-positive zrA/zrB, or a non-finite
// result. Unit-test hook and the single source of the relation.
[[nodiscard]] std::optional<double> zrRateMillimetersPerHour(double reflectivityDbz,
                                                             double zrA = 300.0,
                                                             double zrB = 1.4);

// Accumulate rain depth onto the newest sample's lowest reflectivity tilt.
// `endSecondsSinceEpoch` optionally closes the trailing interval (normally the
// wall clock); NaN - the default - credits nothing after the last volume, so a
// stale scan cannot claim rain that was never observed.
[[nodiscard]] RainfallGrid accumulateRainfall(const std::vector<RainfallSample>& samples,
                                              double radarElevationMeters,
                                              double endSecondsSinceEpoch = std::numeric_limits<double>::quiet_NaN(),
                                              RainfallConfig config = {});

} // namespace cappedweather
