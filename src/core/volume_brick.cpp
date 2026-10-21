#include "core/volume_brick.hpp"

#include "core/level2_decoder.hpp"
#include "core/radar_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace cappedweather {

EnuPoint gateEnu(const double azimuthDegrees, const double slantMeters,
                 const double elevationDegrees) {
    const BeamGeometry beam = standardBeamGeometry(slantMeters, elevationDegrees);
    const double azimuthRadians = azimuthDegrees * std::numbers::pi / 180.0;
    EnuPoint point;
    point.x = beam.surfaceDistanceMeters * std::sin(azimuthRadians);
    point.y = beam.surfaceDistanceMeters * std::cos(azimuthRadians);
    point.z = beam.heightAboveRadarMeters;
    return point;
}

namespace {

int voxelAxis(const double coord, const double halfExtent, const int count) {
    const double normalized = (coord + halfExtent) / (2.0 * halfExtent);
    const int index = static_cast<int>(std::floor(normalized * static_cast<double>(count)));
    return std::clamp(index, 0, count - 1);
}

} // namespace

std::size_t brickFlatIndex(const VolumeBrick& brick, const int ix, const int iy, const int iz) {
    return (static_cast<std::size_t>(iz) * static_cast<std::size_t>(brick.ny) +
            static_cast<std::size_t>(iy)) *
               static_cast<std::size_t>(brick.nx) +
           static_cast<std::size_t>(ix);
}

std::optional<BrickIndex> brickIndexOf(const VolumeBrick& brick, const double xMeters,
                                       const double yMeters, const double zMeters) {
    if (brick.nx <= 0 || brick.ny <= 0 || brick.nz <= 0 || !(brick.topMeters > 0.0) ||
        !(brick.halfExtentMeters > 0.0)) {
        return std::nullopt;
    }
    const double localX = xMeters - brick.centerXMeters;
    const double localY = yMeters - brick.centerYMeters;
    if (std::fabs(localX) > brick.halfExtentMeters || std::fabs(localY) > brick.halfExtentMeters ||
        zMeters < 0.0 || zMeters > brick.topMeters) {
        return std::nullopt;
    }
    BrickIndex index;
    index.ix = voxelAxis(localX, brick.halfExtentMeters, brick.nx);
    index.iy = voxelAxis(localY, brick.halfExtentMeters, brick.ny);
    index.iz = std::clamp(
        static_cast<int>(std::floor(zMeters / brick.topMeters * static_cast<double>(brick.nz))), 0,
        brick.nz - 1);
    index.flat = brickFlatIndex(brick, index.ix, index.iy, index.iz);
    return index;
}

bool brickVoxelInterpolated(const VolumeBrick& brick, const std::size_t flatIndex) {
    return flatIndex < brick.interpolated.size() && brick.interpolated[flatIndex] != 0;
}

VolumeBrick buildVolumeBrick(const DecodedChunk& chunk, const ProductKind product,
                             const VolumeBrickDims& dims, const bool incomplete) {
    VolumeBrick brick;
    brick.nx = dims.nx;
    brick.ny = dims.ny;
    brick.nz = dims.nz;
    brick.halfExtentMeters = dims.halfExtentMeters;
    brick.topMeters = dims.topMeters;
    brick.centerXMeters = dims.centerXMeters;
    brick.centerYMeters = dims.centerYMeters;
    brick.product = product;
    brick.incomplete = incomplete;
    const std::size_t voxels = static_cast<std::size_t>(dims.nx) * static_cast<std::size_t>(dims.ny) *
                               static_cast<std::size_t>(dims.nz);
    brick.value.assign(voxels, 0);
    brick.present.assign(voxels, 0);
    brick.interpolated.assign(voxels, 0);

    const auto productIndex = static_cast<std::size_t>(product);
    const ProductDescriptor& descriptor = productDescriptor(product);
    const double span = descriptor.maximum - descriptor.minimum;

    // Sweep identity. Each ray's decoder elevation number is the tilt id
    // (Message 31 rejects rays without one; ODIM numbers its sweeps), so
    // encoder jitter inside a tilt cannot look like a second view. A chunk with
    // no numbers falls back to a 0.1-degree bucket, well below the closest
    // pair of real tilts. Key 0 = no gate, so a column never bridges to itself,
    // and beyond 254 distinct sweeps the extra tilts simply are not bridged
    // (conservative, not invented).
    bool numberedSweeps = true;
    for (const DecodedRay& ray : chunk.rays) {
        if (ray.has(product) && ray.elevationNumber == 0) {
            numberedSweeps = false;
            break;
        }
    }
    const auto sweepId = [numberedSweeps](const DecodedRay& ray) {
        return numberedSweeps ? static_cast<int>(ray.elevationNumber)
                              : static_cast<int>(std::lround(ray.elevationDegrees * 10.0F));
    };
    std::vector<int> sweeps;
    for (const DecodedRay& ray : chunk.rays) {
        if (ray.has(product)) sweeps.push_back(sweepId(ray));
    }
    std::sort(sweeps.begin(), sweeps.end());
    sweeps.erase(std::unique(sweeps.begin(), sweeps.end()), sweeps.end());
    if (sweeps.size() > 254) sweeps.resize(254);
    const auto sweepKey = [&sweeps](const int id) {
        const auto found = std::lower_bound(sweeps.begin(), sweeps.end(), id);
        if (found == sweeps.end()) return static_cast<std::uint8_t>(0);
        return static_cast<std::uint8_t>((found - sweeps.begin()) + 1);
    };
    // Winning gate's sweep per voxel. Local: it exists only to decide what may
    // be bridged, and the public brick keeps just the interpolated mask.
    std::vector<std::uint8_t> measuredSweep(voxels, 0);

    for (const DecodedRay& ray : chunk.rays) {
        if (!ray.has(product)) continue;
        const auto& field = ray.fields.at(productIndex);
        const int firstGate = ray.firstGateByProduct.at(productIndex);
        const int spacing = ray.gateSpacingByProduct.at(productIndex);
        const float scale = ray.scaleByProduct.at(productIndex);
        const float offset = ray.offsetByProduct.at(productIndex);
        const std::uint8_t sweep = sweepKey(sweepId(ray));
        for (std::size_t gate = 0; gate < field.size(); ++gate) {
            const auto native = nativeValue(field[gate], scale, offset);
            if (!native.has_value()) continue; // missing / below threshold: honest hole
            double normalized = (*native - descriptor.minimum) / span;
            normalized = std::clamp(normalized, 0.0, 1.0);
            const auto quantized = static_cast<std::uint8_t>(std::lround(normalized * 255.0));
            const double slant = gateCenterSlantRange(firstGate, spacing, gate);
            const EnuPoint point = gateEnu(ray.azimuthDegrees, slant, ray.elevationDegrees);
            const double localX = point.x - brick.centerXMeters;
            const double localY = point.y - brick.centerYMeters;
            if (std::fabs(localX) > brick.halfExtentMeters ||
                std::fabs(localY) > brick.halfExtentMeters || point.z < 0.0 ||
                point.z > brick.topMeters) {
                continue;
            }
            const int ix = voxelAxis(localX, brick.halfExtentMeters, brick.nx);
            const int iy = voxelAxis(localY, brick.halfExtentMeters, brick.ny);
            const int iz = std::clamp(
                static_cast<int>(std::floor(point.z / brick.topMeters * static_cast<double>(brick.nz))),
                0, brick.nz - 1);
            const std::size_t flat = brickFlatIndex(brick, ix, iy, iz);
            if (brick.present[flat] == 0) {
                brick.present[flat] = 1;
                ++brick.measuredVoxels;
            }
            // max wins: storm stays visible. The sweep recorded is the one that
            // won, so a low value from another tilt cannot claim the voxel and
            // stop the fill.
            if (quantized >= brick.value[flat]) {
                brick.value[flat] = quantized;
                measuredSweep[flat] = sweep;
            }
        }
    }

    // Vertical fill: walk each (ix, iy) column and bridge the empty levels
    // between two measured voxels, lerping the value so the column transitions
    // rather than stepping. Only measured samples bound a run, so no column
    // grows a top or a bottom that no gate supports; only different sweeps are
    // bridged, so a single tilt is never filled with itself; and the fill is
    // bounded by its two ends, so it never invents a stronger return.
    std::vector<std::uint8_t> bridged;
    if (!sweeps.empty()) bridged.assign(sweeps.size() + 1, 0);
    for (int iy = 0; iy < brick.ny; ++iy) {
        for (int ix = 0; ix < brick.nx; ++ix) {
            int lowerZ = -1;
            int lowerValue = 0;
            std::uint8_t lowerSweep = 0;
            for (int iz = 0; iz < brick.nz; ++iz) {
                const std::size_t flat = brickFlatIndex(brick, ix, iy, iz);
                if (brick.present[flat] == 0) continue;
                const std::uint8_t sweep = measuredSweep[flat];
                if (lowerZ >= 0 && iz - lowerZ > 1 && sweep != lowerSweep) {
                    const int upperValue = brick.value[flat];
                    const int gapLevels = iz - lowerZ;
                    for (int fill = lowerZ + 1; fill < iz; ++fill) {
                        const double t = static_cast<double>(fill - lowerZ) /
                                         static_cast<double>(gapLevels);
                        const auto blended = static_cast<std::uint8_t>(std::lround(
                            static_cast<double>(lowerValue) +
                            (static_cast<double>(upperValue) - static_cast<double>(lowerValue)) * t));
                        const std::size_t fillFlat = brickFlatIndex(brick, ix, iy, fill);
                        brick.present[fillFlat] = 1;
                        brick.interpolated[fillFlat] = 1;
                        brick.value[fillFlat] = blended;
                        ++brick.interpolatedVoxels;
                    }
                    if (!bridged.empty()) {
                        bridged[lowerSweep] = 1;
                        bridged[sweep] = 1;
                    }
                }
                lowerZ = iz;
                lowerValue = brick.value[flat];
                lowerSweep = sweep;
            }
        }
    }
    for (const auto flag : bridged) brick.interpolatedTilts += flag;
    return brick;
}

std::optional<float> sampleBrickNearest(const VolumeBrick& brick, const double xMeters,
                                        const double yMeters, const double zMeters) {
    const auto index = brickIndexOf(brick, xMeters, yMeters, zMeters);
    if (!index.has_value()) return std::nullopt;
    if (brick.present[index->flat] == 0) return std::nullopt;
    return static_cast<float>(brick.value[index->flat]) / 255.0F;
}

std::optional<BrickSample> sampleBrickTrilinear(const VolumeBrick& brick, const double xMeters,
                                                const double yMeters, const double zMeters) {
    if (brick.nx <= 0 || brick.ny <= 0 || brick.nz <= 0) return std::nullopt;
    const double localX = xMeters - brick.centerXMeters;
    const double localY = yMeters - brick.centerYMeters;
    if (std::fabs(localX) > brick.halfExtentMeters || std::fabs(localY) > brick.halfExtentMeters ||
        zMeters < 0.0 || zMeters > brick.topMeters) {
        return std::nullopt;
    }
    const auto axis = [](const double coord, const double span, const int count) {
        const double cell = (coord / span) * static_cast<double>(count) - 0.5;
        const int lo = static_cast<int>(std::floor(cell));
        return std::pair<int, double>{lo, cell - static_cast<double>(lo)};
    };
    const auto [ix, tx] = axis(localX + brick.halfExtentMeters, 2.0 * brick.halfExtentMeters, brick.nx);
    const auto [iy, ty] = axis(localY + brick.halfExtentMeters, 2.0 * brick.halfExtentMeters, brick.ny);
    const auto [iz, tz] = axis(zMeters, brick.topMeters, brick.nz);

    double accumulated = 0.0;
    double weightSum = 0.0;
    int presentCorners = 0;
    for (int dz = 0; dz < 2; ++dz) {
        for (int dy = 0; dy < 2; ++dy) {
            for (int dx = 0; dx < 2; ++dx) {
                const int sx = std::clamp(ix + dx, 0, brick.nx - 1);
                const int sy = std::clamp(iy + dy, 0, brick.ny - 1);
                const int sz = std::clamp(iz + dz, 0, brick.nz - 1);
                const std::size_t flat = (static_cast<std::size_t>(sz) * static_cast<std::size_t>(brick.ny) +
                                          static_cast<std::size_t>(sy)) * static_cast<std::size_t>(brick.nx) +
                                         static_cast<std::size_t>(sx);
                if (brick.present[flat] == 0) continue;
                const double wx = dx == 0 ? 1.0 - tx : tx;
                const double wy = dy == 0 ? 1.0 - ty : ty;
                const double wz = dz == 0 ? 1.0 - tz : tz;
                const double weight = wx * wy * wz;
                accumulated += weight * (static_cast<double>(brick.value[flat]) / 255.0);
                weightSum += weight;
                ++presentCorners;
            }
        }
    }
    if (presentCorners == 0 || weightSum <= 0.0) return std::nullopt;
    return BrickSample{static_cast<float>(accumulated / weightSum),
                       static_cast<float>(presentCorners) / 8.0F};
}

std::optional<EnuVector> brickSurfaceNormal(const VolumeBrick& brick, const double xMeters,
                                            const double yMeters, const double zMeters) {
    if (brick.nx <= 0 || brick.ny <= 0 || brick.nz <= 0) return std::nullopt;
    const double hx = brick.halfExtentMeters / static_cast<double>(brick.nx);
    const double hy = brick.halfExtentMeters / static_cast<double>(brick.ny);
    const double hz = brick.topMeters / (2.0 * static_cast<double>(brick.nz));
    const auto at = [&](const double ox, const double oy, const double oz) {
        const auto sample = sampleBrickTrilinear(brick, xMeters + ox, yMeters + oy, zMeters + oz);
        // Value times coverage: renormalised trilinear keeps a value constant right
        // up to a hole boundary, so the raw field has no gradient there. Folding
        // coverage in gives the boundary its own falloff, which is what the
        // shading needs to see at the edge of known data.
        return sample.has_value() ? sample->value01 * sample->coverage : 0.0F;
    };
    if (!sampleBrickTrilinear(brick, xMeters, yMeters, zMeters).has_value()) return std::nullopt;
    EnuVector normal{
        -static_cast<double>(at(hx, 0.0, 0.0) - at(-hx, 0.0, 0.0)),
        -static_cast<double>(at(0.0, hy, 0.0) - at(0.0, -hy, 0.0)),
        -static_cast<double>(at(0.0, 0.0, hz) - at(0.0, 0.0, -hz)),
    };
    const double length =
        std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
    if (!(length > 1e-9)) return std::nullopt;
    normal.x /= length;
    normal.y /= length;
    normal.z /= length;
    return normal;
}

std::optional<RayBoxHit> intersectVolumeBox(const VolumeBrick& brick, const double eye[3],
                                            const double dir[3]) {
    const double bounds[3][2] = {
        {brick.centerXMeters - brick.halfExtentMeters, brick.centerXMeters + brick.halfExtentMeters},
        {brick.centerYMeters - brick.halfExtentMeters, brick.centerYMeters + brick.halfExtentMeters},
        {0.0, brick.topMeters},
    };
    double tEnter = 0.0;
    double tExit = std::numeric_limits<double>::infinity();
    for (int axis = 0; axis < 3; ++axis) {
        if (std::fabs(dir[axis]) < 1e-12) {
            if (eye[axis] < bounds[axis][0] || eye[axis] > bounds[axis][1]) return std::nullopt;
            continue;
        }
        double t0 = (bounds[axis][0] - eye[axis]) / dir[axis];
        double t1 = (bounds[axis][1] - eye[axis]) / dir[axis];
        if (t0 > t1) std::swap(t0, t1);
        tEnter = std::max(tEnter, t0);
        tExit = std::min(tExit, t1);
        if (tEnter > tExit) return std::nullopt;
    }
    return RayBoxHit{tEnter, tExit};
}

Rgba evaluateTransfer(const std::vector<RgbaStop>& stops, const float value01) {
    if (stops.empty()) return {};
    if (value01 <= stops.front().value) {
        const auto& s = stops.front();
        return {s.r, s.g, s.b, s.a};
    }
    if (value01 >= stops.back().value) {
        const auto& s = stops.back();
        return {s.r, s.g, s.b, s.a};
    }
    for (std::size_t i = 1; i < stops.size(); ++i) {
        if (value01 <= stops[i].value) {
            const RgbaStop& lo = stops[i - 1];
            const RgbaStop& hi = stops[i];
            const float span = hi.value - lo.value;
            const float t = span > 0.0F ? (value01 - lo.value) / span : 0.0F;
            return {
                lo.r + (hi.r - lo.r) * t,
                lo.g + (hi.g - lo.g) * t,
                lo.b + (hi.b - lo.b) * t,
                lo.a + (hi.a - lo.a) * t,
            };
        }
    }
    const auto& s = stops.back();
    return {s.r, s.g, s.b, s.a};
}

} // namespace cappedweather
