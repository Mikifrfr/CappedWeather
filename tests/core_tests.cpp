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
    assert(productDescriptors().size() == 10);
    assert(productDescriptor(ProductKind::Reflectivity).unit == "dBZ");
    assert(productDescriptor(ProductKind::CorrelationCoefficient).maximum == 1.05);
    assert(productDescriptor(ProductKind::EchoTop).unit == "kft MSL");
    assert(productDescriptor(ProductKind::VerticallyIntegratedLiquid).unit == "kg/m2");
    assert(productDescriptor(ProductKind::CompositeReflectivity).unit == "dBZ");
    assert(productDescriptor(ProductKind::Cappi).unit == "dBZ");
    assert(isDerivedProduct(ProductKind::EchoTop));
    assert(isDerivedProduct(ProductKind::VerticallyIntegratedLiquid));
    assert(isDerivedProduct(ProductKind::CompositeReflectivity));
    assert(isDerivedProduct(ProductKind::Cappi));
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
    testStormTracker();
    testVolumeBrick();
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
    testMessage31MalformedInputs();
    testOptionalRealChunk();
    testLightningJumpDetection();
    testLightningJumpNoiseFloor();
    testFlashRateGrid();
    testFlashRateGridUsesConfiguredDetectorAndPrunes();
    testGlmL2Keys();
    testGlmReaderBuildContract();
    testOdimReader();
    testOdimAzimuthConvention();
    testOdimNyquistFromHow();
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
