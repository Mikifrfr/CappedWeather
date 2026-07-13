#pragma once

#include <string>
#include <optional>
#include <vector>

namespace cappedweather {

struct RadarSite {
    std::string id;
    std::string name;
    std::string owner;
    std::optional<double> latitude;
    std::optional<double> longitude;
    double antennaElevationMeters;
};

class RadarCatalog {
public:
    static std::vector<RadarSite> defaultSites();
    static bool isValidRadarId(const std::string& id);
};

} // namespace cappedweather
