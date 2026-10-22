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
    double halfExtentMeters = 150000.0; // brick spans [centre-half, centre+half] in x/y
    double topMeters = 20000.0;         // brick spans [0, top] in z
    double centerXMeters = 0.0;         // ENU centre; 0,0 is the radar site
    double centerYMeters = 0.0;
};

struct VolumeBrick {
    int nx = 0;
    int ny = 0;
    int nz = 0;
    double halfExtentMeters = 0.0;
    double topMeters = 0.0;
    double centerXMeters = 0.0;
    double centerYMeters = 0.0;
    ProductKind product = ProductKind::Reflectivity;
    bool incomplete = false; // partial volume: render what exists, say so
    std::vector<std::uint8_t> value;   // normalized 0..255, max wins on collision
    // 1 where a voxel carries a value: measured (a gate splatted it) or
    // reconstructed (filled between two measured gates on the same column).
    std::vector<std::uint8_t> present;
    // 1 exactly where `value` was reconstructed rather than measured. A gate
    // never sets this bit, so `present && !interpolated` is the observed set.
    // Same size as `value` after a build; empty on a hand-built brick.
    std::vector<std::uint8_t> interpolated;
    std::size_t measuredVoxels = 0;     // voxels a decoded gate wrote
    std::size_t interpolatedVoxels = 0; // voxels filled between two measured gates
    int interpolatedTilts = 0;          // distinct sweeps that took part in a fill
};

// Flat index into value/present/interpolated for a voxel address.
std::size_t brickFlatIndex(const VolumeBrick& brick, int ix, int iy, int iz);

// Voxel address of an ENU point, using the same mapping the splat used. nullopt
// outside the box, below ground, or for a degenerate brick. Tests and probes
// address a voxel through this rather than re-deriving the mapping.
struct BrickIndex {
    int ix = 0;
    int iy = 0;
    int iz = 0;
    std::size_t flat = 0;
};
std::optional<BrickIndex> brickIndexOf(const VolumeBrick& brick, double xMeters, double yMeters,
                                       double zMeters);

// True when this voxel's value was reconstructed between two measured gates.
// Hand-built bricks carry no mask (and no reconstruction), so they read false.
bool brickVoxelInterpolated(const VolumeBrick& brick, std::size_t flatIndex);

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
