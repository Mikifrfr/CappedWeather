#pragma once

// IEM NEXRAD N0Q national mosaic as observed context: the archive PNG plus
// its world file decode to a linear lat/lon grid (EPSG:4326, verified live
// 2026-10-10: 0.005 deg pixels, UL (-126, 50)). Timestamps ride the
// filenames (n0q_YYYYmmddHHMM.png, 5-minute cadence). Pure parsing and grid
// math here, no Qt: PNG decode itself lives in the app source (QImageReader
// with scaled-down decode). Fail closed on every malformed input.

#include <cstddef>
#include <optional>
#include <string>

namespace cappedweather {

// Linear mosaic grid: lon(ix) = westLongitude + ix * degreesPerPixel,
// lat(iy) = northLatitude + iy * degreesPerPixelY (negative: row 0 north).
struct MosaicGrid {
    double westLongitude = 0.0;
    double northLatitude = 0.0;
    double degreesPerPixelX = 0.0;
    double degreesPerPixelY = 0.0;
    std::size_t nx = 0;
    std::size_t ny = 0;
};

// "n0q_202610101730.png" (or a full key/path ending in it) to unix seconds
// UTC. Nullopt on any shape mismatch.
std::optional<long long> parseMosaicTime(const std::string& fileName);

// Six-line world file to a grid: pixel size, no rotation, UL corner.
// Image dimensions supplied by the decoded PNG. Nullopt unless dx > 0,
// dy < 0, rotation terms are exactly 0, and the corner is finite.
std::optional<MosaicGrid> parseWorldFile(const std::string& text, std::size_t nx, std::size_t ny);

// Cell center lon/lat. NaN when the grid is degenerate.
void mosaicCellLonLat(const MosaicGrid& grid, std::size_t ix, std::size_t iy, double& lon,
                      double& lat);

}  // namespace cappedweather
