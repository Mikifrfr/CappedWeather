#pragma once

// Three-body scatter spike (TBSS, Lemon 1998): large hail inside a 60+ dBZ
// core scatters energy ground-hail-ground, which returns late and paints a
// down-radial spike of weak echo rooted at the storm's far edge. Range/
// azimuth geometry only: no dual-pol, no soundings, works on NEXRAD and OPERA.
//
// Per-ray rule (documented limits, guidance only): the first weak-echo run
// downrange rooted at a storm edge (previous gate >= 30 dBZ), where run
// gates read 5-25 dBZ with up to 2 consecutive echo-free gates tolerated
// (spike speckle), ending cleanly after 3+ consecutive echo-free gates
// (fade into clear air) rather than at a strong gate. The run needs >= 6
// weak gates over a 4-30 km span, plus a 60+ dBZ core within 20 km uprange
// on the same ray. Detections cluster across tilts by core proximity (5 km
// seed ball, strongest core first): a spike paints several tilts, while
// same-tilt chaining strands real spikes as singles. A cluster needs >= 3
// rays with at most 8 degrees of azimuth spread and at most 24 rays: spikes
// are narrow, stratiform far edges are broad. Single-ray speckles and wide
// sheets never mark. Output is anchored at the strongest uprange core (the
// hail lives there, not in the artifact).
//
// Pure function, no UI, deterministic. NaN/inf gates are skipped.

#include "core/level2_decoder.hpp"

#include <cstddef>
#include <utility>
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
    double rootMinDbz = 30.0;  // spike roots at the storm's far edge
    double spikeMinDbz = 5.0;  // below this is noise/edge, not spike echo
    double spikeMaxDbz = 25.0;  // TBSS spikes read weak (< ~20-25 dBZ)
    double minSpikeKm = 4.0;  // shorter runs are sidelobe/speckle junk
    double maxSpikeKm = 30.0;  // longer runs are stratiform, not spikes
    double maxCoreDistanceKm = 20.0;  // core-to-spike association window
    int minWeakGates = 6;  // spike carries real echo, not a bridged gap
    int minAdjacentRays = 3;  // azimuthal continuity kills speckles
    double maxClusterWidthDeg = 8.0;  // spikes are narrow, sheets are broad
    int maxDetections = 12;
};

// Veto census for replay calibration (--tbss-diagnose): which rule drops
// each core-bearing ray, so thresholds tune against counts, not vibes.
struct TbssDiagnostics {
    int raysWithCore = 0;
    int noRun = 0;  // no rooted run, no core link, or ray-end overrun
    int runTooShort = 0;  // under 6 weak gates or under 4 km
    int runTooLong = 0;
    int dirtyEnd = 0;  // run ends at strong echo, not a fade-out
    int isolated = 0;  // passed geometry but no adjacent-ray company
    int wideCluster = 0;  // passed rays forming a broad sheet, not a spike
    // Core positions of isolated passing rays (capped): tells whether
    // near-misses sit behind the hail core or scatter randomly.
    std::vector<std::pair<double, double>> isolatedPositions;
};

std::vector<TbssDetection> detectTbss(const DecodedChunk& chunk, double radarLatitudeDeg,
                                      double radarLongitudeDeg, double radarElevationMeters,
                                      TbssConfig config = {}, TbssDiagnostics* diagnostics = nullptr);

}  // namespace cappedweather
