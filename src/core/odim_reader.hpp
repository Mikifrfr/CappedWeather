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
// 16-bit scale/offset pairs so downstream math is untouched, but the sentinel
// order is the reverse of Message 31's: 0 = missing (nodata), 1 = below
// threshold (undetect). The chunk records MomentEncoding::Odim so consumers
// can decode the two codes instead of guessing the format.
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
