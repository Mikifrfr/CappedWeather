#pragma once

#include "core/level2_decoder.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>

namespace cappedweather {

// Reads an EUMETNET OPERA ODIM_H5 polar volume (PVOL) from memory into the
// shared DecodedRay model, so European radars flow through the same panels,
// tilts, products, probing, and export paths as NEXRAD volumes.
//
// Moment mapping: DBZH -> Reflectivity, VRADH -> Velocity. TH (unfiltered
// reflectivity) backs Reflectivity only when an elevation carries no DBZH.
// Dual-polarization quantities are skipped: OPERA serves them from 2027+.
// Elevations are numbered sequentially sorted by angle because ODIM carries
// no stable tilt numbering across sites. Gate encoding reuses the NEXRAD
// 16-bit convention (0 = missing, 1 = below threshold) with matching
// scale/offset pairs, so downstream math is untouched.
//
// Returns nullopt with *error set when the buffer is not a readable PVOL.
// Requires HDF5 at build time; without it every call fails closed, see
// odimReaderAvailable().
std::optional<DecodedChunk> readOdimPvol(std::span<const std::byte> bytes,
                                         std::string radarId,
                                         std::string* error = nullptr);

// True when this build linked HDF5 and can actually decode ODIM volumes.
bool odimReaderAvailable();

} // namespace cappedweather
