#pragma once

// M1 of the 3D volume design (docs/gpu-volume-rendering.md): a Qt-free
// Cartesian voxel brick splatted from a polar DecodedChunk, shared by the
// future RHI raymarcher and the CPU fallback so both backends agree.

#include "core/product.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace cappedweather {

struct DecodedChunk;

struct VolumeBrickDims {
    int nx = 256;
    int ny = 256;
    int nz = 96;
    double halfExtentMeters = 150000.0; // brick spans [-half, +half] in x/y
    double topMeters = 20000.0;         // brick spans [0, top] in z
};

struct VolumeBrick {
    int nx = 0;
    int ny = 0;
    int nz = 0;
    double halfExtentMeters = 0.0;
    double topMeters = 0.0;
    ProductKind product = ProductKind::Reflectivity;
    bool incomplete = false; // partial volume: render what exists, say so
    std::vector<std::uint8_t> value;   // normalized 0..255, max wins on collision
    std::vector<std::uint8_t> present; // 1 where at least one gate splatted
};

// Local ENU meters relative to the radar site: x east, y north, z up.
struct EnuPoint {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

// Gate position from ray geometry. Height uses the shared 4/3-earth beam
// model so the brick agrees with Echo Tops / VIL on altitude.
EnuPoint gateEnu(double azimuthDegrees, double slantMeters, double elevationDegrees);

VolumeBrick buildVolumeBrick(const DecodedChunk& chunk, ProductKind product,
                             const VolumeBrickDims& dims = VolumeBrickDims{},
                             bool incomplete = false);

// Nearest-voxel read. nullopt outside the box or where no gate landed.
std::optional<float> sampleBrickNearest(const VolumeBrick& brick, double xMeters, double yMeters,
                                        double zMeters);

struct BrickSample {
    float value01 = 0.0F;
    float coverage = 0.0F; // fraction of the 8 corners that carry a gate, 0..1
};

// Trilinear read. `value01` averages only the corners that carry gates, so a
// hole is never interpolated as zero echo; `coverage` is how much of the cell is
// known, and callers scale opacity by it rather than inventing density.
std::optional<BrickSample> sampleBrickTrilinear(const VolumeBrick& brick, double xMeters,
                                                double yMeters, double zMeters);

struct EnuVector {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

// Outward unit normal (opposite the value gradient) from central differences.
// nullopt when the neighbourhood is unknown or the field is flat, so the caller
// shades with ambient only instead of a fabricated surface.
std::optional<EnuVector> brickSurfaceNormal(const VolumeBrick& brick, double xMeters,
                                            double yMeters, double zMeters);

// Ray vs brick box (slab method). Distances are in units of dir; callers pass
// a normalized direction to get meters.
struct RayBoxHit {
    double tEnter = 0.0;
    double tExit = 0.0;
};
std::optional<RayBoxHit> intersectVolumeBox(const VolumeBrick& brick, const double eye[3],
                                            const double dir[3]);

// Transfer function: plain RGBA stops over normalized value, evaluated on CPU
// and mirrored in the RHI shader. The app builds stops from its palettes so
// 2D and 3D agree on color; core only lerps.
struct RgbaStop {
    float value = 0.0F; // normalized 0..1, ascending
    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;
    float a = 0.0F;
};
struct Rgba {
    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;
    float a = 0.0F;
};
Rgba evaluateTransfer(const std::vector<RgbaStop>& stops, float value01);

} // namespace cappedweather
