#pragma once

// VIL-density hail cores: operational proxy for severe hail from the
// volume-derived VIL + 18 dBZ echo-top fields (Amburn and Wolf 1997,
// NWS Tulsa/Louisville guidance). VIL density = VIL * 1000 / echo-top
// height normalizes out airmass dependence that makes raw VIL thresholds
// a per-day guessing game ("VIL of the day").
//
// Thresholds (grid-based VIL density, g/m^3):
//   3.5 = reasonable severe-hail threshold (POD ~0.9, FAR ~0.05 in AW97)
//   4.0 = likely severe, larger stones probable
//   4.5 = near-100% severe in the study sample
// Size hints are guidance ranges from the same studies, never precise
// diameters. This is NOT the Witt et al. MESH/SHI model (which needs
// freezing-level heights we do not ingest); the validation doc says so.
//
// Input is a chunk AFTER appendDerivedProducts (VIL + ET18 live on the
// lowest reflectivity sweep). Pure function, no UI, deterministic.

#include "core/level2_decoder.hpp"

#include <cstddef>
#include <string_view>
#include <vector>

namespace cappedweather {

struct HailCore {
    double latitudeDeg = 0.0;
    double longitudeDeg = 0.0;
    double maxVildGPerCubicMeter = 0.0;  // VIL density at the core peak
    double maxVilKgPerSquareMeter = 0.0;
    double echoTopMslMeters = 0.0;
    double maxReflectivityDbz = 0.0;
    int tier = 0;  // 1 = severe possible, 2 = likely, 3 = significant
};

struct HailConfig {
    double minVildGPerCubicMeter = 3.5;  // AW97 reasonable threshold
    double minVilKgPerSquareMeter = 10.0;  // guard: thin echo with a tiny
    // echo top divides into a huge density. Severe-hail VILs in the studies
    // start around 15; 10 keeps low-topped severe storms while rejecting
    // shallow non-convective VIL speckles.
    double minEchoTopMeters = 3000.0;  // ~10 kft; below this the quotient
    // is geometry noise, not a storm column.
    double minReflectivityDbz = 50.0;  // hail cores are high-reflectivity;
    // keeps 35 dBZ stratiform with a noisy top out of the layer.
    double clusterKm = 10.0;  // greedy peak separation (storm scale). Swept
    // 5/8/10 on three archive cases, ranked by warning CSI like rotation:
    // 10 wins on all three with no report-POD loss, and one marker per
    // ~10 km reads as footprint extent instead of label soup.
    int maxCores = 24;
};

// Tier boundaries for display + replay scoring. Tier 0 = below min.
int hailTier(double vildGPerCubicMeter);
// Guidance size range for a tier, from the NWS Louisville VIL-density
// table. Deliberately a range, never a diameter claim.
std::string_view hailSizeHint(int tier);

std::vector<HailCore> detectHailCores(const DecodedChunk& chunk,
                                      double radarLatitudeDeg,
                                      double radarLongitudeDeg,
                                      double radarElevationMeters,
                                      HailConfig config = {});

}  // namespace cappedweather
