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
    // NEXRAD 4-letter (all-upper) or OPERA 5-letter (upper+digit) shape check.
    // Note: Iceland's 4-letter digit codes (ISX1/ISX2) are real OPERA sites
    // but intentionally invalid here; check OPERA membership via operaSites()
    // instead of widening this to digits.
    static bool isValidRadarId(const std::string& id);
};

} // namespace cappedweather
