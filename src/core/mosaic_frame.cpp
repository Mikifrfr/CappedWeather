#include "core/mosaic_frame.hpp"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace cappedweather {
namespace {

// Strict fixed-width digits, sscanf-free (MSVC warning-clean).
bool takeDigits(const std::string& text, const std::size_t pos, const std::size_t width,
                int& out) {
    if (pos + width > text.size()) return false;
    int value = 0;
    for (std::size_t i = 0; i < width; ++i) {
        const char digit = text[pos + i];
        if (digit < '0' || digit > '9') return false;
        value = value * 10 + (digit - '0');
    }
    out = value;
    return true;
}

bool takeDouble(const std::string& text, double& out) {
    if (text.empty() || text.size() > 32) return false;
    std::size_t pos = 0;
    bool negative = false;
    if (text[pos] == '+' || text[pos] == '-') {
        negative = text[pos] == '-';
        ++pos;
    }
    bool anyDigits = false;
    double value = 0.0;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        value = value * 10.0 + (text[pos] - '0');
        ++pos;
        anyDigits = true;
    }
    if (pos < text.size() && text[pos] == '.') {
        ++pos;
        double place = 0.1;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            value += (text[pos] - '0') * place;
            place *= 0.1;
            ++pos;
            anyDigits = true;
        }
    }
    if (!anyDigits || pos != text.size()) return false;
    out = negative ? -value : value;
    return std::isfinite(out);
}

}  // namespace

std::optional<long long> parseMosaicTime(const std::string& fileName) {
    // Tail match: ".../n0q_202610101730.png" or with a query suffix.
    const auto n0q = fileName.rfind("n0q_");
    if (n0q == std::string::npos) return std::nullopt;
    const std::string tail = fileName.substr(n0q + 4);
    if (tail.size() < 16 || tail[12] != '.' ) return std::nullopt;
    int year = 0, month = 0, day = 0, hour = 0, minute = 0;
    if (!takeDigits(tail, 0, 4, year) || !takeDigits(tail, 4, 2, month) ||
        !takeDigits(tail, 6, 2, day) || !takeDigits(tail, 8, 2, hour) ||
        !takeDigits(tail, 10, 2, minute))
        return std::nullopt;
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59)
        return std::nullopt;
    // Extension must be .png (allow trailing query/fragment after it).
    if (tail.compare(12, 4, ".png") != 0) return std::nullopt;
    // Days-from-civil (Howard Hinnant), UTC.
    int y = year;
    const unsigned m = static_cast<unsigned>(month);
    y -= m <= 2 ? 1 : 0;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy =
        (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + static_cast<unsigned>(day) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long long days = era * 146097 + static_cast<long long>(doe) - 719468;
    return days * 86400LL + static_cast<long long>(hour) * 3600LL +
           static_cast<long long>(minute) * 60LL;
}

std::optional<MosaicGrid> parseWorldFile(const std::string& text, const std::size_t nx,
                                         const std::size_t ny) {
    if (nx == 0 || ny == 0 || nx > 32768 || ny > 32768) return std::nullopt;
    // Six whitespace-separated lines; be liberal with trailing whitespace.
    std::vector<std::string> lines;
    std::string current;
    for (const char ch : text) {
        if (ch == '\n' || ch == '\r') {
            if (!current.empty()) {
                lines.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) lines.push_back(current);
    auto trim = [](std::string s) {
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
        return s;
    };
    if (lines.size() < 6) return std::nullopt;
    double values[6] = {};
    for (int i = 0; i < 6; ++i) {
        if (!takeDouble(trim(lines[i]), values[i])) return std::nullopt;
    }
    if (!(values[0] > 0.0) || values[1] != 0.0 || values[2] != 0.0 || !(values[3] < 0.0) ||
        !std::isfinite(values[4]) || !std::isfinite(values[5]))
        return std::nullopt;
    MosaicGrid grid;
    grid.degreesPerPixelX = values[0];
    grid.degreesPerPixelY = values[3];
    grid.westLongitude = values[4];
    grid.northLatitude = values[5];
    grid.nx = nx;
    grid.ny = ny;
    return grid;
}

void mosaicCellLonLat(const MosaicGrid& grid, const std::size_t ix, const std::size_t iy,
                      double& lon, double& lat) {
    if (grid.nx == 0 || grid.ny == 0 || ix >= grid.nx || iy >= grid.ny ||
        !(grid.degreesPerPixelX > 0.0) || !(grid.degreesPerPixelY < 0.0)) {
        lon = std::numeric_limits<double>::quiet_NaN();
        lat = std::numeric_limits<double>::quiet_NaN();
        return;
    }
    lon = grid.westLongitude + (static_cast<double>(ix) + 0.5) * grid.degreesPerPixelX;
    lat = grid.northLatitude + (static_cast<double>(iy) + 0.5) * grid.degreesPerPixelY;
}

}  // namespace cappedweather
