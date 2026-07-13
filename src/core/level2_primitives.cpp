#include "core/level2_primitives.hpp"

namespace cappedweather {

std::optional<std::uint16_t> readBigEndian16(const std::span<const std::byte> bytes, const std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 2) {
        return std::nullopt;
    }
    const auto first = std::to_integer<std::uint8_t>(bytes[offset]);
    const auto second = std::to_integer<std::uint8_t>(bytes[offset + 1]);
    return static_cast<std::uint16_t>((first << 8U) | second);
}

std::optional<std::uint32_t> readBigEndian32(const std::span<const std::byte> bytes, const std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4) {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value = (value << 8U) | std::to_integer<std::uint8_t>(bytes[offset + index]);
    }
    return value;
}

std::optional<double> decodeByte(const std::uint8_t encoded, const Quantization quantization) {
    if (encoded == quantization.missing) {
        return std::nullopt;
    }
    return static_cast<double>(encoded) * quantization.scale + quantization.offset;
}

} // namespace cappedweather
