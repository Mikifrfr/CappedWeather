#pragma once

// Storm-relative helicity (0-1 and 0-3 km, Davies-Jones/Markowski form):
// SRH = Σ [(u_{i+1}-cx)(v_i-cy) - (u_i-cx)(v_{i+1}-cy)] over qualifying VAD
// rows bottom-to-top inside each layer, storm motion (cx, cy) resolved from
// the Bunkers estimate (right-mover when the shear splits, mean wind when
// weak). Positive = cyclonic (northern hemisphere).
//
// Limits, stated plainly: VAD rows are layer means, not soundings; there is
// no surface-observation interpolation (the lowest VAD row anchors the
// bottom), so low-level curvature under the first row is missed and 0-1 km
// reads low when the boundary layer is under-sampled. Quality mirrors VAD:
// both layers spanned by >= 2 rows and >= 4 rows total = Good, one layer or
// 2-3 rows = Marginal, else Invalid. Guidance only.
//
// Pure function, no UI, deterministic.

#include "core/bunkers.hpp"
#include "core/vad.hpp"

#include <vector>

namespace cappedweather {

struct SrhEstimate {
    bool valid = false;
    double srh01M2PerS2 = 0.0;  // 0-1 km AGL storm-relative helicity
    double srh03M2PerS2 = 0.0;  // 0-3 km AGL storm-relative helicity
    bool srh01Valid = false;
    bool srh03Valid = false;
    VadQuality quality = VadQuality::Invalid;
};

struct SrhConfig {
    double layer01TopAglMeters = 1000.0;
    double layer03TopAglMeters = 3000.0;
    double minSpanMeters = 500.0;  // a layer needs real depth, not one row
    double minSpan03Meters = 1500.0;  // 0-3 km from two shallow rows is
    // a boundary-layer number wearing a deep-layer label: needs 3+ rows.
    int minPairs03 = 2;
};

// Sorted wind limb shared by every SRH input (VAD rows, soundings):
// height above ground plus storm-relative-ready u/v components.
struct SrhWindPoint {
    double aglMeters = 0.0;
    double uMps = 0.0;
    double vMps = 0.0;
};

// Storm motion to (cx, cy) under the same right-mover/weak-shear choice
// the scan dialog prints. False when the motion is unusable.
bool resolveStormMotionUv(const BunkersMotion& stormMotion, double& cxMps, double& cyMps);
// Trapezoidal pair-sum over caller-supplied points (shared by VAD and
// sounding paths so ground truth and estimate integrate identically).
SrhEstimate integrateSrhPoints(std::vector<SrhWindPoint> points, double cxMps, double cyMps,
                              SrhConfig config = {});

SrhEstimate estimateSrh(const std::vector<VadLevel>& levels, const BunkersMotion& stormMotion,
                        double radarElevationMeters, SrhConfig config = {});

}  // namespace cappedweather
