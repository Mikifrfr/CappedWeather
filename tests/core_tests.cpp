#include "core/level2_primitives.hpp"
#include "core/level2_decoder.hpp"
#include "core/product.hpp"
#include "core/radar_catalog.hpp"
#include "core/radar_geometry.hpp"
#include "core/scan_diagnostics.hpp"
#include "core/velocity_processing.hpp"
#include "core/vad.hpp"
#include "core/bright_band.hpp"
#include "core/bunkers.hpp"
#include "core/hail_swath.hpp"
#include "core/hrrr_frame.hpp"
#include "core/lambert.hpp"
#include "core/goes_frame.hpp"
#include "core/fixedgrid.hpp"
#include "core/mosaic_frame.hpp"
#include "core/sounding.hpp"
#include "core/srh.hpp"
#include "core/tbss_detector.hpp"
#include "core/zdr_column.hpp"
#include "core/lightning_jump.hpp"
#include "core/flash_rate_grid.hpp"
#include "core/glm_l2_reader.hpp"
#include "core/odim_reader.hpp"
#include "core/radar_lightning_fusion.hpp"
#include "core/circulation_detector.hpp"
#include "core/hail_detector.hpp"
#include "core/debris_detector.hpp"
#include "core/shear_grid.hpp"
#include "core/storm_tracker.hpp"
#include "core/volume_brick.hpp"
#include "core/volume_spill.hpp"
#include "core/derived_products.hpp"

#include <array>
#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <random>
#include <atomic>
#include <mutex>
#include <thread>
#include <cstdint>
#include <string>
#include <tuple>
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
    assert(productDescriptors().size() == 11);
    assert(productDescriptor(ProductKind::Reflectivity).unit == "dBZ");
    assert(productDescriptor(ProductKind::CorrelationCoefficient).maximum == 1.05);
    assert(productDescriptor(ProductKind::EchoTop).unit == "kft MSL");
    assert(productDescriptor(ProductKind::VerticallyIntegratedLiquid).unit == "kg/m2");
    assert(productDescriptor(ProductKind::CompositeReflectivity).unit == "dBZ");
    assert(productDescriptor(ProductKind::Cappi).unit == "dBZ");
    // KDP is the 11th product, appended after Cappi so no persisted row index
    // moves, and unbound to a digit key (ODIM-only moment, no NEXRAD block).
    const auto& kdp = productDescriptor(ProductKind::SpecificDifferentialPhase);
    assert(kdp.shortName == "KDP");
    assert(kdp.label == "Specific differential phase");
    assert(kdp.unit == "deg/km");
    assert(kdp.minimum == -10.0);
    assert(kdp.maximum == 20.0);
    assert(isDerivedProduct(ProductKind::EchoTop));
    assert(isDerivedProduct(ProductKind::VerticallyIntegratedLiquid));
    assert(isDerivedProduct(ProductKind::CompositeReflectivity));
    assert(isDerivedProduct(ProductKind::Cappi));
    assert(!isDerivedProduct(ProductKind::Reflectivity));
    assert(!isDerivedProduct(ProductKind::SpecificDifferentialPhase));
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
    assert(!chunk.rays.front().has(ProductKind::CompositeReflectivity));
    assert(!chunk.rays.front().has(ProductKind::Cappi));
}

void testCrefIsColumnMax() {
    // Low tilt hot (30), high tilt weak (10): CREF reads the low tilt.
    DecodedChunk chunk;
    chunk.rays.push_back(syntheticDerivedRay(90.0F, 1, 0.5F, 30.0));
    chunk.rays.push_back(syntheticDerivedRay(90.0F, 2, 5.0F, 10.0));
    const auto summary = appendDerivedProducts(chunk, 370.0);
    assert(summary.crefGateCount > 0);
    const auto crefIndex = static_cast<std::size_t>(ProductKind::CompositeReflectivity);
    const auto& field = chunk.rays.front().fields.at(crefIndex);
    assert(!field.empty());
    const auto value = nativeValue(field.at(10), 2.0F, 66.0F);
    assert(value && std::abs(*value - 30.0) < 0.6);
    // Flip the profile: CREF follows the hot tilt aloft.
    DecodedChunk flipped;
    flipped.rays.push_back(syntheticDerivedRay(90.0F, 1, 0.5F, 10.0));
    flipped.rays.push_back(syntheticDerivedRay(90.0F, 2, 5.0F, 45.0));
    appendDerivedProducts(flipped, 370.0);
    const auto flippedValue =
        nativeValue(flipped.rays.front().fields.at(crefIndex).at(10), 2.0F, 66.0F);
    assert(flippedValue && std::abs(*flippedValue - 45.0) < 0.6);
}

void testCappiInterpolatesAndClamps() {
    // Gate 10 sits near 10 km slant: ~0.46 km MSL on the 0.5-deg tilt,
    // ~1.25 km MSL on the 5-deg tilt (radar at 370 m).
    DecodedChunk chunk;
    chunk.rays.push_back(syntheticDerivedRay(90.0F, 1, 0.5F, 30.0));
    chunk.rays.push_back(syntheticDerivedRay(90.0F, 2, 5.0F, 10.0));
    const auto cappiIndex = static_cast<std::size_t>(ProductKind::Cappi);
    const auto read = [&chunk, cappiIndex](double heightM) {
        appendDerivedProducts(chunk, 370.0, heightM);
        return nativeValue(chunk.rays.front().fields.at(cappiIndex).at(10), 2.0F, 66.0F);
    };
    // Between the beams: interpolation, strictly inside (10, 30).
    const auto mid = read(800.0);
    assert(mid && *mid > 10.0 && *mid < 30.0);
    // Below the lowest sample: clamped to the lowest tilt.
    const auto low = read(100.0);
    assert(low && std::abs(*low - 30.0) < 0.6);
    // Above the scan top: missing, never invented.
    appendDerivedProducts(chunk, 370.0, 20000.0);
    assert(chunk.rays.front().fields.at(cappiIndex).at(10) <= 1);
}

void testDerivedSupportGate() {
    // Empty and single-tilt volumes must skip the background derive copy.
    {
        DecodedChunk empty;
        assert(!chunkSupportsDerivedProducts(empty));
        DecodedChunk single;
        single.rays.push_back(syntheticDerivedRay(45.0F, 1, 0.5F, 40.0));
        assert(!chunkSupportsDerivedProducts(single));
    }
    // Two tilts qualify, even when other rays carry no reflectivity.
    {
        DecodedChunk chunk;
        chunk.rays.push_back(syntheticDerivedRay(90.0F, 1, 0.5F, 30.0));
        DecodedRay bare;
        bare.elevationNumber = 2;
        chunk.rays.push_back(bare);
        assert(!chunkSupportsDerivedProducts(chunk));
        chunk.rays.back() = syntheticDerivedRay(90.0F, 2, 5.0F, 10.0);
        assert(chunkSupportsDerivedProducts(chunk));
    }
}

// ---------------------------------------------------------------------------
// Z-R rainfall accumulation over volume history
// ---------------------------------------------------------------------------

DecodedRay syntheticRainRay(const float azimuth, const std::uint8_t elevationNumber,
                            const float elevationDegrees, const int firstGateMeters,
                            const int gateSpacingMeters, const std::size_t gateCount,
                            const double reflectivityDbz, const bool echo = true) {
    DecodedRay ray;
    ray.azimuthDegrees = azimuth;
    ray.azimuthResolutionDegrees = 1.0F;
    ray.elevationNumber = elevationNumber;
    ray.elevationDegrees = elevationDegrees;
    const auto index = static_cast<std::size_t>(ProductKind::Reflectivity);
    const auto encoded =
        echo ? static_cast<std::uint16_t>(std::lround(reflectivityDbz * 2.0 + 66.0))
             : static_cast<std::uint16_t>(0);
    ray.fields.at(index).assign(gateCount, encoded);
    ray.firstGateByProduct.at(index) = static_cast<std::int16_t>(firstGateMeters);
    ray.gateSpacingByProduct.at(index) = static_cast<std::uint16_t>(gateSpacingMeters);
    ray.scaleByProduct.at(index) = 2.0F;
    ray.offsetByProduct.at(index) = 66.0F;
    return ray;
}

void attachRainCorrelation(DecodedRay& ray, const double correlationCoefficient) {
    const auto index = static_cast<std::size_t>(ProductKind::CorrelationCoefficient);
    const auto reflectivityIndex = static_cast<std::size_t>(ProductKind::Reflectivity);
    const auto count = ray.fields.at(reflectivityIndex).size();
    ray.fields.at(index).assign(
        count, static_cast<std::uint16_t>(std::lround(correlationCoefficient * 100.0)));
    ray.firstGateByProduct.at(index) = ray.firstGateByProduct.at(reflectivityIndex);
    ray.gateSpacingByProduct.at(index) = ray.gateSpacingByProduct.at(reflectivityIndex);
    ray.scaleByProduct.at(index) = 100.0F;
    ray.offsetByProduct.at(index) = 0.0F;
}

// Lowest tilt at 0.5 deg over ground 10-25 km (gateCount 16) plus a 30 deg tilt
// that samples the same ground range up to ~60 km, so the column guard can
// actually look above 3 km. Radar altitude is the caller's problem.
DecodedChunk rainVolume(const std::string& radarId, const double reflectivityDbz,
                        const std::size_t gateCount = 16, const double topReflectivityDbz = 40.0,
                        const bool echo = true) {
    DecodedChunk chunk;
    chunk.radarId = radarId;
    chunk.rays.push_back(
        syntheticRainRay(90.0F, 1, 0.5F, 10000, 1000, gateCount, reflectivityDbz, echo));
    chunk.rays.push_back(
        syntheticRainRay(90.0F, 2, 30.0F, 10000, 1000, 60, topReflectivityDbz, true));
    return chunk;
}

void testZrRateRelation() {
    // Marshall-Palmer Z = 300 R^1.4: R(30) = 2.363 mm/h, R(40) = 12.240 mm/h.
    const auto at30 = zrRateMillimetersPerHour(30.0);
    const auto at40 = zrRateMillimetersPerHour(40.0);
    assert(at30 && std::abs(*at30 - 2.363115) < 1e-4);
    assert(at40 && std::abs(*at40 - 12.239693) < 1e-4);
    // 10 dB is a factor 5.18 in rate for b = 1.4, not the 10 of a linear Z-R.
    assert(std::abs(*at40 / *at30 - 5.179) < 1e-3);
    // Convective variant is wetter for the same Z.
    const auto convective = zrRateMillimetersPerHour(40.0, 300.0, 1.5);
    assert(convective && *convective < *at40);
    assert(!zrRateMillimetersPerHour(std::numeric_limits<double>::quiet_NaN()));
    assert(!zrRateMillimetersPerHour(30.0, 0.0, 1.4));
    assert(!zrRateMillimetersPerHour(30.0, 300.0, 0.0));
}

void testRainfallAccumulationClosedForm() {
    // Four volumes 5 min apart, uniform 40 dBZ, echo to 30 deg. Three credited
    // intervals of 300 s: 3 * 12.2397 mm/h * (300/3600) h = 3.06 mm.
    const auto chunk = rainVolume("KTLX", 40.0);
    const std::vector<RainfallSample> samples{
        {&chunk, 0.0}, {&chunk, 300.0}, {&chunk, 600.0}, {&chunk, 900.0}};
    const auto grid = accumulateRainfall(samples, 370.0);
    assert(grid.summary.valid);
    assert(grid.summary.sampleCount == 4);
    assert(grid.summary.rejectedSampleCount == 0);
    assert(grid.summary.intervalCount == 3);
    assert(std::abs(grid.summary.coveredSeconds - 900.0) < 1e-6);
    assert(std::abs(grid.summary.windowSeconds - 900.0) < 1e-6);
    assert(grid.rays.size() == 1);
    assert(grid.rays.front().gates.size() == 16);
    assert(grid.summary.depthGateCount == 16);
    // 3 * 12.239693 * 300/360 = 30.599 tenths -> 31.
    for (const auto& gate : grid.rays.front().gates) {
        assert(gate.depthTenthsMillimeters == 31);
    }
    assert(std::abs(grid.summary.maximumMillimeters - 3.1) < 1e-9);
    assert(std::abs(grid.summary.meanMillimeters - 3.1) < 1e-9);
    assert(grid.summary.haveMaximum);
    assert(grid.summary.maximumGateIndex >= 0);
    assert(grid.summary.quality == RainfallQuality::Good);
    // Ordering is internal: the same samples reversed must give the same total.
    const std::vector<RainfallSample> reversed{samples.rbegin(), samples.rend()};
    const auto same = accumulateRainfall(reversed, 370.0);
    assert(same.rays.front().gates.front().depthTenthsMillimeters == 31);
}

void testRainfallClutterGuard() {
    // 45 dBZ on the lowest tilt with nothing above it: ground clutter or AP.
    // The volume did look above 3 km, so "no echo up there" is a measurement.
    DecodedChunk clutter;
    clutter.radarId = "KTLX";
    clutter.rays.push_back(syntheticRainRay(90.0F, 1, 0.5F, 10000, 1000, 16, 45.0));
    clutter.rays.push_back(syntheticRainRay(90.0F, 2, 30.0F, 10000, 1000, 60, 0.0, false));
    const std::vector<RainfallSample> clutterSamples{
        {&clutter, 0.0}, {&clutter, 300.0}, {&clutter, 600.0}, {&clutter, 900.0}};
    const auto guarded = accumulateRainfall(clutterSamples, 370.0);
    assert(guarded.summary.valid);
    for (const auto& gate : guarded.rays.front().gates) {
        assert(gate.depthTenthsMillimeters == 0);
        assert((gate.flags & RainfallGateClutterSuspect) != 0);
        assert((gate.flags & RainfallGateNonMeteorological) == 0);
    }
    assert(guarded.summary.clutterSuspectGateCount == 16);
    assert(guarded.summary.depthGateCount == 0);
    // Excluding every candidate gate is not a usable total.
    assert(guarded.summary.quality == RainfallQuality::Poor);

    // Same reflectivity with a real column above it: keep the rain.
    const auto storm = rainVolume("KTLX", 45.0);
    const std::vector<RainfallSample> stormSamples{
        {&storm, 0.0}, {&storm, 300.0}, {&storm, 600.0}, {&storm, 900.0}};
    const auto kept = accumulateRainfall(stormSamples, 370.0);
    assert(kept.summary.clutterSuspectGateCount == 0);
    // R(45) * 3 * 300 s = 69.6 tenths.
    assert(kept.rays.front().gates.front().depthTenthsMillimeters == 70);
    assert(kept.summary.quality == RainfallQuality::Good);

    // Single-tilt volume: the vertical guard cannot run. The depth survives but
    // the gate says so and the product cannot claim Good.
    DecodedChunk blind;
    blind.radarId = "KTLX";
    blind.rays.push_back(syntheticRainRay(90.0F, 1, 0.5F, 10000, 1000, 16, 45.0));
    const std::vector<RainfallSample> blindSamples{
        {&blind, 0.0}, {&blind, 300.0}, {&blind, 600.0}, {&blind, 900.0}};
    const auto unchecked = accumulateRainfall(blindSamples, 370.0);
    assert(unchecked.summary.noVerticalCheckGateCount == 16);
    assert(unchecked.summary.clutterSuspectGateCount == 0);
    assert(unchecked.rays.front().gates.front().depthTenthsMillimeters == 70);
    assert(unchecked.summary.quality == RainfallQuality::Marginal);
}

void testRainfallDualPolVeto() {
    // 24 gates over ground 10-33 km, CC 0.98 everywhere except gate 3 (0.55).
    DecodedChunk mixed = rainVolume("KTLX", 45.0, 24);
    attachRainCorrelation(mixed.rays.front(), 0.98);
    mixed.rays.front()
        .fields.at(static_cast<std::size_t>(ProductKind::CorrelationCoefficient))
        .at(3) = static_cast<std::uint16_t>(55);
    const std::vector<RainfallSample> samples{
        {&mixed, 0.0}, {&mixed, 300.0}, {&mixed, 600.0}, {&mixed, 900.0}};
    const auto grid = accumulateRainfall(samples, 370.0);
    assert(grid.summary.valid);
    assert(grid.summary.nonMeteorologicalGateCount == 1);
    assert(grid.rays.front().gates.at(3).depthTenthsMillimeters == 0);
    assert((grid.rays.front().gates.at(3).flags & RainfallGateNonMeteorological) != 0);
    assert(grid.rays.front().gates.at(2).depthTenthsMillimeters == 70);
    assert(grid.rays.front().gates.at(4).depthTenthsMillimeters == 70);
    // One exclusion in 24 candidates is 4.2%: below the Poor threshold.
    assert(grid.summary.quality == RainfallQuality::Good);
}

void testRainfallBeamHeightGuard() {
    // 1-230 km on the lowest tilt, 30 dBZ everywhere. Gates whose beam climbs
    // above 4 km cannot be rain; between 2 and 4 km the depth counts but the
    // gate is flagged, because a 4 km beam is not a surface rain gauge.
    DecodedChunk chunk;
    chunk.radarId = "KTLX";
    chunk.rays.push_back(syntheticRainRay(45.0F, 1, 0.5F, 1000, 1000, 230, 30.0));
    const std::vector<RainfallSample> samples{
        {&chunk, 0.0}, {&chunk, 300.0}, {&chunk, 600.0}, {&chunk, 900.0}};
    const auto grid = accumulateRainfall(samples, 370.0);
    assert(grid.summary.valid);
    assert(grid.rays.size() == 1);
    const auto& gates = grid.rays.front().gates;
    assert(gates.size() == 230);
    std::size_t contributing = 0;
    std::size_t flagged = 0;
    for (std::size_t gate = 0; gate < gates.size(); ++gate) {
        const auto slant = gateCenterSlantRange(1000, 1000, gate);
        const auto heightMsl =
            standardBeamGeometry(slant, 0.5).heightAboveRadarMeters + 370.0;
        const bool contributes = heightMsl <= 4000.0;
        const bool flagExpected = heightMsl > 2000.0;
        if (contributes) ++contributing;
        if (flagExpected) ++flagged;
        assert((gates[gate].depthTenthsMillimeters > 0) == contributes);
        assert(((gates[gate].flags & RainfallGateBeamHigh) != 0) == flagExpected);
        assert((gates[gate].flags & RainfallGateRangeLimited) == 0);
        if (contributes) {
            // R(30) * 3 * 300 s = 5.9 tenths.
            assert(gates[gate].depthTenthsMillimeters == 6);
        }
    }
    assert(contributing > 100);
    assert(flagged > 0);
    assert(grid.summary.depthGateCount == contributing);
    assert(grid.summary.beamHighGateCount == flagged);
    // The far edge of a 230 km ray is above 4 km: no depth, flagged.
    assert(gates.back().depthTenthsMillimeters == 0);
    assert((gates.back().flags & RainfallGateBeamHigh) != 0);
}

void testRainfallGapClamping() {
    // Two volumes 20 min apart: volumes are missing in between. Crediting the
    // whole gap at the older rate would double the depth.
    const auto chunk = rainVolume("KTLX", 40.0);
    const std::vector<RainfallSample> samples{{&chunk, 0.0}, {&chunk, 1200.0}};
    const auto grid = accumulateRainfall(samples, 370.0);
    assert(grid.summary.valid);
    assert(std::abs(grid.summary.windowSeconds - 1200.0) < 1e-6);
    assert(std::abs(grid.summary.coveredSeconds - 600.0) < 1e-6);
    assert(grid.summary.gapClampedIntervalCount == 1);
    assert(grid.summary.quality == RainfallQuality::Marginal);
    // R(40) * 600 s = 20.4 tenths, not the 40.8 an unclamped gap would invent.
    assert(grid.rays.front().gates.front().depthTenthsMillimeters == 20);
    assert((grid.rays.front().gates.front().flags & RainfallGateGapClamped) != 0);
    assert(grid.summary.gapClampedGateCount == 16);

    // A closing clock only helps inside the same cap: 600 s clamped plus a
    // real 300 s trailing interval = 900 s credited, 30.6 tenths.
    const auto trailing = accumulateRainfall(samples, 370.0, 1500.0);
    assert(trailing.summary.gapClampedIntervalCount == 1);
    assert(std::abs(trailing.summary.coveredSeconds - 900.0) < 1e-6);
    assert(trailing.rays.front().gates.front().depthTenthsMillimeters == 31);
}

void testRainfallGeometryRemap() {
    // Source volumes use 500 m gates, the newest uses 1000 m: the accumulation
    // must follow ground range, not gate index, and the newest volume owns the
    // output grid.
    DecodedChunk fine;
    fine.radarId = "KTLX";
    fine.rays.push_back(syntheticRainRay(90.0F, 1, 0.5F, 10000, 500, 31, 40.0));
    fine.rays.push_back(syntheticRainRay(90.0F, 2, 30.0F, 10000, 500, 120, 40.0));
    const auto coarse = rainVolume("KTLX", 40.0);
    const std::vector<RainfallSample> samples{
        {&fine, 0.0}, {&fine, 300.0}, {&fine, 600.0}, {&coarse, 900.0}};
    const auto grid = accumulateRainfall(samples, 370.0);
    assert(grid.summary.valid);
    assert(grid.firstGateMeters == 10000);
    assert(grid.gateSpacingMeters == 1000);
    assert(grid.rays.front().gates.size() == 16);
    assert(grid.summary.depthGateCount == 16);
    assert(grid.summary.incompleteGateCount == 0);
    for (const auto& gate : grid.rays.front().gates) {
        assert(gate.depthTenthsMillimeters == 31);
        assert((gate.flags & RainfallGateIncomplete) == 0);
    }
}

void testRainfallIncompleteCoverage() {
    // The middle volume's lowest tilt stops at 17 km: the far gates lose one
    // interval and must say so instead of quietly reading as a full total.
    DecodedChunk truncated;
    truncated.radarId = "KTLX";
    truncated.rays.push_back(syntheticRainRay(90.0F, 1, 0.5F, 10000, 1000, 8, 40.0));
    truncated.rays.push_back(syntheticRainRay(90.0F, 2, 30.0F, 10000, 1000, 60, 40.0));
    const auto full = rainVolume("KTLX", 40.0);
    const std::vector<RainfallSample> samples{
        {&full, 0.0}, {&truncated, 300.0}, {&full, 600.0}, {&full, 900.0}};
    const auto grid = accumulateRainfall(samples, 370.0);
    assert(grid.summary.valid);
    assert(grid.summary.incompleteGateCount == 8);
    assert(grid.rays.front().gates.at(0).depthTenthsMillimeters == 31);
    assert(grid.rays.front().gates.at(15).depthTenthsMillimeters == 20);
    assert((grid.rays.front().gates.at(15).flags & RainfallGateIncomplete) != 0);
    assert(grid.summary.quality == RainfallQuality::Marginal);
}

void testRainfallSentinelConvention() {
    // qa file, touched deliberately (card t_965bd7e5, science): the
    // accumulator now consumes DecodedChunk::momentEncoding. Same 16-gate
    // 40 dBZ volume as the closed-form case above; only gate 5 carries a
    // sentinel code. NEXRAD Message 31 packs 0 = below threshold and
    // 1 = range folded / missing; ODIM packs 0 = missing and 1 = below
    // threshold. A missing gate is a coverage gap and must be flagged; a
    // below-threshold gate was measured and stayed dry; a chunk that records no
    // convention (legacy spill file) cannot claim either.
    const auto volumeWithSentinel = [](const std::uint16_t sentinelCode,
                                       const MomentEncoding encoding) {
        auto chunk = rainVolume("KTLX", 40.0);
        chunk.momentEncoding = encoding;
        chunk.rays.front()
            .fields.at(static_cast<std::size_t>(ProductKind::Reflectivity))
            .at(5) = sentinelCode;
        return chunk;
    };
    constexpr auto nexrad = MomentEncoding::NexradMessage31;
    constexpr auto odim = MomentEncoding::Odim;
    constexpr auto unknown = MomentEncoding::Unknown;

    {
        // Gate 5 is range folded / missing in the older volume (NEXRAD code 1)
        // and below threshold in the newest (code 0). Both credit zero depth;
        // only the missing one is a coverage gap. Gate 4 was measured in both:
        // one 300 s interval at R(40) = 12.2397 mm/h = 1.02 mm -> 10 tenths.
        const auto missing = volumeWithSentinel(1, nexrad);
        const auto dry = volumeWithSentinel(0, nexrad);
        const std::vector<RainfallSample> samples{{&missing, 0.0}, {&dry, 300.0}};
        const auto grid = accumulateRainfall(samples, 370.0);
        assert(grid.summary.valid);
        assert(grid.summary.incompleteGateCount == 1);
        assert(grid.rays.front().gates.at(5).depthTenthsMillimeters == 0);
        assert((grid.rays.front().gates.at(5).flags & RainfallGateIncomplete) != 0);
        assert(grid.rays.front().gates.at(4).depthTenthsMillimeters == 10);
        assert((grid.rays.front().gates.at(4).flags & RainfallGateIncomplete) == 0);
    }
    {
        // The same gate below threshold in every volume: measured, no echo. Zero
        // depth and no coverage claim. Four volumes keep the volume-count rule
        // out of the way, so quality is Good and only a dry gate can be the
        // reason. Three intervals at R(40) -> 30.599 tenths -> 31 elsewhere.
        const auto dry = volumeWithSentinel(0, nexrad);
        const std::vector<RainfallSample> samples{
            {&dry, 0.0}, {&dry, 300.0}, {&dry, 600.0}, {&dry, 900.0}};
        const auto grid = accumulateRainfall(samples, 370.0);
        assert(grid.summary.valid);
        assert(grid.summary.incompleteGateCount == 0);
        assert(grid.rays.front().gates.at(5).depthTenthsMillimeters == 0);
        assert((grid.rays.front().gates.at(5).flags & RainfallGateIncomplete) == 0);
        assert(grid.rays.front().gates.at(4).depthTenthsMillimeters == 31);
        assert(grid.summary.quality == RainfallQuality::Good);
    }
    {
        // ODIM swaps the codes: 0 is the missing gate, 1 is below threshold. If
        // the accumulator ignored the recorded convention the two cases would
        // come out the other way round. Encoding only decides the sentinel here;
        // scale/offset stay per ray.
        const auto odimMissing = volumeWithSentinel(0, odim);
        const auto odimDry = volumeWithSentinel(1, odim);
        const std::vector<RainfallSample> samples{
            {&odimMissing, 0.0}, {&odimDry, 300.0}, {&odimDry, 600.0}, {&odimDry, 900.0}};
        const auto grid = accumulateRainfall(samples, 370.0);
        assert(grid.summary.valid);
        assert(grid.summary.incompleteGateCount == 1);
        assert(grid.rays.front().gates.at(5).depthTenthsMillimeters == 0);
        assert((grid.rays.front().gates.at(5).flags & RainfallGateIncomplete) != 0);
        assert(grid.rays.front().gates.at(4).depthTenthsMillimeters == 31);
        assert(grid.summary.quality == RainfallQuality::Marginal);
    }
    {
        // A chunk with no recorded convention (legacy spill file) cannot tell
        // the codes apart: keep the old reading, zero depth and no claim.
        const auto legacy = volumeWithSentinel(1, unknown);
        const std::vector<RainfallSample> samples{
            {&legacy, 0.0}, {&legacy, 300.0}, {&legacy, 600.0}, {&legacy, 900.0}};
        const auto grid = accumulateRainfall(samples, 370.0);
        assert(grid.summary.valid);
        assert(grid.summary.incompleteGateCount == 0);
        assert(grid.rays.front().gates.at(5).depthTenthsMillimeters == 0);
        assert((grid.rays.front().gates.at(5).flags & RainfallGateIncomplete) == 0);
        assert(grid.rays.front().gates.at(4).depthTenthsMillimeters == 31);
        assert(grid.summary.quality == RainfallQuality::Good);
    }
}

void testRainfallInvalidWindows() {
    {
        const auto grid = accumulateRainfall({}, 370.0);
        assert(!grid.summary.valid);
        assert(grid.summary.quality == RainfallQuality::Invalid);
        assert(grid.rays.empty());
    }
    {
        const auto chunk = rainVolume("KTLX", 40.0);
        const std::vector<RainfallSample> samples{{&chunk, 0.0}};
        const auto grid = accumulateRainfall(samples, 370.0);
        assert(!grid.summary.valid);
        assert(grid.summary.sampleCount == 1);
        assert(grid.rays.empty());
    }
    {
        // Duplicate and null samples leave no interval at all.
        const auto chunk = rainVolume("KTLX", 40.0);
        const std::vector<RainfallSample> samples{
            {&chunk, 500.0}, {&chunk, 500.0}, {nullptr, 600.0}};
        const auto grid = accumulateRainfall(samples, 370.0);
        assert(!grid.summary.valid);
        assert(grid.summary.sampleCount == 1);
        assert(grid.summary.rejectedSampleCount == 2);
    }
    {
        // Rain below the 5 dBZ floor is measured, not missing: zero depth and
        // no Incomplete flag.
        const auto chunk = rainVolume("KTLX", 3.0);
        const std::vector<RainfallSample> samples{
            {&chunk, 0.0}, {&chunk, 300.0}, {&chunk, 600.0}, {&chunk, 900.0}};
        const auto grid = accumulateRainfall(samples, 370.0);
        assert(grid.summary.valid);
        assert(grid.summary.depthGateCount == 0);
        assert(grid.summary.incompleteGateCount == 0);
        assert(std::abs(grid.summary.maximumMillimeters) < 1e-9);
        assert(grid.summary.quality == RainfallQuality::Good);
    }
    {
        // Code 1 is "missing" in one format and "below threshold" in the other,
        // and the chunk does not say which. It must never become invented rain,
        // and it must not be claimed as a coverage gap at that gate either.
        auto chunk = rainVolume("KTLX", 40.0);
        chunk.rays.front()
            .fields.at(static_cast<std::size_t>(ProductKind::Reflectivity))
            .at(5) = 1;
        const std::vector<RainfallSample> samples{
            {&chunk, 0.0}, {&chunk, 300.0}, {&chunk, 600.0}, {&chunk, 900.0}};
        const auto grid = accumulateRainfall(samples, 370.0);
        assert(grid.rays.front().gates.at(5).depthTenthsMillimeters == 0);
        assert((grid.rays.front().gates.at(5).flags & RainfallGateIncomplete) == 0);
        assert(grid.rays.front().gates.at(4).depthTenthsMillimeters == 31);
        assert(grid.summary.incompleteGateCount == 0);
    }
    {
        // A relation or interval cap that cannot produce a number fails closed
        // instead of emitting zeros that look like "no rain".
        const auto chunk = rainVolume("KTLX", 40.0);
        const std::vector<RainfallSample> samples{
            {&chunk, 0.0}, {&chunk, 300.0}, {&chunk, 600.0}, {&chunk, 900.0}};
        RainfallConfig broken;
        broken.zrA = 0.0;
        const auto grid = accumulateRainfall(samples, 370.0,
                                             std::numeric_limits<double>::quiet_NaN(), broken);
        assert(!grid.summary.valid);
        assert(grid.summary.quality == RainfallQuality::Invalid);
        assert(grid.rays.empty());
    }
}

void testRainfallPartialReferenceRays() {
    // Rays inside one reference tilt need not share a gate count. The map for
    // each ray must follow that ray's own length; a short first ray must not
    // truncate the long one.
    DecodedChunk chunk;
    chunk.radarId = "KTLX";
    chunk.rays.push_back(syntheticRainRay(90.0F, 1, 0.5F, 10000, 1000, 10, 40.0));
    chunk.rays.push_back(syntheticRainRay(91.0F, 1, 0.5F, 10000, 1000, 16, 40.0));
    chunk.rays.push_back(syntheticRainRay(90.0F, 2, 30.0F, 10000, 1000, 60, 40.0));
    chunk.rays.push_back(syntheticRainRay(91.0F, 2, 30.0F, 10000, 1000, 60, 40.0));
    const std::vector<RainfallSample> samples{
        {&chunk, 0.0}, {&chunk, 300.0}, {&chunk, 600.0}, {&chunk, 900.0}};
    const auto grid = accumulateRainfall(samples, 370.0);
    assert(grid.summary.valid);
    assert(grid.rays.size() == 2);
    assert(grid.rays.at(0).gates.size() == 10);
    assert(grid.rays.at(1).gates.size() == 16);
    for (const auto& ray : grid.rays) {
        for (const auto& gate : ray.gates) {
            assert(gate.depthTenthsMillimeters == 31);
            assert((gate.flags & RainfallGateIncomplete) == 0);
        }
    }
    assert(grid.summary.depthGateCount == 26);
}

// ---------------------------------------------------------------------------
// Dual-pol rain-rate relations beside the Z-R path
// ---------------------------------------------------------------------------

// ZDR on the WSR-88D Message 31 packing (scale 16, offset 129), attached to the
// same gate count/geometry as reflectivity so the relation can read it.
void attachRainDifferentialReflectivity(DecodedRay& ray,
                                        const double differentialReflectivityDb) {
    const auto index = static_cast<std::size_t>(ProductKind::DifferentialReflectivity);
    const auto reflectivityIndex = static_cast<std::size_t>(ProductKind::Reflectivity);
    const auto count = ray.fields.at(reflectivityIndex).size();
    const auto encoded = static_cast<std::uint16_t>(std::clamp<long>(
        std::lround(differentialReflectivityDb * 16.0 + 129.0), 2, 65535));
    ray.fields.at(index).assign(count, encoded);
    ray.firstGateByProduct.at(index) = ray.firstGateByProduct.at(reflectivityIndex);
    ray.gateSpacingByProduct.at(index) = ray.gateSpacingByProduct.at(reflectivityIndex);
    ray.scaleByProduct.at(index) = 16.0F;
    ray.offsetByProduct.at(index) = 129.0F;
}

// KDP on the ODIM 16-bit packing the reader emits (scale 1023.9375,
// offset 32768), attached to the reflectivity geometry.
void attachRainSpecificDifferentialPhase(DecodedRay& ray, const double kdpDegreesPerKm) {
    const auto index = static_cast<std::size_t>(ProductKind::SpecificDifferentialPhase);
    const auto reflectivityIndex = static_cast<std::size_t>(ProductKind::Reflectivity);
    const auto count = ray.fields.at(reflectivityIndex).size();
    const auto encoded = static_cast<std::uint16_t>(std::clamp<long>(
        std::lround(kdpDegreesPerKm * 1023.9375 + 32768.0), 2, 65534));
    ray.fields.at(index).assign(count, encoded);
    ray.firstGateByProduct.at(index) = ray.firstGateByProduct.at(reflectivityIndex);
    ray.gateSpacingByProduct.at(index) = ray.gateSpacingByProduct.at(reflectivityIndex);
    ray.scaleByProduct.at(index) = 1023.9375F;
    ray.offsetByProduct.at(index) = 32768.0F;
}

void testDualPolRateRelations() {
    // R(Z,ZDR) = 0.0142 * Z^0.770 * 10^(-0.167 ZDR), Z in mm^6/m^3.
    // 40 dBZ -> Z = 10000; 10000^0.770 = 1202.264; 10^-0.167 = 0.680769:
    // R = 0.0142 * 1202.264 * 0.680769 = 11.6222 mm/h.
    const auto at40 = zzdrRateMillimetersPerHour(40.0, 1.0);
    assert(at40 && std::abs(*at40 - 11.6222) < 1e-3);
    // At the same Z a larger drop (higher ZDR) gives a lower rate.
    const auto at43 = zzdrRateMillimetersPerHour(40.0, 3.0);
    assert(at43 && std::abs(*at43 - 5.38627) < 1e-3 && *at43 < *at40);
    // R(KDP) = 44 |KDP|^0.822 sign(KDP): KDP = 1 -> exactly 44 mm/h,
    // KDP = 2 -> 44 * 2^0.822 = 77.7856 mm/h.
    const auto kdp1 = kdpRateMillimetersPerHour(1.0);
    assert(kdp1 && std::abs(*kdp1 - 44.0) < 1e-12);
    const auto kdp2 = kdpRateMillimetersPerHour(2.0);
    assert(kdp2 && std::abs(*kdp2 - 77.7856) < 1e-3);
    const auto kdpNegative = kdpRateMillimetersPerHour(-2.0);
    assert(kdpNegative && std::abs(*kdpNegative + 77.7856) < 1e-3);
    // Unusable constants or inputs fail closed instead of inventing a rate.
    assert(!zzdrRateMillimetersPerHour(std::numeric_limits<double>::quiet_NaN(), 1.0));
    assert(!zzdrRateMillimetersPerHour(40.0, std::numeric_limits<double>::quiet_NaN()));
    assert(!zzdrRateMillimetersPerHour(40.0, 1.0, 0.0, 0.770, 1.67));
    assert(!zzdrRateMillimetersPerHour(40.0, 1.0, 0.0142, 0.0, 1.67));
    assert(!zzdrRateMillimetersPerHour(40.0, 1.0, 0.0142, 0.770, -1.0));
    assert(!kdpRateMillimetersPerHour(std::numeric_limits<double>::quiet_NaN()));
    assert(!kdpRateMillimetersPerHour(1.0, 0.0, 0.822));
    assert(!kdpRateMillimetersPerHour(1.0, 44.0, 0.0));
}

void testRainfallZdrAccumulationClosedForm() {
    // Uniform 40 dBZ and +1.0 dB ZDR: R = 11.6222 mm/h. Three 300 s intervals
    // credit 11.6222 * 0.25 h = 2.9056 mm -> 29 tenths per gate.
    auto chunk = rainVolume("KTLX", 40.0);
    chunk.momentEncoding = MomentEncoding::NexradMessage31;
    attachRainCorrelation(chunk.rays.front(), 0.98);
    attachRainDifferentialReflectivity(chunk.rays.front(), 1.0);
    const std::vector<RainfallSample> samples{
        {&chunk, 0.0}, {&chunk, 300.0}, {&chunk, 600.0}, {&chunk, 900.0}};
    RainfallConfig config;
    config.relation = RainfallRelation::Zdr;
    const auto grid = accumulateRainfall(samples, 370.0,
                                         std::numeric_limits<double>::quiet_NaN(), config);
    assert(grid.summary.valid);
    assert(grid.summary.relationAvailable);
    assert(grid.summary.relationUnavailableReason.empty());
    assert(grid.summary.relation == RainfallRelation::Zdr);
    assert(grid.rays.size() == 1);
    assert(grid.rays.front().gates.size() == 16);
    for (const auto& gate : grid.rays.front().gates) {
        assert(gate.depthTenthsMillimeters == 29);
        assert((gate.flags & RainfallGateIncomplete) == 0);
        assert((gate.flags & RainfallGateInputClamped) == 0);
        assert((gate.flags & RainfallGateRateCapped) == 0);
    }
    assert(std::abs(grid.summary.maximumMillimeters - 2.9) < 1e-9);
    assert(grid.summary.quality == RainfallQuality::Good);
    // The Z-R path on the same volume is untouched: 3.06 mm -> 31 tenths.
    const auto zr = accumulateRainfall(samples, 370.0);
    assert(zr.rays.front().gates.front().depthTenthsMillimeters == 31);
}

void testRainfallKdpAccumulationClosedForm() {
    // Reflectivity is guard-only for R(KDP); KDP = 1.0 deg/km -> 44 mm/h.
    // Three 300 s intervals: 44 * 0.25 h = 11.0 mm -> 110 tenths per gate.
    auto chunk = rainVolume("KTLX", 40.0);
    chunk.momentEncoding = MomentEncoding::Odim;
    attachRainSpecificDifferentialPhase(chunk.rays.front(), 1.0);
    const std::vector<RainfallSample> samples{
        {&chunk, 0.0}, {&chunk, 300.0}, {&chunk, 600.0}, {&chunk, 900.0}};
    RainfallConfig config;
    config.relation = RainfallRelation::Kdp;
    const auto grid = accumulateRainfall(samples, 370.0,
                                         std::numeric_limits<double>::quiet_NaN(), config);
    assert(grid.summary.valid);
    assert(grid.summary.relationAvailable);
    assert(grid.summary.relation == RainfallRelation::Kdp);
    for (const auto& gate : grid.rays.front().gates) {
        assert(gate.depthTenthsMillimeters == 110);
        assert((gate.flags & RainfallGateIncomplete) == 0);
        assert((gate.flags & RainfallGateInputClamped) == 0);
        assert((gate.flags & RainfallGateRateCapped) == 0);
    }
    assert(std::abs(grid.summary.maximumMillimeters - 11.0) < 1e-9);
    assert(grid.summary.quality == RainfallQuality::Good);

    // Below the noise floor and negative KDP are measured no-rain gates: zero
    // depth, no coverage gap, no clamp.
    const auto quietVolume = [](const double kdp) {
        auto quiet = rainVolume("KTLX", 40.0);
        quiet.momentEncoding = MomentEncoding::Odim;
        attachRainSpecificDifferentialPhase(quiet.rays.front(), kdp);
        return quiet;
    };
    for (const double kdp : {0.05, -1.0}) {
        const auto quiet = quietVolume(kdp);
        const std::vector<RainfallSample> quietSamples{
            {&quiet, 0.0}, {&quiet, 300.0}, {&quiet, 600.0}, {&quiet, 900.0}};
        const auto quietGrid = accumulateRainfall(
            quietSamples, 370.0, std::numeric_limits<double>::quiet_NaN(), config);
        assert(quietGrid.summary.valid);
        assert(quietGrid.summary.depthGateCount == 0);
        assert(quietGrid.summary.incompleteGateCount == 0);
        assert(quietGrid.summary.inputClampedGateCount == 0);
    }

    // Reflectivity's own sentinel under R(KDP). KDP is the rate input, so a
    // missing or unclassified Z keeps the KDP depth while the column guard is
    // blind; a measured dry Z still wins the veto. These are the rows the
    // validation record's guard table now states.
    const auto reflectivityIndex = static_cast<std::size_t>(ProductKind::Reflectivity);
    const auto withReflectivityCode = [&](const MomentEncoding encoding,
                                          const std::uint16_t code) {
        auto chunk = quietVolume(1.0);  // KDP = 1.0 deg/km -> 110 tenths
        chunk.momentEncoding = encoding;
        chunk.rays.front().fields.at(reflectivityIndex).assign(16, code);
        return chunk;
    };
    const auto accumulateWith = [&](const DecodedChunk& chunk) {
        const std::vector<RainfallSample> window{
            {&chunk, 0.0}, {&chunk, 300.0}, {&chunk, 600.0}, {&chunk, 900.0}};
        return accumulateRainfall(window, 370.0,
                                  std::numeric_limits<double>::quiet_NaN(), config);
    };
    {
        // Message 31: 1 = range folded (missing). The KDP rate survives, the
        // guard cannot run, and the gate is still a reflectivity gap.
        const auto missing = withReflectivityCode(MomentEncoding::NexradMessage31, 1);
        const auto grid = accumulateWith(missing);
        assert(grid.summary.valid);
        assert(grid.summary.depthGateCount == 16);
        assert(grid.summary.noVerticalCheckGateCount == 16);
        assert(grid.summary.incompleteGateCount == 16);
        for (const auto& gate : grid.rays.front().gates) {
            assert(gate.depthTenthsMillimeters == 110);
            assert((gate.flags & RainfallGateNoVerticalCheck) != 0);
            assert((gate.flags & RainfallGateIncomplete) != 0);
        }
        assert(grid.summary.quality == RainfallQuality::Marginal);
    }
    {
        // A legacy chunk records no convention: same depth and guard flag, but
        // the missing/absent distinction cannot be claimed.
        const auto legacy = withReflectivityCode(MomentEncoding::Unknown, 1);
        const auto grid = accumulateWith(legacy);
        assert(grid.summary.valid);
        assert(grid.summary.depthGateCount == 16);
        assert(grid.summary.noVerticalCheckGateCount == 16);
        assert(grid.summary.incompleteGateCount == 0);
        for (const auto& gate : grid.rays.front().gates) {
            assert(gate.depthTenthsMillimeters == 110);
            assert((gate.flags & RainfallGateNoVerticalCheck) != 0);
            assert((gate.flags & RainfallGateIncomplete) == 0);
        }
    }
    {
        // Message 31: 0 = below threshold. A measured dry gate, so KDP cannot
        // mint rain there: zero depth, no flag.
        const auto dry = withReflectivityCode(MomentEncoding::NexradMessage31, 0);
        const auto grid = accumulateWith(dry);
        assert(grid.summary.valid);
        assert(grid.summary.depthGateCount == 0);
        assert(grid.summary.noVerticalCheckGateCount == 0);
        assert(grid.summary.incompleteGateCount == 0);
        for (const auto& gate : grid.rays.front().gates) {
            assert(gate.depthTenthsMillimeters == 0);
            assert((gate.flags & RainfallGateNoVerticalCheck) == 0);
            assert((gate.flags & RainfallGateIncomplete) == 0);
        }
    }
}

void testRainfallDualPolClampFlags() {
    // The 150 mm/h rate ceiling is not KDP-only. At the 53 dBZ Z cap,
    // Z^0.770 = 12050, so R(Z,ZDR) = 0.0142 * 12050 * 10^(-0.167 ZDR):
    // 171.1 mm/h at ZDR 0 dB and 251.4 mm/h at the -1 dB ZDR clamp. A hail
    // core with near-neutral ZDR therefore hits RateCapped, and that ceiling —
    // not the 53 dBZ cap — is what bounds it.
    const auto configFor = [](const RainfallRelation relation) {
        RainfallConfig config;
        config.relation = relation;
        return config;
    };
    const auto fourSamples = [](const DecodedChunk& chunk) {
        return std::vector<RainfallSample>{
            {&chunk, 0.0}, {&chunk, 300.0}, {&chunk, 600.0}, {&chunk, 900.0}};
    };
    const auto accumulate = [&](const DecodedChunk& chunk, const RainfallRelation relation) {
        return accumulateRainfall(fourSamples(chunk), 370.0,
                                  std::numeric_limits<double>::quiet_NaN(),
                                  configFor(relation));
    };

    // 55 dBZ + ZDR 0 dB: Z capped to 53 -> 171.1 mm/h -> ceiling 150 mm/h ->
    // 150 * 0.25 h = 37.5 mm = 375 tenths per gate.
    auto hail = rainVolume("KTLX", 55.0);
    hail.momentEncoding = MomentEncoding::NexradMessage31;
    attachRainCorrelation(hail.rays.front(), 0.98);
    attachRainDifferentialReflectivity(hail.rays.front(), 0.0);
    const auto hailGrid = accumulate(hail, RainfallRelation::Zdr);
    assert(hailGrid.summary.valid);
    assert(hailGrid.summary.relationAvailable);
    assert(hailGrid.summary.hailCappedGateCount == 16);
    assert(hailGrid.summary.rateCappedGateCount == 16);
    assert(hailGrid.summary.inputClampedGateCount == 0);
    for (const auto& gate : hailGrid.rays.front().gates) {
        assert(gate.depthTenthsMillimeters == 375);
        assert((gate.flags & RainfallGateHailCapped) != 0);
        assert((gate.flags & RainfallGateRateCapped) != 0);
        assert((gate.flags & RainfallGateInputClamped) == 0);
    }
    assert(std::abs(hailGrid.summary.maximumMillimeters - 37.5) < 1e-9);

    // 40 dBZ + ZDR -5 dB: clamped to the -1 dB edge -> 25.0777 mm/h ->
    // 25.0777 * 0.25 h = 6.2694 mm = 63 tenths per gate. Below the ceiling.
    auto subRangeZdr = rainVolume("KTLX", 40.0);
    subRangeZdr.momentEncoding = MomentEncoding::NexradMessage31;
    attachRainCorrelation(subRangeZdr.rays.front(), 0.98);
    attachRainDifferentialReflectivity(subRangeZdr.rays.front(), -5.0);
    const auto subRangeGrid = accumulate(subRangeZdr, RainfallRelation::Zdr);
    assert(subRangeGrid.summary.valid);
    assert(subRangeGrid.summary.inputClampedGateCount == 16);
    assert(subRangeGrid.summary.rateCappedGateCount == 0);
    for (const auto& gate : subRangeGrid.rays.front().gates) {
        assert(gate.depthTenthsMillimeters == 63);
        assert((gate.flags & RainfallGateInputClamped) != 0);
        assert((gate.flags & RainfallGateRateCapped) == 0);
    }

    // 40 dBZ + KDP 25 deg/km: magnitude clamped to 20 -> 44 * 20^0.822 =
    // 516.3 mm/h -> ceiling 150 mm/h -> 375 tenths per gate, clamped and capped.
    auto kdpSpike = rainVolume("KTLX", 40.0);
    kdpSpike.momentEncoding = MomentEncoding::Odim;
    attachRainSpecificDifferentialPhase(kdpSpike.rays.front(), 25.0);
    const auto spikeGrid = accumulate(kdpSpike, RainfallRelation::Kdp);
    assert(spikeGrid.summary.valid);
    assert(spikeGrid.summary.relationAvailable);
    assert(spikeGrid.summary.inputClampedGateCount == 16);
    assert(spikeGrid.summary.rateCappedGateCount == 16);
    for (const auto& gate : spikeGrid.rays.front().gates) {
        assert(gate.depthTenthsMillimeters == 375);
        assert((gate.flags & RainfallGateInputClamped) != 0);
        assert((gate.flags & RainfallGateRateCapped) != 0);
    }
    assert(std::abs(spikeGrid.summary.maximumMillimeters - 37.5) < 1e-9);

    std::cout << "dual-pol clamps: R(Z,ZDR) hail ZDR=0dB depth="
              << hailGrid.rays.front().gates.front().depthTenthsMillimeters
              << " tenths hail_capped=" << hailGrid.summary.hailCappedGateCount
              << " rate_capped=" << hailGrid.summary.rateCappedGateCount
              << "; R(Z,ZDR) ZDR=-5dB depth="
              << subRangeGrid.rays.front().gates.front().depthTenthsMillimeters
              << " tenths input_clamped=" << subRangeGrid.summary.inputClampedGateCount
              << "; R(KDP) KDP=25deg/km depth="
              << spikeGrid.rays.front().gates.front().depthTenthsMillimeters
              << " tenths input_clamped=" << spikeGrid.summary.inputClampedGateCount
              << " rate_capped=" << spikeGrid.summary.rateCappedGateCount << "\n";
}

void testRainfallDualPolUnavailable() {
    // A single-pol chunk carries neither ZDR nor KDP. Both dual-pol relations
    // must report unavailability with the missing product named, and neither
    // may fall back to Z-R.
    const auto singlePol = rainVolume("KTLX", 40.0);
    const std::vector<RainfallSample> samples{
        {&singlePol, 0.0}, {&singlePol, 300.0}, {&singlePol, 600.0}, {&singlePol, 900.0}};
    for (const auto relation : {RainfallRelation::Zdr, RainfallRelation::Kdp}) {
        RainfallConfig config;
        config.relation = relation;
        const auto grid = accumulateRainfall(samples, 370.0,
                                             std::numeric_limits<double>::quiet_NaN(), config);
        assert(!grid.summary.valid);
        assert(grid.summary.quality == RainfallQuality::Invalid);
        assert(!grid.summary.relationAvailable);
        assert(grid.summary.relation == relation);
        const std::string expected = relation == RainfallRelation::Zdr
                                         ? "Differential reflectivity"
                                         : "Specific differential phase";
        assert(grid.summary.relationUnavailableReason.find(expected) != std::string::npos);
        assert(grid.rays.empty());
        // The window itself was fine: the failure is the relation's input.
        assert(grid.summary.sampleCount == 4);
    }
    // Z-R is still available on that same volume.
    const auto zr = accumulateRainfall(samples, 370.0);
    assert(zr.summary.valid);
    assert(zr.summary.relationAvailable);
    assert(zr.summary.relation == RainfallRelation::MarshallPalmer);
    assert(zr.rays.front().gates.front().depthTenthsMillimeters == 31);

    // A moment that exists only on a higher tilt cannot produce the reference
    // grid's product: unavailable, not an all-Incomplete "valid" grid.
    RainfallConfig zdrConfig;
    zdrConfig.relation = RainfallRelation::Zdr;
    DecodedChunk higherTiltOnly;
    higherTiltOnly.radarId = "KTLX";
    higherTiltOnly.rays.push_back(
        syntheticRainRay(90.0F, 1, 0.5F, 10000, 1000, 16, 40.0));
    auto upperTilt = syntheticRainRay(90.0F, 2, 30.0F, 10000, 1000, 60, 40.0);
    attachRainDifferentialReflectivity(upperTilt, 1.0);
    higherTiltOnly.rays.push_back(upperTilt);
    const std::vector<RainfallSample> higherSamples{
        {&higherTiltOnly, 0.0}, {&higherTiltOnly, 300.0},
        {&higherTiltOnly, 600.0}, {&higherTiltOnly, 900.0}};
    const auto higherGrid = accumulateRainfall(higherSamples, 370.0,
                                               std::numeric_limits<double>::quiet_NaN(),
                                               zdrConfig);
    assert(!higherGrid.summary.valid);
    assert(!higherGrid.summary.relationAvailable);
    assert(higherGrid.summary.relationUnavailableReason.find("lowest reflectivity tilt") !=
           std::string::npos);
    assert(higherGrid.rays.empty());

    // An older volume that lost ZDR is a coverage gap, never a Z-R substitute:
    // the newest volume carries ZDR, so the relation stays available and only
    // the 600-900 s interval is credited (2.9056 / 3 mm -> 10 tenths).
    auto withZdr = rainVolume("KTLX", 40.0);
    attachRainDifferentialReflectivity(withZdr.rays.front(), 1.0);
    const auto withoutZdr = rainVolume("KTLX", 40.0);
    const std::vector<RainfallSample> mixed{{&withoutZdr, 0.0}, {&withoutZdr, 300.0},
                                            {&withZdr, 600.0}, {&withZdr, 900.0}};
    const auto mixedGrid = accumulateRainfall(mixed, 370.0,
                                              std::numeric_limits<double>::quiet_NaN(),
                                              zdrConfig);
    assert(mixedGrid.summary.valid);
    assert(mixedGrid.summary.relationAvailable);
    assert(mixedGrid.summary.incompleteGateCount == 16);
    for (const auto& gate : mixedGrid.rays.front().gates) {
        assert(gate.depthTenthsMillimeters == 10);
        assert((gate.flags & RainfallGateIncomplete) != 0);
    }
    // 10 tenths, not the 31 a silent Z-R fallback would have produced.
    assert(mixedGrid.summary.quality == RainfallQuality::Marginal);
}

void testRainfallDualPolGuardParity() {
    // The AP/clutter guard, the gap clamp and the vertical-check accounting are
    // the same code for all three relations: same flags, same depth fate.
    const auto withDualPol = [](const bool echoAloft, const double reflectivityDbz,
                                const double zdrDb, const double kdp) {
        DecodedChunk chunk;
        chunk.radarId = "KTLX";
        chunk.momentEncoding = MomentEncoding::NexradMessage31;
        chunk.rays.push_back(
            syntheticRainRay(90.0F, 1, 0.5F, 10000, 1000, 16, reflectivityDbz));
        chunk.rays.push_back(
            syntheticRainRay(90.0F, 2, 30.0F, 10000, 1000, 60,
                             echoAloft ? reflectivityDbz : 0.0, echoAloft));
        attachRainCorrelation(chunk.rays.front(), 0.98);
        attachRainDifferentialReflectivity(chunk.rays.front(), zdrDb);
        attachRainSpecificDifferentialPhase(chunk.rays.front(), kdp);
        return chunk;
    };
    const auto configFor = [](const RainfallRelation relation) {
        RainfallConfig config;
        config.relation = relation;
        return config;
    };
    const auto fourSamples = [](const DecodedChunk& chunk) {
        return std::vector<RainfallSample>{
            {&chunk, 0.0}, {&chunk, 300.0}, {&chunk, 600.0}, {&chunk, 900.0}};
    };

    // Strong low echo with no column above 3 km: clutter or AP. Every relation
    // must drop all 16 gates and say why.
    const auto clutter = withDualPol(false, 45.0, 0.0, 1.0);
    for (const auto relation : {RainfallRelation::MarshallPalmer, RainfallRelation::Zdr,
                                RainfallRelation::Kdp}) {
        const auto grid = accumulateRainfall(fourSamples(clutter), 370.0,
                                             std::numeric_limits<double>::quiet_NaN(),
                                             configFor(relation));
        assert(grid.summary.valid);
        assert(grid.summary.clutterSuspectGateCount == 16);
        assert(grid.summary.depthGateCount == 0);
        assert(grid.summary.quality == RainfallQuality::Poor);
        for (const auto& gate : grid.rays.front().gates) {
            assert(gate.depthTenthsMillimeters == 0);
            assert((gate.flags & RainfallGateClutterSuspect) != 0);
        }
    }

    // Single-tilt volume: the column guard cannot run. Same NoVerticalCheck
    // accounting for all three, depth kept at each relation's own rate.
    const auto blind = withDualPol(false, 45.0, 0.0, 1.0);
    DecodedChunk singleTilt;
    singleTilt.radarId = "KTLX";
    singleTilt.momentEncoding = MomentEncoding::NexradMessage31;
    singleTilt.rays.push_back(blind.rays.front());
    // 45 dBZ, 0 dB ZDR: R = 41.4275 mm/h -> 103.57 tenths -> 104.
    const std::uint16_t expectedBlind[3] = {70, 104, 110};
    int index = 0;
    for (const auto relation : {RainfallRelation::MarshallPalmer, RainfallRelation::Zdr,
                                RainfallRelation::Kdp}) {
        const auto grid = accumulateRainfall(fourSamples(singleTilt), 370.0,
                                             std::numeric_limits<double>::quiet_NaN(),
                                             configFor(relation));
        assert(grid.summary.valid);
        assert(grid.summary.noVerticalCheckGateCount == 16);
        assert(grid.summary.clutterSuspectGateCount == 0);
        assert(grid.summary.quality == RainfallQuality::Marginal);
        for (const auto& gate : grid.rays.front().gates) {
            assert(gate.depthTenthsMillimeters == expectedBlind[index]);
            assert((gate.flags & RainfallGateNoVerticalCheck) != 0);
        }
        ++index;
    }

    // A 20 min volume gap is credited as 600 s and flagged for every relation.
    // 45 dBZ + real column: Z-R 46 tenths (600 s), R(Z,ZDR) 69, R(KDP) 73.
    const auto storm = withDualPol(true, 45.0, 0.0, 1.0);
    const std::vector<RainfallSample> gapSamples{{&storm, 0.0}, {&storm, 1200.0}};
    const std::uint16_t expectedGap[3] = {46, 69, 73};
    index = 0;
    for (const auto relation : {RainfallRelation::MarshallPalmer, RainfallRelation::Zdr,
                                RainfallRelation::Kdp}) {
        const auto grid = accumulateRainfall(gapSamples, 370.0,
                                             std::numeric_limits<double>::quiet_NaN(),
                                             configFor(relation));
        assert(grid.summary.valid);
        assert(grid.summary.gapClampedIntervalCount == 1);
        assert(grid.summary.gapClampedGateCount == 16);
        for (const auto& gate : grid.rays.front().gates) {
            assert(gate.depthTenthsMillimeters == expectedGap[index]);
            assert((gate.flags & RainfallGateGapClamped) != 0);
        }
        ++index;
    }
}

static RadarStormFeature syntheticFeature(const double lat, const double lon, const double dbz,
                                                 const bool rotating = false) {
    RadarStormFeature feature;
    feature.latitudeDeg = lat;
    feature.longitudeDeg = lon;
    feature.maximumReflectivityDbz = dbz;
    feature.rotating = rotating;
    feature.azimuthalShearPerSecond = rotating ? 0.004 : 0.001;
    return feature;
}

void testStormTracker() {
    StormTracker tracker;
    // Two storms 300 km apart drift east; a third pops up later.
    auto tracks = tracker.update({syntheticFeature(35.0, -97.0, 60.0, true),
                                  syntheticFeature(35.0, -94.0, 55.0)},
                                 1000.0);
    assert(tracks.size() == 2);
    const int northId = tracks.at(0).id != tracks.at(1).id ? tracks.at(0).id : -1;
    assert(northId > 0);
    // Second volume: both move ~+0.1 deg lon (~9 km in 300 s — sane speed).
    // IDs must survive.
    tracks = tracker.update({syntheticFeature(35.0, -96.9, 61.0, true),
                             syntheticFeature(35.0, -93.9, 54.0)},
                            1300.0);
    assert(tracks.size() == 2);
    for (const auto& track : tracks) assert(track.fixes.size() == 2);
    // Scrub guard: an older volume must not corrupt the tracks.
    tracks = tracker.update({syntheticFeature(35.0, -97.0, 60.0, true)}, 1000.0);
    assert(tracks.size() == 2);
    for (const auto& track : tracks) assert(track.fixes.size() == 2);
    // One storm vanishes for a volume (coasted), a new one is born elsewhere.
    tracks = tracker.update({syntheticFeature(35.0, -96.8, 62.0, true),
                             syntheticFeature(36.0, -92.0, 50.0)},
                            1600.0);
    assert(tracks.size() == 3);
    // The vanished storm coasts once more, then dies.
    tracks = tracker.update({syntheticFeature(35.0, -96.7, 63.0, true),
                             syntheticFeature(36.0, -91.9, 51.0)},
                            1900.0);
    assert(tracks.size() == 3);
    tracks = tracker.update({syntheticFeature(35.0, -96.6, 63.0, true),
                             syntheticFeature(36.0, -91.8, 51.0)},
                            2200.0);
    assert(tracks.size() == 2);
    // Teleporting max must not yank a track: 500 km jump starts a new one.
    tracks = tracker.update({syntheticFeature(35.0, -96.5, 64.0, true),
                             syntheticFeature(30.0, -85.0, 70.0)},
                            2500.0);
    bool foundTeleported = false;
    for (const auto& track : tracks) {
        if (track.fixes.size() == 1 && track.fixes.back().maximumReflectivityDbz == 70.0)
            foundTeleported = true;
    }
    assert(foundTeleported);
}

void testVolumeSpillRoundTrip() {
    DecodedChunk chunk;
    chunk.radarId = "KTLX";
    chunk.messageCount = 7;
    chunk.rejectedMessages = 1;
    chunk.rejectedBlocks = 2;
    chunk.volumeCoveragePattern = 212;
    chunk.rays.push_back(syntheticDerivedRay(90.0F, 1, 0.5F, 30.0));
    chunk.rays.push_back(syntheticDerivedRay(180.0F, 2, 5.0F, 10.0));
    const auto path =
        (std::filesystem::temp_directory_path() / "cappedweather-spill-test.cwdv").string();
    assert(spillVolume(chunk, path));
    const auto loaded = loadSpilledVolume(path);
    assert(loaded);
    assert(loaded->radarId == "KTLX");
    assert(loaded->messageCount == 7);
    assert(loaded->rejectedMessages == 1);
    assert(loaded->rejectedBlocks == 2);
    assert(loaded->volumeCoveragePattern == 212);
    assert(loaded->rays.size() == 2);
    const auto refIndex = static_cast<std::size_t>(ProductKind::Reflectivity);
    for (std::size_t i = 0; i < 2; ++i) {
        const auto& want = chunk.rays[i];
        const auto& got = loaded->rays[i];
        assert(got.azimuthDegrees == want.azimuthDegrees);
        assert(got.elevationNumber == want.elevationNumber);
        assert(got.elevationDegrees == want.elevationDegrees);
        assert(got.fields[refIndex] == want.fields[refIndex]);
        assert(got.scaleByProduct[refIndex] == want.scaleByProduct[refIndex]);
        assert(got.offsetByProduct[refIndex] == want.offsetByProduct[refIndex]);
    }
    // Corrupt copy must fail clean, never partial data.
    assert(!loadSpilledVolume("no-such-file.cwdv"));
    std::remove(path.c_str());
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
    // Offline outbreak-belt coordinates (api.weather.gov/radar/stations):
    // the mosaic probe and replay tools need these without a network round.
    for (const auto& [id, lat, lon] :
         {std::tuple{"KPAH", 37.06833, -88.77194}, {"KVWX", 38.26024, -87.72452},
          {"KOHX", 36.24722, -86.5625}, {"KHPX", 36.73666, -87.28499},
          {"KLVX", 37.97527, -85.94388}, {"KDGX", 32.27999, -89.98444},
          {"KHTX", 34.93055, -86.08361}, {"KGSP", 34.8833, -82.21983}}) {
        const auto found =
            std::find_if(sites.begin(), sites.end(), [&](const RadarSite& site) { return site.id == id; });
        assert(found != sites.end());
        assert(found->latitude.has_value() && found->longitude.has_value());
        assert(std::abs(*found->latitude - lat) < 1e-3 && std::abs(*found->longitude - lon) < 1e-3);
    }
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

namespace {
DecodedRay syntheticVadRay(const float azimuth, const std::uint8_t elevationNumber,
                           const float elevationDegrees, const double radialVelocity) {
    DecodedRay ray;
    ray.azimuthDegrees = azimuth;
    ray.azimuthResolutionDegrees = 10.0F;
    ray.elevationNumber = elevationNumber;
    ray.elevationDegrees = elevationDegrees;
    const auto index = static_cast<std::size_t>(ProductKind::Velocity);
    // (encoded - offset) / scale encoding; offset keeps negatives encodable.
    const auto encoded =
        static_cast<std::uint16_t>(std::lround(radialVelocity * 2.0 + 2000.0));
    ray.fields.at(index).assign(20, encoded);
    ray.firstGateByProduct.at(index) = 0;
    ray.gateSpacingByProduct.at(index) = 1000;
    ray.scaleByProduct.at(index) = 2.0F;
    ray.offsetByProduct.at(index) = 2000.0F;
    return ray;
}
}  // namespace

namespace {
DecodedRay syntheticZdrRay(const float azimuth, const std::uint8_t elevationNumber,
                           const float elevationDegrees, const double zdrDb,
                           const double reflectivityDbz, const int firstGateMeters = 0,
                           const std::size_t gateCount = 40) {
    DecodedRay ray;
    ray.azimuthDegrees = azimuth;
    ray.azimuthResolutionDegrees = 1.0F;
    ray.elevationNumber = elevationNumber;
    ray.elevationDegrees = elevationDegrees;
    const auto zdrIndex = static_cast<std::size_t>(ProductKind::DifferentialReflectivity);
    const auto refIndex = static_cast<std::size_t>(ProductKind::Reflectivity);
    ray.fields.at(zdrIndex).assign(
        gateCount, static_cast<std::uint16_t>(std::lround(zdrDb * 100.0)));
    ray.firstGateByProduct.at(zdrIndex) = static_cast<std::int16_t>(firstGateMeters);
    ray.gateSpacingByProduct.at(zdrIndex) = 1000;
    ray.scaleByProduct.at(zdrIndex) = 100.0F;
    ray.offsetByProduct.at(zdrIndex) = 0.0F;
    ray.fields.at(refIndex).assign(
        gateCount, static_cast<std::uint16_t>(std::lround(reflectivityDbz * 2.0 + 66.0)));
    ray.firstGateByProduct.at(refIndex) = static_cast<std::int16_t>(firstGateMeters);
    ray.gateSpacingByProduct.at(refIndex) = 1000;
    ray.scaleByProduct.at(refIndex) = 2.0F;
    ray.offsetByProduct.at(refIndex) = 66.0F;
    return ray;
}
}  // namespace

void testZdrColumns() {
    // One compact column: 4 adjacent gates on two tilts (1 and 10 deg).
    // Every candidate sits inside a 3 km ball, so seed-radius grouping
    // yields exactly one column reaching past 3 km MSL with ~2.9 km depth.
    DecodedChunk chunk;
    chunk.radarId = "KTLX";
    chunk.rays.push_back(syntheticZdrRay(90.0F, 1, 1.0F, 2.5, 55.0, 15000, 4));
    chunk.rays.push_back(syntheticZdrRay(90.0F, 2, 10.0F, 2.5, 55.0, 15000, 4));
    const auto columns = detectZdrColumns(chunk, 35.0, -97.0, 300.0, {});
    assert(columns.size() == 1);
    assert(std::abs(columns.front().maxZdrDb - 2.5) < 0.05);
    assert(columns.front().topMslMeters > 3000.0);
    assert(columns.front().depthMeters > 1500.0);
    assert(columns.front().tiltCount == 2);
    assert(columns.front().maxReflectivityDbz > 50.0);
    // High ZDR in weak echo is not a column.
    {
        DecodedChunk weak = chunk;
        for (auto& ray : weak.rays) {
            auto& ref = ray.fields.at(static_cast<std::size_t>(ProductKind::Reflectivity));
            std::fill(ref.begin(), ref.end(), static_cast<std::uint16_t>(66 + 20 * 2));
        }
        assert(detectZdrColumns(weak, 35.0, -97.0, 300.0, {}).empty());
    }
    // One tilt is never a column, however hot.
    {
        DecodedChunk single;
        single.radarId = "KTLX";
        single.rays.push_back(syntheticZdrRay(90.0F, 2, 5.0F, 3.0, 60.0));
        assert(detectZdrColumns(single, 35.0, -97.0, 300.0, {}).empty());
    }
    // Single-pol volumes (no ZDR moments, like OPERA today) yield nothing.
    {
        DecodedChunk singlePol;
        singlePol.radarId = "deneu";
        singlePol.rays.push_back(syntheticDerivedRay(90.0F, 1, 0.5F, 55.0));
        assert(detectZdrColumns(singlePol, 50.0, 6.0, 100.0, {}).empty());
    }
    // Low-topped warmth stays out via the top guard.
    {
        ZdrColumnConfig low;
        low.minTopMslMeters = 20000.0;
        assert(detectZdrColumns(chunk, 35.0, -97.0, 300.0, low).empty());
    }
}

namespace {
DecodedRay syntheticBrightBandRay(const float azimuth, const std::uint8_t elevationNumber,
                                  const float elevationDegrees) {
    // Stratiform ray: CC dips to 0.93 exactly where the beam crosses
    // 2700-3300 m, 0.995 elsewhere, REF steady at 30 dBZ.
    DecodedRay ray;
    ray.azimuthDegrees = azimuth;
    ray.azimuthResolutionDegrees = 3.0F;
    ray.elevationNumber = elevationNumber;
    ray.elevationDegrees = elevationDegrees;
    const auto ccIndex = static_cast<std::size_t>(ProductKind::CorrelationCoefficient);
    const auto refIndex = static_cast<std::size_t>(ProductKind::Reflectivity);
    ray.fields.at(ccIndex).assign(120, static_cast<std::uint16_t>(995));
    ray.firstGateByProduct.at(ccIndex) = 0;
    ray.gateSpacingByProduct.at(ccIndex) = 1000;
    ray.scaleByProduct.at(ccIndex) = 1000.0F;
    ray.offsetByProduct.at(ccIndex) = 0.0F;
    ray.fields.at(refIndex).assign(120, static_cast<std::uint16_t>(30 * 2 + 66));
    ray.firstGateByProduct.at(refIndex) = 0;
    ray.gateSpacingByProduct.at(refIndex) = 1000;
    ray.scaleByProduct.at(refIndex) = 2.0F;
    ray.offsetByProduct.at(refIndex) = 66.0F;
    // Dip gates are chosen by the same curved beam height the detector
    // measures (flat slant*sin would sit ~600 m low at 100 km).
    for (std::size_t gate = 0; gate < 120; ++gate) {
        const double slant = 500.0 + gate * 1000.0;
        const double height =
            standardBeamGeometry(slant, elevationDegrees).heightAboveRadarMeters + 300.0;
        if (height >= 2700.0 && height <= 3300.0)
            ray.fields.at(ccIndex).at(gate) = static_cast<std::uint16_t>(930);
    }
    return ray;
}
}  // namespace

void testBrightBand() {
    DecodedChunk chunk;
    chunk.radarId = "KTLX";
    for (int step = 0; step < 120; ++step) {
        const float azimuth = static_cast<float>(step * 3);
        chunk.rays.push_back(syntheticBrightBandRay(azimuth, 1, 1.5F));
        chunk.rays.push_back(syntheticBrightBandRay(azimuth, 2, 3.0F));
    }
    const auto estimate = estimateBrightBand(chunk, 300.0, {});
    assert(estimate.valid);
    assert(estimate.quality == BrightBandQuality::Good);
    assert(std::abs(estimate.heightMslMeters - 3000.0) < 300.0);
    assert(estimate.azimuthCoverageDegrees > 340.0);
    // Clean air (no dip) has no melting level.
    {
        DecodedChunk clean;
        clean.radarId = "KTLX";
        clean.rays.push_back(syntheticBrightBandRay(90.0F, 1, 1.5F));
        for (auto& value : clean.rays.back().fields.at(
                 static_cast<std::size_t>(ProductKind::CorrelationCoefficient)))
            value = static_cast<std::uint16_t>(995);
        const auto none = estimateBrightBand(clean, 300.0, {});
        assert(!none.valid && none.quality == BrightBandQuality::Invalid);
    }
    // Convective-only echo (REF 55 everywhere) is excluded by the REF
    // window, even with a CC dip present.
    {
        DecodedChunk convective = chunk;
        for (auto& ray : convective.rays) {
            auto& ref = ray.fields.at(static_cast<std::size_t>(ProductKind::Reflectivity));
            std::fill(ref.begin(), ref.end(), static_cast<std::uint16_t>(55 * 2 + 66));
        }
        const auto none = estimateBrightBand(convective, 300.0, {});
        assert(!none.valid);
    }
    // Narrow-sector data cannot be Good, however clean the ring.
    {
        DecodedChunk sector;
        sector.radarId = "KTLX";
        for (int step = 0; step < 10; ++step)
            sector.rays.push_back(
                syntheticBrightBandRay(static_cast<float>(step * 3), 1, 1.5F));
        const auto narrow = estimateBrightBand(sector, 300.0, {});
        assert(narrow.quality != BrightBandQuality::Good);
    }
}

namespace {
DecodedRay syntheticTbssRay(const float azimuth, const std::uint8_t elevationNumber,
                            const float elevationDegrees, const double coreDbz = 65.0,
                            const std::size_t spikeGates = 10) {
    // Core at gates 20-24, weak-echo spike immediately behind (gates 25+),
    // echo-free (encoded 1) everywhere else.
    DecodedRay ray;
    ray.azimuthDegrees = azimuth;
    ray.azimuthResolutionDegrees = 1.0F;
    ray.elevationNumber = elevationNumber;
    ray.elevationDegrees = elevationDegrees;
    const auto refIndex = static_cast<std::size_t>(ProductKind::Reflectivity);
    ray.fields.at(refIndex).assign(60, static_cast<std::uint16_t>(1));
    ray.firstGateByProduct.at(refIndex) = 0;
    ray.gateSpacingByProduct.at(refIndex) = 1000;
    ray.scaleByProduct.at(refIndex) = 2.0F;
    ray.offsetByProduct.at(refIndex) = 66.0F;
    const auto setDbz = [&](const std::size_t gate, const double dbz) {
        ray.fields.at(refIndex).at(gate) =
            static_cast<std::uint16_t>(std::lround(dbz * 2.0 + 66.0));
    };
    for (std::size_t gate = 20; gate <= 24; ++gate) setDbz(gate, coreDbz);
    for (std::size_t gate = 25; gate < 25 + spikeGates && gate < 60; ++gate) setDbz(gate, 15.0);
    return ray;
}
}  // namespace

void testTbss() {
    // Three adjacent rays with core + 10 km spike: one detection.
    DecodedChunk chunk;
    chunk.radarId = "KTLX";
    chunk.rays.push_back(syntheticTbssRay(90.0F, 1, 0.5F));
    chunk.rays.push_back(syntheticTbssRay(91.0F, 1, 0.5F));
    chunk.rays.push_back(syntheticTbssRay(92.0F, 1, 0.5F));
    const auto detections = detectTbss(chunk, 35.0, -97.0, 300.0, {});
    assert(detections.size() == 1);
    assert(detections.front().rayCount == 3);
    assert(std::abs(detections.front().maxCoreDbz - 65.0) < 0.6);
    assert(std::abs(detections.front().spikeLengthKm - 9.0) < 0.6);
    // Sub-60 core is not TBSS territory.
    {
        DecodedChunk weak = chunk;
        for (auto& ray : weak.rays)
            for (std::size_t gate = 20; gate <= 24; ++gate)
                ray.fields.at(static_cast<std::size_t>(ProductKind::Reflectivity)).at(gate) =
                    static_cast<std::uint16_t>(std::lround(55.0 * 2.0 + 66.0));
        assert(detectTbss(weak, 35.0, -97.0, 300.0, {}).empty());
    }
    // A 2 km flicker is speckle, not a spike.
    {
        DecodedChunk shortSpike;
        shortSpike.radarId = "KTLX";
        shortSpike.rays.push_back(syntheticTbssRay(90.0F, 1, 0.5F, 65.0, 2));
        shortSpike.rays.push_back(syntheticTbssRay(91.0F, 1, 0.5F, 65.0, 2));
        shortSpike.rays.push_back(syntheticTbssRay(92.0F, 1, 0.5F, 65.0, 2));
        assert(detectTbss(shortSpike, 35.0, -97.0, 300.0, {}).empty());
    }
    // One lonely ray never marks, however clean the spike.
    {
        DecodedChunk single;
        single.radarId = "KTLX";
        single.rays.push_back(syntheticTbssRay(90.0F, 1, 0.5F));
        assert(detectTbss(single, 35.0, -97.0, 300.0, {}).empty());
    }
    // A spike running to the last gate is unterminated edge, not TBSS.
    {
        DecodedChunk edge;
        edge.radarId = "KTLX";
        for (const float azimuth : {90.0F, 91.0F, 92.0F}) {
            auto ray = syntheticTbssRay(azimuth, 1, 0.5F, 65.0, 35);
            edge.rays.push_back(ray);
        }
        assert(detectTbss(edge, 35.0, -97.0, 300.0, {}).empty());
    }
    // Strong echo right behind the core is storm zone, not a veto: the run
    // behind it still roots at a storm edge with a 60+ core 2 gates uprange.
    // What fails is a run with no 60+ core linked (core too far uprange).
    {
        DecodedChunk farCore;
        farCore.radarId = "KTLX";
        const auto refIndex = static_cast<std::size_t>(ProductKind::Reflectivity);
        for (const float azimuth : {90.0F, 91.0F, 92.0F}) {
            auto ray = syntheticTbssRay(azimuth, 1, 0.5F, 65.0, 10);
            auto& field = ray.fields.at(refIndex);
            for (std::size_t gate = 20; gate <= 24; ++gate)
                field.at(gate) = static_cast<std::uint16_t>(1);  // clear the near core
            for (std::size_t gate = 2; gate <= 4; ++gate)
                field.at(gate) =
                    static_cast<std::uint16_t>(std::lround(65.0 * 2.0 + 66.0));  // 21 km uprange
            field.at(24) = static_cast<std::uint16_t>(std::lround(35.0 * 2.0 + 66.0));  // root
            farCore.rays.push_back(ray);
        }
        assert(detectTbss(farCore, 35.0, -97.0, 300.0, {}).empty());
    }
    // A weak run with no storm root (clear air before it) is not a spike.
    {
        DecodedChunk noRoot;
        noRoot.radarId = "KTLX";
        const auto refIndex = static_cast<std::size_t>(ProductKind::Reflectivity);
        for (const float azimuth : {90.0F, 91.0F, 92.0F}) {
            auto ray = syntheticTbssRay(azimuth, 1, 0.5F, 65.0, 10);
            ray.fields.at(refIndex).at(24) = static_cast<std::uint16_t>(1);  // clear-air root
            noRoot.rays.push_back(ray);
        }
        assert(detectTbss(noRoot, 35.0, -97.0, 300.0, {}).empty());
    }
    // Fade-out end: weak echo dissolving into clear air still anchors.
    {
        DecodedChunk fade;
        fade.radarId = "KTLX";
        const auto refIndex = static_cast<std::size_t>(ProductKind::Reflectivity);
        for (const float azimuth : {90.0F, 91.0F, 92.0F}) {
            auto ray = syntheticTbssRay(azimuth, 1, 0.5F, 65.0, 12);
            fade.rays.push_back(ray);
        }
        const auto faded = detectTbss(fade, 35.0, -97.0, 300.0, {});
        assert(faded.size() == 1);
        assert(std::abs(faded.front().spikeLengthKm - 11.0) < 0.6);
    }
    // A broad sheet (10 adjacent rays, 9 degrees wide) is stratiform edge,
    // not a spike, however clean each ray reads.
    {
        DecodedChunk sheet;
        sheet.radarId = "KTLX";
        for (int step = 0; step < 10; ++step)
            sheet.rays.push_back(syntheticTbssRay(90.0F + step, 1, 0.5F, 65.0, 10));
        assert(detectTbss(sheet, 35.0, -97.0, 300.0, {}).empty());
    }
    // A run ending at strong echo hits another echo mass, not clear air.
    {
        DecodedChunk dirty;
        dirty.radarId = "KTLX";
        const auto refIndex = static_cast<std::size_t>(ProductKind::Reflectivity);
        for (const float azimuth : {90.0F, 91.0F, 92.0F}) {
            auto ray = syntheticTbssRay(azimuth, 1, 0.5F, 65.0, 6);
            ray.fields.at(refIndex).at(31) =
                static_cast<std::uint16_t>(std::lround(40.0 * 2.0 + 66.0));
            dirty.rays.push_back(ray);
        }
        assert(detectTbss(dirty, 35.0, -97.0, 300.0, {}).empty());
    }
}

namespace {
VadLevel syntheticVadLevel(const double heightMslMeters, const double uEast, const double vNorth,
                           const std::size_t gates = 200,
                           const VadQuality quality = VadQuality::Good) {
    VadLevel level;
    level.heightMslMeters = heightMslMeters;
    level.speedMetersPerSecond = std::hypot(uEast, vNorth);
    level.directionFromDegrees =
        std::fmod(std::atan2(-uEast, -vNorth) * 180.0 / 3.14159265358979323846 + 360.0, 360.0);
    level.gateCount = gates;
    level.azimuthCoverageDegrees = 360.0;
    level.quality = quality;
    return level;
}
}  // namespace

void testBunkers() {
    // Uniform westerly flow (u=10 everywhere): zero shear, motion = mean.
    {
        const std::vector<VadLevel> levels = {
            syntheticVadLevel(500.0, 10.0, 0.0), syntheticVadLevel(1500.0, 10.0, 0.0),
            syntheticVadLevel(4500.0, 10.0, 0.0), syntheticVadLevel(5500.0, 10.0, 0.0)};
        const auto motion = estimateBunkersMotion(levels, 300.0, {});
        assert(motion.valid && motion.weakShear);
        assert(motion.quality == VadQuality::Good);
        assert(std::abs(motion.shearMagnitudeMps) < 0.01);
        assert(std::abs(motion.meanSpeedMps - 10.0) < 0.01);
        assert(std::abs(motion.meanFromDegrees - 270.0) < 0.5);
        assert(std::abs(motion.rightSpeedMps - 10.0) < 0.01);
    }
    // Sheared flow (5 m/s low, 20 m/s high, both westerly): mean 12.5 m/s,
    // shear 15 m/s east, right-mover deviated 7.5 south of the mean.
    {
        const std::vector<VadLevel> levels = {
            syntheticVadLevel(500.0, 5.0, 0.0), syntheticVadLevel(1500.0, 5.0, 0.0),
            syntheticVadLevel(4500.0, 20.0, 0.0), syntheticVadLevel(5500.0, 20.0, 0.0)};
        const auto motion = estimateBunkersMotion(levels, 300.0, {});
        assert(motion.valid && !motion.weakShear);
        assert(std::abs(motion.shearMagnitudeMps - 15.0) < 0.01);
        assert(std::abs(motion.meanSpeedMps - 12.5) < 0.01);
        assert(std::abs(motion.rightSpeedMps - 14.58) < 0.05);
        assert(std::abs(motion.rightFromDegrees - 301.0) < 1.0);
        // Left mover mirrors across the mean wind.
        assert(std::abs(motion.leftSpeedMps - 14.58) < 0.05);
        assert(std::abs(motion.leftFromDegrees - 239.0) < 1.0);
    }
    // Lower half missing: no shear, no motion.
    {
        const std::vector<VadLevel> levels = {syntheticVadLevel(4500.0, 20.0, 0.0),
                                              syntheticVadLevel(5500.0, 20.0, 0.0)};
        assert(!estimateBunkersMotion(levels, 300.0, {}).valid);
    }
    // Poor rows steer nothing: all-Poor profile is invalid.
    {
        const std::vector<VadLevel> levels = {
            syntheticVadLevel(500.0, 10.0, 0.0, 200, VadQuality::Poor),
            syntheticVadLevel(4500.0, 10.0, 0.0, 200, VadQuality::Poor)};
        assert(!estimateBunkersMotion(levels, 300.0, {}).valid);
    }
    // Two qualifying levels still give a Marginal motion.
    {
        const std::vector<VadLevel> levels = {syntheticVadLevel(1000.0, 10.0, 0.0),
                                              syntheticVadLevel(5000.0, 10.0, 0.0)};
        const auto motion = estimateBunkersMotion(levels, 300.0, {});
        assert(motion.valid && motion.quality == VadQuality::Marginal);
    }
}

void testSrh() {
    // Zero SRH: uniform flow equal to storm motion, every (V-C) vanishes.
    {
        const std::vector<VadLevel> levels = {
            syntheticVadLevel(400.0, 10.0, 0.0), syntheticVadLevel(1000.0, 10.0, 0.0),
            syntheticVadLevel(1800.0, 10.0, 0.0), syntheticVadLevel(2800.0, 10.0, 0.0)};
        BunkersMotion motion;
        motion.valid = true;
        motion.weakShear = true;
        motion.meanFromDegrees = 270.0;
        motion.meanSpeedMps = 10.0;
        motion.quality = VadQuality::Good;
        const auto srh = estimateSrh(levels, motion, 300.0, {});
        assert(srh.valid && srh.srh01Valid && srh.srh03Valid);
        assert(srh.quality == VadQuality::Good);
        assert(std::abs(srh.srh01M2PerS2) < 0.01);
        assert(std::abs(srh.srh03M2PerS2) < 0.01);
    }
    // Curved hodograph: linear speed shear u = 10 + 0.005*z with an
    // off-line right-mover c = (12.5, -7.5). Closed form per pair:
    // -cy * du = 7.5 * du, giving SRH01 = 18.75 and
    // SRH03 = 18.75 + 28.125 + 37.5 = 84.375.
    {
        const std::vector<VadLevel> levels = {
            syntheticVadLevel(550.0, 11.25, 0.0), syntheticVadLevel(1050.0, 13.75, 0.0),
            syntheticVadLevel(1800.0, 17.5, 0.0), syntheticVadLevel(2800.0, 22.5, 0.0)};
        BunkersMotion motion;
        motion.valid = true;
        motion.weakShear = false;
        motion.rightFromDegrees = 301.0;
        motion.rightSpeedMps = 14.58;
        motion.quality = VadQuality::Good;
        const auto srh = estimateSrh(levels, motion, 300.0, {});
        assert(srh.valid && srh.srh01Valid && srh.srh03Valid);
        assert(srh.quality == VadQuality::Good);
        assert(std::abs(srh.srh01M2PerS2 - 18.75) < 1.5);
        assert(std::abs(srh.srh03M2PerS2 - 84.38) < 3.0);
    }
    // Single row: no pair, no helicity.
    {
        const std::vector<VadLevel> levels = {syntheticVadLevel(500.0, 10.0, 0.0)};
        BunkersMotion motion;
        motion.valid = true;
        motion.weakShear = true;
        motion.meanFromDegrees = 270.0;
        motion.meanSpeedMps = 10.0;
        assert(!estimateSrh(levels, motion, 300.0, {}).valid);
    }
    // No storm motion, no SRH.
    {
        const std::vector<VadLevel> levels = {syntheticVadLevel(500.0, 10.0, 0.0),
                                              syntheticVadLevel(1500.0, 10.0, 0.0)};
        assert(!estimateSrh(levels, BunkersMotion{}, 300.0, {}).valid);
    }
    // Two shallow rows: 0-1 km Marginal without the 0-3 km layer.
    {
        const std::vector<VadLevel> levels = {syntheticVadLevel(500.0, 10.0, 0.0),
                                              syntheticVadLevel(1100.0, 12.0, 0.0)};
        BunkersMotion motion;
        motion.valid = true;
        motion.weakShear = true;
        motion.meanFromDegrees = 270.0;
        motion.meanSpeedMps = 11.0;
        const auto srh = estimateSrh(levels, motion, 300.0, {});
        assert(srh.valid && srh.srh01Valid && !srh.srh03Valid);
        assert(srh.quality == VadQuality::Marginal);
    }
}

void testHailSwaths() {
    const auto trackAt = [](const int id, const double lat, const double lon) {
        StormTrack track;
        track.id = id;
        track.fixes.push_back({lat, lon, 55.0, 0.0, false, 1000.0});
        return track;
    };
    const auto coreAt = [](const double lat, const double lon, const int tier) {
        HailCore core;
        core.latitudeDeg = lat;
        core.longitudeDeg = lon;
        core.tier = tier;
        core.maxReflectivityDbz = 60.0;
        return core;
    };
    // Three volumes, one storm: fixes accumulate on the same swath.
    HailSwathTracker tracker;
    const std::vector<StormTrack> tracks = {trackAt(7, 35.0, -97.0)};
    tracker.update(tracks, {coreAt(35.01, -97.0, 1)}, 1000.0);
    tracker.update(tracks, {coreAt(35.02, -96.99, 2)}, 1300.0);
    tracker.update(tracks, {coreAt(35.0, -96.98, 1)}, 1600.0);
    assert(tracker.swaths().size() == 1);
    assert(tracker.swaths().front().trackId == 7);
    assert(tracker.swaths().front().fixes.size() == 3);
    assert(tracker.swaths().front().maxTier == 2);
    assert(hailSwathHull(tracker.swaths().front()).size() >= 3);
    // A distant core belongs to no storm.
    {
        HailSwathTracker far;
        far.update(tracks, {coreAt(36.0, -95.0, 3)}, 1000.0);
        assert(far.swaths().empty());
    }
    // Tier-0 and non-finite cores never seed fixes.
    {
        HailSwathTracker junk;
        junk.update(tracks, {coreAt(35.01, -97.0, 0)}, 1000.0);
        HailCore nan = coreAt(35.01, -97.0, 2);
        nan.latitudeDeg = std::numeric_limits<double>::quiet_NaN();
        junk.update(tracks, {nan}, 1300.0);
        assert(junk.swaths().empty());
    }
    // Collinear fixes are a path, not a footprint: no hull.
    {
        HailSwath line;
        line.trackId = 9;
        line.fixes = {{35.0, -97.0, 1, 1000.0}, {35.01, -97.0, 1, 1300.0}, {35.02, -97.0, 1, 1600.0}};
        assert(hailSwathHull(line).empty());
    }
    // Fix cap keeps the newest.
    {
        HailSwathConfig small;
        small.maxFixesPerSwath = 4;
        HailSwathTracker capped(small);
        for (int i = 0; i < 6; ++i)
            capped.update(tracks, {coreAt(35.0 + i * 0.005, -97.0, 1)}, 1000.0 + i * 300.0);
        assert(capped.swaths().front().fixes.size() == 4);
        assert(capped.swaths().front().fixes.back().epochSeconds == 2500.0);
    }
    // Bad epochs fail closed.
    {
        HailSwathTracker bad;
        bad.update(tracks, {coreAt(35.01, -97.0, 2)},
                   std::numeric_limits<double>::quiet_NaN());
        bad.update(tracks, {coreAt(35.01, -97.0, 2)}, -5.0);
        assert(bad.swaths().empty());
    }
}

void testSounding() {
    // SRH parity: the same sheared hodograph through sounding levels must
    // integrate identically to the VAD path (18.75 / 84.38 closed form).
    // Sounding winds ride knots + FROM degrees; heights go MSL with the
    // first level as the AGL anchor.
    {
        Sounding sounding;
        sounding.stationId = "KOUN";
        sounding.validEpochSeconds = 1000;
        const double winds[][3] = {
            {550.0, 11.25, 0.0}, {1050.0, 13.75, 0.0}, {1800.0, 17.5, 0.0}, {2800.0, 22.5, 0.0}};
        for (const auto& [msl, u, v] : winds) {
            SoundingLevel level;
            level.heightMslMeters = msl;
            level.tempCelsius = 10.0;
            // u/vwesterly-equivalent: FROM 270 at the matching speed.
            const double speed = std::hypot(u, v);
            level.directionFromDegrees =
                std::fmod(std::atan2(-u, -v) * 180.0 / 3.14159265358979323846 + 360.0, 360.0);
            level.speedKnots = speed / 0.5144444444444445;
            sounding.levels.push_back(level);
        }
        const auto points = soundingWindPoints(sounding);
        assert(points.size() == 4);
        assert(std::abs(points.front().aglMeters - 0.0) < 0.01);
        assert(std::abs(points.back().aglMeters - 2250.0) < 0.01);
        BunkersMotion motion;
        motion.valid = true;
        motion.weakShear = false;
        motion.rightFromDegrees = 301.0;
        motion.rightSpeedMps = 14.58;
        motion.quality = VadQuality::Good;
        double cx = 0.0, cy = 0.0;
        assert(resolveStormMotionUv(motion, cx, cy));
        const auto srh = integrateSrhPoints(points, cx, cy, {});
        assert(srh.valid && srh.srh01Valid && srh.srh03Valid);
        assert(srh.quality == VadQuality::Good);
        assert(std::abs(srh.srh01M2PerS2 - 18.75) < 1.5);
        assert(std::abs(srh.srh03M2PerS2 - 84.38) < 3.0);
    }
    // Windless levels never steer: null winds are skipped, not zeroed.
    {
        Sounding sounding;
        SoundingLevel calm;
        calm.heightMslMeters = 500.0;
        calm.tempCelsius = 10.0;
        sounding.levels.push_back(calm);
        SoundingLevel windy;
        windy.heightMslMeters = 1000.0;
        windy.tempCelsius = 8.0;
        windy.directionFromDegrees = 270.0;
        windy.speedKnots = 20.0;
        sounding.levels.push_back(windy);
        const auto points = soundingWindPoints(sounding);
        assert(points.size() == 1);
        assert(std::abs(points.front().aglMeters - 500.0) < 0.01);
    }
    // Freezing level: linear interpolation of the first upward crossing.
    {
        Sounding sounding;
        const double profile[][2] = {{500.0, 5.0}, {1500.0, 2.0}, {2500.0, -2.0}, {3500.0, -6.0}};
        for (const auto& [msl, temp] : profile) {
            SoundingLevel level;
            level.heightMslMeters = msl;
            level.tempCelsius = temp;
            sounding.levels.push_back(level);
        }
        // Crosses between 1500 (2 C) and 2500 (-2 C): 2000 m.
        assert(std::abs(soundingFreezingLevelMsl(sounding) - 2000.0) < 0.01);
    }
    // Never crosses: NaN, never a guess. All-below-freezing included.
    {
        Sounding warm;
        SoundingLevel a;
        a.heightMslMeters = 500.0;
        a.tempCelsius = 10.0;
        SoundingLevel b;
        b.heightMslMeters = 1500.0;
        b.tempCelsius = 5.0;
        warm.levels = {a, b};
        assert(std::isnan(soundingFreezingLevelMsl(warm)));
        Sounding cold;
        SoundingLevel c;
        c.heightMslMeters = 500.0;
        c.tempCelsius = -5.0;
        SoundingLevel d;
        d.heightMslMeters = 1500.0;
        d.tempCelsius = -10.0;
        cold.levels = {c, d};
        assert(std::isnan(soundingFreezingLevelMsl(cold)));
        Sounding empty;
        assert(std::isnan(soundingFreezingLevelMsl(empty)));
        assert(soundingWindPoints(empty).empty());
    }
}

namespace {
std::string fixturePath(const char* name) {
    const std::string roots[] = {
        "tests/fixtures/",
        "../tests/fixtures/",
        "../../tests/fixtures/",
    };
    for (const auto& root : roots) {
        std::ifstream file(root + name, std::ios::binary);
        if (file) return root + name;
    }
    return {};
}
}  // namespace

void testLambert() {
    // HRRR conic: tangent at 25 N, meridian 265 E, GRS-ish sphere 6371229 m.
    LambertConformal hrrr{25.0, 265.0, 25.0, 25.0, 6371229.0};
    // Origin maps to the apex offset (0, 0) and back.
    const auto origin = lambertForward(hrrr, -95.0, 25.0);
    assert(std::abs(origin.xMeters) < 1e-6 && std::abs(origin.yMeters) < 1e-6);
    const auto back = lambertInverse(hrrr, 0.0, 0.0);
    assert(std::abs(back.longitudeDeg + 95.0) < 1e-9);
    assert(std::abs(back.latitudeDeg - 25.0) < 1e-9);
    // Mirror symmetry about the central meridian.
    const auto east = lambertForward(hrrr, -90.0, 35.0);
    const auto west = lambertForward(hrrr, -100.0, 35.0);
    assert(std::abs(east.xMeters + west.xMeters) < 1e-6);
    assert(std::abs(east.yMeters - west.yMeters) < 1e-6);
    // Round trip over the CONUS domain to sub-micron degrees.
    for (double lat = 25.0; lat <= 50.0; lat += 2.5) {
        for (double lon = -125.0; lon <= -65.0; lon += 2.5) {
            const auto projected = lambertForward(hrrr, lon, lat);
            const auto unprojected = lambertInverse(hrrr, projected.xMeters, projected.yMeters);
            assert(std::abs(unprojected.longitudeDeg - lon) < 1e-9);
            assert(std::abs(unprojected.latitudeDeg - lat) < 1e-9);
        }
    }
    // NaN in, NaN out; degenerate sphere fails closed.
    const auto bad = lambertForward(hrrr, std::numeric_limits<double>::quiet_NaN(), 35.0);
    assert(std::isnan(bad.xMeters) && std::isnan(bad.yMeters));
    LambertConformal flat = hrrr;
    flat.earthRadiusMeters = 0.0;
    const auto flatOut = lambertForward(flat, -95.0, 35.0);
    assert(std::isnan(flatOut.xMeters));
    const auto flatBack = lambertInverse(flat, 1000.0, 1000.0);
    assert(std::isnan(flatBack.longitudeDeg));
}

void testHrrrFixture() {
    if (!hrrrReaderAvailable()) return;
#ifdef CAPPEDWEATHER_EXPECT_NETCDF
    assert(hrrrReaderAvailable());
#endif
    const auto path = fixturePath("hrrr_refc_ok.nc");
    assert(!path.empty());
    const auto frame = readHrrrRefc(path);
    assert(frame.has_value());
    assert(frame->nx == 223 && frame->ny == 183);
    assert(frame->reflectivityDbz.size() == 223 * 183);
    assert(std::abs(frame->dxKm - 2.54) < 0.01 && std::abs(frame->dyKm - 2.54) < 0.01);
    assert(std::abs(frame->projection.latitudeOriginDeg - 25.0) < 1e-9);
    assert(std::abs(frame->projection.longitudeMeridianEastDeg - 265.0) < 1e-9);
    assert(std::abs(frame->projection.firstParallelDeg - 25.0) < 1e-9);
    assert(std::abs(frame->projection.earthRadiusMeters - 6371229.0) < 1.0);
    // Requested bbox was 33-37 N, 100-94 W: the grid must cover it (NCSS
    // snaps outward by an unspecified margin, so assert coverage with a
    // 0.2-degree skirt, not corner coincidence). Orientation-free: every
    // grid corner inverse-projects inside the skirted box.
    const double gridX[2] = {frame->x0Km, frame->x0Km + (frame->nx - 1) * frame->dxKm};
    const double gridY[2] = {frame->y0Km, frame->y0Km + (frame->ny - 1) * frame->dyKm};
    for (const double gx : gridX) {
        for (const double gy : gridY) {
            const auto corner =
                lambertInverse(frame->projection, gx * 1000.0, gy * 1000.0);
            assert(std::isfinite(corner.latitudeDeg) && std::isfinite(corner.longitudeDeg));
            assert(corner.latitudeDeg > 33.0 - 0.2 && corner.latitudeDeg < 37.0 + 0.2);
            assert(corner.longitudeDeg > -100.0 - 0.2 && corner.longitudeDeg < -94.0 + 0.2);
        }
    }
    // And the requested corners land on the grid plane within one skirt of
    // some grid edge: forward maps the bbox into the grid extent.
    const double requestCorners[4][2] = {{33.0, -100.0}, {33.0, -94.0},
                                         {37.0, -100.0}, {37.0, -94.0}};
    const double minGx = std::min(gridX[0], gridX[1]);
    const double maxGx = std::max(gridX[0], gridX[1]);
    const double minGy = std::min(gridY[0], gridY[1]);
    const double maxGy = std::max(gridY[0], gridY[1]);
    for (const auto& [cornerLat, cornerLon] : requestCorners) {
        const auto projected = lambertForward(frame->projection, cornerLon, cornerLat);
        const double px = projected.xMeters / 1000.0;
        const double py = projected.yMeters / 1000.0;
        assert(px > minGx - 15.0 && px < maxGx + 15.0);
        assert(py > minGy - 15.0 && py < maxGy + 15.0);
    }
    // Valid time decodes: 13Z on 2026-10-10 (1791637200 UTC, base
    // 2026-10-07T00:00Z plus the file's 85 h time step).
    assert(frame->validEpochSeconds == 1791637200LL);
    // Quiet morning box: every gate reads the -10 dB floor, all finite
    // after fill handling (no NaN leaks, no phantom echo).
    for (const float value : frame->reflectivityDbz) {
        assert(std::isfinite(value));
        assert(value < -9.0F);
    }
    // Garbage fails closed.
    assert(!readHrrrRefc("tests/fixtures/does-not-exist.nc").has_value());
}

void testFixedGrid() {
    // GOES-18/West geometry straight from the fixture's projection record.
    GoesFixedGrid west{35786023.0, 6378137.0, 6356752.31414, -137.0};
    // Subpoint maps to the grid origin and back.
    const auto origin = goesFixedForward(west, -137.0, 0.0);
    assert(std::abs(origin.xRadians) < 1e-9 && std::abs(origin.yRadians) < 1e-9);
    const auto back = goesFixedInverse(west, 0.0, 0.0);
    assert(std::abs(back.longitudeDeg + 137.0) < 1e-6);
    assert(std::abs(back.latitudeDeg) < 1e-6);
    // Mirror symmetry about the subpoint meridian.
    const auto east = goesFixedForward(west, -130.0, 35.0);
    const auto wside = goesFixedForward(west, -144.0, 35.0);
    assert(std::abs(east.xRadians + wside.xRadians) < 1e-9);
    assert(std::abs(east.yRadians - wside.yRadians) < 1e-6);
    // Round trip over the West scene and over CONUS under East. Tolerance
    // is 0.01 deg (10x the measured 1-3e-3 worst case at high-latitude
    // limb corners; interior round-trips to 1e-9). Longitudes compare
    // wrap-aware so the Aleutian dateline stays quiet.
    const auto wrapDiff = [](double a, double b) {
        double delta = std::fmod(a - b + 540.0, 360.0) - 180.0;
        return std::abs(delta);
    };
    for (double lat = 15.0; lat <= 50.0; lat += 2.5) {
        for (double lon = -170.0; lon <= -100.0; lon += 2.5) {
            const auto projected = goesFixedForward(west, lon, lat);
            const auto unprojected = goesFixedInverse(west, projected.xRadians, projected.yRadians);
            assert(wrapDiff(unprojected.longitudeDeg, lon) < 1e-2);
            assert(std::abs(unprojected.latitudeDeg - lat) < 1e-2);
        }
    }
    // Same code path under GOES-19/East (the app default bucket).
    GoesFixedGrid eastGrid{35786023.0, 6378137.0, 6356752.31414, -75.2};
    for (double lat = 20.0; lat <= 50.0; lat += 2.5) {
        for (double lon = -125.0; lon <= -65.0; lon += 2.5) {
            const auto projected = goesFixedForward(eastGrid, lon, lat);
            const auto unprojected =
                goesFixedInverse(eastGrid, projected.xRadians, projected.yRadians);
            assert(wrapDiff(unprojected.longitudeDeg, lon) < 1e-2);
            assert(std::abs(unprojected.latitudeDeg - lat) < 1e-2);
        }
    }
    const auto norman = goesFixedForward(eastGrid, -97.44, 35.22);
    const auto normanBack = goesFixedInverse(eastGrid, norman.xRadians, norman.yRadians);
    assert(wrapDiff(normanBack.longitudeDeg, -97.44) < 1e-2);
    assert(std::abs(normanBack.latitudeDeg - 35.22) < 1e-2);
    // Off-limb sight lines fail closed, never clamped to the disc edge.
    const auto limb = goesFixedInverse(west, 0.3, 0.3);
    assert(std::isnan(limb.longitudeDeg) && std::isnan(limb.latitudeDeg));
    // NaN in, NaN out; degenerate ellipsoid fails closed.
    const auto bad = goesFixedForward(west, std::numeric_limits<double>::quiet_NaN(), 35.0);
    assert(std::isnan(bad.xRadians) && std::isnan(bad.yRadians));
    GoesFixedGrid flat = west;
    flat.semiMinorMeters = 0.0;
    const auto flatOut = goesFixedForward(flat, -137.0, 20.0);
    assert(std::isnan(flatOut.xRadians));
    const auto flatBack = goesFixedInverse(flat, 0.01, 0.01);
    assert(std::isnan(flatBack.longitudeDeg));
}

void testGoesFixture() {
    if (!goesReaderAvailable()) return;
#ifdef CAPPEDWEATHER_EXPECT_NETCDF
    assert(goesReaderAvailable());
#endif
    const auto path = fixturePath("goes13_ok.nc");
    assert(!path.empty());
    const auto frame = readGoesRad(path);
    assert(frame.has_value());
    assert(frame->nx == 250 && frame->ny == 150);
    assert(frame->brightnessCelsius.size() == 250 * 150);
    assert(frame->channel == 13);
    assert(std::abs(frame->grid.subpointLongitudeDeg + 137.0) < 1e-9);
    assert(std::abs(frame->grid.perspectiveHeightMeters - 35786023.0) < 1.0);
    // Absolute placement against the file's own `geospatial_lat_lon_extent`
    // record (west 175.62E, north 53.5, east -89.62, south 14.57, center
    // -137/29.97): grid corners land on those bounds to 1 deg, which a
    // spherical-`a` got wrong by 20 deg. y runs north-down in storage, so
    // row 0 is the north edge; longitudes compare wrap-aware.
    const double cornerLon[2][2] = {{175.63, -90.21}, {-161.59, -112.6}};
    const double cornerLat[2][2] = {{53.5, 53.44}, {14.98, 14.98}};
    for (std::size_t iy = 0; iy < 2; ++iy) {
        for (std::size_t ix = 0; ix < 2; ++ix) {
            const double gx = (ix == 0) ? frame->x0Radians
                                        : frame->x0Radians + (frame->nx - 1) * frame->dxRadians;
            const double gy = (iy == 0) ? frame->y0Radians
                                        : frame->y0Radians + (frame->ny - 1) * frame->dyRadians;
            const auto corner = goesFixedInverse(frame->grid, gx, gy);
            assert(std::isfinite(corner.latitudeDeg) && std::isfinite(corner.longitudeDeg));
            double lonDelta = std::fmod(corner.longitudeDeg - cornerLon[iy][ix] + 540.0, 360.0);
            lonDelta -= 180.0;
            assert(std::abs(lonDelta) < 1.0);
            assert(std::abs(corner.latitudeDeg - cornerLat[iy][ix]) < 1.0);
        }
    }
    // Scan-start time: 2026-10-10 15:02:38Z (J2000 seconds + 946728000).
    assert(frame->validEpochSeconds == 1791644558LL);
    // Live afternoon scene: real BT spread (cold tops near -53 C, warm
    // ground near +25 C), median near +12 C, spot gate near +6.9 C.
    // Tolerances are loose (float32 Planck math), the shape is the contract.
    float finiteMin = std::numeric_limits<float>::infinity();
    float finiteMax = -std::numeric_limits<float>::infinity();
    double sum = 0.0;
    std::size_t finite = 0;
    for (const float value : frame->brightnessCelsius) {
        if (!std::isfinite(value)) continue;
        finiteMin = std::min(finiteMin, value);
        finiteMax = std::max(finiteMax, value);
        sum += value;
        ++finite;
    }
    assert(finite == 250 * 150);
    assert(finiteMin > -60.0F && finiteMin < -45.0F);
    assert(finiteMax > 20.0F && finiteMax < 30.0F);
    assert(sum / finite > 5.0 && sum / finite < 20.0);
    assert(std::abs(frame->brightnessCelsius[75 * 250 + 125] - 6.9F) < 1.0F);
    // Garbage fails closed.
    assert(!readGoesRad("tests/fixtures/does-not-exist.nc").has_value());
}

void testGoes08Fixture() {
    // Water-vapor twin of the C13 contract: band 8 from GOES-19/East,
    // uint16 counts in int16 containers plus out-of-range strays, so this
    // exercises the _Unsigned reinterpretation and the valid_range gate
    // the C13 fixture never touches.
    if (!goesReaderAvailable()) return;
    const auto path = fixturePath("goes08_ok.nc");
    assert(!path.empty());
    const auto frame = readGoesRad(path);
    assert(frame.has_value());
    assert(frame->nx == 250 && frame->ny == 150);
    assert(frame->brightnessCelsius.size() == 250 * 150);
    assert(frame->channel == 8);
    assert(std::abs(frame->grid.subpointLongitudeDeg + 75.0) < 1e-9);
    assert(frame->validEpochSeconds == 1791644556LL);
    // Upper-tropospheric water vapor: cold throughout (-78..-18 C,
    // median near -36 C), 500 off-disc gates NaN, spot gate near -30 C.
    std::size_t finite = 0;
    float finiteMin = std::numeric_limits<float>::infinity();
    float finiteMax = -std::numeric_limits<float>::infinity();
    double sum = 0.0;
    for (const float value : frame->brightnessCelsius) {
        if (!std::isfinite(value)) continue;
        ++finite;
        finiteMin = std::min(finiteMin, value);
        finiteMax = std::max(finiteMax, value);
        sum += value;
    }
    assert(finite == 37000);
    assert(finiteMin > -85.0F && finiteMin < -70.0F);
    assert(finiteMax > -25.0F && finiteMax < -10.0F);
    assert(sum / finite > -45.0 && sum / finite < -25.0);
    assert(std::abs(frame->brightnessCelsius[75 * 250 + 125] - -30.11F) < 0.5F);
}

void testMosaicFrame() {
    // Filename clock: n0q_202610101730.png is 2026-10-10 17:30Z (1791653400).
    // Full keys, query suffixes, and garbage behave.
    assert(parseMosaicTime("n0q_202610101730.png") == 1791653400LL);
    assert(parseMosaicTime("GIS/uscomp/n0q_202610101730.png?x=1") == 1791653400LL);
    assert(!parseMosaicTime("n0q_2026101017.png").has_value());
    assert(!parseMosaicTime("n0q_202613011730.png").has_value());
    assert(!parseMosaicTime("n0r_202610101730.png").has_value());
    assert(!parseMosaicTime("").has_value());
    // Live world file: 0.005 deg pixels, UL (-126, 50).
    const std::string world("0.005\n0.0\n0.0\n-0.005\n-126.0\n50.0\n");
    const auto grid = parseWorldFile(world, 12200, 5400);
    assert(grid.has_value());
    assert(grid->nx == 12200 && grid->ny == 5400);
    assert(std::abs(grid->degreesPerPixelX - 0.005) < 1e-12);
    assert(std::abs(grid->degreesPerPixelY + 0.005) < 1e-12);
    assert(std::abs(grid->westLongitude + 126.0) < 1e-12);
    assert(std::abs(grid->northLatitude - 50.0) < 1e-12);
    // Corners land on the documented domain (126 W..65 W, 50 N..23 N).
    double lon = 0.0, lat = 0.0;
    mosaicCellLonLat(*grid, 0, 0, lon, lat);
    assert(std::abs(lon + 125.9975) < 1e-9 && std::abs(lat - 49.9975) < 1e-9);
    mosaicCellLonLat(*grid, 12199, 5399, lon, lat);
    assert(std::abs(lon + 65.0025) < 1e-6 && std::abs(lat - 23.0025) < 1e-6);
    // Rotated worlds, north-up flips, and degenerate sizes fail closed.
    assert(!parseWorldFile("0.005\n0.1\n0.0\n-0.005\n-126.0\n50.0\n", 100, 100).has_value());
    assert(!parseWorldFile("0.005\n0.0\n0.0\n0.005\n-126.0\n50.0\n", 100, 100).has_value());
    assert(!parseWorldFile(world, 0, 100).has_value());
    assert(!parseWorldFile("junk", 100, 100).has_value());
    mosaicCellLonLat(*grid, 12200, 0, lon, lat);
    assert(std::isnan(lon) && std::isnan(lat));
}

void testVadProfile() {
    // Known wind: 10 m/s east + 5 m/s north. Vr(az) = (u sin + v cos) cos(el).
    // True speed hypot(10,5) = 11.18; toward = atan2(10,5) = 63.4, FROM = 243.4.
    constexpr double kWindU = 10.0;
    constexpr double kWindV = 5.0;
    DecodedChunk chunk;
    chunk.radarId = "KTLX";
    for (int step = 0; step < 36; ++step) {
        const double azimuth = step * 10.0;
        const double azimuthRad = azimuth * 3.14159265358979323846 / 180.0;
        const double radial =
            (kWindU * std::sin(azimuthRad) + kWindV * std::cos(azimuthRad)) * std::cos(0.5 * 3.14159265358979323846 / 180.0);
        chunk.rays.push_back(syntheticVadRay(static_cast<float>(azimuth), 1, 0.5F, radial));
    }
    for (int step = 0; step < 36; ++step) {
        const double azimuth = step * 10.0;
        const double azimuthRad = azimuth * 3.14159265358979323846 / 180.0;
        const double radial =
            (kWindU * std::sin(azimuthRad) + kWindV * std::cos(azimuthRad)) * std::cos(15.0 * 3.14159265358979323846 / 180.0);
        chunk.rays.push_back(syntheticVadRay(static_cast<float>(azimuth), 2, 15.0F, radial));
    }
    const auto dealiased = processVelocity(chunk, {VelocityProcessingMode::Native, 25.0, 0.0, 0.0});
    const auto levels = fitVadProfile(chunk, dealiased, 300.0);
    assert(levels.size() == 2);
    for (const auto& level : levels) {
        assert(level.quality == VadQuality::Good);
        assert(level.gateCount == 720);
        assert(std::abs(level.speedMetersPerSecond - 11.18) < 0.3);
        assert(std::abs(level.directionFromDegrees - 243.4) < 2.0);
        assert(level.azimuthCoverageDegrees > 340.0);
        assert(level.rmseMetersPerSecond < 1.0);
    }
    // Height separates the tilts: low tilt near the radar level, 15-degree
    // tilt kilometres up. Catches a forgotten /cos(elevation) or a height
    // mixup between tilts.
    assert(levels[0].heightMslMeters < 3000.0);
    assert(levels[1].heightMslMeters > 4000.0);
    // Fail-closed inputs produce no output, never phantoms.
    assert(fitVadProfile(DecodedChunk{}, ProcessedVelocity{}).empty());
    assert(fitVadProfile(chunk, ProcessedVelocity{}).empty());
    // Single-ray tilt: degenerate azimuth spread goes singular, which must
    // surface as one Invalid row, not a phantom wind and not a dropped tilt.
    {
        DecodedChunk single;
        single.radarId = "KTLX";
        single.rays.push_back(syntheticVadRay(90.0F, 1, 0.5F, 8.0));
        // Widen the single ray so the gate-count guard cannot mask the
        // singular-matrix path.
        single.rays.back().fields.at(static_cast<std::size_t>(ProductKind::Velocity)).assign(64, 2016);
        const auto singleDealiased =
            processVelocity(single, {VelocityProcessingMode::Native, 25.0, 0.0, 0.0});
        const auto singleLevels = fitVadProfile(single, singleDealiased, 0.0);
        assert(singleLevels.size() == 1);
        assert(singleLevels.front().quality == VadQuality::Invalid);
    }
    // All-missing tilt: no usable gates, no row at all.
    {
        DecodedChunk missing;
        missing.radarId = "KTLX";
        DecodedRay ray;
        ray.azimuthDegrees = 0.0F;
        ray.elevationNumber = 1;
        ray.elevationDegrees = 0.5F;
        ray.fields.at(static_cast<std::size_t>(ProductKind::Velocity)).assign(20, 0);
        missing.rays.push_back(ray);
        const auto missingDealiased =
            processVelocity(missing, {VelocityProcessingMode::Native, 25.0, 0.0, 0.0});
        assert(fitVadProfile(missing, missingDealiased, 0.0).empty());
    }
    // Non-finite gates (inf) are skipped, never fitted.
    {
        ProcessedVelocity inf(1, std::vector<float>(32, std::numeric_limits<float>::infinity()));
        DecodedChunk infChunk;
        infChunk.radarId = "KTLX";
        DecodedRay ray;
        ray.azimuthDegrees = 0.0F;
        ray.elevationNumber = 1;
        ray.elevationDegrees = 0.5F;
        ray.fields.at(static_cast<std::size_t>(ProductKind::Velocity))
            .assign(32, static_cast<std::uint16_t>(2000));
        infChunk.rays.push_back(ray);
        assert(fitVadProfile(infChunk, inf, 0.0).empty());
    }
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

void testMessage31SentinelConvention() {
    // Minimal single-radial Message 31 layout (ICD 2620001), not a synthesised
    // volume: the smallest transfer/radial/data-block headers a DR moment needs.
    // Gates 0/1/66 are the three cases: below threshold, range folded, valid.
    constexpr std::size_t radialLength = 103;
    constexpr std::size_t momentOffset = 72;
    std::vector<std::byte> raw(radialLength);
    for (std::size_t index = 0; index < 4; ++index) {
        raw[index] = static_cast<std::byte>(static_cast<unsigned char>("KTLX"[index]));
    }
    writeBigEndian16(raw, 18, static_cast<std::uint16_t>(radialLength));
    writeBigEndian32(raw, 12, std::bit_cast<std::uint32_t>(12.5F));
    raw[20] = static_cast<std::byte>(1);
    raw[22] = static_cast<std::byte>(1);
    writeBigEndian32(raw, 24, std::bit_cast<std::uint32_t>(0.5F));
    writeBigEndian16(raw, 30, 1);
    writeBigEndian32(raw, 32, static_cast<std::uint32_t>(momentOffset));
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
    raw[momentOffset + 28] = static_cast<std::byte>(0);  // below threshold
    raw[momentOffset + 29] = static_cast<std::byte>(1);  // range folded
    raw[momentOffset + 30] = static_cast<std::byte>(66); // valid: 0.0 dBZ

    const auto decoded = decodeMessage31Chunk(raw, "KTLX");
    assert(decoded.messageCount == 1);
    assert(decoded.rays.size() == 1);
    assert(decoded.momentEncoding == MomentEncoding::NexradMessage31);
    const auto codes = momentSentinelCodes(decoded.momentEncoding);
    assert(codes.has_value());
    assert(codes->belowThreshold == 0);
    assert(codes->missing == 1);
    const auto index = static_cast<std::size_t>(ProductKind::Reflectivity);
    const auto& ray = decoded.rays.front();
    assert(ray.fields.at(index) == std::vector<std::uint16_t>({0, 1, 66}));
    assert(momentGateState(decoded.momentEncoding, 0) == MomentGateState::BelowThreshold);
    assert(momentGateState(decoded.momentEncoding, 1) == MomentGateState::Missing);
    assert(momentGateState(decoded.momentEncoding, 66) == MomentGateState::Valid);
    // The value path is untouched: both sentinel codes still decode to nullopt.
    assert(!nativeValue(0, ray.scaleByProduct.at(index), ray.offsetByProduct.at(index)).has_value());
    assert(!nativeValue(1, ray.scaleByProduct.at(index), ray.offsetByProduct.at(index)).has_value());
    const auto zero = nativeValue(66, ray.scaleByProduct.at(index), ray.offsetByProduct.at(index));
    assert(zero.has_value());
    assert(std::abs(*zero) < 1e-9);
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
    assert(decoded.momentEncoding == MomentEncoding::NexradMessage31);
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
            // Message 31 has no DPKP block; KDP arrives only from ODIM_H5
            // volumes, so a US chunk must never be expected to carry it.
            if (kind.kind == ProductKind::SpecificDifferentialPhase) continue;
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

void testRadarLightningFusionSplitCut() {
    // Real VCP split cuts: reflectivity-only surveillance (#1) below lowest
    // Doppler (#2). ROT must still fire (regression: old single-elevation code
    // gave velocitySamples=0 and 0/68 ROT on archive cases).
    DecodedChunk chunk;
    chunk.radarId = "KTLX";
    auto r1 = syntheticFusionRay(89.0F, ProductKind::Reflectivity, 156);
    r1.elevationNumber = 1;
    auto r2 = syntheticFusionRay(90.0F, ProductKind::Reflectivity, 156);
    r2.elevationNumber = 1;
    auto v1 = syntheticFusionRay(89.0F, ProductKind::Velocity, 89);
    v1.elevationNumber = 2;
    auto v2 = syntheticFusionRay(91.0F, ProductKind::Velocity, 169);
    v2.elevationNumber = 2;
    chunk.rays = {r1, r2, v1, v2};
    RadarFeatureConfig config;
    config.gridSizeDeg = 0.02;
    config.minimumReflectivityCells = 1;
    config.minimumAzimuthalShearPerSecond = 0.0005;
    const auto features = extractRadarStormFeatures(chunk, 35.333, -97.278, config);
    assert(!features.empty());
    assert(features.front().rotating);
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
    const std::string roots[] = {
        "tests/fixtures/",
        "../tests/fixtures/",
        "../../tests/fixtures/",
    };
    for (const auto& root : roots) {
        std::ifstream file(root + name, std::ios::binary);
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

void testRainfallDualPolRealArchive() {
    // Real KTLX 2013-05-20 (Moore) radial, sliced from an archive volume:
    // reflectivity, ZDR and RHOHV are real decoded gates. NEXRAD Message 31
    // has no DPKP block, so R(KDP) must be unavailable on the same bytes —
    // the real single-pol-moment case, not a synthetic stand-in.
    const auto bytes = readFixtureBytes("level2_radial_ktlx_20130520_vcp12.bin");
    assert(!bytes.empty());
    const auto chunk = decodeMessage31Chunk(bytes, "KTLX");
    assert(chunk.rays.size() == 1);
    const auto& ray = chunk.rays.front();
    assert(ray.has(ProductKind::Reflectivity));
    assert(ray.has(ProductKind::DifferentialReflectivity));
    assert(ray.has(ProductKind::CorrelationCoefficient));
    assert(!ray.has(ProductKind::SpecificDifferentialPhase));
    const std::vector<RainfallSample> samples{
        {&chunk, 0.0}, {&chunk, 300.0}, {&chunk, 600.0}, {&chunk, 900.0}};

    const auto zr = accumulateRainfall(samples, 370.0);
    assert(zr.summary.valid);
    assert(zr.summary.relationAvailable);
    assert(zr.summary.relation == RainfallRelation::MarshallPalmer);

    RainfallConfig zdrConfig;
    zdrConfig.relation = RainfallRelation::Zdr;
    const auto zdr = accumulateRainfall(samples, 370.0,
                                        std::numeric_limits<double>::quiet_NaN(), zdrConfig);
    assert(zdr.summary.valid);
    assert(zdr.summary.relationAvailable);
    assert(zdr.summary.relation == RainfallRelation::Zdr);
    assert(zdr.summary.depthGateCount > 0);
    assert(zdr.summary.nonMeteorologicalGateCount <= zr.summary.nonMeteorologicalGateCount);

    RainfallConfig kdpConfig;
    kdpConfig.relation = RainfallRelation::Kdp;
    const auto kdp = accumulateRainfall(samples, 370.0,
                                        std::numeric_limits<double>::quiet_NaN(), kdpConfig);
    assert(!kdp.summary.valid);
    assert(!kdp.summary.relationAvailable);
    assert(kdp.summary.relationUnavailableReason.find("Specific differential phase") !=
           std::string::npos);

    std::cout << "real KTLX Moore radial: Z-R peak=" << zr.summary.maximumMillimeters
              << " mm mean=" << zr.summary.meanMillimeters
              << " gates=" << zr.summary.depthGateCount
              << " no_vertical_check=" << zr.summary.noVerticalCheckGateCount
              << "; R(Z,ZDR) peak=" << zdr.summary.maximumMillimeters
              << " mm mean=" << zdr.summary.meanMillimeters
              << " gates=" << zdr.summary.depthGateCount
              << " input_clamped=" << zdr.summary.inputClampedGateCount
              << " rate_capped=" << zdr.summary.rateCappedGateCount
              << "; R(KDP): " << kdp.summary.relationUnavailableReason << "\n";
}

void testLevel2VolumeCoveragePattern() {
    // Real Message 31 radials sliced verbatim out of archive volumes under
    // data/ (provenance: docs/vcp-parsing.md). Byte 40 of each radial's RVOL
    // volume constant block is the Volume Coverage Pattern — ICD 2620001
    // Table XVII-E, confirmed by the published elevation table each pattern
    // scans (VCP 12/212: 14 cuts to 19.5 deg, VCP 35: 9 cuts to 6.4 deg).
    struct RealRadial {
        const char* fixture;
        const char* radarId;
        std::uint16_t pattern;
        float elevationDegrees;
        float azimuthDegrees;
    };
    const RealRadial radials[] = {
        // KTLX 2013-05-20 20:12:29Z (Moore tornado), 44-byte v1.0 RVOL block.
        {"level2_radial_ktlx_20130520_vcp12.bin", "KTLX", 12, 0.68F, 245.25F},
        // KPAH 2021-12-11 01:03:21Z (Mayfield supercell), 52-byte v3.0 block.
        {"level2_radial_kpah_20211211_vcp212.bin", "KPAH", 212, 0.57F, 252.22F},
        // KTLX 2026-10-09 16:37:06Z (clear air), 52-byte v3.0 block.
        {"level2_radial_ktlx_20261009_vcp35.bin", "KTLX", 35, 0.60F, 171.25F},
    };
    for (const auto& radial : radials) {
        const auto bytes = readFixtureBytes(radial.fixture);
        assert(!bytes.empty());
        const auto decoded = decodeMessage31Chunk(bytes, radial.radarId);
        assert(decoded.messageCount == 1);
        assert(decoded.rays.size() == 1);
        assert(!decoded.volumeCoveragePatternConflicting);
        assert(decoded.volumeCoveragePattern == radial.pattern);
        assert(std::abs(decoded.rays.front().elevationDegrees - radial.elevationDegrees) < 0.01F);
        assert(std::abs(decoded.rays.front().azimuthDegrees - radial.azimuthDegrees) < 0.01F);
        assert(decoded.rays.front().has(ProductKind::Reflectivity));
        // RVOL/RELV/RRAD are constant blocks every radial carries; a clean
        // radial must not report them as rejected blocks.
        assert(decoded.rejectedBlocks == 0);
    }
}

void testLevel2VolumeCoveragePatternSyntheticBytesOnly() {
    // Only the byte layout the Message 31 spec requires: a data header, a block
    // pointer and one volume constant block. The real radials above cover the
    // positive cases; these cover the fail-closed paths they cannot show.
    constexpr std::size_t kBlockOffset = 64;
    const auto buildRadial = [](const std::uint16_t pattern, const std::uint16_t declaredSize,
                                const bool includeBlock) {
        const std::size_t length = kBlockOffset + 44;
        std::vector<std::byte> raw(length);
        for (std::size_t index = 0; index < 4; ++index)
            raw[index] = static_cast<std::byte>(static_cast<unsigned char>("KTLX"[index]));
        writeBigEndian16(raw, 18, static_cast<std::uint16_t>(length));
        raw[20] = static_cast<std::byte>(1);
        raw[22] = static_cast<std::byte>(1);
        writeBigEndian32(raw, 24, std::bit_cast<std::uint32_t>(0.5F));
        writeBigEndian16(raw, 30, 1);
        writeBigEndian32(raw, 32, static_cast<std::uint32_t>(kBlockOffset));
        const char* tag = includeBlock ? "RVOL" : "XXXX";
        for (std::size_t index = 0; index < 4; ++index)
            raw[kBlockOffset + index] =
                static_cast<std::byte>(static_cast<unsigned char>(tag[index]));
        writeBigEndian16(raw, kBlockOffset + 4, declaredSize);
        writeBigEndian16(raw, kBlockOffset + 40, pattern);
        return raw;
    };

    // The pattern is volume-level: it decodes even when no moment block follows.
    const auto parsed = decodeMessage31Chunk(buildRadial(212, 44, true), "KTLX");
    assert(parsed.volumeCoveragePattern == 212);
    assert(!parsed.volumeCoveragePatternConflicting);
    assert(parsed.rejectedBlocks == 0);

    // Missing block, a block declaring less than the 44 spec bytes, a zero
    // pattern and an out-of-range pattern all report "no pattern" instead of a
    // number that would land on the console.
    assert(decodeMessage31Chunk(buildRadial(212, 44, false), "KTLX").volumeCoveragePattern == 0);
    const auto shortBlock = decodeMessage31Chunk(buildRadial(212, 40, true), "KTLX");
    assert(shortBlock.volumeCoveragePattern == 0);
    assert(shortBlock.rejectedBlocks == 1); // a malformed constant block is a real defect
    assert(decodeMessage31Chunk(buildRadial(0, 44, true), "KTLX").volumeCoveragePattern == 0);
    assert(decodeMessage31Chunk(buildRadial(1000, 44, true), "KTLX").volumeCoveragePattern == 0);

    // Two radials of one volume disagreeing: no single number describes the
    // volume, so nothing is reported. The 28-byte transfer gap is the stride
    // decodeMessage31Chunk expects between messages.
    auto mixed = buildRadial(212, 44, true);
    const auto second = buildRadial(35, 44, true);
    mixed.insert(mixed.end(), 28, std::byte{0});
    mixed.insert(mixed.end(), second.begin(), second.end());
    const auto conflicting = decodeMessage31Chunk(mixed, "KTLX");
    assert(conflicting.volumeCoveragePattern == 0);
    assert(conflicting.volumeCoveragePatternConflicting);

    // The record-by-record merge (archive: one volume split into bzip records;
    // live: one volume split into chunks) keeps a pattern only while every
    // piece agrees, and a conflict is sticky: a later agreeing piece must not
    // resurrect a number that describes only part of the volume.
    DecodedChunk accumulated;
    DecodedChunk first;
    first.volumeCoveragePattern = 212;
    mergeVolumeCoveragePattern(accumulated, first);
    assert(accumulated.volumeCoveragePattern == 212);
    assert(!accumulated.volumeCoveragePatternConflicting);
    DecodedChunk agreeing;
    agreeing.volumeCoveragePattern = 212;
    mergeVolumeCoveragePattern(accumulated, agreeing);
    assert(accumulated.volumeCoveragePattern == 212);
    DecodedChunk patternless; // a piece with no RVOL block must not clear the number
    mergeVolumeCoveragePattern(accumulated, patternless);
    assert(accumulated.volumeCoveragePattern == 212);
    DecodedChunk different;
    different.volumeCoveragePattern = 35;
    mergeVolumeCoveragePattern(accumulated, different);
    assert(accumulated.volumeCoveragePattern == 0);
    assert(accumulated.volumeCoveragePatternConflicting);
    mergeVolumeCoveragePattern(accumulated, first);
    assert(accumulated.volumeCoveragePattern == 0);
    assert(accumulated.volumeCoveragePatternConflicting);
}

void testMessage31RealArchiveChunkSentinelConvention() {
    // Real bytes, not a synthesised volume: 60 Message 31 radials cut verbatim
    // from KINX20130520_201333_V06 (Moore, OK outbreak), elevation 2, transfer
    // headers included. Only the offset and length were chosen.
    const auto bytes = readFixtureBytes("nexrad_m31_kinx_20130520.bin");
    if (bytes.empty()) {
        std::cout << "note: KINX fixture missing, skipping real-chunk asserts\n";
        return;
    }
    const auto chunk = decodeMessage31Chunk(bytes, "KINX");
    assert(chunk.momentEncoding == MomentEncoding::NexradMessage31);
    assert(chunk.rays.size() == chunk.messageCount);
    assert(chunk.messageCount == 60);
    const auto codes = momentSentinelCodes(chunk.momentEncoding);
    assert(codes.has_value());
    assert(codes->belowThreshold == 0);
    assert(codes->missing == 1);

    std::size_t below = 0, folded = 0, valid = 0;
    const auto index = static_cast<std::size_t>(ProductKind::Reflectivity);
    for (const auto& ray : chunk.rays) {
        for (const auto encoded : ray.fields.at(index)) {
            switch (momentGateState(chunk.momentEncoding, encoded)) {
            case MomentGateState::BelowThreshold: ++below; break;
            case MomentGateState::Missing: ++folded; break;
            case MomentGateState::Valid: ++valid; break;
            case MomentGateState::Undetermined: assert(false); break;
            }
        }
    }
    std::cout << "KINX real sweep sentinels: below=" << below << " folded=" << folded
              << " valid=" << valid << " gates\n";
    // A real sweep mixes all three: dry air below threshold, range-folded gates
    // that carry no measurement, and echoes.
    assert(below > 0);
    assert(folded > 0);
    assert(valid > 0);
}

void testOdimSentinelConvention() {
    // ODIM packs the opposite way round from Message 31: 0 = missing (nodata),
    // 1 = below threshold (undetect). Proven on both real DWD/OPERA fixtures.
    if (!odimReaderAvailable()) return;
    const auto index = static_cast<std::size_t>(ProductKind::Reflectivity);

    const auto deneuBytes = readFixtureBytes("odim_deneu_dbzh.h5");
    if (deneuBytes.empty()) {
        std::cout << "note: deneu fixture missing, skipping sentinel asserts\n";
        return;
    }
    std::string error;
    const auto deneu = readOdimPvol(deneuBytes, "deneu", &error);
    assert(deneu.has_value());
    assert(deneu->momentEncoding == MomentEncoding::Odim);
    assert(momentSentinelCodes(deneu->momentEncoding).has_value());
    assert(momentSentinelCodes(deneu->momentEncoding)->missing == 0);
    assert(momentSentinelCodes(deneu->momentEncoding)->belowThreshold == 1);
    std::size_t deneuMissing = 0, deneuBelow = 0, deneuValid = 0;
    for (const auto& ray : deneu->rays) {
        for (const auto encoded : ray.fields.at(index)) {
            switch (momentGateState(deneu->momentEncoding, encoded)) {
            case MomentGateState::Missing: ++deneuMissing; break;
            case MomentGateState::BelowThreshold: ++deneuBelow; break;
            case MomentGateState::Valid: ++deneuValid; break;
            case MomentGateState::Undetermined: assert(false); break;
            }
        }
    }
    // The DWD fixture is a dense clear-air sweep: every gate is undetect, so it
    // pins the below-threshold code but cannot show nodata.
    std::cout << "DWD deneu ODIM sentinels: missing=" << deneuMissing << " below=" << deneuBelow
              << " valid=" << deneuValid << " gates\n";
    assert(deneuMissing == 0);
    assert(deneuBelow > 0);
    assert(deneuValid == 0);

    const auto nohurBytes = readFixtureBytes("odim_nohur_dbzh.h5");
    if (nohurBytes.empty()) {
        std::cout << "note: nohur fixture missing, skipping mixed-sweep asserts\n";
        return;
    }
    const auto nohur = readOdimPvol(nohurBytes, "nohur", &error);
    assert(nohur.has_value());
    assert(nohur->momentEncoding == MomentEncoding::Odim);
    std::size_t nohurMissing = 0, nohurBelow = 0, nohurValid = 0;
    for (const auto& ray : nohur->rays) {
        for (const auto encoded : ray.fields.at(index)) {
            switch (momentGateState(nohur->momentEncoding, encoded)) {
            case MomentGateState::Missing: ++nohurMissing; break;
            case MomentGateState::BelowThreshold: ++nohurBelow; break;
            case MomentGateState::Valid: ++nohurValid; break;
            case MomentGateState::Undetermined: assert(false); break;
            }
        }
    }
    std::cout << "OPERA nohur ODIM sentinels: missing=" << nohurMissing << " below=" << nohurBelow
              << " valid=" << nohurValid << " gates\n";
    assert(nohurBelow > 0);
    assert(nohurValid > 0);
    // Whatever the fixtures hold, the same numeric code must classify the other
    // way under Message 31: that is the bug this discriminator closes.
    assert(momentGateState(MomentEncoding::NexradMessage31, 0) == MomentGateState::BelowThreshold);
    assert(momentGateState(MomentEncoding::NexradMessage31, 1) == MomentGateState::Missing);
}

void testOdimMinimalContainerMixedSentinels() {
    // Minimal ODIM_H5 container per the ODIM spec (dataset1/data1/what + where
    // plus a 1-ray, 4-gate DBZH array), NOT a radar volume: it exists only to
    // exercise nodata, which neither real fixture contains (both are dense
    // sweeps where every gate is either undetect or a value). Gates are
    // 255 = nodata, 0 = undetect, 20 dBZ, 30 dBZ.
    if (!odimReaderAvailable()) return;
    const auto bytes = readFixtureBytes("odim_minimal_mixed.h5");
    if (bytes.empty()) {
        std::cout << "note: minimal ODIM fixture missing, skipping nodata asserts\n";
        return;
    }
    std::string error;
    const auto chunk = readOdimPvol(bytes, "TEST", &error);
    assert(chunk.has_value());
    assert(chunk->momentEncoding == MomentEncoding::Odim);
    assert(chunk->rays.size() == 1);
    const auto index = static_cast<std::size_t>(ProductKind::Reflectivity);
    const auto& ray = chunk->rays.front();
    // 20 dBZ -> 20*2+66 = 106, 30 dBZ -> 126, the same packing the reader uses.
    assert(ray.fields.at(index) == std::vector<std::uint16_t>({0, 1, 106, 126}));
    assert(momentGateState(chunk->momentEncoding, 0) == MomentGateState::Missing);
    assert(momentGateState(chunk->momentEncoding, 1) == MomentGateState::BelowThreshold);
    assert(momentGateState(chunk->momentEncoding, 106) == MomentGateState::Valid);
    // Both sentinel codes still read as "no value" downstream.
    assert(!nativeValue(0, ray.scaleByProduct.at(index), ray.offsetByProduct.at(index)).has_value());
    assert(!nativeValue(1, ray.scaleByProduct.at(index), ray.offsetByProduct.at(index)).has_value());
    const auto value = nativeValue(106, ray.scaleByProduct.at(index), ray.offsetByProduct.at(index));
    assert(value.has_value());
    assert(std::abs(*value - 20.0) < 1e-9);
}

void testMomentSentinelConventionFlipsBetweenFormats() {
    // Same gate codes, two formats: 0 and 1 swap meaning. A chunk with no
    // recorded convention must not pretend to know which is which.
    for (const auto encoding : {MomentEncoding::NexradMessage31, MomentEncoding::Odim}) {
        assert(momentGateState(encoding, 66) == MomentGateState::Valid);
        assert(momentGateState(encoding, 65534) == MomentGateState::Valid);
    }
    assert(momentGateState(MomentEncoding::NexradMessage31, 0) == MomentGateState::BelowThreshold);
    assert(momentGateState(MomentEncoding::NexradMessage31, 1) == MomentGateState::Missing);
    assert(momentGateState(MomentEncoding::Odim, 0) == MomentGateState::Missing);
    assert(momentGateState(MomentEncoding::Odim, 1) == MomentGateState::BelowThreshold);
    assert(!momentSentinelCodes(MomentEncoding::Unknown).has_value());
    assert(momentGateState(MomentEncoding::Unknown, 0) == MomentGateState::Undetermined);
    assert(momentGateState(MomentEncoding::Unknown, 1) == MomentGateState::Undetermined);
    assert(momentGateState(MomentEncoding::Unknown, 66) == MomentGateState::Valid);
}

void testSpillCarriesMomentEncoding() {
    DecodedChunk chunk;
    chunk.radarId = "KINX";
    chunk.rays.push_back(syntheticDerivedRay(90.0F, 1, 0.5F, 30.0));
    const auto path =
        (std::filesystem::temp_directory_path() / "cappedweather-spill-encoding.cwdv").string();
    chunk.momentEncoding = MomentEncoding::NexradMessage31;
    assert(spillVolume(chunk, path));
    {
        const auto loaded = loadSpilledVolume(path);
        assert(loaded);
        assert(loaded->momentEncoding == MomentEncoding::NexradMessage31);
    }
    chunk.momentEncoding = MomentEncoding::Odim;
    assert(spillVolume(chunk, path));
    {
        const auto loaded = loadSpilledVolume(path);
        assert(loaded);
        assert(loaded->momentEncoding == MomentEncoding::Odim);
    }
    // Unknown writes no trailer, so files from before the field existed still
    // load and stay Unknown instead of assuming a format.
    chunk.momentEncoding = MomentEncoding::Unknown;
    assert(spillVolume(chunk, path));
    {
        const auto loaded = loadSpilledVolume(path);
        assert(loaded);
        assert(loaded->momentEncoding == MomentEncoding::Unknown);
    }
    std::remove(path.c_str());
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

void testOdimAzimuthConvention() {
    // Regression: the reader once used a1gate (a RANGE index) as the azimuth
    // offset, rotating whole OPERA sweeps by 100+ degrees (DWD echoes drawn
    // in the wrong direction). Azimuths must follow how/startazA (+stopazA
    // midpoint) per ray, or uniform-from-north when the how group is absent.
    if (!odimReaderAvailable()) return;
    const auto deneu = readFixtureBytes("odim_deneu_dbzh.h5");
    if (deneu.empty()) {
        std::cout << "note: deneu fixture missing, skipping azimuth asserts\n";
        return;
    }
    // Real DWD SCAN file: 360 rays, per-ray startazA ~= index, a1gate = 104
    // (the old code put ray 0 at 104 degrees).
    std::string error;
    const auto chunk = readOdimPvol(deneu, "deneu", &error);
    assert(chunk.has_value());
    assert(chunk->rays.size() == 360);
    for (const std::size_t ray : {0, 45, 90, 135, 180, 225, 270, 315, 359}) {
        const double azimuth = chunk->rays[ray].azimuthDegrees;
        double distance = std::fabs(azimuth - static_cast<double>(ray));
        if (distance > 180.0) distance = 360.0 - distance;
        assert(distance < 1.0);
    }
    // Fixture without a how group (nohur): uniform from north, one ray per
    // resolution step, no a1gate rotation (dataset1: a1gate 634, 720 rays).
    const auto nohur = readFixtureBytes("odim_nohur_dbzh.h5");
    if (nohur.empty()) {
        std::cout << "note: nohur fixture missing, skipping uniform asserts\n";
        return;
    }
    const auto flat = readOdimPvol(nohur, "nohur", &error);
    assert(flat.has_value());
    for (const auto& ray : flat->rays) {
        if (ray.elevationNumber != 1) continue;
        const double expected =
            std::fmod(static_cast<double>(&ray - flat->rays.data()) * 0.5, 360.0);
        double distance = std::fabs(static_cast<double>(ray.azimuthDegrees) - expected);
        if (distance > 180.0) distance = 360.0 - distance;
        assert(distance < 0.01);
    }
}

void testOdimNyquistFromHow() {
    // Regression: OPERA volumes used to unfold at the 25 m/s panel default
    // while C-band scans fold as low as ~6 m/s (HRGOL NI 6.1/8.3), leaving
    // fold boundaries in the grid that pass meso gates at close range.
    // Rays must carry their dataset how/NI so unfolding uses the true fold.
    if (!odimReaderAvailable()) return;
    const auto deneu = readFixtureBytes("odim_deneu_dbzh.h5");
    if (deneu.empty()) {
        std::cout << "note: deneu fixture missing, skipping NI asserts\n";
        return;
    }
    std::string error;
    const auto chunk = readOdimPvol(deneu, "deneu", &error);
    assert(chunk.has_value());
    // DWD fixture dataset1 how/NI is 31.97 m/s.
    for (const auto& ray : chunk->rays) {
        assert(ray.nyquistMetersPerSecond > 0.0F);
        assert(std::abs(ray.nyquistMetersPerSecond - 31.97F) < 0.05F);
    }
}

// ---------------------------------------------------------------------------
// OPERA dual-pol decode. Three real DWD open-data sweeps (PCP scan, NOD deneu,
// 2026-10-20T15:35:33Z, one moment per object) pin the mapping against real
// published bytes; a hand-built container pins the multi-moment layout and the
// U-prefixed aliases. Provenance and the assumed 2027 layout live in
// docs/europe-sources.md ("OPERA dual-pol: what exists and what 2027 adds").
struct DualPolSweepCase {
    const char* fixture;
    ProductKind kind;
    // Physical values of ray 0, gates 0..2 as published in the source file.
    double gate0;
    double gate1;
    double gate2;
    // Round-trip tolerance: half a packing step for this product.
    double tolerance;
    double minimumBound;
    double maximumBound;
};

void testOdimDualPolAbsentOnSinglePolVolumes() {
    // updateProductAvailability() greys a product when no ray in the frame
    // carries it. That gate is only honest if a single-pol volume reports no
    // dual-pol field at all: both real DWD fixtures carry DBZH and nothing
    // else, so ZDR/CC/PHI must stay absent (and the panels stay disabled).
    if (!odimReaderAvailable()) return;
    for (const char* fixture : {"odim_deneu_dbzh.h5", "odim_nohur_dbzh.h5"}) {
        const auto bytes = readFixtureBytes(fixture);
        if (bytes.empty()) {
            std::cout << "note: " << fixture << " missing, skipping dual-pol absence asserts\n";
            continue;
        }
        std::string error;
        const auto chunk = readOdimPvol(bytes, "deneu", &error);
        assert(chunk.has_value());
        for (const auto& ray : chunk->rays) {
            assert(ray.has(ProductKind::Reflectivity));
            assert(!ray.has(ProductKind::DifferentialReflectivity));
            assert(!ray.has(ProductKind::CorrelationCoefficient));
            assert(!ray.has(ProductKind::DifferentialPhase));
            assert(!ray.has(ProductKind::Velocity));
        }
    }
}

void testOdimDualPolFromRealDwdSweeps() {
    // Each file is one real DWD dual-pol moment: UZDR, URHOHV, UPHIDP, 360
    // rays x 600 gates at 0.802 deg, rstart 0, rscale 250 m. No gate value is
    // altered; the asserts below are the published ray 0 gates and the
    // published value range.
    if (!odimReaderAvailable()) return;
    const DualPolSweepCase cases[] = {
        {"odim_deneu_uzdr.h5", ProductKind::DifferentialReflectivity, 0.3472, 5.1365, -1.3111,
         0.002, -32.001, 32.001},
        {"odim_deneu_urhohv.h5", ProductKind::CorrelationCoefficient, 0.8141, 0.8496, 0.8414,
         0.0001, 0.0, 1.0001},
        {"odim_deneu_uphidp.h5", ProductKind::DifferentialPhase, 61.9245, 46.4221, -43.9061,
         0.02, -180.001, 180.001},
    };
    for (const auto& c : cases) {
        const auto bytes = readFixtureBytes(c.fixture);
        if (bytes.empty()) {
            std::cout << "note: " << c.fixture << " missing, skipping dual-pol decode asserts\n";
            continue;
        }
        std::string error;
        const auto chunk = readOdimPvol(bytes, "deneu", &error);
        assert(chunk.has_value());
        assert(chunk->rays.size() == 360);
        const auto index = static_cast<std::size_t>(c.kind);
        std::size_t validGates = 0;
        double minimum = 1.0e9;
        double maximum = -1.0e9;
        for (const auto& ray : chunk->rays) {
            assert(ray.has(c.kind));
            assert(!ray.has(ProductKind::Reflectivity));  // one moment per file
            assert(!ray.has(ProductKind::Velocity));
            assert(!ray.has(ProductKind::SpectrumWidth));
            for (const ProductKind other : {ProductKind::DifferentialReflectivity,
                                            ProductKind::CorrelationCoefficient,
                                            ProductKind::DifferentialPhase}) {
                if (other != c.kind) assert(!ray.has(other));
            }
            assert(ray.elevationNumber == 1);
            assert(std::fabs(ray.elevationDegrees - 0.802F) < 0.001F);
            assert(ray.gateCount == 600);
            assert(ray.firstGateByProduct.at(index) == 0);
            assert(ray.gateSpacingByProduct.at(index) == 250);
            assert(ray.scaleByProduct.at(index) > 0.0F);
            for (const auto encoded : ray.fields.at(index)) {
                const auto value = nativeValue(encoded, ray.scaleByProduct.at(index),
                                               ray.offsetByProduct.at(index));
                if (!value.has_value()) continue;
                ++validGates;
                minimum = std::min(minimum, *value);
                maximum = std::max(maximum, *value);
            }
        }
        assert(minimum >= c.minimumBound);
        assert(maximum <= c.maximumBound);
        // DWD publishes no nodata/undetect gates in this sweep.
        assert(validGates == 360 * 600);
        // Azimuths come from the per-ray how/startazA+stopazA midpoint, not
        // from a1gate (33 in this sweep) and not from a uniform guess.
        assert(std::fabs(chunk->rays[0].azimuthDegrees - 0.5219) < 0.01);
        assert(std::fabs(chunk->rays[90].azimuthDegrees - 90.5219) < 0.01);
        assert(std::fabs(chunk->rays[359].azimuthDegrees - 359.5193) < 0.01);
        // Published ray 0 gates survive the 16-bit re-encode within half a
        // packing step (no gate is invented, none is shifted).
        const auto& firstRay = chunk->rays.front();
        const double expected[3] = {c.gate0, c.gate1, c.gate2};
        for (std::size_t gate = 0; gate < 3; ++gate) {
            const auto value = nativeValue(firstRay.fields.at(index).at(gate),
                                           firstRay.scaleByProduct.at(index),
                                           firstRay.offsetByProduct.at(index));
            assert(value.has_value());
            assert(std::fabs(*value - expected[gate]) < c.tolerance);
        }
        std::cout << "odim dual-pol " << c.fixture << ": rays=" << chunk->rays.size()
                  << " gates=" << chunk->rays.front().gateCount << " valid=" << validGates
                  << " range=[" << minimum << ", " << maximum << "]"
                  << " ray0_gate0=" << expected[0] << "\n";
    }
}

void testOdimDualPolSpecShapedDataset() {
    // NOT an observation. odim_specshape_dualpol_3ray.h5 is a minimal ODIM_H5
    // container constructed byte by byte (see make_odim_dualpol_specshape.py):
    // dataset1 holds DBZH+ZDR+RHOHV+PHIDP in one sweep, dataset2 holds the
    // U-prefixed (unfiltered) aliases, dataset3 holds UKDP only (raw 100 at
    // gain 0.01 -> 1.0 deg/km).
    if (!odimReaderAvailable()) return;
    const auto bytes = readFixtureBytes("odim_specshape_dualpol_3ray.h5");
    if (bytes.empty()) {
        std::cout << "note: spec-shaped dual-pol fixture missing, skipping layout asserts\n";
        return;
    }
    std::string error;
    const auto chunk = readOdimPvol(bytes, "specc", &error);
    assert(chunk.has_value());
    // Every dataset now has a product slot, including the KDP-only one.
    assert(chunk->rejectedBlocks == 0);
    assert(chunk->rays.size() == 9);
    const auto refIndex = static_cast<std::size_t>(ProductKind::Reflectivity);
    const auto zdrIndex = static_cast<std::size_t>(ProductKind::DifferentialReflectivity);
    const auto ccIndex = static_cast<std::size_t>(ProductKind::CorrelationCoefficient);
    const auto phiIndex = static_cast<std::size_t>(ProductKind::DifferentialPhase);
    const auto kdpIndex = static_cast<std::size_t>(ProductKind::SpecificDifferentialPhase);
    auto decoded = [&](const std::size_t ray, const std::size_t index, const std::size_t gate) {
        const auto& r = chunk->rays.at(ray);
        return nativeValue(r.fields.at(index).at(gate), r.scaleByProduct.at(index),
                           r.offsetByProduct.at(index));
    };
    // dataset1: standard names, elangle 1.5, startazA 0/120/240.
    const float expectedAzimuth[3] = {60.0F, 180.0F, 300.0F};
    for (std::size_t ray = 0; ray < 3; ++ray) {
        const auto& r = chunk->rays.at(ray);
        assert(r.elevationNumber == 1);
        assert(std::fabs(r.elevationDegrees - 1.5F) < 0.001F);
        // Wrap-aware midpoint: the third ray runs 240 -> 0 deg, so its center
        // is 300, not 370.
        assert(std::fabs(r.azimuthDegrees - expectedAzimuth[ray]) < 0.01F);
        assert(r.gateCount == 4);
        assert(r.firstGateByProduct.at(refIndex) == 0);
        assert(r.gateSpacingByProduct.at(refIndex) == 500);
        assert(r.has(ProductKind::Reflectivity));
        assert(r.has(ProductKind::DifferentialReflectivity));
        assert(r.has(ProductKind::CorrelationCoefficient));
        assert(r.has(ProductKind::DifferentialPhase));
        assert(!r.has(ProductKind::Velocity));
        // DBZH: gain 0.5 offset -32 -> 100 = 18 dBZ, 200 = 68 dBZ, then
        // undetect (0) and nodata (65535).
        assert(std::fabs(*decoded(ray, refIndex, 0) - 18.0) < 0.26);
        assert(std::fabs(*decoded(ray, refIndex, 1) - 68.0) < 0.26);
        assert(!decoded(ray, refIndex, 2).has_value());
        assert(!decoded(ray, refIndex, 3).has_value());
        // ZDR: gain 0.01 offset -5 -> 1000 = 5 dB, 0 = undetect,
        // 65535 = nodata, 500 = 0 dB.
        assert(std::fabs(*decoded(ray, zdrIndex, 0) - 5.0) < 0.002);
        assert(!decoded(ray, zdrIndex, 1).has_value());
        assert(!decoded(ray, zdrIndex, 2).has_value());
        assert(std::fabs(*decoded(ray, zdrIndex, 3) - 0.0) < 0.002);
        // RHOHV: gain 1/65535 -> 65534 ~ 0.99998, 32768 ~ 0.5.
        assert(std::fabs(*decoded(ray, ccIndex, 0) - 65534.0 / 65535.0) < 0.0001);
        assert(!decoded(ray, ccIndex, 1).has_value());
        assert(!decoded(ray, ccIndex, 2).has_value());
        assert(std::fabs(*decoded(ray, ccIndex, 3) - 32768.0 / 65535.0) < 0.0001);
        // PHIDP: gain 0.01 offset -180 -> 18000 = 0 deg, 27000 = 90 deg.
        assert(std::fabs(*decoded(ray, phiIndex, 0) - 0.0) < 0.02);
        assert(!decoded(ray, phiIndex, 1).has_value());
        assert(!decoded(ray, phiIndex, 2).has_value());
        assert(std::fabs(*decoded(ray, phiIndex, 3) - 90.0) < 0.02);
    }
    // dataset2: U-prefixed aliases, elangle 2.5, startazA 10/130/250.
    for (std::size_t ray = 3; ray < 6; ++ray) {
        const auto& r = chunk->rays.at(ray);
        assert(r.elevationNumber == 2);
        assert(std::fabs(r.elevationDegrees - 2.5F) < 0.001F);
        assert(!r.has(ProductKind::Reflectivity));  // no DBZH in this sweep
        assert(r.has(ProductKind::DifferentialReflectivity));
        assert(r.has(ProductKind::CorrelationCoefficient));
        assert(r.has(ProductKind::DifferentialPhase));
        assert(std::fabs(*decoded(ray, zdrIndex, 0) - 3.0) < 0.002);
        assert(std::fabs(*decoded(ray, zdrIndex, 3) - 2.0) < 0.002);
        assert(std::fabs(*decoded(ray, ccIndex, 0) - 32768.0 / 65535.0) < 0.0001);
        assert(std::fabs(*decoded(ray, ccIndex, 3) - 65534.0 / 65535.0) < 0.0001);
        assert(std::fabs(*decoded(ray, phiIndex, 0) + 90.0) < 0.02);
        assert(std::fabs(*decoded(ray, phiIndex, 3) - 0.0) < 0.02);
    }
    assert(std::fabs(chunk->rays.at(4).azimuthDegrees - 190.0F) < 0.01F);
    assert(std::fabs(chunk->rays.at(5).azimuthDegrees - 310.0F) < 0.01F);
    // Neither dataset1 nor dataset2 carries KDP, so its slot must stay empty:
    // absence is absence, never a zero-filled moment.
    for (std::size_t ray = 0; ray < 6; ++ray) {
        assert(!chunk->rays.at(ray).has(ProductKind::SpecificDifferentialPhase));
    }
    // dataset3: UKDP-only sweep, elangle 3.5, startazA 20/140/260. It decodes
    // now that ProductKind::SpecificDifferentialPhase exists.
    for (std::size_t ray = 6; ray < 9; ++ray) {
        const auto& r = chunk->rays.at(ray);
        assert(r.elevationNumber == 3);
        assert(std::fabs(r.elevationDegrees - 3.5F) < 0.001F);
        assert(r.gateCount == 4);
        assert(r.has(ProductKind::SpecificDifferentialPhase));
        // Honest absence: a UKDP-only container puts gates ONLY into the KDP
        // slot; nothing smears into DBZH/ZDR/RHOHV/PHIDP/VEL.
        assert(!r.has(ProductKind::Reflectivity));
        assert(!r.has(ProductKind::Velocity));
        assert(!r.has(ProductKind::DifferentialReflectivity));
        assert(!r.has(ProductKind::CorrelationCoefficient));
        assert(!r.has(ProductKind::DifferentialPhase));
        // UKDP: gain 0.01 offset 0 -> raw 100 = 1.0 deg/km, raw 200 = 2.0.
        // Packing step is 1/1023.9375 = 0.000977 deg/km, so half a step is
        // 0.00049: anything coarser would mean the gate was clipped.
        const auto kdp0 = decoded(ray, kdpIndex, 0);
        assert(kdp0.has_value());
        assert(std::fabs(*kdp0 - 1.0) < 0.00049);
        // raw 0 is undetect (below threshold), raw 65535 is nodata: both are
        // missing, never a clipped -32/+32 value.
        assert(!decoded(ray, kdpIndex, 1).has_value());
        assert(!decoded(ray, kdpIndex, 2).has_value());
        const auto kdp3 = decoded(ray, kdpIndex, 3);
        assert(kdp3.has_value());
        assert(std::fabs(*kdp3 - 2.0) < 0.00049);
        // Gates are real codes in the shared 16-bit convention, not sentinels.
        assert(r.fields.at(kdpIndex).at(0) >= 2);
        assert(r.fields.at(kdpIndex).at(3) >= 2);
    }
    assert(std::fabs(chunk->rays.at(7).azimuthDegrees - 200.0F) < 0.01F);
    assert(std::fabs(chunk->rays.at(8).azimuthDegrees - 320.0F) < 0.01F);
    std::cout << "odim dual-pol spec-shaped: rays=" << chunk->rays.size()
              << " rejected_datasets=" << chunk->rejectedBlocks
              << " kdp_gate0=" << *decoded(6, kdpIndex, 0)
              << " kdp_gate3=" << *decoded(6, kdpIndex, 3) << "\n";
}

void testSlantRangeVerticalBeam() {
    const double slant = slantRangeForSurfaceDistance(10000.0, 90.0);
    assert(std::isfinite(slant));
    assert(slant > 9000.0 && slant < 11000.0);
}

void testLightningJumpSanitizedAndFarFuturePoll() {
    LightningJumpConfig zero;
    zero.rateResolutionSec = 0.0;
    assert(zero.sanitized().rateResolutionSec == 60.0);
    LightningJumpConfig negative;
    negative.rateResolutionSec = -5.0;
    assert(negative.sanitized().rateResolutionSec == 60.0);

    const std::int64_t t0 = 1784556000;
    LightningJumpDetector detector;
    detector.addFlash(static_cast<double>(t0));
    detector.addFlash(static_cast<double>(t0 + 1));
    const std::int64_t farFuture = t0 + static_cast<std::int64_t>(10) * 365 * 24 * 3600;
    const auto events = detector.poll(farFuture);
    assert(events.size() < 2000);
}

void testStormTrackerClearAndFixCap() {
    StormTracker tracker;
    auto tracks = tracker.update({syntheticFeature(35.0, -97.0, 60.0, true)}, 1000.0);
    assert(!tracks.empty());
    tracker.clear();
    tracks = tracker.update({syntheticFeature(35.0, -97.0, 60.0, true)}, 2000.0);
    assert(tracks.size() == 1);
    assert(tracks.front().id == 1);

    StormTracker capped;
    for (int i = 0; i < 100; ++i) {
        capped.update({syntheticFeature(35.0, -97.0, 50.0, false)}, 1000.0 + i * 60.0);
    }
    assert(!capped.tracks().empty());
    assert(capped.tracks().front().fixes.size() <= 64);
}

void testStormTrackerMotionPrediction() {
    // Steady eastward mover: second update seeds velocity, third must follow
    // the prediction, not just the tail.
    StormTracker tracker;
    tracker.update({syntheticFeature(35.0, -97.0, 60.0, true)}, 1000.0);
    tracker.update({syntheticFeature(35.0, -96.9, 60.0, true)}, 1300.0);
    const auto& seeded = tracker.tracks();
    assert(seeded.size() == 1);
    assert(seeded.front().fixes.size() == 2);
    // Velocity should point east (lon increasing).
    assert(seeded.front().velocityLonDegPerSec > 0.0);
    double plat = 0.0, plon = 0.0;
    StormTracker::predictPosition(seeded.front(), 1600.0, plat, plon);
    assert(plon > -96.9);  // predicted ahead of the tail
    // Third volume continues the line: same id must survive.
    const int id = seeded.front().id;
    auto tracks = tracker.update({syntheticFeature(35.0, -96.8, 60.0, true)}, 1600.0);
    assert(tracks.size() == 1);
    assert(tracks.front().id == id);
    assert(tracks.front().fixes.size() == 3);
}

void testStormTrackerSplitParent() {
    // One cell splits: the divergent piece births with parentId set.
    StormTracker tracker;
    tracker.update({syntheticFeature(35.0, -97.0, 60.0, true)}, 1000.0);
    auto tracks = tracker.update({syntheticFeature(35.0, -96.9, 60.0, true),
                                  syntheticFeature(35.05, -96.85, 55.0, false)},
                                 1300.0);
    assert(tracks.size() == 2);
    bool foundChild = false;
    for (const auto& track : tracks) {
        if (track.fixes.size() == 1 && track.parentId != 0) foundChild = true;
    }
    assert(foundChild);
}

void testStormTrackerIntensityGate() {
    // Same motion, wildly different intensity still matches when close
    // (dBZ penalty capped), but a teleport + intensity jump births new.
    StormTracker tracker;
    tracker.update({syntheticFeature(35.0, -97.0, 60.0, true)}, 1000.0);
    auto tracks = tracker.update({syntheticFeature(35.0, -96.95, 45.0, true)}, 1300.0);
    assert(tracks.size() == 1);
    assert(tracks.front().fixes.size() == 2);
}

// --- Circulation detector (TVS/meso) synthetic-volume tests ---
// Geometry: radar KTLX-ish (35.333,-97.278,370m); two rays per tilt at
// 90/91 deg (1 deg sep); firstGate 10000m, spacing 250m, 200 gates so
// gates 150-160 sit at ~47-50 km slant where meso/TVS thresholds and
// 0.5/4.0-deg tilt depth (~3.6 km) both pass. Detection consumes the
// dealiased float grid directly; encoded velocity fields only need to
// exist so ray.has(Velocity) is true.

DecodedRay syntheticCoupletRay(const float azimuth, const std::uint8_t elevationNumber,
                               const float elevationDegrees, const double reflectivityDbz) {
    DecodedRay ray;
    ray.azimuthDegrees = azimuth;
    ray.azimuthResolutionDegrees = 1.0F;
    ray.elevationNumber = elevationNumber;
    ray.elevationDegrees = elevationDegrees;
    constexpr std::size_t gates = 200;
    const auto reflIndex = static_cast<std::size_t>(ProductKind::Reflectivity);
    const auto velIndex = static_cast<std::size_t>(ProductKind::Velocity);
    const auto reflEncoded =
        static_cast<std::uint16_t>(std::lround(reflectivityDbz * 2.0 + 66.0));
    ray.fields.at(reflIndex).assign(gates, reflEncoded);
    ray.firstGateByProduct.at(reflIndex) = 10000;
    ray.gateSpacingByProduct.at(reflIndex) = 250;
    ray.scaleByProduct.at(reflIndex) = 2.0F;
    ray.offsetByProduct.at(reflIndex) = 66.0F;
    ray.fields.at(velIndex).assign(gates, 129); // placeholder; grid carries truth
    ray.firstGateByProduct.at(velIndex) = 10000;
    ray.gateSpacingByProduct.at(velIndex) = 250;
    ray.scaleByProduct.at(velIndex) = 2.0F;
    ray.offsetByProduct.at(velIndex) = 129.0F;
    return ray;
}

std::vector<std::vector<float>> coupletGrid(const DecodedChunk& chunk, const float neg,
                                            const float pos, const std::size_t firstGate,
                                            const std::size_t lastGate) {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    std::vector<std::vector<float>> grid(chunk.rays.size());
    for (std::size_t r = 0; r < chunk.rays.size(); ++r) {
        grid[r].assign(chunk.rays[r].fields.at(static_cast<std::size_t>(ProductKind::Velocity)).size(),
                       nan);
    }
    // Even ray index = negative side, odd = positive side (pairs at 90/91).
    for (std::size_t r = 0; r < chunk.rays.size(); ++r) {
        const float value = (r % 2 == 0) ? neg : pos;
        for (std::size_t g = firstGate; g <= lastGate && g < grid[r].size(); ++g) {
            grid[r][g] = value;
        }
    }
    return grid;
}

DecodedChunk twoTiltCoupletChunk(const double reflDbz, const float lowElev = 0.5F,
                                 const float highElev = 4.0F, const float azA = 90.0F,
                                 const float azB = 91.0F) {
    DecodedChunk chunk;
    chunk.radarId = "KTLX";
    // Filler ray at 200 deg breaks the wraparound pair: with only two rays
    // the 91->90 wrap gap is 1 deg so the detector treats it as adjacent
    // and double-counts. A distant third ray forces both non-couplet pairs
    // past the 5-deg adjacency cap, leaving exactly one couplet per tilt.
    chunk.rays.push_back(syntheticCoupletRay(azA, 1, lowElev, reflDbz));
    chunk.rays.push_back(syntheticCoupletRay(azB, 1, lowElev, reflDbz));
    chunk.rays.push_back(syntheticCoupletRay(200.0F, 1, lowElev, 10.0));
    chunk.rays.push_back(syntheticCoupletRay(azA, 2, highElev, reflDbz));
    chunk.rays.push_back(syntheticCoupletRay(azB, 2, highElev, reflDbz));
    chunk.rays.push_back(syntheticCoupletRay(200.0F, 2, highElev, 10.0));
    return chunk;
}

void testCirculationMesocyclonePositive() {
    auto chunk = twoTiltCoupletChunk(45.0);
    // deltaV 20: above meso gate (~13.2 at 53 km) but below TVS gate (~29.3).
    // Gates 168-178 sit at ~52-54.5 km slant where the 0.5/4.0-deg tilt
    // pair gives ~3.2 km depth (meso needs 3000 m).
    auto grid = coupletGrid(chunk, -10.0F, 10.0F, 168, 178);
    std::vector<ShearWatchCandidate> watch;
    const auto dets =
        detectCirculations(chunk, grid, 35.333, -97.278, 370.0, {}, nullptr, &watch);
    assert(dets.size() == 1);
    assert(dets.front().kind == CirculationKind::Mesocyclone);
    assert(dets.front().tiltCount == 2);
    assert(dets.front().depthMeters >= 3000.0);
    assert(std::abs(dets.front().maxDeltaVelocityMetersPerSecond - 20.0) < 0.01);
    assert(watch.empty()); // classified detections never double as watch
}

void testCirculationTvsClassification() {
    // TVS-strength compact couplet -> TornadoVortex.
    auto chunk = twoTiltCoupletChunk(50.0);
    auto grid = coupletGrid(chunk, -30.0F, 30.0F, 168, 178); // deltaV 60
    auto dets = detectCirculations(chunk, grid, 35.333, -97.278, 370.0, {});
    assert(dets.size() == 1);
    assert(dets.front().kind == CirculationKind::TornadoVortex);
    assert(dets.front().diameterKm <= 3.0);
    // Same strength but 4-deg separation -> diameter >3 km -> meso only.
    auto wide = twoTiltCoupletChunk(50.0, 0.5F, 4.0F, 90.0F, 94.0F);
    auto wideGrid = coupletGrid(wide, -30.0F, 30.0F, 168, 178);
    dets = detectCirculations(wide, wideGrid, 35.333, -97.278, 370.0, {});
    assert(dets.size() == 1);
    assert(dets.front().kind == CirculationKind::Mesocyclone);
    assert(dets.front().diameterKm > 3.0);
}

void testCirculationReflectivityVeto() {
    // Strong couplet in echo-free air must fail closed.
    auto clear = twoTiltCoupletChunk(-3.0);
    auto grid = coupletGrid(clear, -30.0F, 30.0F, 150, 160);
    assert(detectCirculations(clear, grid, 35.333, -97.278, 370.0, {}).empty());
    // Missing reflectivity moment entirely -> also empty.
    auto missing = twoTiltCoupletChunk(45.0);
    for (auto& ray : missing.rays) {
        ray.fields.at(static_cast<std::size_t>(ProductKind::Reflectivity)).clear();
    }
    auto missingGrid = coupletGrid(missing, -30.0F, 30.0F, 150, 160);
    assert(detectCirculations(missing, missingGrid, 35.333, -97.278, 370.0, {}).empty());
}

void testCirculationSingleTiltVeto() {
    // Vertical continuity requires 2+ tilts.
    DecodedChunk chunk;
    chunk.radarId = "KTLX";
    chunk.rays.push_back(syntheticCoupletRay(90.0F, 1, 0.5F, 50.0));
    chunk.rays.push_back(syntheticCoupletRay(91.0F, 1, 0.5F, 50.0));
    auto grid = coupletGrid(chunk, -30.0F, 30.0F, 150, 160);
    assert(detectCirculations(chunk, grid, 35.333, -97.278, 370.0, {}).empty());
}

void testCirculationDealiasAndWeakVeto() {
    // Folded/missing gate (NaN) on one side kills the couplet.
    auto chunk = twoTiltCoupletChunk(50.0);
    auto grid = coupletGrid(chunk, -30.0F, 30.0F, 150, 160);
    for (auto& row : grid) {
        for (std::size_t g = 150; g <= 160; ++g) row[g] = std::numeric_limits<float>::quiet_NaN();
    }
    // Restore only the negative side; positive side stays NaN.
    for (std::size_t r = 0; r < grid.size(); r += 2) {
        for (std::size_t g = 150; g <= 160; ++g) grid[r][g] = -30.0F;
    }
    assert(detectCirculations(chunk, grid, 35.333, -97.278, 370.0, {}).empty());
    // Weak couplet below the range-dependent meso gate (10+0.06*~48 = ~12.9).
    auto weak = twoTiltCoupletChunk(50.0);
    auto weakGrid = coupletGrid(weak, -6.0F, 6.0F, 150, 160); // deltaV 12
    assert(detectCirculations(weak, weakGrid, 35.333, -97.278, 370.0, {}).empty());
    // Single-gate speckle never forms a run (minRunGates 2).
    auto speck = twoTiltCoupletChunk(50.0);
    auto speckGrid = coupletGrid(speck, -30.0F, 30.0F, 155, 155);
    assert(detectCirculations(speck, speckGrid, 35.333, -97.278, 370.0, {}).empty());
}

void testCirculationOverUnfoldVeto() {
    // Stacked 2*Nyquist unfolding (e.g. 4x56 = 224 m/s at Nyquist 28) must not
    // become a detection: gates above maxDeltaV break the run.
    auto chunk = twoTiltCoupletChunk(50.0);
    auto grid = coupletGrid(chunk, -112.0F, 112.0F, 168, 178);  // deltaV 224
    assert(detectCirculations(chunk, grid, 35.333, -97.278, 370.0, {}).empty());
    // Disabled cap recovers the old behavior (test-only, proves the veto did it).
    CirculationConfig noCap{};
    noCap.maxDeltaVelocityMetersPerSecond = 0.0;
    assert(!detectCirculations(chunk, grid, 35.333, -97.278, 370.0, noCap).empty());
}

void testVelocityRespectsRayNyquist() {
    // HRGOL-style fold boundary at panel Nyquist 25: raw -5.9 / +5.9 passes
    // through unfolded (delta 11.8: a phantom meso gate at close range).
    // With the ray's true NI 6.1 the same pair unfolds to one interval.
    DecodedRay ray;
    ray.azimuthDegrees = 0.0F;
    const auto index = static_cast<std::size_t>(ProductKind::Velocity);
    ray.fields.at(index) = {117, 141};  // (-5.9, +5.9) at scale 2 / offset 129
    ray.scaleByProduct.at(index) = 2.0F;
    ray.offsetByProduct.at(index) = 129.0F;
    DecodedChunk chunk{"HRGOL", {ray}, 1};
    const auto folded =
        processVelocity(chunk, {VelocityProcessingMode::Dealiased, 25.0, 0.0, 0.0, 90.0});
    assert(std::abs(folded[0][0] - folded[0][1]) > 10.0);  // no unfold at NI 25
    chunk.rays.front().nyquistMetersPerSecond = 6.1F;  // ODIM how/NI
    const auto unfolded =
        processVelocity(chunk, {VelocityProcessingMode::Dealiased, 25.0, 0.0, 0.0, 90.0});
    assert(std::abs(unfolded[0][0] - unfolded[0][1]) < 1.0);  // one interval
}

void testVelocityConditionalUnfold() {    // Small gate-to-gate drift must not accumulate into stacked folds;
    // true folds (jump > Nyquist) still unfold; over-physical unfolds NaN.
    DecodedRay ray;
    ray.azimuthDegrees = 0.0F;
    const auto index = static_cast<std::size_t>(ProductKind::Velocity);
    // 4 gates near +10 m/s with scale 2 / offset 129: encoded = v*2+129? No:
    // nativeValue(enc, scale, offset) = (enc - offset)/scale? Check helper:
    // use same convention as testVelocityProcessing: 169 -> +20, 89 -> -20.
    // So +10 -> 149, +12 -> 153.
    ray.fields.at(index) = {149, 153, 151, 155};
    ray.scaleByProduct.at(index) = 2.0F;
    ray.offsetByProduct.at(index) = 129.0F;
    DecodedChunk chunk{"KTLX", {ray}, 1};
    const auto out =
        processVelocity(chunk, {VelocityProcessingMode::Dealiased, 25.0, 0.0, 0.0, 90.0});
    assert(out[0].size() == 4);
    for (const auto v : out[0]) assert(std::isfinite(v) && std::abs(v) < 20.0);
}

void testCirculationSameSign() {
    // Storm-motion-biased shear (both sides positive) is missed by the classic
    // opposite-sign gate but caught when sameSignBoost is enabled; disabled by
    // default since the Moore sweep showed it adds FAR without hits.
    auto chunk = twoTiltCoupletChunk(45.0);
    auto grid = coupletGrid(chunk, 6.0F, 30.0F, 168, 178);  // deltaV 24, same sign
    assert(detectCirculations(chunk, grid, 35.333, -97.278, 370.0, {}).empty());
    CirculationConfig cfg{};
    cfg.sameSignBoost = 1.8;
    assert(!detectCirculations(chunk, grid, 35.333, -97.278, 370.0, cfg).empty());
}

void testCirculationSingleTiltTvsOptIn() {
    // Single-tilt strong compact couplet: vetoed by default, allowed opt-in.
    DecodedChunk chunk;
    chunk.radarId = "KTLX";
    chunk.rays.push_back(syntheticCoupletRay(90.0F, 1, 0.5F, 50.0));
    chunk.rays.push_back(syntheticCoupletRay(91.0F, 1, 0.5F, 50.0));
    chunk.rays.push_back(syntheticCoupletRay(200.0F, 1, 0.5F, 10.0));
    auto grid = coupletGrid(chunk, -30.0F, 30.0F, 150, 160);
    assert(detectCirculations(chunk, grid, 35.333, -97.278, 370.0, {}).empty());
    CirculationConfig opt{};
    opt.allowSingleTiltTvs = true;
    const auto dets = detectCirculations(chunk, grid, 35.333, -97.278, 370.0, opt);
    assert(!dets.empty());
    assert(dets.front().kind == CirculationKind::TornadoVortex);
}

void testShearGridFindsCouplet() {
    // LLSD shear must fire on the same synthetic couplet the gate-to-gate
    // detector fires on, and stay quiet on uniform flow.
    auto chunk = twoTiltCoupletChunk(45.0);
    auto grid = coupletGrid(chunk, -10.0F, 10.0F, 168, 178);  // deltaV 20
    ShearGridConfig cfg{};
    cfg.minShearPerSecond = 0.002;
    const auto peaks = detectShearPeaks(chunk, grid, 35.333, -97.278, 370.0, cfg);
    assert(!peaks.empty());
    assert(peaks.front().equivalentDeltaVMps > 10.0);
    auto flat = coupletGrid(chunk, 10.0F, 10.0F, 168, 178);  // no shear
    assert(detectShearPeaks(chunk, flat, 35.333, -97.278, 370.0, cfg).empty());
}

void testCirculationShallowVeto() {
    // TVS-strength but 0.5/1.0-deg tilts at ~30 km slant -> depth ~260 m.
    DecodedChunk chunk;
    chunk.radarId = "KTLX";
    chunk.rays.push_back(syntheticCoupletRay(90.0F, 1, 0.5F, 50.0));
    chunk.rays.push_back(syntheticCoupletRay(91.0F, 1, 0.5F, 50.0));
    chunk.rays.push_back(syntheticCoupletRay(200.0F, 1, 0.5F, 10.0));
    chunk.rays.push_back(syntheticCoupletRay(90.0F, 2, 1.0F, 50.0));
    chunk.rays.push_back(syntheticCoupletRay(91.0F, 2, 1.0F, 50.0));
    chunk.rays.push_back(syntheticCoupletRay(200.0F, 2, 1.0F, 10.0));
    auto grid = coupletGrid(chunk, -30.0F, 30.0F, 80, 90); // slant ~30-32 km
    std::vector<ShearWatchCandidate> watch;
    assert(detectCirculations(chunk, grid, 35.333, -97.278, 370.0, {}, nullptr, &watch).empty());
    // TVS-strength but ~260 m deep: no marker, exactly one shear-watch
    // candidate carrying the same column.
    assert(watch.size() == 1);
    assert(std::abs(watch.front().maxDeltaVelocityMetersPerSecond - 60.0) < 0.01);
    assert(watch.front().tiltCount == 2);
    assert(watch.front().depthMeters < 3000.0);
}

void testShearWatchQuietUniformFlow() {
    // Uniform environmental flow with light ripple: no opposite-sign pairs,
    // so neither a marker nor a watch candidate may result. Regression for
    // the quiet-echo pink soup (span-heuristic ROT fired on such volumes).
    auto chunk = twoTiltCoupletChunk(40.0);
    auto grid = coupletGrid(chunk, 8.0F, 12.0F, 150, 160); // deltaV 4, same sign
    std::vector<ShearWatchCandidate> watch;
    assert(detectCirculations(chunk, grid, 35.333, -97.278, 370.0, {}, nullptr, &watch).empty());
    assert(watch.empty());
}

void testCirculationMisalignedAndGapVeto() {
    // Upper-tilt couplet displaced far in azimuth -> two size-1 columns.
    DecodedChunk chunk;
    chunk.radarId = "KTLX";
    chunk.rays.push_back(syntheticCoupletRay(90.0F, 1, 0.5F, 50.0));
    chunk.rays.push_back(syntheticCoupletRay(91.0F, 1, 0.5F, 50.0));
    chunk.rays.push_back(syntheticCoupletRay(100.0F, 2, 4.0F, 50.0));
    chunk.rays.push_back(syntheticCoupletRay(101.0F, 2, 4.0F, 50.0));
    auto grid = coupletGrid(chunk, -30.0F, 30.0F, 150, 160);
    assert(detectCirculations(chunk, grid, 35.333, -97.278, 370.0, {}).empty());
    // 6-deg pair separation exceeds the 5-deg adjacency cap -> skipped.
    auto wide = twoTiltCoupletChunk(50.0, 0.5F, 4.0F, 90.0F, 96.0F);
    auto wideGrid = coupletGrid(wide, -30.0F, 30.0F, 150, 160);
    assert(detectCirculations(wide, wideGrid, 35.333, -97.278, 370.0, {}).empty());
}

// --- Hail cores (VIL density) synthetic tests ---
// Geometry matches the circulation synthetics: firstGate 10000 m, spacing
// 250 m, 200 gates. VIL/ET18 use the derived-product encoding (scale 10,
// offset 2); ET is kft MSL.

DecodedRay syntheticHailRay(const float azimuth, const double vilKgPerM2,
                            const double echoTopKft, const double reflectivityDbz,
                            const std::size_t firstGate = 150, const std::size_t lastGate = 159) {
    DecodedRay ray;
    ray.azimuthDegrees = azimuth;
    ray.azimuthResolutionDegrees = 1.0F;
    ray.elevationNumber = 1;
    ray.elevationDegrees = 0.5F;
    constexpr std::size_t gates = 200;
    const auto reflIndex = static_cast<std::size_t>(ProductKind::Reflectivity);
    const auto vilIndex = static_cast<std::size_t>(ProductKind::VerticallyIntegratedLiquid);
    const auto etIndex = static_cast<std::size_t>(ProductKind::EchoTop);
    ray.fields.at(reflIndex).assign(
        gates, static_cast<std::uint16_t>(std::lround(10.0 * 2.0 + 66.0)));
    for (std::size_t g = firstGate; g <= lastGate && g < gates; ++g) {
        ray.fields.at(reflIndex)[g] =
            static_cast<std::uint16_t>(std::lround(reflectivityDbz * 2.0 + 66.0));
    }
    ray.firstGateByProduct.at(reflIndex) = 10000;
    ray.gateSpacingByProduct.at(reflIndex) = 250;
    ray.scaleByProduct.at(reflIndex) = 2.0F;
    ray.offsetByProduct.at(reflIndex) = 66.0F;
    ray.fields.at(vilIndex).assign(gates, 2);  // 0.0 kg/m2 background
    ray.firstGateByProduct.at(vilIndex) = 10000;
    ray.gateSpacingByProduct.at(vilIndex) = 250;
    ray.scaleByProduct.at(vilIndex) = 10.0F;
    ray.offsetByProduct.at(vilIndex) = 2.0F;
    ray.fields.at(etIndex).assign(gates, 2);  // 0.0 kft background
    ray.firstGateByProduct.at(etIndex) = 10000;
    ray.gateSpacingByProduct.at(etIndex) = 250;
    ray.scaleByProduct.at(etIndex) = 10.0F;
    ray.offsetByProduct.at(etIndex) = 2.0F;
    for (std::size_t g = firstGate; g <= lastGate && g < gates; ++g) {
        ray.fields.at(vilIndex)[g] =
            static_cast<std::uint16_t>(std::lround(vilKgPerM2 * 10.0 + 2.0));
        ray.fields.at(etIndex)[g] =
            static_cast<std::uint16_t>(std::lround(echoTopKft * 10.0 + 2.0));
    }
    return ray;
}

void testHailTierBoundaries() {
    assert(hailTier(3.49) == 0);
    assert(hailTier(3.5) == 1);
    assert(hailTier(3.99) == 1);
    assert(hailTier(4.0) == 2);
    assert(hailTier(4.49) == 2);
    assert(hailTier(4.5) == 3);
    assert(hailTier(9.0) == 3);
    assert(hailTier(std::numeric_limits<double>::quiet_NaN()) == 0);
    assert(!hailSizeHint(1).empty() && !hailSizeHint(3).empty());
}

void testHailCorePositive() {
    // VIL 40 + ET 30 kft -> 40000/9144 = 4.37 g/m3, tier 2.
    DecodedChunk chunk;
    chunk.radarId = "KTLX";
    chunk.rays.push_back(syntheticHailRay(90.0F, 40.0, 30.0, 60.0));
    const auto cores = detectHailCores(chunk, 35.333, -97.278, 370.0, {});
    assert(cores.size() == 1);
    assert(cores.front().tier == 2);
    assert(std::abs(cores.front().maxVildGPerCubicMeter - 4.37) < 0.05);
    assert(std::abs(cores.front().maxVilKgPerSquareMeter - 40.0) < 0.2);
    assert(std::abs(cores.front().maxReflectivityDbz - 60.0) < 0.6);
}

void testHailVetoes() {
    // Thin VIL divides into a huge quotient on a tiny top: guarded.
    DecodedChunk thin;
    thin.radarId = "KTLX";
    thin.rays.push_back(syntheticHailRay(90.0F, 5.0, 30.0, 60.0));
    assert(detectHailCores(thin, 35.333, -97.278, 370.0, {}).empty());
    // Tiny echo top: geometry noise, not a storm column.
    DecodedChunk shallow;
    shallow.radarId = "KTLX";
    shallow.rays.push_back(syntheticHailRay(90.0F, 40.0, 5.0, 60.0));
    assert(detectHailCores(shallow, 35.333, -97.278, 370.0, {}).empty());
    // High density math but stratiform reflectivity: guarded.
    DecodedChunk weakRef;
    weakRef.radarId = "KTLX";
    weakRef.rays.push_back(syntheticHailRay(90.0F, 40.0, 30.0, 40.0));
    assert(detectHailCores(weakRef, 35.333, -97.278, 370.0, {}).empty());
    // Missing echo-top moment entirely: fail closed.
    DecodedChunk noTop;
    noTop.radarId = "KTLX";
    noTop.rays.push_back(syntheticHailRay(90.0F, 40.0, 30.0, 60.0));
    noTop.rays.front().fields.at(static_cast<std::size_t>(ProductKind::EchoTop)).clear();
    assert(detectHailCores(noTop, 35.333, -97.278, 370.0, {}).empty());
}

void testHailZeroTopNeverDivides() {
    // Zero echo top with the floor zeroed out: the old code divided into
    // inf VILD and pushed a phantom core. Must stay empty.
    DecodedChunk zeroTop;
    zeroTop.radarId = "KTLX";
    zeroTop.rays.push_back(syntheticHailRay(90.0F, 40.0, 0.0, 60.0));
    HailConfig noFloor{};
    noFloor.minEchoTopMeters = 0.0;
    assert(detectHailCores(zeroTop, 35.333, -97.278, 370.0, noFloor).empty());
}

void testHailClustering() {
    // Two azimuth-separated cores survive; neighbors merge.
    DecodedChunk chunk;
    chunk.radarId = "KTLX";
    chunk.rays.push_back(syntheticHailRay(90.0F, 40.0, 30.0, 60.0));
    chunk.rays.push_back(syntheticHailRay(91.0F, 45.0, 28.0, 62.0));
    chunk.rays.push_back(syntheticHailRay(200.0F, 42.0, 29.0, 61.0));
    const auto cores = detectHailCores(chunk, 35.333, -97.278, 370.0, {});
    assert(cores.size() == 2);
    // Strongest first.
    assert(cores.front().maxVildGPerCubicMeter >= cores.back().maxVildGPerCubicMeter);
}

// --- Debris signature (TDS) synthetic tests ---
// CC uses the 8-bit DRHO convention ((enc-1)/254); ZDR the DZDR one
// ((enc-129)/16). Parents are placed at real gate geometry so the
// neighborhood search has something to find.

DecodedRay syntheticTdsRay(const float azimuth, const double cc, const double refDbz,
                           const double zdrDb = 0.5) {
    DecodedRay ray;
    ray.azimuthDegrees = azimuth;
    ray.azimuthResolutionDegrees = 1.0F;
    ray.elevationNumber = 1;
    ray.elevationDegrees = 0.5F;
    constexpr std::size_t gates = 200;
    const auto reflIndex = static_cast<std::size_t>(ProductKind::Reflectivity);
    const auto ccIndex = static_cast<std::size_t>(ProductKind::CorrelationCoefficient);
    const auto zdrIndex = static_cast<std::size_t>(ProductKind::DifferentialReflectivity);
    ray.fields.at(reflIndex).assign(
        gates, static_cast<std::uint16_t>(std::lround(refDbz * 2.0 + 66.0)));
    ray.firstGateByProduct.at(reflIndex) = 10000;
    ray.gateSpacingByProduct.at(reflIndex) = 250;
    ray.scaleByProduct.at(reflIndex) = 2.0F;
    ray.offsetByProduct.at(reflIndex) = 66.0F;
    ray.fields.at(ccIndex).assign(
        gates, static_cast<std::uint16_t>(std::lround(cc * 254.0 + 1.0)));
    ray.firstGateByProduct.at(ccIndex) = 10000;
    ray.gateSpacingByProduct.at(ccIndex) = 250;
    ray.scaleByProduct.at(ccIndex) = 254.0F;
    ray.offsetByProduct.at(ccIndex) = 1.0F;
    ray.fields.at(zdrIndex).assign(
        gates, static_cast<std::uint16_t>(std::lround(zdrDb * 16.0 + 129.0)));
    ray.firstGateByProduct.at(zdrIndex) = 10000;
    ray.gateSpacingByProduct.at(zdrIndex) = 250;
    ray.scaleByProduct.at(zdrIndex) = 16.0F;
    ray.offsetByProduct.at(zdrIndex) = 129.0F;
    return ray;
}

CirculationDetection syntheticTdsParent(double lat, double lon) {
    CirculationDetection parent;
    parent.kind = CirculationKind::TornadoVortex;
    parent.latitudeDeg = lat;
    parent.longitudeDeg = lon;
    parent.maxDeltaVelocityMetersPerSecond = 45.0;
    return parent;
}

void testTdsPositive() {
    // Localized debris hole: inner rays at 0.65, surrounding rays at 0.96.
    // Contrast gate (median-outer minus min-inner >= 0.05) passes.
    DecodedChunk chunk;
    chunk.radarId = "KTLX";
    chunk.rays.push_back(syntheticTdsRay(86.0F, 0.96, 55.0));
    chunk.rays.push_back(syntheticTdsRay(87.0F, 0.96, 55.0));
    chunk.rays.push_back(syntheticTdsRay(90.0F, 0.65, 55.0));
    chunk.rays.push_back(syntheticTdsRay(91.0F, 0.65, 55.0));
    chunk.rays.push_back(syntheticTdsRay(94.0F, 0.96, 55.0));
    chunk.rays.push_back(syntheticTdsRay(95.0F, 0.96, 55.0));
    // Parent at gate-155 geometry on the 90.5-deg radial (~48 km slant).
    const double slant = 10000.0 + 155.0 * 250.0;
    const auto beam = standardBeamGeometry(slant, 0.5);
    const auto pos = destinationPoint({35.333, -97.278}, 90.5, beam.surfaceDistanceMeters);
    const auto dets = detectDebrisSignatures(
        chunk, {syntheticTdsParent(pos.latitude, pos.longitude)}, 35.333, -97.278, 370.0, {});
    assert(dets.size() == 1);
    assert(std::abs(dets.front().minCorrelationCoefficient - 0.65) < 0.02);
    assert(dets.front().maxReflectivityDbz >= 20.0);
    assert(dets.front().hasDifferentialReflectivity);
    assert(std::abs(dets.front().meanDifferentialReflectivityDb - 0.5) < 0.15);
    assert(dets.front().parentKind == CirculationKind::TornadoVortex);
}

void testTdsContrastVeto() {
    // Broad hail-shield depression (0.72 everywhere): under the CC line but
    // with no compact hole, so the contrast gate holds it back. Disabling
    // contrast recovers the old behavior (proves the gate did it).
    DecodedChunk chunk;
    chunk.radarId = "KTLX";
    for (const float az : {86.0F, 87.0F, 90.0F, 91.0F, 94.0F, 95.0F}) {
        chunk.rays.push_back(syntheticTdsRay(az, 0.72, 55.0));
    }
    const double slant = 10000.0 + 155.0 * 250.0;
    const auto beam = standardBeamGeometry(slant, 0.5);
    const auto pos = destinationPoint({35.333, -97.278}, 90.5, beam.surfaceDistanceMeters);
    const std::vector<CirculationDetection> parents{
        syntheticTdsParent(pos.latitude, pos.longitude)};
    assert(detectDebrisSignatures(chunk, parents, 35.333, -97.278, 370.0, {}).empty());
    TdsConfig noContrast{};
    noContrast.minContrast = 0.0;
    assert(!detectDebrisSignatures(chunk, parents, 35.333, -97.278, 370.0, noContrast).empty());
}

void testTdsVetoes() {
    const double slant = 10000.0 + 155.0 * 250.0;
    const auto beam = standardBeamGeometry(slant, 0.5);
    const auto pos = destinationPoint({35.333, -97.278}, 90.5, beam.surfaceDistanceMeters);
    const std::vector<CirculationDetection> parents{
        syntheticTdsParent(pos.latitude, pos.longitude)};
    // Meteorological CC: no debris call.
    DecodedChunk clean;
    clean.radarId = "KTLX";
    clean.rays.push_back(syntheticTdsRay(90.0F, 0.97, 55.0));
    clean.rays.push_back(syntheticTdsRay(91.0F, 0.97, 55.0));
    assert(detectDebrisSignatures(clean, parents, 35.333, -97.278, 370.0, {}).empty());
    // Low CC but weak echo: signal-quality floor holds.
    DecodedChunk weak;
    weak.radarId = "KTLX";
    weak.rays.push_back(syntheticTdsRay(90.0F, 0.60, 10.0));
    weak.rays.push_back(syntheticTdsRay(91.0F, 0.60, 10.0));
    assert(detectDebrisSignatures(weak, parents, 35.333, -97.278, 370.0, {}).empty());
    // No CC moment at all (single-pol): never a guess.
    DecodedChunk noCc;
    noCc.radarId = "KTLX";
    noCc.rays.push_back(syntheticCoupletRay(90.0F, 1, 0.5F, 55.0));
    noCc.rays.push_back(syntheticCoupletRay(91.0F, 1, 0.5F, 55.0));
    assert(detectDebrisSignatures(noCc, parents, 35.333, -97.278, 370.0, {}).empty());
    // No parent circulation: CC drops alone are hail or clutter, not debris.
    DecodedChunk chunk;
    chunk.radarId = "KTLX";
    chunk.rays.push_back(syntheticTdsRay(90.0F, 0.65, 55.0));
    chunk.rays.push_back(syntheticTdsRay(91.0F, 0.65, 55.0));
    assert(detectDebrisSignatures(chunk, {}, 35.333, -97.278, 370.0, {}).empty());
}

void testTdsNanParentFailsClosed() {
    // NaN vortex fix must not bypass the range gate (all NaN comparisons
    // are false) and must never emit a phantom detection. Proved with
    // contrast disabled: with the default contrast gate the empty outer
    // annulus would screen it anyway, hiding the range-gate hole.
    DecodedChunk chunk;
    chunk.radarId = "KTLX";
    chunk.rays.push_back(syntheticTdsRay(90.0F, 0.60, 55.0));
    chunk.rays.push_back(syntheticTdsRay(91.0F, 0.60, 55.0));
    TdsConfig noContrast{};
    noContrast.minContrast = 0.0;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const std::vector<CirculationDetection> parents{
        syntheticTdsParent(nan, nan)};
    assert(detectDebrisSignatures(chunk, parents, 35.333, -97.278, 370.0, noContrast).empty());
    const std::vector<CirculationDetection> halfNan{
        syntheticTdsParent(35.5, nan)};
    assert(detectDebrisSignatures(chunk, halfNan, 35.333, -97.278, 370.0, noContrast).empty());
    // And with default contrast on, still clean.
    assert(detectDebrisSignatures(chunk, parents, 35.333, -97.278, 370.0, {}).empty());
}



void testDecodedRayLegacyGeometryMaxGates() {
    // Wide product first, narrow second: legacy gateCount must be the max,
    // not the last block parsed.
    constexpr std::size_t radialLength = 140;
    constexpr std::size_t wideOffset = 72;
    constexpr std::size_t narrowOffset = 105;
    std::vector<std::byte> raw(radialLength);
    for (std::size_t index = 0; index < 4; ++index) {
        raw[index] = static_cast<std::byte>(static_cast<unsigned char>("KTLX"[index]));
    }
    writeBigEndian16(raw, 18, static_cast<std::uint16_t>(radialLength));
    writeBigEndian32(raw, 12, std::bit_cast<std::uint32_t>(90.0F));
    raw[20] = static_cast<std::byte>(1);
    raw[22] = static_cast<std::byte>(1);
    writeBigEndian32(raw, 24, std::bit_cast<std::uint32_t>(0.5F));
    writeBigEndian16(raw, 30, 2);
    writeBigEndian32(raw, 32, static_cast<std::uint32_t>(wideOffset));
    writeBigEndian32(raw, 36, static_cast<std::uint32_t>(narrowOffset));

    raw[wideOffset] = static_cast<std::byte>('D');
    raw[wideOffset + 1] = static_cast<std::byte>('R');
    raw[wideOffset + 2] = static_cast<std::byte>('E');
    raw[wideOffset + 3] = static_cast<std::byte>('F');
    writeBigEndian16(raw, wideOffset + 8, 5);
    writeBigEndian16(raw, wideOffset + 10, static_cast<std::uint16_t>(static_cast<std::int16_t>(0)));
    writeBigEndian16(raw, wideOffset + 12, 1000);
    raw[wideOffset + 19] = static_cast<std::byte>(8);
    writeBigEndian32(raw, wideOffset + 20, std::bit_cast<std::uint32_t>(2.0F));
    writeBigEndian32(raw, wideOffset + 24, std::bit_cast<std::uint32_t>(66.0F));
    for (std::size_t gate = 0; gate < 5; ++gate) {
        raw[wideOffset + 28 + gate] = static_cast<std::byte>(66);
    }

    raw[narrowOffset] = static_cast<std::byte>('D');
    raw[narrowOffset + 1] = static_cast<std::byte>('V');
    raw[narrowOffset + 2] = static_cast<std::byte>('E');
    raw[narrowOffset + 3] = static_cast<std::byte>('L');
    writeBigEndian16(raw, narrowOffset + 8, 3);
    writeBigEndian16(raw, narrowOffset + 10, static_cast<std::uint16_t>(static_cast<std::int16_t>(0)));
    writeBigEndian16(raw, narrowOffset + 12, 250);
    raw[narrowOffset + 19] = static_cast<std::byte>(8);
    writeBigEndian32(raw, narrowOffset + 20, std::bit_cast<std::uint32_t>(2.0F));
    writeBigEndian32(raw, narrowOffset + 24, std::bit_cast<std::uint32_t>(129.0F));
    for (std::size_t gate = 0; gate < 3; ++gate) {
        raw[narrowOffset + 28 + gate] = static_cast<std::byte>(129);
    }

    const auto decoded = decodeMessage31Chunk(raw, "KTLX");
    assert(decoded.rays.size() == 1);
    assert(decoded.rays.front().gateCount == 5);
}

void testVolumeBrickTrilinear() {
    // 2 x 2 x 2 brick, 1 km cells, every corner present at full value.
    VolumeBrick brick;
    brick.nx = 2;
    brick.ny = 2;
    brick.nz = 2;
    brick.halfExtentMeters = 1000.0;
    brick.topMeters = 1000.0;
    brick.value.assign(8, 255);
    brick.present.assign(8, 1);

    // Trilinear: the cell centre of a uniform field reads that field exactly,
    // and interpolation is continuous rather than snapping to a voxel.
    const auto centre = sampleBrickTrilinear(brick, 0.0, 0.0, 500.0);
    assert(centre.has_value());
    assert(std::fabs(static_cast<double>(centre->value01) - 1.0) < 1e-4);
    assert(std::fabs(static_cast<double>(centre->coverage) - 1.0) < 1e-4);

    brick.value.at(0) = 0; // one low corner: the blend must land between, not snap
    const auto blended = sampleBrickTrilinear(brick, 0.0, 0.0, 500.0);
    assert(blended.has_value());
    assert(blended->value01 > 0.80F && blended->value01 < 1.0F);

    // A hole is unknown, not zero echo. Drop one corner entirely: the value must
    // stay at full strength (renormalised among known corners) and coverage must
    // report that a fraction of the cell is missing.
    brick.value.assign(8, 255);
    brick.present.assign(8, 1);
    brick.present.at(7) = 0;
    brick.value.at(7) = 0;
    const auto holed = sampleBrickTrilinear(brick, 0.0, 0.0, 500.0);
    assert(holed.has_value());
    assert(std::fabs(static_cast<double>(holed->value01) - 1.0) < 1e-4); // not darkened
    assert(std::fabs(static_cast<double>(holed->coverage) - 0.875) < 1e-4);

    // Every corner absent: no claim at all.
    brick.present.assign(8, 0);
    assert(!sampleBrickTrilinear(brick, 0.0, 0.0, 500.0).has_value());

    // Outside the box or below ground.
    brick.present.assign(8, 1);
    assert(!sampleBrickTrilinear(brick, 0.0, 0.0, 5000.0).has_value());
    assert(!sampleBrickTrilinear(brick, 9000.0, 0.0, 500.0).has_value());
    assert(!sampleBrickTrilinear(brick, 0.0, 0.0, -1.0).has_value());

    // Surface normal points outward (toward decreasing density): dense west half,
    // empty east half gives +x.
    VolumeBrick half;
    half.nx = 4;
    half.ny = 4;
    half.nz = 4;
    half.halfExtentMeters = 2000.0;
    half.topMeters = 2000.0;
    half.value.assign(64, 0);
    half.present.assign(64, 0);
    for (int iz = 0; iz < 4; ++iz) {
        for (int iy = 0; iy < 4; ++iy) {
            for (int ix = 0; ix < 2; ++ix) {
                const auto flat = static_cast<std::size_t>((iz * 4 + iy) * 4 + ix);
                half.value.at(flat) = 255;
                half.present.at(flat) = 1;
            }
        }
    }
    const auto normal = brickSurfaceNormal(half, -100.0, 0.0, 1000.0);
    assert(normal.has_value());
    assert(normal->x > 0.9); // points east, out of the dense half
    assert(std::fabs(normal->y) < 0.1);

    // A flat field has no surface: ambient only, no invented normal.
    VolumeBrick flatBrick = brick;
    flatBrick.value.assign(8, 128);
    flatBrick.present.assign(8, 1);
    assert(!brickSurfaceNormal(flatBrick, 0.0, 0.0, 500.0).has_value());
    assert(!brickSurfaceNormal(brick, 0.0, 0.0, 5000.0).has_value());
}

void testVolumeBrick() {
    // One ray due east, low tilt, a single 50 dBZ gate in a sea of missing.
    DecodedChunk chunk;
    chunk.radarId = "KTLX";
    DecodedRay ray;
    ray.azimuthDegrees = 90.0F;
    ray.elevationDegrees = 0.5F;
    ray.elevationNumber = 1;
    const auto pi = static_cast<std::size_t>(ProductKind::Reflectivity);
    ray.fields.at(pi).assign(64, 0); // 0 encodes missing
    ray.fields.at(pi).at(20) = static_cast<std::uint16_t>(50.0 * 2.0 + 66.0);
    ray.firstGateByProduct.at(pi) = 0;
    ray.gateSpacingByProduct.at(pi) = 1000;
    ray.scaleByProduct.at(pi) = 2.0F;
    ray.offsetByProduct.at(pi) = 66.0F;
    chunk.rays.push_back(ray);

    VolumeBrickDims dims;
    dims.nx = 64;
    dims.ny = 64;
    dims.nz = 16;
    const VolumeBrick brick = buildVolumeBrick(chunk, ProductKind::Reflectivity, dims);

    // The hot gate lands on the east axis at the beam height the derived
    // products would report for the same gate.
    const double slant = gateCenterSlantRange(0, 1000, 20);
    const EnuPoint expected = gateEnu(90.0, slant, 0.5);
    assert(expected.x > 0.0);
    assert(std::fabs(expected.y) < 1.0);
    const auto sample = sampleBrickNearest(brick, expected.x, expected.y, expected.z);
    assert(sample.has_value());
    const double want = (50.0 - (-32.0)) / (95.0 - (-32.0)); // REF range clamp
    assert(std::fabs(static_cast<double>(*sample) - want) < 0.01);

    // Exactly one voxel lit; everything else is honest empty.
    std::size_t lit = 0;
    for (const auto flag : brick.present) lit += flag;
    assert(lit == 1);
    assert(!sampleBrickNearest(brick, -100000.0, -100000.0, 10000.0).has_value());
    assert(!sampleBrickNearest(brick, 500000.0, 0.0, 0.0).has_value()); // outside box

    // Same gate higher tilt sits higher (monotonic beam height).
    const EnuPoint hi = gateEnu(90.0, slant, 5.0);
    assert(hi.z > expected.z);

    // All-missing rays leave a dark brick.
    DecodedChunk empty = chunk;
    std::fill(empty.rays.front().fields.at(pi).begin(), empty.rays.front().fields.at(pi).end(),
              static_cast<std::uint16_t>(0));
    const VolumeBrick dark = buildVolumeBrick(empty, ProductKind::Reflectivity, dims);
    lit = 0;
    for (const auto flag : dark.present) lit += flag;
    assert(lit == 0);

    // Transfer function: midpoint lerps, ends clamp, empty is transparent.
    const std::vector<RgbaStop> stops = {{0.0F, 0.0F, 0.0F, 0.0F, 0.0F},
                                         {1.0F, 1.0F, 1.0F, 1.0F, 1.0F}};
    const Rgba mid = evaluateTransfer(stops, 0.5F);
    assert(std::fabs(mid.r - 0.5F) < 1e-5 && std::fabs(mid.a - 0.5F) < 1e-5);
    assert(evaluateTransfer(stops, -1.0F).a == 0.0F);
    assert(evaluateTransfer(stops, 2.0F).a == 1.0F);
    assert(evaluateTransfer({}, 0.5F).a == 0.0F);

    // Ray-box: from the south looking north enters then exits; away misses.
    const double eye[3] = {0.0, -300000.0, 5000.0};
    const double toward[3] = {0.0, 1.0, 0.0};
    const auto hit = intersectVolumeBox(brick, eye, toward);
    assert(hit.has_value());
    assert(hit->tEnter > 0.0 && hit->tExit > hit->tEnter);
    const double away[3] = {0.0, -1.0, 0.0};
    assert(!intersectVolumeBox(brick, eye, away).has_value());
}

// One gate per sweep on the same azimuth, at two heights with empty levels
// between them. The build must bridge that gap, mark every bridged voxel as
// reconstructed, and never grow the column past either measured gate.
void testVolumeBrickFillsBetweenTilts() {
    const auto pi = static_cast<std::size_t>(ProductKind::Reflectivity);
    const auto makeRay = [&](const float elevation, const std::size_t gate, const double dBZ) {
        DecodedRay ray;
        ray.azimuthDegrees = 90.0F;
        ray.elevationDegrees = elevation;
        ray.fields.at(pi).assign(200, 0); // 0 encodes missing: no invented gates
        ray.fields.at(pi).at(gate) = static_cast<std::uint16_t>(dBZ * 2.0 + 66.0);
        ray.firstGateByProduct.at(pi) = 0;
        ray.gateSpacingByProduct.at(pi) = 1000;
        ray.scaleByProduct.at(pi) = 2.0F;
        ray.offsetByProduct.at(pi) = 66.0F;
        ray.gateCount = 200;
        return ray;
    };

    VolumeBrickDims dims;
    dims.nx = 8; // 37.5 km columns over the full-range box: the two sweeps share one
    dims.ny = 8;
    dims.nz = 64; // 312 m levels

    DecodedChunk chunk;
    chunk.radarId = "KTLX";
    chunk.rays.push_back(makeRay(0.5F, 100, 50.0)); // ~1.5 km up at 100 km slant
    chunk.rays.push_back(makeRay(5.0F, 100, 20.0)); // ~9.4 km up

    const double slant = gateCenterSlantRange(0, 1000, 100);
    const EnuPoint low = gateEnu(90.0, slant, 0.5);
    const EnuPoint high = gateEnu(90.0, slant, 5.0);

    const VolumeBrick brick = buildVolumeBrick(chunk, ProductKind::Reflectivity, dims);
    const auto lowIndex = brickIndexOf(brick, low.x, low.y, low.z);
    const auto highIndex = brickIndexOf(brick, high.x, high.y, high.z);
    assert(lowIndex.has_value() && highIndex.has_value());
    assert(lowIndex->ix == highIndex->ix && lowIndex->iy == highIndex->iy); // one column
    assert(highIndex->iz > lowIndex->iz + 3);                              // a real gap

    // Two measured voxels; every level between them filled exactly once.
    const int gapLevels = highIndex->iz - lowIndex->iz - 1;
    assert(brick.measuredVoxels == 2);
    assert(brick.interpolatedVoxels == static_cast<std::size_t>(gapLevels));
    assert(brick.interpolatedTilts == 2); // both sweeps took part
    std::size_t present = 0;
    std::size_t interpolated = 0;
    for (const auto flag : brick.present) present += flag;
    for (const auto flag : brick.interpolated) interpolated += flag;
    assert(present == brick.measuredVoxels + brick.interpolatedVoxels);
    assert(interpolated == brick.interpolatedVoxels);

    // The column is continuous between the two heights and the mask is set
    // exactly on the filled voxels, clear on the measured ones.
    const int lowValue =
        brick.value[brickFlatIndex(brick, lowIndex->ix, lowIndex->iy, lowIndex->iz)];
    const int highValue =
        brick.value[brickFlatIndex(brick, highIndex->ix, highIndex->iy, highIndex->iz)];
    assert(lowValue > highValue); // 50 dBZ below, 20 dBZ above
    int previous = lowValue;
    for (int iz = lowIndex->iz; iz <= highIndex->iz; ++iz) {
        const std::size_t flat = brickFlatIndex(brick, lowIndex->ix, lowIndex->iy, iz);
        assert(brick.present[flat] == 1); // continuous, not shells
        const bool measuredEnd = iz == lowIndex->iz || iz == highIndex->iz;
        assert(brickVoxelInterpolated(brick, flat) == !measuredEnd);
        const int value = brick.value[flat];
        assert(value <= lowValue && value >= highValue); // bounded by its two ends
        assert(value <= previous);                        // smooth, not stepped
        previous = value;
    }

    // No invented top or bottom: every other level in the column stays empty.
    for (int iz = 0; iz < brick.nz; ++iz) {
        if (iz >= lowIndex->iz && iz <= highIndex->iz) continue;
        assert(brick.present[brickFlatIndex(brick, lowIndex->ix, lowIndex->iy, iz)] == 0);
    }

    // A third sweep with a gate in another column must not be bridged to
    // anything: one sample alone has nothing on its other side.
    DecodedChunk withStranger = chunk;
    DecodedRay stranger = makeRay(10.0F, 20, 35.0);
    stranger.azimuthDegrees = 270.0F;
    withStranger.rays.push_back(stranger);
    const VolumeBrick withStrangerBrick =
        buildVolumeBrick(withStranger, ProductKind::Reflectivity, dims);
    assert(withStrangerBrick.measuredVoxels == 3);
    assert(withStrangerBrick.interpolatedVoxels == brick.interpolatedVoxels);
    assert(withStrangerBrick.interpolatedTilts == 2); // the lone sweep adds none
}

// A single sweep is never filled with itself, even where its own beam climbs
// several levels inside one column: that is one view, not two.
void testVolumeBrickDoesNotFillOneCone() {
    const auto pi = static_cast<std::size_t>(ProductKind::Reflectivity);
    VolumeBrickDims dims;
    dims.nx = 8;
    dims.ny = 8;
    dims.nz = 64;

    DecodedRay ray;
    ray.azimuthDegrees = 90.0F;
    ray.elevationDegrees = 19.5F;
    ray.fields.at(pi).assign(8, 0);
    ray.fields.at(pi).at(2) = static_cast<std::uint16_t>(40.0 * 2.0 + 66.0); // ~3.3 km up
    ray.fields.at(pi).at(4) = static_cast<std::uint16_t>(30.0 * 2.0 + 66.0); // ~6.7 km up
    ray.firstGateByProduct.at(pi) = 0;
    ray.gateSpacingByProduct.at(pi) = 5000;
    ray.scaleByProduct.at(pi) = 2.0F;
    ray.offsetByProduct.at(pi) = 66.0F;
    ray.gateCount = 8;

    DecodedChunk chunk;
    chunk.radarId = "KTLX";
    chunk.rays.push_back(ray);

    // Shown, not assumed: the two gates really are in one column with empty
    // levels between them, so the only reason the fill stays empty is the rule.
    const EnuPoint near = gateEnu(90.0, gateCenterSlantRange(0, 5000, 2), 19.5);
    const EnuPoint far = gateEnu(90.0, gateCenterSlantRange(0, 5000, 4), 19.5);
    const VolumeBrick brick = buildVolumeBrick(chunk, ProductKind::Reflectivity, dims);
    const auto nearIndex = brickIndexOf(brick, near.x, near.y, near.z);
    const auto farIndex = brickIndexOf(brick, far.x, far.y, far.z);
    assert(nearIndex.has_value() && farIndex.has_value());
    assert(nearIndex->ix == farIndex->ix && nearIndex->iy == farIndex->iy);
    assert(farIndex->iz > nearIndex->iz + 1);

    assert(brick.measuredVoxels == 2);
    assert(brick.interpolatedVoxels == 0);
    assert(brick.interpolatedTilts == 0);
    for (const auto flag : brick.interpolated) assert(flag == 0);
    // And the gap is honestly empty rather than filled.
    for (int iz = nearIndex->iz + 1; iz < farIndex->iz; ++iz) {
        assert(brick.present[brickFlatIndex(brick, nearIndex->ix, nearIndex->iy, iz)] == 0);
    }
}

void testOdimParallelDecode() {
    // Regression for the hdf5.dll AVs: ODIM decodes fan out across the pool
    // while GLM netCDF reads run on theirs, all inside one non-threadsafe
    // HDF5. Decode the DWD fixture from several threads at once; every copy
    // must come back identical.
    if (!odimReaderAvailable()) return;
    const auto bytes = readFixtureBytes("odim_deneu_dbzh.h5");
    if (bytes.empty()) {
        std::cout << "note: deneu fixture missing, skipping parallel asserts\n";
        return;
    }
    constexpr int kThreads = 4;
    constexpr int kIters = 5;
    std::mutex mutex;
    std::vector<std::size_t> rayCounts;
    std::vector<double> firstAzimuths;
    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&] {
            for (int i = 0; i < kIters; ++i) {
                std::string error;
                const auto chunk = readOdimPvol(bytes, "deneu", &error);
                if (!chunk.has_value() || chunk->rays.empty()) {
                    failures.fetch_add(1);
                    return;
                }
                const std::lock_guard<std::mutex> guard(mutex);
                rayCounts.push_back(chunk->rays.size());
                firstAzimuths.push_back(chunk->rays.front().azimuthDegrees);
            }
        });
    }
    for (auto& worker : workers) worker.join();
    assert(failures.load() == 0);
    assert(rayCounts.size() == static_cast<std::size_t>(kThreads * kIters));
    for (const auto count : rayCounts) assert(count == rayCounts.front());
    for (const auto azimuth : firstAzimuths) assert(azimuth == firstAzimuths.front());
}

int main() {
    testProducts();
    testDerivedProducts();
    testDerivedEchoTopLowerBoundWithMissingUpperData();
    testDerivedProductsRequireVolumeDepth();
    testCrefIsColumnMax();
    testCappiInterpolatesAndClamps();
    testDerivedSupportGate();
    testZrRateRelation();
    testRainfallAccumulationClosedForm();
    testRainfallClutterGuard();
    testRainfallDualPolVeto();
    testRainfallBeamHeightGuard();
    testRainfallGapClamping();
    testRainfallGeometryRemap();
    testRainfallIncompleteCoverage();
    testRainfallSentinelConvention();
    testRainfallInvalidWindows();
    testRainfallPartialReferenceRays();
    testDualPolRateRelations();
    testRainfallZdrAccumulationClosedForm();
    testRainfallKdpAccumulationClosedForm();
    testRainfallDualPolClampFlags();
    testRainfallDualPolUnavailable();
    testRainfallDualPolGuardParity();
    testStormTracker();
    testVolumeBrickTrilinear();
    testVolumeBrick();
    testVolumeBrickFillsBetweenTilts();
    testVolumeBrickDoesNotFillOneCone();
    testVolumeSpillRoundTrip();
    testDerivedProductsSupportNegativeFirstGate();
    testCatalog();
    testOperaCatalog();
    testBinaryPrimitives();
    testNativeEncodings();
    testRadarGeometry();
    testScanDiagnostics();
    testVelocityProcessing();
    testVadProfile();
    testBrightBand();
    testBunkers();
    testHailSwaths();
    testLambert();
    testHrrrFixture();
    testFixedGrid();
    testGoesFixture();
    testGoes08Fixture();
    testMosaicFrame();
    testSounding();
    testSrh();
    testTbss();
    testZdrColumns();
    testMessage31MomentLayout();
    testMessage31SixteenBitMoment();
    testMessage31SentinelConvention();
    testMessage31RealArchiveChunkSentinelConvention();
    testMessage31MalformedInputs();
    testLevel2VolumeCoveragePattern();
    testRainfallDualPolRealArchive();
    testLevel2VolumeCoveragePatternSyntheticBytesOnly();
    testOptionalRealChunk();
    testLightningJumpDetection();
    testLightningJumpNoiseFloor();
    testFlashRateGrid();
    testFlashRateGridUsesConfiguredDetectorAndPrunes();
    testGlmL2Keys();
    testGlmReaderBuildContract();
    testOdimReader();
    testOdimAzimuthConvention();
    testOdimSentinelConvention();
    testOdimMinimalContainerMixedSentinels();
    testOdimNyquistFromHow();
    testOdimDualPolAbsentOnSinglePolVolumes();
    testOdimDualPolFromRealDwdSweeps();
    testOdimDualPolSpecShapedDataset();
    testMomentSentinelConventionFlipsBetweenFormats();
    testSpillCarriesMomentEncoding();
    testOdimParallelDecode();
    testRadarLightningFusion();
    testRadarLightningFusionSplitCut();
    testSlantRangeVerticalBeam();
    testLightningJumpSanitizedAndFarFuturePoll();
    testStormTrackerClearAndFixCap();
    testStormTrackerMotionPrediction();
    testStormTrackerSplitParent();
    testStormTrackerIntensityGate();
    testCirculationMesocyclonePositive();
    testCirculationTvsClassification();
    testCirculationReflectivityVeto();
    testCirculationSingleTiltVeto();
    testCirculationDealiasAndWeakVeto();
    testCirculationOverUnfoldVeto();
    testVelocityConditionalUnfold();
    testVelocityRespectsRayNyquist();
    testCirculationSameSign();
    testCirculationSingleTiltTvsOptIn();
    testShearGridFindsCouplet();
    testCirculationShallowVeto();
    testShearWatchQuietUniformFlow();
    testCirculationMisalignedAndGapVeto();
    testHailTierBoundaries();
    testHailCorePositive();
    testHailVetoes();
    testHailZeroTopNeverDivides();
    testHailClustering();
    testTdsPositive();
    testTdsContrastVeto();
    testTdsVetoes();
    testTdsNanParentFailsClosed();
    testDecodedRayLegacyGeometryMaxGates();
    std::cout << "core tests passed\n";
}
