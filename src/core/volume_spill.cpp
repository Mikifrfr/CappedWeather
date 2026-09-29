#include "core/volume_spill.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <type_traits>
#include <vector>

namespace cappedweather {
namespace {

constexpr std::uint32_t kMagic = 0x43574456; // "CWDV"
constexpr std::uint16_t kVersion = 1;

template <typename T>
void appendLe(std::vector<std::byte>& out, const T value) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::byte bytes[sizeof(T)];
    std::memcpy(bytes, &value, sizeof(T));
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    for (std::size_t i = 0; i < sizeof(T) / 2; ++i)
        std::swap(bytes[i], bytes[sizeof(T) - 1 - i]);
#endif
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

template <typename T>
bool readLe(const std::byte*& cursor, const std::byte* end, T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (cursor + sizeof(T) > end) return false;
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    std::byte flipped[sizeof(T)];
    for (std::size_t i = 0; i < sizeof(T); ++i) flipped[i] = cursor[sizeof(T) - 1 - i];
    std::memcpy(&value, flipped, sizeof(T));
#else
    std::memcpy(&value, cursor, sizeof(T));
#endif
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
    std::vector<std::byte> out;
    out.reserve(chunk.rays.size() * 2048);
    appendLe(out, kMagic);
    appendLe(out, kVersion);
    appendLe(out, static_cast<std::uint16_t>(kProductCount));
    appendLe(out, static_cast<std::uint64_t>(chunk.radarId.size()));
    out.insert(out.end(), reinterpret_cast<const std::byte*>(chunk.radarId.data()),
               reinterpret_cast<const std::byte*>(chunk.radarId.data() + chunk.radarId.size()));
    appendLe(out, static_cast<std::uint64_t>(chunk.messageCount));
    appendLe(out, static_cast<std::uint64_t>(chunk.rejectedMessages));
    appendLe(out, static_cast<std::uint64_t>(chunk.rejectedBlocks));
    appendLe(out, static_cast<std::uint64_t>(chunk.rays.size()));
    for (const auto& ray : chunk.rays) {
        appendLe(out, ray.azimuthDegrees);
        appendLe(out, ray.azimuthResolutionDegrees);
        appendLe(out, ray.elevationNumber);
        appendLe(out, ray.elevationDegrees);
        appendLe(out, ray.gateCount);
        appendLe(out, ray.firstGateMeters);
        appendLe(out, ray.gateSpacingMeters);
        for (std::size_t product = 0; product < kProductCount; ++product) {
            const auto& gates = ray.fields[product];
            appendLe(out, static_cast<std::uint64_t>(gates.size()));
            if (!gates.empty()) {
                const auto* raw = reinterpret_cast<const std::byte*>(gates.data());
                out.insert(out.end(), raw, raw + gates.size() * sizeof(std::uint16_t));
            }
            appendLe(out, ray.firstGateByProduct[product]);
            appendLe(out, ray.gateSpacingByProduct[product]);
            appendLe(out, ray.scaleByProduct[product]);
            appendLe(out, ray.offsetByProduct[product]);
        }
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
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
    if (!readLe(cursor, end, version) || version != kVersion) return nullptr;
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
    if (cursor != end) return nullptr; // trailing garbage: not our file
    return chunk;
}

} // namespace cappedweather
