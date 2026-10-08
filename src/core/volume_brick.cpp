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

VolumeBrick buildVolumeBrick(const DecodedChunk& chunk, const ProductKind product,
                             const VolumeBrickDims& dims, const bool incomplete) {
    VolumeBrick brick;
    brick.nx = dims.nx;
    brick.ny = dims.ny;
    brick.nz = dims.nz;
    brick.halfExtentMeters = dims.halfExtentMeters;
    brick.topMeters = dims.topMeters;
    brick.product = product;
    brick.incomplete = incomplete;
    const std::size_t voxels = static_cast<std::size_t>(dims.nx) * static_cast<std::size_t>(dims.ny) *
                               static_cast<std::size_t>(dims.nz);
    brick.value.assign(voxels, 0);
    brick.present.assign(voxels, 0);

    const auto productIndex = static_cast<std::size_t>(product);
    const ProductDescriptor& descriptor = productDescriptor(product);
    const double span = descriptor.maximum - descriptor.minimum;

    for (const DecodedRay& ray : chunk.rays) {
        if (!ray.has(product)) continue;
        const auto& field = ray.fields.at(productIndex);
        const int firstGate = ray.firstGateByProduct.at(productIndex);
        const int spacing = ray.gateSpacingByProduct.at(productIndex);
        const float scale = ray.scaleByProduct.at(productIndex);
        const float offset = ray.offsetByProduct.at(productIndex);
        for (std::size_t gate = 0; gate < field.size(); ++gate) {
            const auto native = nativeValue(field[gate], scale, offset);
            if (!native.has_value()) continue; // missing / below threshold: honest hole
            double normalized = (*native - descriptor.minimum) / span;
            normalized = std::clamp(normalized, 0.0, 1.0);
            const auto quantized = static_cast<std::uint8_t>(std::lround(normalized * 255.0));
            const double slant = gateCenterSlantRange(firstGate, spacing, gate);
            const EnuPoint point = gateEnu(ray.azimuthDegrees, slant, ray.elevationDegrees);
            if (std::fabs(point.x) > brick.halfExtentMeters ||
                std::fabs(point.y) > brick.halfExtentMeters || point.z < 0.0 ||
                point.z > brick.topMeters) {
                continue;
            }
            const int ix = voxelAxis(point.x, brick.halfExtentMeters, brick.nx);
            const int iy = voxelAxis(point.y, brick.halfExtentMeters, brick.ny);
            const int iz = std::clamp(
                static_cast<int>(std::floor(point.z / brick.topMeters * static_cast<double>(brick.nz))),
                0, brick.nz - 1);
            const std::size_t flat = (static_cast<std::size_t>(iz) * static_cast<std::size_t>(brick.ny) +
                                      static_cast<std::size_t>(iy)) *
                                         static_cast<std::size_t>(brick.nx) +
                                     static_cast<std::size_t>(ix);
            brick.present[flat] = 1;
            brick.value[flat] = std::max(brick.value[flat], quantized); // max wins: storm stays visible
        }
    }
    return brick;
}

std::optional<float> sampleBrickNearest(const VolumeBrick& brick, const double xMeters,
                                        const double yMeters, const double zMeters) {
    if (brick.nx <= 0 || std::fabs(xMeters) > brick.halfExtentMeters ||
        std::fabs(yMeters) > brick.halfExtentMeters || zMeters < 0.0 || zMeters > brick.topMeters) {
        return std::nullopt;
    }
    const int ix = voxelAxis(xMeters, brick.halfExtentMeters, brick.nx);
    const int iy = voxelAxis(yMeters, brick.halfExtentMeters, brick.ny);
    const int iz = std::clamp(
        static_cast<int>(std::floor(zMeters / brick.topMeters * static_cast<double>(brick.nz))), 0,
        brick.nz - 1);
    const std::size_t flat = (static_cast<std::size_t>(iz) * static_cast<std::size_t>(brick.ny) +
                              static_cast<std::size_t>(iy)) *
                                 static_cast<std::size_t>(brick.nx) +
                             static_cast<std::size_t>(ix);
    if (brick.present[flat] == 0) return std::nullopt;
    return static_cast<float>(brick.value[flat]) / 255.0F;
}

std::optional<RayBoxHit> intersectVolumeBox(const VolumeBrick& brick, const double eye[3],
                                            const double dir[3]) {
    const double bounds[3][2] = {
        {-brick.halfExtentMeters, brick.halfExtentMeters},
        {-brick.halfExtentMeters, brick.halfExtentMeters},
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
