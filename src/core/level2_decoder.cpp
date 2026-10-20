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
constexpr std::size_t kMaximumGatesPerMoment = 4096;

// Volume data constant block ("RVOL"): ICD 2620001 Table XVII-E. Real archives
// carry the version 1.0 block (44 bytes) and the version 3.0 block (52 bytes);
// both place the Volume Coverage Pattern at block byte 40 as a big-endian u16,
// after lat/lon, site height, feedhorn height, calibration and initial phase.
// Verified against real KTLX 2013 (44-byte, VCP 12), KVWX 2021 (44-byte, VCP
// 212) and KPAH 2021 / KTLX 2026 (52-byte, VCP 212 / 35) volumes.
constexpr std::size_t kVolumeDataBlockMinimumSize = 44;
constexpr std::size_t kVolumeCoveragePatternOffset = 40;
constexpr std::uint16_t kMaximumVolumeCoveragePattern = 999;

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

bool blockTagAt(const std::span<const std::byte> bytes, const std::size_t offset, const char* tag) {
    if (offset > bytes.size() || bytes.size() - offset < 4) {
        return false;
    }
    for (std::size_t index = 0; index < 4; ++index) {
        if (std::to_integer<unsigned char>(bytes[offset + index]) !=
            static_cast<unsigned char>(tag[index])) {
            return false;
        }
    }
    return true;
}

// Returns the absolute offset of the Volume Coverage Pattern inside a volume
// data constant block, or nullopt when the block is not a well-formed RVOL
// record that fits inside this radial. The block size is self-declared, so a
// radial truncated mid-block is rejected rather than read past the message.
std::optional<std::size_t> volumeCoveragePatternOffset(const std::span<const std::byte> bytes,
                                                       const std::size_t blockOffset,
                                                       const std::size_t radialEnd) {
    if (blockOffset > radialEnd ||
        radialEnd - blockOffset < kVolumeDataBlockMinimumSize) {
        return std::nullopt;
    }
    const auto declaredSize = readBigEndian16(bytes, blockOffset + 4);
    if (!declaredSize.has_value() || *declaredSize < kVolumeDataBlockMinimumSize ||
        *declaredSize > radialEnd - blockOffset) {
        return std::nullopt;
    }
    return blockOffset + kVolumeCoveragePatternOffset;
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
    case ProductKind::EchoTop:
    case ProductKind::VerticallyIntegratedLiquid:
    // Message 31 has no DPKP block: KDP arrives only from ODIM_H5 volumes, so
    // a US ray must never invent a specific-differential-phase value.
    case ProductKind::SpecificDifferentialPhase:
        return std::nullopt;
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

    // Volume-level pattern: every radial's RVOL block repeats it, so a volume
    // assembled from several records (or several radials) reports one number
    // only when all of them agree.
    std::optional<std::uint16_t> volumeCoveragePattern;

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
            ++result.rejectedMessages;
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
        if (!std::isfinite(ray.azimuthDegrees) || ray.azimuthDegrees < 0.0F || ray.azimuthDegrees >= 360.0F ||
            !std::isfinite(ray.elevationDegrees) || ray.elevationDegrees < -10.0F ||
            ray.elevationDegrees > 90.0F || (azimuthResolutionCode != 1 && azimuthResolutionCode != 2) ||
            ray.elevationNumber == 0 || pos + 32 + static_cast<std::size_t>(*blockCount) * 4 > radialEnd) {
            ++result.rejectedMessages;
            pos += static_cast<std::size_t>(*radialLength) + kTransferHeaderSize;
            continue;
        }

        bool hasField = false;
        std::size_t legacyBestGates = 0;
        for (std::size_t block = 0; block < *blockCount; ++block) {
            const auto pointer = readBigEndian32(bytes, pos + 32 + block * 4);
            if (!pointer.has_value() || *pointer > *radialLength - kDataHeaderSize) {
                ++result.rejectedBlocks;
                continue;
            }
            const auto blockOffset = pos + *pointer;

            // Constant blocks appear in every radial and are not moment data:
            // RVOL is the volume constant record (the only one consumed here,
            // for the Volume Coverage Pattern), RELV and RRAD are the
            // elevation and radial constants this decoder does not use, and
            // DCFP is the clutter-filter power block that maps to no product.
            // They are expected records, not parse failures — counting them as
            // "rejected blocks" lit the parser-rejected badge on every clean
            // volume. A tag this decoder does not recognise still counts.
            if (blockTagAt(bytes, blockOffset, "RVOL")) {
                const auto patternOffset =
                    volumeCoveragePatternOffset(bytes, blockOffset, radialEnd);
                if (!patternOffset.has_value()) {
                    ++result.rejectedBlocks;
                    continue;
                }
                const auto pattern = readBigEndian16(bytes, *patternOffset);
                if (pattern.has_value() && *pattern != 0 &&
                    *pattern <= kMaximumVolumeCoveragePattern) {
                    if (!volumeCoveragePattern.has_value()) {
                        volumeCoveragePattern = *pattern;
                    } else if (*volumeCoveragePattern != *pattern) {
                        result.volumeCoveragePatternConflicting = true;
                    }
                }
                continue;
            }
            if (blockTagAt(bytes, blockOffset, "RELV") || blockTagAt(bytes, blockOffset, "RRAD") ||
                blockTagAt(bytes, blockOffset, "DCFP")) {
                continue;
            }

            const auto kind = kindForBlock(bytes, blockOffset);
            if (!kind.has_value() || blockOffset > radialEnd ||
                radialEnd - blockOffset < kDataHeaderSize) {
                ++result.rejectedBlocks;
                continue;
            }

            const auto gates = readBigEndian16(bytes, blockOffset + 8);
            const auto firstGateRaw = readBigEndian16(bytes, blockOffset + 10);
            const auto spacing = readBigEndian16(bytes, blockOffset + 12);
            const auto wordBits = std::to_integer<std::uint8_t>(bytes[blockOffset + 19]);
            const auto scale = readFloat(bytes, blockOffset + 20);
            const auto offset = readFloat(bytes, blockOffset + 24);
            if (!gates.has_value() || *gates == 0 || *gates > kMaximumGatesPerMoment ||
                !firstGateRaw.has_value() || !spacing.has_value() || *spacing == 0 || *spacing > 5000 ||
                (wordBits != 8 && wordBits != 16) || !std::isfinite(scale) || scale <= 0.0F ||
                !std::isfinite(offset)) {
                ++result.rejectedBlocks;
                continue;
            }
            const auto firstGate = static_cast<std::int16_t>(*firstGateRaw);
            const auto dataOffset = blockOffset + kDataHeaderSize;
            const auto bytesPerGate = static_cast<std::size_t>(wordBits / 8);
            if (dataOffset > radialEnd || radialEnd - dataOffset < *gates * bytesPerGate) {
                ++result.rejectedBlocks;
                continue;
            }

            // Legacy single-geometry fields are deprecated: products can differ
            // in first-gate/spacing. Keep the geometry of the widest product so
            // legacy readers get the superset, not the last block parsed.
            if (*gates > legacyBestGates) {
                legacyBestGates = *gates;
                ray.gateCount = *gates;
                ray.firstGateMeters = firstGate;
                ray.gateSpacingMeters = *spacing;
            }
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
    // Conflicting radials leave the number at 0: the flag says the volume had
    // patterns this decoder cannot reduce to one, so nothing is displayed.
    if (volumeCoveragePattern.has_value() && !result.volumeCoveragePatternConflicting) {
        result.volumeCoveragePattern = *volumeCoveragePattern;
    }
    return result;
}

void mergeVolumeCoveragePattern(DecodedChunk& destination, const DecodedChunk& source) {
    if (destination.volumeCoveragePatternConflicting || source.volumeCoveragePatternConflicting) {
        destination.volumeCoveragePattern = 0;
        destination.volumeCoveragePatternConflicting = true;
        return;
    }
    if (source.volumeCoveragePattern == 0) return;
    if (destination.volumeCoveragePattern == 0) {
        destination.volumeCoveragePattern = source.volumeCoveragePattern;
    } else if (destination.volumeCoveragePattern != source.volumeCoveragePattern) {
        destination.volumeCoveragePattern = 0;
        destination.volumeCoveragePatternConflicting = true;
    }
}

} // namespace cappedweather
