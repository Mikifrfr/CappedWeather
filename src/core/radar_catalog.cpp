#include "core/radar_catalog.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

#include <string_view>
#include <unordered_map>
#include <utility>

namespace cappedweather {

std::vector<RadarSite> RadarCatalog::defaultSites() {
    // Official NWS WSR-88D radar list. Coordinates are refreshed from the
    // NWS radar-stations GeoJSON at application startup; this real catalog is
    // the offline fallback and intentionally contains no generated sites.
    static constexpr std::string_view kFallbackCatalog = R"CATALOG(
KABR|Aberdeen, SD|NWS|1383
KENX|Albany, NY|NWS|1934
KABX|Albuquerque, NM|NWS|5950
KAMA|Amarillo, TX|NWS|3702
PAHG|Anchorage/Kenai, AK|FAA|356
PGUA|Andersen AFB, Guam|DOD|386
KFFC|Atlanta, GA|NWS|972
KBBX|Beale AFB, CA|DOD|221
PABC|Bethel, AK|FAA|193
KBLX|Billings, MT|NWS|3702
KBGM|Binghamton, NY|NWS|1703
PACG|Biorka Island/Sitka, AK|FAA|272
KBMX|Birmingham, AL|NWS|759
KBIS|Bismarck, ND|NWS|1755
KFCX|Blacksburg, VA|NWS|2965
KCBX|Boise, ID|NWS|3171
KBOX|Boston, MA|NWS|232
KBRO|Brownsville, TX|NWS|88
KBUF|Buffalo, NY|NWS|790
KCXX|Burlington, VT|NWS|431
RKSG|Camp Humphreys, KO|DOD|1521
KFDX|Cannon AFB, NM|DOD|4697
KCBW|Caribou, ME|NWS|860
KICX|Cedar City, UT|NWS|10754
KGRK|Central Texas/Ft Hood, TX|DOD|603
KCLX|Charleston, SC|NWS|229
KRLX|Charleston, WV|NWS|1213
KCYS|Cheyenne, WY|NWS|6191
KLOT|Chicago, IL|NWS|760
KILN|Cincinnati/Wilmington, OH|NWS|1170
KCLE|Cleveland, OH|NWS|860
KCAE|Columbia, SC|NWS|345
KGWX|Columbus AFB, MS|DOD|589
KCRP|Corpus Christi, TX|NWS|142
KFTG|Denver, CO|NWS|5609
KDMX|Des Moines, IA|NWS|1095
KDTX|Detroit, MI|NWS|1216
KDDC|Dodge City, KS|NWS|2670
KDOX|Dover AFB, DE|DOD|164
KDLH|Duluth, MN|NWS|1541
KDYX|Dyess AFB, TX|DOD|1581
KEYX|Edwards AFB, CA|DOD|2872
KEPZ|El Paso, TX|NWS|4217
KLRX|Elko, NV|NWS|6894
KBHX|Eureka, CA|NWS|2515
KVWX|Evansville, IN|NWS|625
PAPD|Fairbanks/Pedro Dome, AK|FAA|2706
KFSX|Flagstaff, AZ|NWS|7513
KSRX|Fort Smith, AR|NWS|737
KFDR|Frederick/Altus AFB, OK|DOD|1315
KHPX|Ft Campbell, KY|DOD|624
KPOE|Ft Polk, LA|DOD|473
KEOX|Ft Rucker, AL|DOD|537
KFWS|Ft Worth, TX|NWS|776
KAPX|Gaylord, MI|NWS|1561
KGGW|Glasgow, MT|NWS|2384
KGLD|Goodland, KS|NWS|3715
KMVX|Grand Forks, ND|NWS|1083
KGJX|Grand Junction, CO|NWS|10098
KGRR|Grand Rapids, MI|NWS|875
KTFX|Great Falls, MT|NWS|3804
KGRB|Green Bay, WI|NWS|806
KGSP|Greer, SC|NWS|1068
KUEX|Hastings, NE|NWS|2057
KHDX|Holloman AFB, NM|DOD|4269
KHGX|Houston, TX|NWS|115
KHTX|Huntsville/Hytop, AL|NWS|1859
KIND|Indianapolis, IN|NWS|887
KJKL|Jackson, KY|NWS|1461
KDGX|Jackson, MS|NWS|609
KJAX|Jacksonville, FL|NWS|160
RODN|Kadena, JP|DOD|332
PHKM|Kamuela/Kohala, HI|FAA|3965
KEAX|Kansas City, MO|NWS|1092
KBYX|Key West, FL|NWS|89
PAKC|King Salmon, AK|FAA|144
KMRX|Knoxville/Morristown, TN|NWS|1434
RKJK|Kunsan, KO|DOD|192
KARX|La Crosse, WI|NWS|1357
KLCH|Lake Charles, LA|NWS|137
KLGX|Langley Hill, WA|NWS|354
KESX|Las Vegas, NV|NWS|4947
KDFX|Laughlin AFB, TX|DOD|1195
KILX|Lincoln, IL|NWS|731
KLZK|Little Rock, AR|NWS|649
KVTX|Los Angeles, CA|NWS|2806
KLVX|Louisville, KY|NWS|833
KLBB|Lubbock, TX|NWS|3377
KMQT|Marquette, MI|NWS|1524
KMXX|Maxwell AFB, AL|DOD|560
KMAX|Medford, OR|NWS|7559
KMLB|Melbourne, FL|NWS|116
KNQA|Memphis, TN|NWS|435
KAMX|Miami, FL|NWS|111
PAIH|Middleton Island, AK|FAA|132
KMAF|Midland/Odessa, TX|NWS|2961
KMKX|Milwaukee, WI|NWS|1022
KMPX|Minneapolis, MN|NWS|1101
KMBX|Minot AFB, ND|DOD|1590
KMSX|Missoula, MT|NWS|7976
KMOB|Mobile, AL|NWS|289
PHMO|Molokai, HI|FAA|1444
KTYX|Montague/Ft Drum, NY|DOD|1959
KVAX|Moody AFB, GA|DOD|330
KMHX|Morehead City, NC|NWS|145
KOHX|Nashville, TN|NWS|676
KLIX|New Orleans, LA|NWS|179
KOKX|New York City/Upton, NY|NWS|199
PAEC|Nome, AK|FAA|90
KLNX|North Platte, NE|NWS|3112
KIWX|Northern Indiana/North Webster, IN|NWS|1056
KEVX|Northwest Florida/Eglin AFB, FL|DOD|221
KTLX|Oklahoma City, OK|NWS|1277
KOAX|Omaha, NE|NWS|1262
KPAH|Paducah, KY|NWS|506
KPDT|Pendleton, OR|NWS|1579
KDIX|Philadelphia, PA|NWS|230
KIWA|Phoenix, AZ|NWS|1426
KPBZ|Pittsburgh, PA|NWS|1266
KSFX|Pocatello, ID|NWS|4538
KGYX|Portland, ME|NWS|474
KRTX|Portland, OR|NWS|1728
KPUX|Pueblo, CO|NWS|5362
KDVN|Quad Cities/Davenport, IA|NWS|851
KRAX|Raleigh/Durham, NC|NWS|462
KUDX|Rapid City, SD|NWS|3194
KRGX|Reno, NV|NWS|8394
KRIW|Riverton, WY|NWS|5631
KJGX|Robins AFB, GA|DOD|618
KDAX|Sacramento, CA|NWS|144
KMTX|Salt Lake City, UT|NWS|6592
KSJT|San Angelo, TX|NWS|2003
KEWX|San Antonio, TX|NWS|767
KNKX|San Diego, CA|NWS|1052
KMUX|San Francisco, CA|NWS|3549
KHNX|San Joaquin Valley, CA|NWS|340
TJUA|San Juan, PR|FAA|2958
KSOX|Santa Ana Mountains, CA|NWS|3105
KATX|Seattle, WA|NWS|642
KSHV|Shreveport, LA|NWS|387
KFSD|Sioux Falls, SD|NWS|1494
PHKI|South Kauai, HI|FAA|340
PHWA|South Shore, HI|FAA|1461
KOTX|Spokane, WA|NWS|2448
KSGF|Springfield, MO|NWS|1375
KLSX|St Louis, MO|NWS|722
KCCX|State College, PA|NWS|2485
KLWX|Sterling, VA|NWS|404
KTLH|Tallahassee, FL|NWS|177
KTBW|Tampa Bay, FL|NWS|122
KTWX|Topeka, KS|NWS|1415
KEMX|Tucson, AZ|NWS|5318
KINX|Tulsa, OK|NWS|749
KVNX|Vance AFB, OK|DOD|1258
KVBX|Vandenberg AFB, CA|DOD|1354
KAKQ|Wakefield, VA|NWS|255
KICT|Wichita, KS|NWS|1399
KLTX|Wilmington, NC|NWS|145
KYUX|Yuma, AZ|NWS|239
)CATALOG";

    std::vector<RadarSite> sites;
    std::istringstream input{std::string(kFallbackCatalog)};
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        std::istringstream fields(line);
        std::string id;
        std::string name;
        std::string owner;
        std::string elevationFeet;
        if (!std::getline(fields, id, '|') || !std::getline(fields, name, '|') ||
            !std::getline(fields, owner, '|') || !std::getline(fields, elevationFeet)) {
            continue;
        }
        sites.push_back({std::move(id), std::move(name), std::move(owner), std::nullopt, std::nullopt,
                         std::stod(elevationFeet) * 0.3048});
    }

    // Real WSR-88D coordinates retained for a useful offline map while the
    // authoritative NWS station catalog is being refreshed.
    static const std::unordered_map<std::string, std::pair<double, double>> kKnownCoordinates{
        {"KABR", {45.4558, -98.4131}}, {"KAMA", {35.2334, -101.7092}},
        {"KATX", {48.1946, -122.4957}}, {"KBUF", {42.9488, -78.7368}},
        {"KDMX", {41.7312, -93.7229}}, {"KDVN", {41.6115, -90.5809}},
        {"KILX", {40.1505, -89.3368}}, {"KINX", {36.1751, -95.5645}},
        {"KLSX", {38.6987, -90.6829}}, {"KMPX", {44.8488, -93.5655}},
        {"KMRX", {36.1686, -83.4018}}, {"KTLX", {35.3331, -97.2778}},
    };
    for (auto& site : sites) {
        if (const auto it = kKnownCoordinates.find(site.id); it != kKnownCoordinates.end()) {
            site.latitude = it->second.first;
            site.longitude = it->second.second;
        }
    }
    return sites;
}

bool RadarCatalog::isValidRadarId(const std::string& id) {
    if (id.size() != 4) {
        return false;
    }
    return std::all_of(id.begin(), id.end(), [](const unsigned char ch) {
        return std::isupper(ch) != 0;
    });
}

} // namespace cappedweather
