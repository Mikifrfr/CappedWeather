#include "core/level2_decoder.hpp"

#include "core/level2_primitives.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <utility>

namespace cappedweather {

namespace {
constexpr std::size_t kTransferHeaderSize = 28;
constexpr std::size_t kDataHeaderSize = 28;
constexpr std::size_t kMaximumDataBlocks = 10;

constexpr std::size_t productIndex(const ProductKind kind) {
    return static_cast<std::size_t>(kind);
}

float readFloat(const std::span<const std::byte> bytes, const std::size_t offset) {
    const auto value = readBigEndian32(bytes, offset);
    if (!value.has_value()) {
        return 0.0F;
    }
    return std::bit_cast<float>(*value);
}

std::optional<ProductKind> kindForBlock(const std::span<const std::byte> bytes, const std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4) {
        return std::nullopt;
    }
    const char id[5]{
        static_cast<char>(std::to_integer<unsigned char>(bytes[offset])),
        static_cast<char>(std::to_integer<unsigned char>(bytes[offset + 1])),
        static_cast<char>(std::to_integer<unsigned char>(bytes[offset + 2])),
        static_cast<char>(std::to_integer<unsigned char>(bytes[offset + 3])),
        '\0',
    };
    if (std::strcmp(id, "DREF") == 0) return ProductKind::Reflectivity;
    if (std::strcmp(id, "DVEL") == 0) return ProductKind::Velocity;
    if (std::strcmp(id, "DSW ") == 0) return ProductKind::SpectrumWidth;
    if (std::strcmp(id, "DZDR") == 0) return ProductKind::DifferentialReflectivity;
    if (std::strcmp(id, "DRHO") == 0) return ProductKind::CorrelationCoefficient;
    if (std::strcmp(id, "DPHI") == 0) return ProductKind::DifferentialPhase;
    return std::nullopt;
}

bool stationAt(const std::span<const std::byte> bytes, const std::size_t offset, const std::string& radarId) {
    if (radarId.size() != 4 || offset > bytes.size() || bytes.size() - offset < 4) {
        return false;
    }
    for (std::size_t index = 0; index < 4; ++index) {
        if (std::to_integer<unsigned char>(bytes[offset + index]) !=
            static_cast<unsigned char>(radarId[index])) {
            return false;
        }
    }
    return true;
}
}

bool DecodedRay::has(const ProductKind kind) const {
    return !fields.at(productIndex(kind)).empty();
}

std::optional<double> nativeValue(const ProductKind kind, const std::uint8_t encoded) {
    // Message 31 uses 0/1 as missing or below-threshold values. The remaining
    // fields use the WSR-88D 8-bit moment encodings.
    if (encoded <= 1) {
        return std::nullopt;
    }
    switch (kind) {
    case ProductKind::Reflectivity:
        return (static_cast<double>(encoded) - 66.0) / 2.0;
    case ProductKind::Velocity:
        return (static_cast<double>(encoded) - 129.0) / 2.0;
    case ProductKind::SpectrumWidth:
        return (static_cast<double>(encoded) - 1.0) / 2.0;
    case ProductKind::DifferentialReflectivity:
        return (static_cast<double>(encoded) - 129.0) / 16.0;
    case ProductKind::CorrelationCoefficient:
        return (static_cast<double>(encoded) - 1.0) / 254.0;
    case ProductKind::DifferentialPhase:
        return (static_cast<double>(encoded) - 2.0) / 2.0 - 180.0;
    }
    return std::nullopt;
}

std::optional<double> nativeValue(const std::uint16_t encoded, const float scale, const float offset) {
    if (encoded <= 1 || !std::isfinite(scale) || scale <= 0.0F || !std::isfinite(offset)) {
        return std::nullopt;
    }
    return (static_cast<double>(encoded) - offset) / scale;
}

DecodedChunk decodeMessage31Chunk(const std::span<const std::byte> bytes, std::string radarId) {
    DecodedChunk result;
    result.radarId = std::move(radarId);

    // A real-time decompressed chunk contains a short transfer header before
    // a sequence of 28-byte transfer records and Message 31 radial messages.
    for (std::size_t pos = 0; pos + 64 <= bytes.size();) {
        if (!stationAt(bytes, pos, result.radarId)) {
            ++pos;
            continue;
        }

        const auto radialLength = readBigEndian16(bytes, pos + 18);
        const auto blockCount = readBigEndian16(bytes, pos + 30);
        if (!radialLength.has_value() || !blockCount.has_value() || *radialLength < 64 ||
            *blockCount == 0 || *blockCount > kMaximumDataBlocks ||
            *radialLength > bytes.size() - pos) {
            ++pos;
            continue;
        }

        const auto radialEnd = pos + *radialLength;

        DecodedRay ray;
        ray.azimuthDegrees = readFloat(bytes, pos + 12);
        const auto azimuthResolutionCode = std::to_integer<std::uint8_t>(bytes[pos + 20]);
        ray.azimuthResolutionDegrees = azimuthResolutionCode == 1 ? 0.5F : 1.0F;
        ray.elevationNumber = std::to_integer<std::uint8_t>(bytes[pos + 22]);
        ray.elevationDegrees = readFloat(bytes, pos + 24);

        bool hasField = false;
        for (std::size_t block = 0; block < *blockCount; ++block) {
            const auto pointer = readBigEndian32(bytes, pos + 32 + block * 4);
            if (!pointer.has_value() || *pointer > *radialLength - kDataHeaderSize) {
                continue;
            }
            const auto blockOffset = pos + *pointer;
            const auto kind = kindForBlock(bytes, blockOffset);
            if (!kind.has_value() || blockOffset > radialEnd ||
                radialEnd - blockOffset < kDataHeaderSize) {
                continue;
            }

            const auto gates = readBigEndian16(bytes, blockOffset + 8);
            const auto firstGateRaw = readBigEndian16(bytes, blockOffset + 10);
            const auto spacing = readBigEndian16(bytes, blockOffset + 12);
            const auto wordBits = std::to_integer<std::uint8_t>(bytes[blockOffset + 19]);
            const auto scale = readFloat(bytes, blockOffset + 20);
            const auto offset = readFloat(bytes, blockOffset + 24);
            if (!gates.has_value() || !firstGateRaw.has_value() || !spacing.has_value() ||
                (wordBits != 8 && wordBits != 16) || !std::isfinite(scale) || scale <= 0.0F ||
                !std::isfinite(offset)) {
                continue;
            }
            const auto firstGate = static_cast<std::int16_t>(*firstGateRaw);
            const auto dataOffset = blockOffset + kDataHeaderSize;
            const auto bytesPerGate = static_cast<std::size_t>(wordBits / 8);
            if (dataOffset > radialEnd || radialEnd - dataOffset < *gates * bytesPerGate) {
                continue;
            }

            ray.gateCount = std::max(ray.gateCount, *gates);
            ray.firstGateMeters = firstGate;
            ray.gateSpacingMeters = *spacing;
            auto& field = ray.fields.at(productIndex(*kind));
            ray.firstGateByProduct.at(productIndex(*kind)) = firstGate;
            ray.gateSpacingByProduct.at(productIndex(*kind)) = *spacing;
            ray.scaleByProduct.at(productIndex(*kind)) = scale;
            ray.offsetByProduct.at(productIndex(*kind)) = offset;
            field.resize(*gates);
            for (std::size_t gate = 0; gate < *gates; ++gate) {
                if (wordBits == 8) {
                    field[gate] = std::to_integer<std::uint8_t>(bytes[dataOffset + gate]);
                } else {
                    field[gate] = *readBigEndian16(bytes, dataOffset + gate * 2);
                }
            }
            hasField = true;
        }

        if (hasField) {
            result.rays.push_back(std::move(ray));
            ++result.messageCount;
        }
        pos += static_cast<std::size_t>(*radialLength) + kTransferHeaderSize;
    }
    return result;
}

} // namespace cappedweather
