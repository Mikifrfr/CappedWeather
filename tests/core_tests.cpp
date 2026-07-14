#include "core/level2_primitives.hpp"
#include "core/level2_decoder.hpp"
#include "core/product.hpp"
#include "core/radar_catalog.hpp"
#include "core/radar_geometry.hpp"
#include "core/scan_diagnostics.hpp"
#include "core/velocity_processing.hpp"
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
#include <random>
#include <cstdint>
#include <string>
#include <vector>

using namespace cappedweather;

namespace {
void check(const bool condition, const char* expression, const char* file, const int line) {
    if (!condition) {
        std::cerr << file << ':' << line << ": check failed: " << expression << std::endl;
        std::exit(EXIT_FAILURE);
    }
}
}

#undef assert
#define assert(expression) check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

void testProducts() {
    assert(productDescriptors().size() == 6);
    assert(productDescriptor(ProductKind::Reflectivity).unit == "dBZ");
    assert(productDescriptor(ProductKind::CorrelationCoefficient).maximum == 1.05);
}

void testCatalog() {
    const auto sites = RadarCatalog::defaultSites();
    assert(sites.size() >= 200);
    assert(sites.front().id == "KABR");
    assert(std::any_of(sites.begin(), sites.end(), [](const RadarSite& site) { return site.id == "PAHG"; }));
    assert(std::any_of(sites.begin(), sites.end(), [](const RadarSite& site) { return site.id == "KTLX"; }));
    const auto tdwr = std::find_if(sites.begin(), sites.end(), [](const RadarSite& site) { return site.id == "TATL"; });
    assert(tdwr != sites.end());
    assert(tdwr->owner == "TDWR");
    assert(tdwr->latitude.has_value() && tdwr->longitude.has_value());
    assert(RadarCatalog::isValidRadarId("KTLX"));
    assert(RadarCatalog::isValidRadarId("PAHG"));
    assert(RadarCatalog::isValidRadarId("TATL"));
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

void testRadarGeometry() {
    const auto east = destinationPoint({0.0, 0.0}, 90.0, 100000.0);
    assert(std::abs(east.latitude) < 0.001);
    assert(std::abs(east.longitude - 0.8993) < 0.002);
    const auto inverse = rangeAndBearing({0.0, 0.0}, east);
    assert(std::abs(inverse.distanceMeters - 100000.0) < 1.0);
    assert(std::abs(inverse.bearingDegrees - 90.0) < 0.01);
    const auto beam = standardBeamGeometry(100000.0, 0.5);
    assert(beam.heightAboveRadarMeters > 1400.0 && beam.heightAboveRadarMeters < 1550.0);
    assert(std::abs(slantRangeForSurfaceDistance(beam.surfaceDistanceMeters, 0.5) - 100000.0) < 0.1);
}

void testScanDiagnostics() {
    DecodedChunk chunk;
    for (const float azimuth : {0.0F, 90.0F, 180.0F, 270.0F}) {
        DecodedRay ray;
        ray.azimuthDegrees = azimuth;
        ray.elevationNumber = 1;
        const auto index = static_cast<std::size_t>(ProductKind::Reflectivity);
        ray.fields.at(index).assign(100, 80);
        ray.gateSpacingByProduct.at(index) = 250;
        chunk.rays.push_back(std::move(ray));
    }
    const auto diagnostics = analyzeScan(chunk, ProductKind::Reflectivity, 1);
    assert(diagnostics.rayCount == 4);
    assert(diagnostics.tiltCount == 1);
    assert(std::abs(diagnostics.largestAzimuthGapDegrees - 90.0) < 0.01);
    assert(std::abs(diagnostics.maximumRangeMeters - 25000.0) < 0.01);
}

void testVelocityProcessing() {
    DecodedRay ray;
    ray.azimuthDegrees = 0.0F;
    const auto index = static_cast<std::size_t>(ProductKind::Velocity);
    ray.fields.at(index) = {169, 89}; // +20 and -20 m/s with scale 2 / offset 129.
    ray.scaleByProduct.at(index) = 2.0F;
    ray.offsetByProduct.at(index) = 129.0F;
    DecodedChunk chunk{"KTLX", {ray}, 1};
    const auto native = processVelocity(chunk, {VelocityProcessingMode::Native, 25.0, 0.0, 0.0});
    assert(std::abs(native[0][0] - 20.0) < 0.01);
    assert(std::abs(native[0][1] + 20.0) < 0.01);
    const auto dealiased = processVelocity(chunk, {VelocityProcessingMode::Dealiased, 25.0, 0.0, 0.0});
    assert(std::abs(dealiased[0][1] - 30.0) < 0.01);
    const auto stormRelative = processVelocity(
        chunk, {VelocityProcessingMode::StormRelative, 25.0, 0.0, 10.0});
    assert(std::abs(stormRelative[0][0] - 10.0) < 0.01);
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

void testMessage31MalformedInputs() {
    std::mt19937 generator(0xCA77EDu);
    std::uniform_int_distribution<int> sizeDistribution(0, 2048);
    std::uniform_int_distribution<int> byteDistribution(0, 255);
    for (int iteration = 0; iteration < 1000; ++iteration) {
        std::vector<std::byte> raw(static_cast<std::size_t>(sizeDistribution(generator)));
        for (auto& value : raw) value = static_cast<std::byte>(byteDistribution(generator));
        const auto decoded = decodeMessage31Chunk(raw, "KTLX");
        assert(decoded.rays.size() == decoded.messageCount);
    }

    std::vector<std::byte> malformed(80);
    for (std::size_t index = 0; index < 4; ++index)
        malformed[index] = static_cast<std::byte>(static_cast<unsigned char>("KTLX"[index]));
    writeBigEndian16(malformed, 18, 79);
    writeBigEndian16(malformed, 30, 10);
    const auto decoded = decodeMessage31Chunk(malformed, "KTLX");
    assert(decoded.rays.empty());
    assert(decoded.rejectedMessages == 1);
}

void testOptionalRealChunk() {
    const char* path = std::getenv("CAPPEDWEATHER_DECOMPRESSED_CHUNK");
    if (!path || *path == '\0') {
        return;
    }
    std::ifstream input(path, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    assert(!raw.empty());
    const char* configuredRadarId = std::getenv("CAPPEDWEATHER_RADAR_ID");
    const std::string radarId = configuredRadarId && *configuredRadarId != '\0' ? configuredRadarId : "KTLX";
    const auto* bytes = reinterpret_cast<const std::byte*>(raw.data());
    const auto decoded = decodeMessage31Chunk(
        std::span<const std::byte>(bytes, raw.size()), radarId);
    assert(decoded.messageCount > 0);
    assert(!decoded.rays.empty());
    assert(decoded.rays.front().has(ProductKind::Reflectivity));
    assert(std::any_of(decoded.rays.begin(), decoded.rays.end(), [](const DecodedRay& ray) {
        return ray.has(ProductKind::Velocity);
    }));
    assert(std::any_of(decoded.rays.begin(), decoded.rays.end(), [](const DecodedRay& ray) {
        return ray.has(ProductKind::SpectrumWidth);
    }));
    const auto isTdwr = radarId.starts_with('T') && radarId != "TJUA";
    if (!isTdwr) {
        for (const auto kind : productDescriptors()) {
            assert(std::any_of(decoded.rays.begin(), decoded.rays.end(), [kind](const DecodedRay& ray) {
                return ray.has(kind.kind);
            }));
        }
    }
}

int main() {
    testProducts();
    testCatalog();
    testS3Selection();
    testBinaryPrimitives();
    testNativeEncodings();
    testRadarGeometry();
    testScanDiagnostics();
    testVelocityProcessing();
    testMessage31MomentLayout();
    testMessage31SixteenBitMoment();
    testMessage31MalformedInputs();
    testOptionalRealChunk();
    std::cout << "core tests passed\n";
}
