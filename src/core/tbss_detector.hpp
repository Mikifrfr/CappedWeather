#pragma once

// Three-body scatter spike (TBSS, Lemon 1998): large hail inside a 60+ dBZ
// core scatters energy ground-hail-ground, which returns late and paints a
// down-radial spike of weak echo directly behind the core. Range/azimuth
// geometry only: no dual-pol, no soundings, works on NEXRAD and OPERA.
//
// Per-ray rule (documented limits, guidance only):
//   - core: at least one gate with REF >= 60 dBZ,
//   - spike: contiguous gates immediately downrange of the last core gate
//     with 5 <= REF < 25 dBZ, spanning >= 4 km and <= 30 km,
//   - the spike must END (echo-free or below-threshold gates after it):
//     a weak-echo region running to the last gate is stratiform edge,
//     not a spike.
// A detection needs >= 3 adjacent rays (within 1.5x azimuth resolution)
// whose spikes overlap in slant range: single-ray speckles never mark.
// Output is anchored at the core centroid (the hail lives there, not in
// the artifact).
//
// Pure function, no UI, deterministic. NaN/inf gates are skipped.

#include "core/level2_decoder.hpp"

#include <cstddef>
#include <vector>

namespace cappedweather {

struct TbssDetection {
    double latitudeDeg = 0.0;
    double longitudeDeg = 0.0;
    double maxCoreDbz = 0.0;
    double spikeLengthKm = 0.0;
    int rayCount = 0;
};

struct TbssConfig {
    double minCoreDbz = 60.0;  // Lemon 1998: TBSS cores run 60+ dBZ
    double spikeMinDbz = 5.0;  // below this is noise/edge, not spike echo
    double spikeMaxDbz = 25.0;  // TBSS spikes read weak (< ~20-25 dBZ)
    double minSpikeKm = 4.0;  // shorter runs are sidelobe/speckle junk
    double maxSpikeKm = 30.0;  // longer runs are stratiform, not spikes
    int minAdjacentRays = 3;  // azimuthal continuity kills speckles
    int maxDetections = 12;
};

std::vector<TbssDetection> detectTbss(const DecodedChunk& chunk, double radarLatitudeDeg,
                                      double radarLongitudeDeg, double radarElevationMeters,
                                      TbssConfig config = {});

}  // namespace cappedweather
