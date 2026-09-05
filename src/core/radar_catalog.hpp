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
    // Data backend: "nexrad" (NWS Level II) or "opera" (EUMETNET OPERA
    // ODIM volumes). Defaults to nexrad so existing initializers compile.
    std::string dataSource = "nexrad";
    // OPERA-only: lowercase ODIM station code (e.g. "nohur") and the S3
    // country segment (e.g. "NO"). Empty for NEXRAD sites.
    std::string odimCode;
    std::string countryCode;
};

class RadarCatalog {
public:
    static std::vector<RadarSite> defaultSites();
    // EUMETNET OPERA single-site catalog (CC BY 4.0). Operational,
    // geolocated sites only; see src/core/opera_catalog.cpp.
    static std::vector<RadarSite> operaSites();
    static bool isValidRadarId(const std::string& id);
};

} // namespace cappedweather
