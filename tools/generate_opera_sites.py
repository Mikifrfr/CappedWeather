"""Regenerate src/core/opera_catalog.cpp from the OPERA station CSV.

Usage:
    python tools/generate_opera_sites.py <OPERA_RADARS.csv>

The CSV is EUMETNET's openradardata-validator station list
(CC BY 4.0). Only operational, geolocated sites are emitted.
The output file is committed; rerun when OPERA publishes updates.
"""
import csv
import sys

HEADER = '''#include "core/radar_catalog.hpp"

#include <sstream>

namespace cappedweather {

// EUMETNET OPERA single-site radar catalog (CC BY 4.0 EUMETNET).
// Source: openradardata-validator stations/OPERA_RADARS.csv, operational
// sites with coordinates only. Regenerate with tools/generate_opera_sites.py.
// Columns: ID|Location, Country|OPERA|antennaM|lat|lon|opera|odim|CC
std::vector<RadarSite> RadarCatalog::operaSites() {
    static constexpr char kOperaCatalog[] = R"CATALOG(
'''

FOOTER = ''')CATALOG";

    std::vector<RadarSite> sites;
    std::istringstream input{kOperaCatalog};
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        std::istringstream fields(line);
        std::string id;
        std::string name;
        std::string owner;
        std::string elevationMeters;
        std::string latitude;
        std::string longitude;
        std::string dataSource;
        std::string odimCode;
        std::string countryCode;
        if (!std::getline(fields, id, '|') || !std::getline(fields, name, '|') ||
            !std::getline(fields, owner, '|') || !std::getline(fields, elevationMeters, '|') ||
            !std::getline(fields, latitude, '|') || !std::getline(fields, longitude, '|') ||
            !std::getline(fields, dataSource, '|') || !std::getline(fields, odimCode, '|') ||
            !std::getline(fields, countryCode)) {
            continue;
        }
        std::optional<double> parsedLatitude;
        std::optional<double> parsedLongitude;
        if (!latitude.empty() && !longitude.empty()) {
            parsedLatitude = std::stod(latitude);
            parsedLongitude = std::stod(longitude);
        }
        RadarSite site{std::move(id), std::move(name), std::move(owner), parsedLatitude,
                       parsedLongitude, std::stod(elevationMeters)};
        site.dataSource = std::move(dataSource);
        site.odimCode = std::move(odimCode);
        site.countryCode = std::move(countryCode);
        sites.push_back(std::move(site));
    }
    return sites;
}

} // namespace cappedweather
'''


def num(x, d=0.0):
    try:
        return float(x)
    except (TypeError, ValueError):
        return d


def main():
    rows = list(csv.DictReader(open(sys.argv[1], encoding='utf-8-sig')))
    out = []
    for r in rows:
        if r['Opera Status'] != '1':
            continue
        if not r['Latitude'] or not r['Longitude'] or not r['ODIM code'].strip():
            continue
        code = r['ODIM code'].strip().lower()
        cc = code[:2].upper()
        h = num(r['Height of station (m)']) + num(r['Height Antenna (m)'])
        loc = r['Location'].strip().replace('|', '/') + ', ' + r['Country'].strip()
        out.append('%s|%s|OPERA|%.1f|%.4f|%.4f|opera|%s|%s'
                   % (code.upper(), loc, h, float(r['Latitude']),
                      float(r['Longitude']), code, cc))
    with open('src/core/opera_catalog.cpp', 'w', encoding='utf-8', newline='\n') as f:
        f.write(HEADER)
        f.write('\n'.join(out) + '\n')
        f.write(FOOTER)
    print('opera sites:', len(out))


main()
