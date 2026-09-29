#pragma once

#include "core/level2_decoder.hpp"

#include <memory>
#include <string>

namespace cappedweather {

// Spill-to-disk for decoded volumes: a versioned little-endian binary dump
// of DecodedChunk (rays, per-product gate tables, counters). Lets the frame
// history outgrow RAM without dropping old volumes: evicted frames are
// written out and faulted back in when scrubbed to. Any format mismatch or
// I/O error fails clean (false / null) and the caller treats the frame as
// missing — never partial data.
bool spillVolume(const DecodedChunk& chunk, const std::string& path);
std::shared_ptr<DecodedChunk> loadSpilledVolume(const std::string& path);

} // namespace cappedweather
