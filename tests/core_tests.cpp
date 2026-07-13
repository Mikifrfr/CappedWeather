#include "core/level2_primitives.hpp"
#include "core/level2_decoder.hpp"
#include "core/product.hpp"
#include "core/radar_catalog.hpp"
#include "core/s3_object.hpp"

#include <array>
#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <cstdint>
#include <vector>

using namespace cappedweather;

void testProducts() {
    assert(productDescriptors().size() == 6);
    assert(productDescriptor(ProductKind::Reflectivity).unit == "dBZ");
    assert(productDescriptor(ProductKind::CorrelationCoefficient).maximum == 1.05);
}

void testCatalog() {
    const auto sites = RadarCatalog::defaultSites();
    assert(sites.size() >= 150);
    assert(sites.front().id == "KABR");
    assert(std::any_of(sites.begin(), sites.end(), [](const RadarSite& site) { return site.id == "PAHG"; }));
    assert(std::any_of(sites.begin(), sites.end(), [](const RadarSite& site) { return site.id == "KTLX"; }));
    assert(RadarCatalog::isValidRadarId("KTLX"));
    assert(RadarCatalog::isValidRadarId("PAHG"));
    assert(!RadarCatalog::isValidRadarId("TLX"));
    assert(!RadarCatalog::isValidRadarId("K1LX"));
    assert(!RadarCatalog::isValidRadarId("ktlx"));
}

void testS3Selection() {
    const std::vector<S3Object> objects{
        {"2026/07/11/KTLX/KTLX_old", "", "2026-07-11T18:00:00.000Z", 100},
        {"2026/07/11/KTLX/KTLX_new", "", "2026-07-11T18:05:00.000Z", 200},
        {"2026/07/11/KTLX/KTLX_empty", "", "2026-07-11T18:06:00.000Z", 0},
    };
    const auto latest = newestObject(objects);
    assert(latest.has_value());
    assert(latest->key.ends_with("KTLX_new"));
    assert(isRadarObjectKey(latest->key, "KTLX"));
    assert(!isRadarObjectKey(latest->key, "KBUF"));
}

void testBinaryPrimitives() {
    const std::array<std::byte, 6> bytes{
        std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
        std::byte{0x05}, std::byte{0x06},
    };
    assert(readBigEndian16(bytes, 0).value() == 0x0102);
    assert(readBigEndian32(bytes, 1).value() == 0x02030405);
    assert(!readBigEndian32(bytes, 4).has_value());

    const auto decoded = decodeByte(100, Quantization{0.5, -32.0, 0});
    assert(decoded.has_value() && *decoded == 18.0);
    assert(!decodeByte(0, Quantization{0.5, -32.0, 0}).has_value());
}

void testNativeEncodings() {
    assert(nativeValue(ProductKind::Reflectivity, 66).value() == 0.0);
    assert(nativeValue(ProductKind::Velocity, 129).value() == 0.0);
    assert(nativeValue(ProductKind::CorrelationCoefficient, 1).has_value() == false);
}

void writeBigEndian16(std::vector<std::byte>& bytes, const std::size_t offset, const std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value >> 8U);
    bytes[offset + 1] = static_cast<std::byte>(value & 0xffU);
}

void writeBigEndian32(std::vector<std::byte>& bytes, const std::size_t offset, const std::uint32_t value) {
    bytes[offset] = static_cast<std::byte>(value >> 24U);
    bytes[offset + 1] = static_cast<std::byte>((value >> 16U) & 0xffU);
    bytes[offset + 2] = static_cast<std::byte>((value >> 8U) & 0xffU);
    bytes[offset + 3] = static_cast<std::byte>(value & 0xffU);
}

void testMessage31MomentLayout() {
    constexpr std::size_t radialLength = 103;
    constexpr std::size_t momentOffset = 72;
    std::vector<std::byte> raw(radialLength);
    for (std::size_t index = 0; index < 4; ++index) {
        raw[index] = static_cast<std::byte>(static_cast<unsigned char>("KTLX"[index]));
    }
    writeBigEndian16(raw, 18, static_cast<std::uint16_t>(radialLength));
    writeBigEndian32(raw, 12, std::bit_cast<std::uint32_t>(12.5F));
    raw[20] = static_cast<std::byte>(1);
    raw[22] = static_cast<std::byte>(3);
    writeBigEndian32(raw, 24, std::bit_cast<std::uint32_t>(0.5F));
    writeBigEndian16(raw, 30, 10);
    writeBigEndian32(raw, 44, static_cast<std::uint32_t>(momentOffset));

    raw[momentOffset] = static_cast<std::byte>('D');
    raw[momentOffset + 1] = static_cast<std::byte>('R');
    raw[momentOffset + 2] = static_cast<std::byte>('E');
    raw[momentOffset + 3] = static_cast<std::byte>('F');
    writeBigEndian16(raw, momentOffset + 8, 3);
    writeBigEndian16(raw, momentOffset + 10,
                     static_cast<std::uint16_t>(static_cast<std::int16_t>(-375)));
    writeBigEndian16(raw, momentOffset + 12, 250);
    raw[momentOffset + 19] = static_cast<std::byte>(8);
    writeBigEndian32(raw, momentOffset + 20, std::bit_cast<std::uint32_t>(2.0F));
    writeBigEndian32(raw, momentOffset + 24, std::bit_cast<std::uint32_t>(66.0F));
    raw[momentOffset + 28] = static_cast<std::byte>(66);
    raw[momentOffset + 29] = static_cast<std::byte>(67);
    raw[momentOffset + 30] = static_cast<std::byte>(68);

    const auto decoded = decodeMessage31Chunk(raw, "KTLX");
    assert(decoded.messageCount == 1);
    assert(decoded.rays.size() == 1);
    assert(decoded.rays.front().azimuthDegrees == 12.5F);
    assert(decoded.rays.front().azimuthResolutionDegrees == 0.5F);
    assert(decoded.rays.front().elevationNumber == 3);
    assert(decoded.rays.front().elevationDegrees == 0.5F);
    assert(decoded.rays.front().firstGateByProduct.at(
               static_cast<std::size_t>(ProductKind::Reflectivity)) == -375);
    assert(decoded.rays.front().fields.at(static_cast<std::size_t>(ProductKind::Reflectivity)) ==
           std::vector<std::uint16_t>({66, 67, 68}));
}

void testMessage31SixteenBitMoment() {
    constexpr std::size_t radialLength = 106;
    constexpr std::size_t momentOffset = 72;
    std::vector<std::byte> raw(radialLength);
    for (std::size_t index = 0; index < 4; ++index) {
        raw[index] = static_cast<std::byte>(static_cast<unsigned char>("KTLX"[index]));
    }
    writeBigEndian16(raw, 18, static_cast<std::uint16_t>(radialLength));
    raw[20] = static_cast<std::byte>(1);
    raw[22] = static_cast<std::byte>(2);
    writeBigEndian32(raw, 24, std::bit_cast<std::uint32_t>(0.5F));
    writeBigEndian16(raw, 30, 1);
    writeBigEndian32(raw, 32, static_cast<std::uint32_t>(momentOffset));
    raw[momentOffset] = static_cast<std::byte>('D');
    raw[momentOffset + 1] = static_cast<std::byte>('P');
    raw[momentOffset + 2] = static_cast<std::byte>('H');
    raw[momentOffset + 3] = static_cast<std::byte>('I');
    writeBigEndian16(raw, momentOffset + 8, 3);
    writeBigEndian16(raw, momentOffset + 12, 250);
    raw[momentOffset + 19] = static_cast<std::byte>(16);
    writeBigEndian32(raw, momentOffset + 20, std::bit_cast<std::uint32_t>(2.8361F));
    writeBigEndian32(raw, momentOffset + 24, std::bit_cast<std::uint32_t>(2.0F));
    writeBigEndian16(raw, momentOffset + 28, 2);
    writeBigEndian16(raw, momentOffset + 30, 512);
    writeBigEndian16(raw, momentOffset + 32, 1023);

    const auto decoded = decodeMessage31Chunk(raw, "KTLX");
    assert(decoded.rays.size() == 1);
    const auto index = static_cast<std::size_t>(ProductKind::DifferentialPhase);
    assert(decoded.rays.front().fields.at(index) == std::vector<std::uint16_t>({2, 512, 1023}));
    assert(std::abs(nativeValue(512, decoded.rays.front().scaleByProduct.at(index),
                                decoded.rays.front().offsetByProduct.at(index)).value() -
                    179.82) < 0.1);
}

void testOptionalRealChunk() {
    const char* path = std::getenv("CAPPEDWEATHER_DECOMPRESSED_CHUNK");
    if (!path || *path == '\0') {
        return;
    }
    std::ifstream input(path, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    assert(!raw.empty());
    const auto* bytes = reinterpret_cast<const std::byte*>(raw.data());
    const auto decoded = decodeMessage31Chunk(
        std::span<const std::byte>(bytes, raw.size()), "KTLX");
    assert(decoded.messageCount > 0);
    assert(!decoded.rays.empty());
    assert(decoded.rays.front().has(ProductKind::Reflectivity));
    assert(std::any_of(decoded.rays.begin(), decoded.rays.end(), [](const DecodedRay& ray) {
        return ray.has(ProductKind::Velocity);
    }));
    assert(std::any_of(decoded.rays.begin(), decoded.rays.end(), [](const DecodedRay& ray) {
        return ray.has(ProductKind::SpectrumWidth);
    }));
    for (const auto kind : productDescriptors()) {
        assert(std::any_of(decoded.rays.begin(), decoded.rays.end(), [kind](const DecodedRay& ray) {
            return ray.has(kind.kind);
        }));
    }
}

int main() {
    testProducts();
    testCatalog();
    testS3Selection();
    testBinaryPrimitives();
    testNativeEncodings();
    testMessage31MomentLayout();
    testMessage31SixteenBitMoment();
    testOptionalRealChunk();
    std::cout << "core tests passed\n";
}
