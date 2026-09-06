"""Regenerate src/core/opera_catalog.cpp from the OPERA station CSV.

Usage:
    python tools/generate_opera_sites.py <OPERA_RADARS.csv>

The CSV is EUMETNET's openradardata-validator station list
(CC BY 4.0, src/openradardata_validator/stations/OPERA_RADARS.csv).
The output file is committed; rerun when OPERA publishes updates.

Bucket reality beats CSV status: the upstream "Opera Status" flag does not
track who actually shares single-site volumes on the anonymous CloudFerro
bucket, so this script layers live-bucket findings over the CSV:

- EXCLUDED_COUNTRY_CODES: national networks whose bucket prefixes are
  completely empty (verified 2026-09-06, today + yesterday). Dead picker
  entries that can never load are worse than no entries; re-add the country
  here when it resumes sharing.
- RENAMED_SITES: bucket-only ODIM codes that replaced a CSV code at the
  same site (old code silent, new code active). Coordinates/names come from
  the old row.
- EXTRA_ACTIVE_SITES: CSV rows flagged non-operational (or unflagged) that
  are verifiably sharing volumes right now. Included when geolocated.

Recheck with tools/check_opera_availability.py before regenerating.
"""
import csv
import sys

# National networks with zero objects under YYYY/MM/DD/CC/ on 2026-09-06
# (today + yesterday, PVOL and SCAN). GR/ES/PT/UK/RS/HU/SK/IL/LV/MD.
EXCLUDED_COUNTRY_CODES = frozenset({
    'GR', 'HU', 'IL', 'LV', 'MD', 'PT', 'RS', 'SK', 'ES', 'UK',
})

# Old ODIM code -> bucket-active replacement code (same site).
# hrzag (Puntijarka, Croatia) went silent as hrpun appeared (6/6 HR slots).
RENAMED_SITES = {
    'hrzag': 'hrpun',
}

# Geolocated CSV rows that share volumes despite a non-'1' Opera Status
# (verified live 2026-09-06). Mobile/coordless codes (e.g. isx1/isx2) stay out.
EXTRA_ACTIVE_SITES = frozenset({
    'dksam',  # Samso, Denmark
    'fikau',  # Kaunispää, Finland
    'isbjo',  # Bjolfur, Iceland
    'isska',  # Skagi, Iceland
    'plgdy',  # Gdynia, Poland
    'plgsa',  # Gora Sw. Anny, Poland
    'ltlau',  # Laukuva, Lithuania (new OPERA country for the app)
    'ltvil',  # Vilnius, Lithuania
})

HEADER = '''#include "core/radar_catalog.hpp"

#include <sstream>

namespace cappedweather {

// EUMETNET OPERA single-site radar catalog (CC BY 4.0 EUMETNET).
// Source: openradardata-validator stations/OPERA_RADARS.csv, bucket-active
// sites with coordinates only (see tools/generate_opera_sites.py: dead
// national networks excluded, renames/additions from live-bucket checks).
// Regenerate with tools/generate_opera_sites.py.
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
    seen = set()
    for r in rows:
        raw_code = r['ODIM code'].strip().lower()
        if not raw_code:
            continue
        code = RENAMED_SITES.get(raw_code, raw_code)
        if code in seen:
            continue
        cc = code[:2].upper()
        if cc in EXCLUDED_COUNTRY_CODES:
            continue
        operational = r['Opera Status'] == '1'
        if not operational and raw_code not in EXTRA_ACTIVE_SITES:
            continue
        if (not r['Latitude'] or not r['Longitude'] or
                r['Latitude'].strip().upper() == 'MOBILE'):
            continue
        h = num(r['Height of station (m)']) + num(r['Height Antenna (m)'])
        loc = r['Location'].strip().replace('|', '/') + ', ' + r['Country'].strip()
        out.append('%s|%s|OPERA|%.1f|%.4f|%.4f|opera|%s|%s'
                   % (code.upper(), loc, h, float(r['Latitude']),
                      float(r['Longitude']), code, cc))
        seen.add(code)
    with open('src/core/opera_catalog.cpp', 'w', encoding='utf-8', newline='\n') as f:
        f.write(HEADER)
        f.write('\n'.join(out) + '\n')
        f.write(FOOTER)
    print('opera sites:', len(out))


main()
