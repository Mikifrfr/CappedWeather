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
    // Deprecated legacy geometry: kept as the widest product's geometry for
    // old readers. New code must use firstGateByProduct/gateSpacingByProduct,
    // which are correct per product when moments differ in range tables.
    std::uint16_t gateCount = 0;
    std::int16_t firstGateMeters = 0;
    std::uint16_t gateSpacingMeters = 0;
    std::array<std::vector<std::uint16_t>, kProductCount> fields;
    std::array<std::int16_t, kProductCount> firstGateByProduct{};
    std::array<std::uint16_t, kProductCount> gateSpacingByProduct{};
    std::array<float, kProductCount> scaleByProduct{};
    std::array<float, kProductCount> offsetByProduct{};
    // True folding Nyquist for this ray's velocity moment, when the source
    // format carries it (ODIM how/NI per dataset). 0 = unknown: unfolding
    // falls back to the panel Nyquist (correct for NEXRAD Message 31,
    // which has no per-ray NI). C-band OPERA scans fold as low as ~6 m/s,
    // so unfolding those at a 25 m/s panel default manufactures couplets.
    float nyquistMetersPerSecond = 0.0F;

    bool has(ProductKind kind) const;
};

struct DecodedChunk {
    std::string radarId;
    std::vector<DecodedRay> rays;
    std::size_t messageCount = 0;
    std::size_t rejectedMessages = 0;
    std::size_t rejectedBlocks = 0;
    // Volume Coverage Pattern from the RVOL volume constant block (ICD 2620001
    // Table XVII-E, block byte 40, big-endian u16). 0 means the volume carried
    // no pattern this decoder can vouch for; volumeCoveragePatternConflicting
    // records the case where radials of one volume disagree, so a single
    // number is never presented as the whole volume's pattern.
    std::uint16_t volumeCoveragePattern = 0;
    bool volumeCoveragePatternConflicting = false;
};

std::optional<double> nativeValue(ProductKind kind, std::uint8_t encoded);
std::optional<double> nativeValue(std::uint16_t encoded, float scale, float offset);
DecodedChunk decodeMessage31Chunk(std::span<const std::byte> bytes, std::string radarId);
// Folds one decoded piece of a volume into the volume-level accumulator. A
// volume decoded record by record (or chunk by chunk on the live feed) keeps
// its pattern only while every piece agrees: two different patterns mean the
// volume spans a change, so the accumulator ends with 0 and the conflicting
// flag set instead of a number that describes half the volume.
void mergeVolumeCoveragePattern(DecodedChunk& destination, const DecodedChunk& source);

} // namespace cappedweather
