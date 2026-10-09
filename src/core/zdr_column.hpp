#pragma once

// ZDR columns: updraft proxy from dual-polarization moments (NEXRAD live
// now; OPERA stays single-pol until 2027 moments land, so those volumes
// simply yield no columns). A column is a vertically coherent stack of
// high differential reflectivity inside a high-reflectivity core:
// big liquid drops lofted above their weight class by an updraft.
//
// Column rule (documented limits, guidance only):
//   - gate ZDR >= 1.0 dB with collocated REF >= 40 dBZ (same slant range,
//     per-product gate tables honored, never index-assumed),
//   - group within 5 km horizontal of the column seed (storm scale),
//   - vertical span >= 1500 m across >= 2 distinct tilts,
//   - column top at or above 3000 m MSL (kills warm-rain low junk without
//     needing a sounding; the bright-band follow-on will gate this on the
//     measured melting level instead).
//
// Pure function, no UI, deterministic. NaN/inf gates are skipped, never
// fitted.

#include "core/level2_decoder.hpp"

#include <cstddef>
#include <vector>

namespace cappedweather {

struct ZdrColumn {
    double latitudeDeg = 0.0;
    double longitudeDeg = 0.0;
    double maxZdrDb = 0.0;
    double topMslMeters = 0.0;
    double depthMeters = 0.0;
    double maxReflectivityDbz = 0.0;
    int tiltCount = 0;
};

struct ZdrColumnConfig {
    double minZdrDb = 1.0;
    double minReflectivityDbz = 40.0;
    double minDepthMeters = 1500.0;
    double minTopMslMeters = 3000.0;
    double clusterKm = 5.0;
    int maxColumns = 12;
};

std::vector<ZdrColumn> detectZdrColumns(const DecodedChunk& chunk, double radarLatitudeDeg,
                                        double radarLongitudeDeg, double radarElevationMeters,
                                        ZdrColumnConfig config = {});

}  // namespace cappedweather
