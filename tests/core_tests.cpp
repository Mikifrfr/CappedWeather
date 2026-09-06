#include "core/level2_primitives.hpp"
#include "core/level2_decoder.hpp"
#include "core/product.hpp"
#include "core/radar_catalog.hpp"
#include "core/radar_geometry.hpp"
#include "core/scan_diagnostics.hpp"
#include "core/velocity_processing.hpp"
#include "core/lightning_jump.hpp"
#include "core/flash_rate_grid.hpp"
#include "core/glm_l2_reader.hpp"
#include "core/odim_reader.hpp"
#include "core/radar_lightning_fusion.hpp"
#include "core/derived_products.hpp"

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
    assert(productDescriptors().size() == 8);
    assert(productDescriptor(ProductKind::Reflectivity).unit == "dBZ");
    assert(productDescriptor(ProductKind::CorrelationCoefficient).maximum == 1.05);
    assert(productDescriptor(ProductKind::EchoTop).unit == "kft MSL");
    assert(productDescriptor(ProductKind::VerticallyIntegratedLiquid).unit == "kg/m2");
    assert(isDerivedProduct(ProductKind::EchoTop));
    assert(isDerivedProduct(ProductKind::VerticallyIntegratedLiquid));
    assert(!isDerivedProduct(ProductKind::Reflectivity));
}

DecodedRay syntheticDerivedRay(const float azimuth, const std::uint8_t elevationNumber,
                               const float elevationDegrees, const double reflectivityDbz) {
    DecodedRay ray;
    ray.azimuthDegrees = azimuth;
    ray.azimuthResolutionDegrees = 1.0F;
    ray.elevationNumber = elevationNumber;
    ray.elevationDegrees = elevationDegrees;
    const auto index = static_cast<std::size_t>(ProductKind::Reflectivity);
    const auto encoded = static_cast<std::uint16_t>(std::lround(reflectivityDbz * 2.0 + 66.0));
    ray.fields.at(index).assign(32, encoded);
    ray.firstGateByProduct.at(index) = 0;
    ray.gateSpacingByProduct.at(index) = 1000;
    ray.scaleByProduct.at(index) = 2.0F;
    ray.offsetByProduct.at(index) = 66.0F;
    return ray;
}

void testDerivedProducts() {
    DecodedChunk chunk;
    chunk.radarId = "KTLX";
    chunk.rays.push_back(syntheticDerivedRay(90.0F, 1, 0.5F, 30.0));
    chunk.rays.push_back(syntheticDerivedRay(90.0F, 2, 5.0F, 10.0));

    const auto summary = appendDerivedProducts(chunk, 370.0);
    assert(summary.sourceTiltCount == 2);
    assert(summary.outputRayCount == 1);
    assert(summary.echoTopGateCount > 0);
    assert(summary.vilGateCount > 0);

    const auto echoIndex = static_cast<std::size_t>(ProductKind::EchoTop);
    const auto vilIndex = static_cast<std::size_t>(ProductKind::VerticallyIntegratedLiquid);
    const auto& output = chunk.rays.front();
    assert(output.has(ProductKind::EchoTop));
    assert(output.has(ProductKind::VerticallyIntegratedLiquid));
    assert(!chunk.rays.back().has(ProductKind::EchoTop));

    constexpr std::size_t gate = 10;
    const auto echoTopKft = nativeValue(output.fields.at(echoIndex).at(gate),
                                        output.scaleByProduct.at(echoIndex),
                                        output.offsetByProduct.at(echoIndex));
    const auto vil = nativeValue(output.fields.at(vilIndex).at(gate),
                                 output.scaleByProduct.at(vilIndex),
                                 output.offsetByProduct.at(vilIndex));
    assert(echoTopKft.has_value());
    assert(vil.has_value());

    const auto groundRange = standardBeamGeometry(10000.0, 0.5).surfaceDistanceMeters;
    const auto lowSlant = slantRangeForSurfaceDistance(groundRange, 0.5);
    const auto highSlant = slantRangeForSurfaceDistance(groundRange, 5.0);
    const auto lowHeight = standardBeamGeometry(lowSlant, 0.5).heightAboveRadarMeters + 370.0;
    const auto highHeight = standardBeamGeometry(highSlant, 5.0).heightAboveRadarMeters + 370.0;
    const auto expectedEchoTopKft = (lowHeight + 0.6 * (highHeight - lowHeight)) / 304.8;
    assert(std::abs(*echoTopKft - expectedEchoTopKft) < 0.11);
    const auto averageLinearReflectivity = 0.5 * (std::pow(10.0, 3.0) + std::pow(10.0, 1.0));
    const auto expectedVil = 3.44e-6 * std::pow(averageLinearReflectivity, 4.0 / 7.0) *
                             (highHeight - lowHeight);
    assert(std::abs(*vil - expectedVil) < 0.11);
}

void testDerivedEchoTopLowerBoundWithMissingUpperData() {
    DecodedChunk chunk;
    chunk.rays.push_back(syntheticDerivedRay(90.0F, 1, 0.5F, 30.0));
    auto upper = syntheticDerivedRay(90.0F, 2, 5.0F, 10.0);
    upper.fields.at(static_cast<std::size_t>(ProductKind::Reflectivity)).assign(32, 0);
    chunk.rays.push_back(std::move(upper));

    const auto summary = appendDerivedProducts(chunk, 370.0);
    const auto echoIndex = static_cast<std::size_t>(ProductKind::EchoTop);
    const auto vilIndex = static_cast<std::size_t>(ProductKind::VerticallyIntegratedLiquid);
    assert(summary.echoTopLowerBoundGateCount > 0);
    assert(chunk.rays.front().fields.at(echoIndex).at(10) > 1);
    assert(chunk.rays.front().fields.at(vilIndex).at(10) == 0);
}

void testDerivedProductsRequireVolumeDepth() {
    DecodedChunk chunk;
    chunk.rays.push_back(syntheticDerivedRay(45.0F, 1, 0.5F, 40.0));
    const auto summary = appendDerivedProducts(chunk, 100.0);
    assert(summary.sourceTiltCount == 1);
    assert(summary.outputRayCount == 0);
    assert(!chunk.rays.front().has(ProductKind::EchoTop));
    assert(!chunk.rays.front().has(ProductKind::VerticallyIntegratedLiquid));
}

void testDerivedProductsSupportNegativeFirstGate() {
    DecodedChunk chunk;
    chunk.rays.push_back(syntheticDerivedRay(90.0F, 1, 0.5F, 30.0));
    chunk.rays.push_back(syntheticDerivedRay(90.0F, 2, 5.0F, 10.0));
    const auto reflectivityIndex = static_cast<std::size_t>(ProductKind::Reflectivity);
    for (auto& ray : chunk.rays) {
        ray.firstGateByProduct.at(reflectivityIndex) = -375;
        ray.gateSpacingByProduct.at(reflectivityIndex) = 250;
    }
    const auto summary = appendDerivedProducts(chunk, 370.0);
    const auto echoIndex = static_cast<std::size_t>(ProductKind::EchoTop);
    const auto vilIndex = static_cast<std::size_t>(ProductKind::VerticallyIntegratedLiquid);
    assert(summary.echoTopGateCount > 0);
    assert(summary.vilGateCount > 0);
    assert(chunk.rays.front().fields.at(echoIndex).at(8) > 1);
    assert(chunk.rays.front().fields.at(vilIndex).at(8) > 1);
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
    // OPERA 5-letter codes ride alongside NEXRAD ids. 4-letter codes with
    // digits (ISX1) are real OPERA sites too, but this guard stays NEXRAD
    // strict on purpose: all of its callers are NEXRAD/archive paths, and
    // OPERA sites are validated by catalog membership (isOperaRadar).
    assert(RadarCatalog::isValidRadarId("NOHUR"));
    assert(!RadarCatalog::isValidRadarId("ISX1"));
    assert(!RadarCatalog::isValidRadarId("nohur"));
    assert(!RadarCatalog::isValidRadarId("NOHUR1"));
}

void testOperaCatalog() {
    const auto sites = RadarCatalog::operaSites();
    // Bucket-active sites only; dark national networks are excluded by the
    // generator (see tools/generate_opera_sites.py).
    assert(sites.size() >= 120);
    const auto hurum =
        std::find_if(sites.begin(), sites.end(),
                     [](const RadarSite& site) { return site.id == "NOHUR"; });
    assert(hurum != sites.end());
    assert(hurum->dataSource == "opera");
    assert(hurum->odimCode == "nohur");
    assert(hurum->countryCode == "NO");
    assert(hurum->latitude.has_value() && hurum->longitude.has_value());
    // Dark networks stay out of the picker (verified empty 2026-09-06).
    for (const char* dark : {"UKCLE", "ESMAD", "HUBUD", "SKKOJ"}) {
        assert(std::none_of(sites.begin(), sites.end(), [dark](const RadarSite& site) {
            return site.id == dark;
        }));
    }
    // Bucket-active additions are in (Croatia rename + new sharers).
    for (const char* live : {"HRPUN", "LTLAU", "LTVIL", "DKSAM", "FIKAU"}) {
        assert(std::find_if(sites.begin(), sites.end(), [live](const RadarSite& site) {
                   return site.id == live;
               }) != sites.end());
    }
    for (const auto& site : sites) {
        assert(site.dataSource == "opera");
        assert(!site.odimCode.empty() && !site.countryCode.empty());
        assert(RadarCatalog::isValidRadarId(site.id));
    }
    // Default US sites are untouched by the new fields.
    assert(RadarCatalog::defaultSites().front().dataSource == "nexrad");
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
    assert(gateCenterSlantRange(-375, 250, 0) == -375.0);
    assert(gateCenterSlantRange(-375, 250, 2) == 125.0);
    assert(gateIndexForSlantRange(5000.0, 5000, 1000, 32).value() == 0);
    assert(gateIndexForSlantRange(6000.0, 5000, 1000, 32).value() == 1);
    assert(gateIndexForSlantRange(5490.0, 5000, 1000, 32).value() == 0);
    assert(gateIndexForSlantRange(5510.0, 5000, 1000, 32).value() == 1);
    assert(!gateIndexForSlantRange(5000.0, 5000, 0, 32).has_value());
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
    assert(std::abs(diagnostics.maximumRangeMeters - 24875.0) < 0.01);
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

void testLightningJumpDetection() {
    // 2026-07-20T14:00:00Z, aligned to a 60 s bin boundary.
    const std::int64_t t0 = 1784556000;

    LightningJumpDetector detector;
    // 20 min of wobbling ~10-14 fl/min baseline (nonzero dfrdt variance).
    for (int m = 0; m < 20; ++m) {
        const int n = (m % 2 == 0) ? 10 : 14;
        for (int k = 0; k < n; ++k) {
            detector.addFlash(static_cast<double>(t0 + m * 60 + k));
        }
    }
    auto events = detector.poll(t0 + 20 * 60);
    assert(events.empty());
    assert(!detector.jumpActive());

    // Surge minute: 60 flashes in one bin -> classic >=2 sigma jump.
    for (int k = 0; k < 60; ++k) {
        detector.addFlash(static_cast<double>(t0 + 20 * 60 + k));
    }
    events = detector.poll(t0 + 21 * 60);
    assert(events.size() == 1);
    assert(events[0].kind == JumpEventKind::JumpStart);
    assert(events[0].sigmaLevel >= 2.0);
    assert(events[0].flashRatePerMin == 60.0);
    assert(events[0].timeEpochSec == t0 + 20 * 60);
    assert(detector.jumpActive());
    assert(detector.lastSigmaLevel() >= 2.0);

    // Data stops: empty bins must decay into jump cessation (dfrdt < 0).
    events = detector.poll(t0 + 26 * 60);
    assert(events.size() == 1);
    assert(events[0].kind == JumpEventKind::JumpCessation);
    assert(events[0].timeEpochSec == t0 + 21 * 60);
    assert(!detector.jumpActive());
}

void testLightningJumpNoiseFloor() {
    const std::int64_t t0 = 1784556000;

    LightningJumpDetector detector;
    for (int m = 0; m < 20; ++m) {
        const int n = (m % 2 == 0) ? 2 : 3;
        for (int k = 0; k < n; ++k) {
            detector.addFlash(static_cast<double>(t0 + m * 60 + k));
        }
    }
    // Surge that passes 2 sigma but stays below the 10 fl/min noise floor.
    for (int k = 0; k < 9; ++k) {
        detector.addFlash(static_cast<double>(t0 + 20 * 60 + k));
    }
    const auto events = detector.poll(t0 + 21 * 60);
    assert(events.empty());
    assert(!detector.jumpActive());
}

void testFlashRateGrid() {
    const std::int64_t t0 = 1784556000;

    FlashRateGrid grid;
    for (int m = 0; m < 20; ++m) {
        const int n = (m % 2 == 0) ? 10 : 14;
        for (int k = 0; k < n; ++k) {
            grid.addFlash(static_cast<double>(t0 + m * 60 + k), 35.1, -97.1);   // OKC-ish
            grid.addFlash(static_cast<double>(t0 + m * 60 + k), 40.1, -100.1); // quiet cell
        }
    }
    assert(grid.poll(t0 + 20 * 60).empty());
    assert(grid.activeCellCount() == 2);

    for (int k = 0; k < 60; ++k) {
        grid.addFlash(static_cast<double>(t0 + 20 * 60 + k), 35.1, -97.1);
    }
    const auto events = grid.poll(t0 + 21 * 60);
    assert(events.size() == 1);
    assert(events[0].kind == JumpEventKind::JumpStart);
    // 0.25 deg cells: floor(35.1/0.25) = 140, floor(-97.1/0.25) = -389.
    assert(events[0].cellY == 140);
    assert(events[0].cellX == -389);
}

void testFlashRateGridUsesConfiguredDetectorAndPrunes() {
    const std::int64_t t0 = 1784556000;
    FlashRateGridConfig config;
    config.jump.minFlashRatePerMin = 100.0;
    config.inactiveCellTtlSec = 120.0;
    FlashRateGrid grid(config);
    for (int minute = 0; minute < 20; ++minute) {
        const int flashes = minute % 2 == 0 ? 10 : 14;
        for (int k = 0; k < flashes; ++k)
            grid.addFlash(t0 + minute * 60 + k, 35.1, -97.1);
    }
    grid.poll(t0 + 20 * 60);
    for (int k = 0; k < 60; ++k)
        grid.addFlash(t0 + 20 * 60 + k, 35.1, -97.1);
    assert(grid.poll(t0 + 21 * 60).empty()); // custom 100/min floor is honored
    grid.poll(t0 + 24 * 60);
    assert(grid.activeCellCount() == 0);
}

DecodedRay syntheticFusionRay(const float azimuth, const ProductKind product,
                              const std::uint16_t encoded) {
    DecodedRay ray;
    ray.azimuthDegrees = azimuth;
    ray.azimuthResolutionDegrees = 1.0F;
    ray.elevationNumber = 1;
    ray.elevationDegrees = 0.5F;
    const auto index = static_cast<std::size_t>(product);
    ray.fields[index] = std::vector<std::uint16_t>(32, encoded);
    ray.firstGateByProduct[index] = 5000;
    ray.gateSpacingByProduct[index] = 1000;
    ray.scaleByProduct[index] = 2.0F;
    ray.offsetByProduct[index] = product == ProductKind::Reflectivity ? 66.0F : 129.0F;
    return ray;
}

void testRadarLightningFusion() {
    DecodedChunk chunk;
    chunk.radarId = "KTLX";
    chunk.rays.push_back(syntheticFusionRay(89.0F, ProductKind::Reflectivity, 156)); // 45 dBZ
    chunk.rays.push_back(syntheticFusionRay(90.0F, ProductKind::Reflectivity, 156));
    chunk.rays.push_back(syntheticFusionRay(89.0F, ProductKind::Velocity, 89));  // -20 m/s
    chunk.rays.push_back(syntheticFusionRay(91.0F, ProductKind::Velocity, 169)); // +20 m/s

    RadarFeatureConfig config;
    config.gridSizeDeg = 0.02;
    config.minimumReflectivityCells = 1;
    config.minimumAzimuthalShearPerSecond = 0.0005;
    const auto features = extractRadarStormFeatures(chunk, 35.333, -97.278, config);
    assert(!features.empty());
    assert(features.front().maximumReflectivityDbz >= 45.0);
    assert(features.front().velocitySpanMetersPerSecond >= 40.0);
    assert(features.front().rotating);

    LightningCellSnapshot lightning;
    lightning.latitudeCenterDeg = features.front().latitudeDeg;
    lightning.longitudeCenterDeg = features.front().longitudeDeg;
    const auto associations = associateLightningWithRadar({lightning}, features, config);
    assert(associations.size() == 1);
    assert(associations.front().distanceKm < 0.1);
    assert(associations.front().radarFeatureIndex == 0);

    DecodedChunk centered;
    auto singleGate = syntheticFusionRay(90.0F, ProductKind::Reflectivity, 156);
    singleGate.fields.at(static_cast<std::size_t>(ProductKind::Reflectivity)).resize(1);
    centered.rays.push_back(std::move(singleGate));
    const auto centeredFeatures = extractRadarStormFeatures(centered, 35.333, -97.278, config);
    assert(centeredFeatures.size() == 1);
    const auto measured = rangeAndBearing(
        {35.333, -97.278},
        {centeredFeatures.front().latitudeDeg, centeredFeatures.front().longitudeDeg});
    const auto expected = standardBeamGeometry(5000.0, 0.5).surfaceDistanceMeters;
    assert(std::abs(measured.distanceMeters - expected) < 1.0);
}

void testGlmL2Keys() {
    assert(glmLcfaPrefix(2026, 201, 14) == "GLM-L2-LCFA/2026/201/14/");
    assert(glmLcfaPrefix(2026, 3, 4) == "GLM-L2-LCFA/2026/003/04/");

    const auto start = parseGlmLcfaStartTime(
        "OR_GLM-L2-LCFA_G19_s20262011400000_e20262011400200_c20262011400220.nc");
    assert(start.has_value());
    assert(*start == 1784556000); // 2026-07-20T14:00:00Z (doy 201)

    assert(!parseGlmLcfaStartTime("OR_ABI-L2-CMIPC_G19_s20262011400000.nc").has_value());
    assert(!parseGlmLcfaStartTime("OR_GLM-L2-LCFA_G19_s20269991400000.nc").has_value());
    assert(!parseGlmLcfaStartTime("random_file.nc").has_value());
}

void testGlmReaderBuildContract() {
#ifdef CAPPEDWEATHER_EXPECT_NETCDF
    assert(glmL2ReaderAvailable());
#endif
}

std::vector<std::byte> readFixtureBytes(const char* name) {
    // Committed fixtures live in tests/fixtures; ctest runs from the build
    // directory, developers sometimes from the source root.
    const char* candidates[] = {
        "tests/fixtures/odim_nohur_dbzh.h5",
        "../tests/fixtures/odim_nohur_dbzh.h5",
        "../../tests/fixtures/odim_nohur_dbzh.h5",
    };
    (void)name;
    for (const char* path : candidates) {
        std::ifstream file(path, std::ios::binary);
        if (!file) continue;
        const std::string raw((std::istreambuf_iterator<char>(file)),
                              std::istreambuf_iterator<char>());
        std::vector<std::byte> data;
        data.reserve(raw.size());
        for (const char c : raw) data.push_back(static_cast<std::byte>(c));
        return data;
    }
    return {};
}

void testOdimReader() {
    if (!odimReaderAvailable()) {
        // Stubbed build: everything fails closed with a message.
        std::string error;
        assert(!readOdimPvol({}, "nohur", &error).has_value());
        assert(!error.empty());
        return;
    }
    const auto bytes = readFixtureBytes("odim_nohur_dbzh.h5");
    if (bytes.empty()) {
        std::cout << "note: odim fixture missing, skipping decode asserts\n";
        return;
    }
    // Real OPERA volume: nohur 2026-09-05, DBZH over 10 elevations
    // (0.5..90 deg), CC BY 4.0 EUMETNET OPERA.
    std::string error;
    const auto chunk = readOdimPvol(bytes, "nohur", &error);
    assert(chunk.has_value());
    assert(chunk->radarId == "nohur");
    // 720 rays on the lowest sweep, 360 on each of the other nine.
    assert(chunk->rays.size() == 720 + 9 * 360);
    std::vector<int> elevationNumbers;
    for (const auto& ray : chunk->rays) elevationNumbers.push_back(ray.elevationNumber);
    std::sort(elevationNumbers.begin(), elevationNumbers.end());
    elevationNumbers.erase(std::unique(elevationNumbers.begin(), elevationNumbers.end()),
                           elevationNumbers.end());
    assert(elevationNumbers.size() == 10);
    for (std::size_t i = 0; i < elevationNumbers.size(); ++i) {
        assert(elevationNumbers[i] == static_cast<int>(i + 1));
    }
    const auto reflectivityIndex = static_cast<std::size_t>(ProductKind::Reflectivity);
    const auto velocityIndex = static_cast<std::size_t>(ProductKind::Velocity);
    bool sawValid = false;
    bool sawBelowThreshold = false;
    double minValid = 1e9;
    double maxValid = -1e9;
    for (const auto& ray : chunk->rays) {
        assert(ray.azimuthDegrees >= 0.0F && ray.azimuthDegrees < 360.0F);
        assert(ray.has(ProductKind::Reflectivity));
        assert(!ray.has(ProductKind::Velocity));  // DBZH-only fixture
        for (const auto encoded : ray.fields[reflectivityIndex]) {
            if (encoded <= 1) {
                sawBelowThreshold = true;
                continue;
            }
            const auto value = nativeValue(encoded, ray.scaleByProduct[reflectivityIndex],
                                           ray.offsetByProduct[reflectivityIndex]);
            assert(value.has_value());
            sawValid = true;
            minValid = std::min(minValid, *value);
            maxValid = std::max(maxValid, *value);
        }
    }
    assert(sawValid && sawBelowThreshold);
    assert(minValid >= -32.5 && maxValid <= 60.0);
    assert(maxValid > 20.0);  // real echoes, not just noise floor
    // Malformed inputs fail closed, never partially.
    assert(!readOdimPvol({}, "nohur", &error).has_value());
    const std::vector<std::byte> garbage = {std::byte{'n'}, std::byte{'o'},
                                            std::byte{'t'}, std::byte{' '},
                                            std::byte{'h'}, std::byte{'d'},
                                            std::byte{'f'}, std::byte{'5'}};
    assert(!readOdimPvol(garbage, "nohur", &error).has_value());
    assert(!readOdimPvol(std::span(bytes).first(100), "nohur", &error).has_value());
}

int main() {
    testProducts();
    testDerivedProducts();
    testDerivedEchoTopLowerBoundWithMissingUpperData();
    testDerivedProductsRequireVolumeDepth();
    testDerivedProductsSupportNegativeFirstGate();
    testCatalog();
    testOperaCatalog();
    testBinaryPrimitives();
    testNativeEncodings();
    testRadarGeometry();
    testScanDiagnostics();
    testVelocityProcessing();
    testMessage31MomentLayout();
    testMessage31SixteenBitMoment();
    testMessage31MalformedInputs();
    testOptionalRealChunk();
    testLightningJumpDetection();
    testLightningJumpNoiseFloor();
    testFlashRateGrid();
    testFlashRateGridUsesConfiguredDetectorAndPrunes();
    testGlmL2Keys();
    testGlmReaderBuildContract();
    testOdimReader();
    testRadarLightningFusion();
    std::cout << "core tests passed\n";
}
