#pragma once

#include "core/product.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace cappedweather {

struct DecodedRay {
    float azimuthDegrees = 0.0F;
    float azimuthResolutionDegrees = 1.0F;
    std::uint8_t elevationNumber = 0;
    float elevationDegrees = 0.0F;
    std::uint16_t gateCount = 0;
    std::int16_t firstGateMeters = 0;
    std::uint16_t gateSpacingMeters = 0;
    std::array<std::vector<std::uint16_t>, 6> fields;
    std::array<std::int16_t, 6> firstGateByProduct{};
    std::array<std::uint16_t, 6> gateSpacingByProduct{};
    std::array<float, 6> scaleByProduct{};
    std::array<float, 6> offsetByProduct{};

    bool has(ProductKind kind) const;
};

struct DecodedChunk {
    std::string radarId;
    std::vector<DecodedRay> rays;
    std::size_t messageCount = 0;
};

std::optional<double> nativeValue(ProductKind kind, std::uint8_t encoded);
std::optional<double> nativeValue(std::uint16_t encoded, float scale, float offset);
DecodedChunk decodeMessage31Chunk(std::span<const std::byte> bytes, std::string radarId);

} // namespace cappedweather
