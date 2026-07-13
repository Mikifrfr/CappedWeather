#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace cappedweather {

std::optional<std::uint16_t> readBigEndian16(std::span<const std::byte> bytes, std::size_t offset);
std::optional<std::uint32_t> readBigEndian32(std::span<const std::byte> bytes, std::size_t offset);

struct Quantization {
    double scale;
    double offset;
    std::uint8_t missing;
};

std::optional<double> decodeByte(std::uint8_t encoded, Quantization quantization);

} // namespace cappedweather
