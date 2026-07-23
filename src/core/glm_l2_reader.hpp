#pragma once

#include "core/lightning_flash.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cappedweather {

// GOES-R series epoch: product_time counts seconds since 2000-01-01T12:00:00Z.
inline constexpr std::int64_t kGoesEpochUnixSec = 946728000;

// --- S3 key helpers (netCDF-free, unit-tested) ---

// Bucket-relative listing prefix, e.g. "GLM-L2-LCFA/2026/201/14/".
// Buckets: noaa-goes19 (GOES-East), noaa-goes18 (GOES-West), anonymous, us-east-1.
std::string glmLcfaPrefix(int year, int dayOfYear, int hour);

// Parses "OR_GLM-L2-LCFA_G19_s20262011400000_e20262011400200_c20262011400220.nc"
// and returns the scan-start time as unix seconds, or nullopt on mismatch.
std::optional<std::int64_t> parseGlmLcfaStartTime(const std::string& fileName);

// --- netCDF parsing ---

// True when the core was built with netCDF support (CAPPEDWEATHER_HAS_NETCDF).
bool glmL2ReaderAvailable();

// Reads a GLM L2 LCFA netCDF-4 file into flashes (one record per flash_id).
// Returns nullopt when netCDF support is unavailable or the file is malformed.
std::optional<std::vector<LightningFlash>> readGlmL2Lcfa(const std::string& path);

} // namespace cappedweather
