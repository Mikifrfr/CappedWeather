#include "core/volume_spill.hpp"

#include <bit>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <type_traits>
#include <vector>

namespace cappedweather {
namespace {

constexpr std::uint32_t kMagic = 0x43574456; // "CWDV"
// v2 appends the volume-level Volume Coverage Pattern after the ray table.
// v1 files still load: their pattern reads as 0 (not carried), which is the
// same value the label already renders as "no token". A v2 file read by an
// older build fails clean instead of being misread.
constexpr std::uint16_t kVersion = 2;
constexpr std::uint16_t kOldestVersion = 1;

template <typename T>
void toLeBytes(const T value, std::byte* out) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::memcpy(out, &value, sizeof(T));
    if constexpr (std::endian::native == std::endian::big) {
        for (std::size_t i = 0; i < sizeof(T) / 2; ++i)
            std::swap(out[i], out[sizeof(T) - 1 - i]);
    }
}

template <typename T>
void appendLe(std::vector<std::byte>& out, const T value) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::byte bytes[sizeof(T)];
    toLeBytes(value, bytes);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

template <typename T>
bool writeLe(std::ofstream& file, const T value) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::byte bytes[sizeof(T)];
    toLeBytes(value, bytes);
    file.write(reinterpret_cast<const char*>(bytes), sizeof(T));
    return static_cast<bool>(file);
}

template <typename T>
bool readLe(const std::byte*& cursor, const std::byte* end, T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (cursor + sizeof(T) > end) return false;
    if constexpr (std::endian::native == std::endian::big) {
        std::byte flipped[sizeof(T)];
        for (std::size_t i = 0; i < sizeof(T); ++i) flipped[i] = cursor[sizeof(T) - 1 - i];
        std::memcpy(&value, flipped, sizeof(T));
    } else {
        std::memcpy(&value, cursor, sizeof(T));
    }
    cursor += sizeof(T);
    return true;
}

bool readBytes(const std::byte*& cursor, const std::byte* end, char* out, std::size_t count) {
    if (cursor + count > end) return false;
    std::memcpy(out, cursor, count);
    cursor += count;
    return true;
}

} // namespace

bool spillVolume(const DecodedChunk& chunk, const std::string& path) {
    // Stream directly to disk instead of building a full in-memory copy:
    // a volume-sized buffer plus the vector peak was ~2x RAM per spill.
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    if (!writeLe(file, kMagic)) return false;
    if (!writeLe(file, kVersion)) return false;
    if (!writeLe(file, static_cast<std::uint16_t>(kProductCount))) return false;
    if (!writeLe(file, static_cast<std::uint64_t>(chunk.radarId.size()))) return false;
    if (!chunk.radarId.empty()) {
        file.write(chunk.radarId.data(), static_cast<std::streamsize>(chunk.radarId.size()));
        if (!file) return false;
    }
    if (!writeLe(file, static_cast<std::uint64_t>(chunk.messageCount))) return false;
    if (!writeLe(file, static_cast<std::uint64_t>(chunk.rejectedMessages))) return false;
    if (!writeLe(file, static_cast<std::uint64_t>(chunk.rejectedBlocks))) return false;
    if (!writeLe(file, static_cast<std::uint64_t>(chunk.rays.size()))) return false;
    for (const auto& ray : chunk.rays) {
        if (!writeLe(file, ray.azimuthDegrees)) return false;
        if (!writeLe(file, ray.azimuthResolutionDegrees)) return false;
        if (!writeLe(file, ray.elevationNumber)) return false;
        if (!writeLe(file, ray.elevationDegrees)) return false;
        if (!writeLe(file, ray.gateCount)) return false;
        if (!writeLe(file, ray.firstGateMeters)) return false;
        if (!writeLe(file, ray.gateSpacingMeters)) return false;
        for (std::size_t product = 0; product < kProductCount; ++product) {
            const auto& gates = ray.fields[product];
            if (!writeLe(file, static_cast<std::uint64_t>(gates.size()))) return false;
            if (!gates.empty()) {
                // Gate payload is native LE uint16; convert on big-endian hosts.
                if constexpr (std::endian::native == std::endian::big) {
                    for (const auto gate : gates) {
                        std::byte bytes[sizeof(gate)];
                        toLeBytes(gate, bytes);
                        file.write(reinterpret_cast<const char*>(bytes), sizeof(gate));
                        if (!file) return false;
                    }
                } else {
                    file.write(reinterpret_cast<const char*>(gates.data()),
                               static_cast<std::streamsize>(gates.size() * sizeof(std::uint16_t)));
                    if (!file) return false;
                }
            }
            if (!writeLe(file, ray.firstGateByProduct[product])) return false;
            if (!writeLe(file, ray.gateSpacingByProduct[product])) return false;
            if (!writeLe(file, ray.scaleByProduct[product])) return false;
            if (!writeLe(file, ray.offsetByProduct[product])) return false;
        }
    }
    if (!writeLe(file, chunk.volumeCoveragePattern)) return false;
    file.close();
    return static_cast<bool>(file);
}

std::shared_ptr<DecodedChunk> loadSpilledVolume(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return nullptr;
    const auto size = file.tellg();
    if (size <= 0 || size > std::streamoff(1024LL * 1024 * 1024)) return nullptr;
    std::vector<std::byte> data(static_cast<std::size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(data.data()), size)) return nullptr;
    const std::byte* cursor = data.data();
    const std::byte* end = cursor + data.size();
    std::uint32_t magic = 0;
    std::uint16_t version = 0, productCount = 0;
    if (!readLe(cursor, end, magic) || magic != kMagic) return nullptr;
    if (!readLe(cursor, end, version) || version < kOldestVersion || version > kVersion) return nullptr;
    if (!readLe(cursor, end, productCount) || productCount != kProductCount) return nullptr;
    auto chunk = std::make_shared<DecodedChunk>();
    std::uint64_t radarIdSize = 0;
    if (!readLe(cursor, end, radarIdSize) || radarIdSize > 64) return nullptr;
    chunk->radarId.resize(static_cast<std::size_t>(radarIdSize));
    if (!readBytes(cursor, end, chunk->radarId.data(), chunk->radarId.size())) return nullptr;
    std::uint64_t messageCount = 0, rejectedMessages = 0, rejectedBlocks = 0, rayCount = 0;
    if (!readLe(cursor, end, messageCount) || !readLe(cursor, end, rejectedMessages) ||
        !readLe(cursor, end, rejectedBlocks) || !readLe(cursor, end, rayCount))
        return nullptr;
    if (rayCount > 100000) return nullptr;
    chunk->messageCount = static_cast<std::size_t>(messageCount);
    chunk->rejectedMessages = static_cast<std::size_t>(rejectedMessages);
    chunk->rejectedBlocks = static_cast<std::size_t>(rejectedBlocks);
    chunk->rays.reserve(static_cast<std::size_t>(rayCount));
    for (std::uint64_t r = 0; r < rayCount; ++r) {
        DecodedRay ray;
        if (!readLe(cursor, end, ray.azimuthDegrees) ||
            !readLe(cursor, end, ray.azimuthResolutionDegrees) || !readLe(cursor, end, ray.elevationNumber) ||
            !readLe(cursor, end, ray.elevationDegrees) || !readLe(cursor, end, ray.gateCount) ||
            !readLe(cursor, end, ray.firstGateMeters) || !readLe(cursor, end, ray.gateSpacingMeters))
            return nullptr;
        for (std::size_t product = 0; product < kProductCount; ++product) {
            std::uint64_t gateSize = 0;
            if (!readLe(cursor, end, gateSize) || gateSize > 100000) return nullptr;
            ray.fields[product].resize(static_cast<std::size_t>(gateSize));
            if (gateSize > 0) {
                const std::size_t bytes = static_cast<std::size_t>(gateSize) * sizeof(std::uint16_t);
                if (cursor + bytes > end) return nullptr;
                std::memcpy(ray.fields[product].data(), cursor, bytes);
                if constexpr (std::endian::native == std::endian::big) {
                    for (auto& gate : ray.fields[product]) {
                        gate = static_cast<std::uint16_t>((gate << 8) | (gate >> 8));
                    }
                }
                cursor += bytes;
            }
            if (!readLe(cursor, end, ray.firstGateByProduct[product]) ||
                !readLe(cursor, end, ray.gateSpacingByProduct[product]) ||
                !readLe(cursor, end, ray.scaleByProduct[product]) ||
                !readLe(cursor, end, ray.offsetByProduct[product]))
                return nullptr;
        }
        chunk->rays.push_back(std::move(ray));
    }
    if (version >= 2 && !readLe(cursor, end, chunk->volumeCoveragePattern)) return nullptr;
    if (cursor != end) return nullptr; // trailing garbage: not our file
    return chunk;
}

} // namespace cappedweather
