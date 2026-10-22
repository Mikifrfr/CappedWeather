#pragma once

// Orbit camera shared by the CPU ray march (RadarMapCanvas::rebuildVolumeRender)
// and the RHI raymarch (VolumeCanvas). One implementation, so the two backends
// shoot identical rays at identical pixels; the GPU/CPU comparison in
// tools/volume3d_probe is only meaningful if this stays single-sourced.

#include "core/volume_brick.hpp"

#include <cmath>
#include <numbers>

namespace cappedweather {

struct VolumeCamera {
    double eye[3] = {0.0, 0.0, 0.0};
    double forward[3] = {0.0, 0.0, 0.0};
    double right[3] = {0.0, 0.0, 0.0};
    double up[3] = {0.0, 0.0, 0.0};
    double focal = 1.0;
};

inline VolumeCamera volumeCameraFor(const VolumeBrick& brick, const double orbitAzimuthDeg,
                                    const double orbitElevationDeg, const double distanceMeters,
                                    const double target[3]) {
    (void)brick; // box dims come from the caller; the camera only needs target
    constexpr double kPi = std::numbers::pi_v<double>;
    VolumeCamera camera;
    const double azimuth = orbitAzimuthDeg * kPi / 180.0;
    const double elevation = orbitElevationDeg * kPi / 180.0;
    const double cosEl = std::cos(elevation);
    camera.eye[0] = target[0] + distanceMeters * cosEl * std::sin(azimuth);
    camera.eye[1] = target[1] + distanceMeters * cosEl * std::cos(azimuth);
    camera.eye[2] = target[2] + distanceMeters * std::sin(elevation);
    double fwd[3] = {target[0] - camera.eye[0], target[1] - camera.eye[1],
                     target[2] - camera.eye[2]};
    const double fwdLen = std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
    for (int axis = 0; axis < 3; ++axis) camera.forward[axis] = fwd[axis] / fwdLen;
    // right = normalize(cross(forward, worldUp)): east on the right when
    // looking north. The sign matters: a negated right flips up with it and
    // the whole view renders upside down (north down, east left). Degenerate
    // only when looking straight down, which the elevation clamp never allows.
    double right[3] = {camera.forward[1], -camera.forward[0], 0.0};
    double rightLen = std::sqrt(right[0] * right[0] + right[1] * right[1]);
    if (rightLen < 1e-9) {
        right[0] = 1.0;
        right[1] = 0.0;
        rightLen = 1.0;
    }
    for (int axis = 0; axis < 3; ++axis) camera.right[axis] = right[axis] / rightLen;
    camera.up[0] = camera.right[1] * camera.forward[2] - camera.right[2] * camera.forward[1];
    camera.up[1] = camera.right[2] * camera.forward[0] - camera.right[0] * camera.forward[2];
    camera.up[2] = camera.right[0] * camera.forward[1] - camera.right[1] * camera.forward[0];
    camera.focal = 1.0 / std::tan(22.5 * kPi / 180.0); // 45-degree vertical FOV
    return camera;
}

} // namespace cappedweather
