// glm_replay: replays archived GLM L2 LCFA netCDF files through the
// lightning-jump engine (FlashRateGrid + LJA) and prints jump events.
//
// Usage:
//   glm_replay <dir-with-nc-files> [--cell DEG] [--min-rate N]
//              [--window SEC] [--resolution SEC] [--csv]
//
// Example:
//   glm_replay data/glm/20250516 --cell 0.25 --min-rate 12 --window 600

#include "core/flash_rate_grid.hpp"
#include "core/glm_l2_reader.hpp"
#include "core/lightning_flash.hpp"
#include "core/lightning_jump.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

std::string isoTime(const std::int64_t epochSec) {
    const std::time_t t = static_cast<std::time_t>(epochSec);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

void usage() {
    std::fprintf(stderr,
        "usage: glm_replay <dir> [--cell DEG] [--min-rate N] [--window SEC] [--resolution SEC] [--csv]\n");
}

} // namespace

int main(int argc, char** argv) {
    using namespace cappedweather;

    if (argc < 2) {
        usage();
        return EXIT_FAILURE;
    }

    std::string dir = argv[1];
    FlashRateGridConfig gridConfig{};
    bool csv = false;
    for (int i = 2; i < argc;) {
        const std::string flag = argv[i];
        if (flag == "--csv") {
            csv = true;
            ++i;
            continue;
        }
        if (i + 1 >= argc) {
            usage();
            return EXIT_FAILURE;
        }
        const double value = std::atof(argv[i + 1]);
        if (flag == "--cell") {
            gridConfig.cellSizeDeg = value;
        } else if (flag == "--min-rate") {
            gridConfig.jump.minFlashRatePerMin = value;
        } else if (flag == "--window") {
            gridConfig.jump.sigmaWindowSec = value;
        } else if (flag == "--resolution") {
            gridConfig.jump.rateResolutionSec = value;
        } else {
            usage();
            return EXIT_FAILURE;
        }
        i += 2;
    }

    if (!glmL2ReaderAvailable()) {
        std::fprintf(stderr, "glm_replay: built without netCDF support; cannot parse LCFA files\n");
        return EXIT_FAILURE;
    }

    // Collect .nc files in scan-start order.
    std::vector<std::pair<std::int64_t, std::filesystem::path>> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
        if (entry.path().extension() != ".nc") {
            continue;
        }
        const auto start = parseGlmLcfaStartTime(entry.path().filename().string());
        if (start.has_value()) {
            files.emplace_back(*start, entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        std::fprintf(stderr, "glm_replay: no GLM LCFA .nc files found in %s\n", dir.c_str());
        return EXIT_FAILURE;
    }

    // Extract all flashes.
    std::vector<LightningFlash> flashes;
    int failedFiles = 0;
    for (const auto& [start, path] : files) {
        auto parsed = readGlmL2Lcfa(path.string());
        if (!parsed.has_value()) {
            ++failedFiles;
            continue;
        }
        flashes.insert(flashes.end(), parsed->begin(), parsed->end());
    }
    std::sort(flashes.begin(), flashes.end(),
              [](const LightningFlash& a, const LightningFlash& b) { return a.timeEpochSec < b.timeEpochSec; });
    if (flashes.empty()) {
        std::fprintf(stderr, "glm_replay: parsed 0 flashes from %zu files\n", files.size());
        return EXIT_FAILURE;
    }

    // Replay at 60 s cadence through the grid.
    FlashRateGrid grid(gridConfig);
    const std::int64_t t0 = static_cast<std::int64_t>(flashes.front().timeEpochSec / 60.0) * 60 + 60;
    const std::int64_t t1 = static_cast<std::int64_t>(flashes.back().timeEpochSec) + 60;

    std::unordered_map<std::int64_t, std::int64_t> cellFlashTotals;
    int jumpStarts = 0;
    int jumpUpdates = 0;
    int cessations = 0;
    double maxSigma = 0.0;
    JumpEvent maxSigmaEvent{};

    std::size_t next = 0;
    if (csv) std::printf("time,kind,latitude,longitude,flash_rate_per_min,sigma_level,cell_x,cell_y\n");
    for (std::int64_t t = t0; t <= t1; t += 60) {
        while (next < flashes.size() && flashes[next].timeEpochSec < static_cast<double>(t)) {
            const auto& f = flashes[next];
            grid.addFlash(f.timeEpochSec, f.latitudeDeg, f.longitudeDeg);
            const int cx = static_cast<int>(std::floor(f.longitudeDeg / gridConfig.cellSizeDeg));
            const int cy = static_cast<int>(std::floor(f.latitudeDeg / gridConfig.cellSizeDeg));
            cellFlashTotals[(static_cast<std::int64_t>(cx) << 32) ^ (cy & 0xFFFFFFFFLL)] += 1;
            ++next;
        }
        for (const JumpEvent& event : grid.poll(t)) {
            const double latCenter = (event.cellY + 0.5) * gridConfig.cellSizeDeg;
            const double lonCenter = (event.cellX + 0.5) * gridConfig.cellSizeDeg;
            const char* kind = event.kind == JumpEventKind::JumpStart ? "JUMP_START"
                             : event.kind == JumpEventKind::JumpUpdate ? "JUMP_UPDATE"
                                                                       : "CESSATION ";
            if (csv) {
                std::printf("%s,%s,%.6f,%.6f,%.3f,%.6f,%d,%d\n",
                            isoTime(event.timeEpochSec).c_str(), kind, latCenter, lonCenter,
                            event.flashRatePerMin, event.sigmaLevel, event.cellX, event.cellY);
            } else {
                std::printf("%s %s cell(%7.2f,%8.2f) rate=%6.1f fl/min sigma=%5.2f\n",
                            isoTime(event.timeEpochSec).c_str(), kind, latCenter, lonCenter,
                            event.flashRatePerMin, event.sigmaLevel);
            }
            if (event.kind == JumpEventKind::JumpStart) {
                ++jumpStarts;
            } else if (event.kind == JumpEventKind::JumpUpdate) {
                ++jumpUpdates;
            } else {
                ++cessations;
            }
            if (event.sigmaLevel > maxSigma) {
                maxSigma = event.sigmaLevel;
                maxSigmaEvent = event;
            }
        }
    }

    // Summary.
    auto* summary = csv ? stderr : stdout;
    std::fprintf(summary, "\n=== replay summary ===\n");
    std::fprintf(summary, "files: %zu ok, %d failed\n", files.size() - failedFiles, failedFiles);
    std::fprintf(summary, "flashes: %zu   cells touched: %zu\n", flashes.size(), cellFlashTotals.size());
    std::fprintf(summary, "jump starts: %d   updates: %d   cessations: %d\n", jumpStarts, jumpUpdates, cessations);
    if (maxSigma > 0.0) {
        std::fprintf(summary, "max sigma: %.2f at %s cell(%7.2f,%8.2f)\n", maxSigma,
                    isoTime(maxSigmaEvent.timeEpochSec).c_str(),
                    (maxSigmaEvent.cellY + 0.5) * gridConfig.cellSizeDeg,
                    (maxSigmaEvent.cellX + 0.5) * gridConfig.cellSizeDeg);
    }

    std::vector<std::pair<std::int64_t, std::int64_t>> cells(cellFlashTotals.begin(), cellFlashTotals.end());
    std::sort(cells.begin(), cells.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::fprintf(summary, "\ntop cells by flashes:\n");
    for (std::size_t i = 0; i < cells.size() && i < 8; ++i) {
        const int cx = static_cast<int>(cells[i].first >> 32);
        const int cy = static_cast<int>(cells[i].first & 0xFFFFFFFFLL);
        std::fprintf(summary, "  cell(%7.2f,%8.2f): %lld flashes\n",
                    (cy + 0.5) * gridConfig.cellSizeDeg, (cx + 0.5) * gridConfig.cellSizeDeg,
                    static_cast<long long>(cells[i].second));
    }
    return EXIT_SUCCESS;
}
