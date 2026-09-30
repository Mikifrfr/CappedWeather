#pragma once

#include <cmath>
#include <optional>
#include <string>

namespace cappedweather {

// Shared safe double parsing for embedded catalogs and CSV inputs.
// Never throws: returns nullopt on empty / trailing garbage / non-finite.
inline std::optional<double> parseDoubleSafe(const std::string& text) {
    if (text.empty()) return std::nullopt;
    try {
        std::size_t used = 0;
        const double value = std::stod(text, &used);
        if (used != text.size() || !std::isfinite(value)) return std::nullopt;
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace cappedweather
