#include "core/radar_catalog.hpp"

#include <sstream>

namespace cappedweather {

// EUMETNET OPERA single-site radar catalog (CC BY 4.0 EUMETNET).
// Source: openradardata-validator stations/OPERA_RADARS.csv, operational
// sites with coordinates only. Regenerate with tools/generate_opera_sites.py.
// Columns: ID|Location, Country|OPERA|antennaM|lat|lon|opera|odim|CC
std::vector<RadarSite> RadarCatalog::operaSites() {
    static constexpr char kOperaCatalog[] = R"CATALOG(
BEWID|Wideumont, Belgium|OPERA|585.0|49.9135|5.5044|opera|bewid|BE
BEJAB|Jabbeke, Belgium|OPERA|54.0|51.1919|3.0641|opera|bejab|BE
BEHEL|Helchteren, Belgium|OPERA|144.0|51.0702|5.4054|opera|behel|BE
HRBIL|Bilogora, Croatia|OPERA|542.0|45.8835|17.2005|opera|hrbil|HR
HRZAG|Puntijarka, Croatia|OPERA|2025.0|45.9078|15.9683|opera|hrzag|HR
HRGRA|Gradište, Croatia|OPERA|202.0|45.1592|18.7033|opera|hrgra|HR
HRGOL|Goli, Croatia|OPERA|1091.0|45.0205|14.1223|opera|hrgol|HR
HRDEB|Debeljak, Croatia|OPERA|401.0|44.0455|15.3765|opera|hrdeb|HR
HRULJ|Uljenje, Croatia|OPERA|908.0|42.8944|17.4783|opera|hrulj|HR
CZSKA|Skalky, Czechia|OPERA|1497.0|49.5011|16.7885|opera|czska|CZ
CZBRD|Brdy-Praha, Czechia|OPERA|1776.0|49.6583|13.8178|opera|czbrd|CZ
DKSTE|Stevns, Denmark|OPERA|95.0|55.3262|12.4493|opera|dkste|DK
DKROM|Römö, Denmark|OPERA|20.0|55.1731|8.5520|opera|dkrom|DK
DKSIN|Sindal, Denmark|OPERA|213.0|57.4893|10.1365|opera|dksin|DK
DKBOR|Bornholm, Denmark|OPERA|333.0|55.1127|14.8875|opera|dkbor|DK
EEHAR|Harku, Estonia|OPERA|102.7|59.3971|24.6021|opera|eehar|EE
EESUR|Sürgavere, Estonia|OPERA|287.0|58.4823|25.5187|opera|eesur|EE
FIANJ|Anjalankoski, Finland|OPERA|240.0|60.9039|27.1081|opera|fianj|FI
FIKUO|Kuopio, Finland|OPERA|507.0|62.8626|27.3815|opera|fikuo|FI
FIKOR|Korpo, Finland|OPERA|96.0|60.1285|21.6434|opera|fikor|FI
FIUTA|Utajärvi, Finland|OPERA|203.0|64.7749|26.3189|opera|fiuta|FI
FILUO|Luosto, Finland|OPERA|1047.0|67.1391|26.8969|opera|filuo|FI
FIVIM|Vimpeli, Finland|OPERA|372.0|63.1048|23.8209|opera|fivim|FI
FIKES|Kesälahti, Finland|OPERA|315.0|61.9069|29.7977|opera|fikes|FI
FIPET|Petäjävesi, Finland|OPERA|512.0|62.3045|25.4401|opera|fipet|FI
FINUR|Nurmes, Finland|OPERA|617.0|63.8378|29.4489|opera|finur|FI
FIVIH|Vihti, Finland|OPERA|329.0|60.5562|24.4956|opera|fivih|FI
FIKAN|Kankaanpää, Finland|OPERA|317.0|61.8108|22.5020|opera|fikan|FI
FRTRA|Trappes, France|OPERA|356.7|48.7746|2.0083|opera|frtra|FR
FRABB|Abbeville, France|OPERA|154.7|50.1360|1.8347|opera|frabb|FR
FRBOR|Bordeaux, France|OPERA|116.0|44.8315|-0.6919|opera|frbor|FR
FRBOU|Bourges, France|OPERA|335.7|47.0586|2.3595|opera|frbou|FR
FRGRE|Grèzes, France|OPERA|710.9|45.1044|1.3697|opera|frgre|FR
FRCAE|Falaise, France|OPERA|322.8|48.9272|-0.1496|opera|frcae|FR
FRNAN|Nancy, France|OPERA|582.1|48.7158|6.5816|opera|frnan|FR
FRNIM|Nîmes, France|OPERA|149.2|43.8061|4.5027|opera|frnim|FR
FRTOU|Toulouse, France|OPERA|344.9|43.5743|1.3763|opera|frtou|FR
FRTRO|Arcis-sur-Aube, France|OPERA|320.4|48.4621|4.3093|opera|frtro|FR
FRLEP|Sembadel, France|OPERA|2258.6|45.2892|3.7095|opera|frlep|FR
FRTRE|Treillères, France|OPERA|149.7|47.3374|-1.6563|opera|frtre|FR
FRBOL|Bollène, France|OPERA|634.3|44.3230|4.7621|opera|frbol|FR
FROPO|Opoul, France|OPERA|1418.9|42.9184|2.8650|opera|fropo|FR
FRNIZ|Saint Nizier, France|OPERA|1827.6|46.0678|4.4453|opera|frniz|FR
FRCOL|Collobrières, France|OPERA|1294.2|43.2166|6.3729|opera|frcol|FR
FRPLA|Plabennec, France|OPERA|207.3|48.4609|-4.4298|opera|frpla|FR
FRALE|Aléria, France|OPERA|113.5|42.1298|9.4964|opera|frale|FR
FRMCL|Montclar, France|OPERA|1346.2|43.9905|2.6096|opera|frmcl|FR
FRAVE|Avesnes, France|OPERA|400.4|50.1283|3.8118|opera|frave|FR
FRCHE|Cherves, France|OPERA|333.8|46.6986|0.0656|opera|frche|FR
FRBLA|Blaisy-Haut, France|OPERA|1199.3|47.3552|4.7759|opera|frbla|FR
FRMOM|Momuy, France|OPERA|272.2|43.6245|-0.6094|opera|frmom|FR
FRMTC|Montancy, France|OPERA|1838.6|47.3686|7.0190|opera|frmtc|FR
FRMAU|Mont Maurel, France|OPERA|3545.0|44.0128|6.5292|opera|frmau|FR
FRAJA|Ajaccio, France|OPERA|1562.0|41.9531|8.7005|opera|fraja|FR
DEMEM|Memmingen, Germany|OPERA|1391.4|48.0421|10.2192|opera|demem|DE
DEOFT|Offenthal, Germany|OPERA|443.8|49.9847|8.7129|opera|deoft|DE
DEASB|Isle of Borkum, Germany|OPERA|40.2|53.5640|6.7482|opera|deasb|DE
DEBOO|Boostedt, Germany|OPERA|216.6|54.0043|10.0468|opera|deboo|DE
DEROS|Rostock, Germany|OPERA|38.1|54.1757|12.0580|opera|deros|DE
DEHNR|Hannover, Germany|OPERA|148.8|52.4600|9.6945|opera|dehnr|DE
DEPRO|Protzel/Berlin, Germany|OPERA|329.9|52.6486|13.8580|opera|depro|DE
DEESS|Essen, Germany|OPERA|337.1|51.4055|6.9669|opera|deess|DE
DEFLD|Flechtdorf, Germany|OPERA|1157.9|51.3112|8.8020|opera|defld|DE
DEUMD|Ummendorf, Germany|OPERA|344.2|52.1601|11.1761|opera|deumd|DE
DENEU|Neuhaus, Germany|OPERA|1725.6|50.5001|11.1351|opera|deneu|DE
DEDRS|Dresden, Germany|OPERA|488.4|51.1246|13.7686|opera|dedrs|DE
DENHB|Neuheilenbach, Germany|OPERA|1137.8|50.1097|6.5483|opera|denhb|DE
DETUR|Tuerkheim, Germany|OPERA|1500.6|48.5853|9.7828|opera|detur|DE
DEEIS|Eisberg, Germany|OPERA|1570.1|49.5407|12.4028|opera|deeis|DE
DEFBG|Feldberg, Germany|OPERA|3011.1|47.8736|8.0039|opera|defbg|DE
DEISN|Isen/München, Germany|OPERA|1307.8|48.1747|12.1017|opera|deisn|DE
GRYMI|Ymittos, Greece|OPERA|2056.0|37.9461|23.8138|opera|grymi|GR
GRLAR|Larisa, Greece|OPERA|185.0|39.6446|22.4603|opera|grlar|GR
GRTHE|Thessaloniki, Greece|OPERA|35.0|40.5282|22.9757|opera|grthe|GR
GRAND|Andravida, Greece|OPERA|51.0|37.9261|21.2894|opera|grand|GR
GRAKT|Aktio, Greece|OPERA|39.0|38.9166|20.7528|opera|grakt|GR
HUNAP|Napkor, Hungary|OPERA|153.0|47.9622|21.8867|opera|hunap|HU
HUPOG|Poganyvar, Hungary|OPERA|311.0|46.6604|17.0624|opera|hupog|HU
HUBUD|Budapest, Hungary|OPERA|161.0|47.4294|19.1817|opera|hubud|HU
HUSZE|Szentes, Hungary|OPERA|124.0|46.6397|20.4325|opera|husze|HU
HUHAR|Harmashegy, Hungary|OPERA|627.0|46.1775|18.3372|opera|huhar|HU
ISKEF|Keflavik, Iceland|OPERA|82.0|64.0257|-22.6353|opera|iskef|IS
ISEGS|Fljotsdalsheidi, Iceland|OPERA|1391.0|65.0279|-15.0382|opera|isegs|IS
IESHA|Shannon, Ireland|OPERA|50.0|52.6928|-8.9201|opera|iesha|IE
IEDUB|Dublin, Ireland|OPERA|184.0|53.4299|-6.2443|opera|iedub|IE
ILTLV|Bet Dagan, Israel|OPERA|77.0|32.0070|34.8146|opera|iltlv|IL
LVRIX|Riga, Latvia|OPERA|48.5|56.9143|23.9897|opera|lvrix|LV
MTGUD|Gudja, Malta|OPERA|196.0|35.8529|14.4748|opera|mtgud|MT
MDCHS|Chisinau, Republic of Moldova|OPERA|31.0|46.9557|28.5703|opera|mdchs|MD
NLDHL|Den Helder, Netherlands|OPERA|107.0|52.9528|4.7906|opera|nldhl|NL
NLHRW|Herwijnen, Netherlands|OPERA|26.0|51.8371|5.1380|opera|nlhrw|NL
NOBML|Boemlo, Norway|OPERA|197.0|59.8540|5.0900|opera|nobml|NO
NORST|Rost, Norway|OPERA|19.5|67.5307|12.0986|opera|norst|NO
NOAND|Andoya, Norway|OPERA|874.0|69.2414|16.0028|opera|noand|NO
NOHAS|Hasvik, Norway|OPERA|881.0|70.6052|22.4430|opera|nohas|NO
NOSTA|Stad, Norway|OPERA|1002.0|62.1871|5.1275|opera|nosta|NO
NOHUR|Hurum, Norway|OPERA|709.4|59.6272|10.5645|opera|nohur|NO
NOHGB|Haegebostad, Norway|OPERA|1252.5|58.3601|7.1648|opera|nohgb|NO
NORSA|Rissa, Norway|OPERA|1220.5|63.6905|10.2037|opera|norsa|NO
NOBER|Berlevaag, Norway|OPERA|928.7|70.5107|29.0184|opera|nober|NO
NOSMN|Soemna, Norway|OPERA|617.0|65.2201|11.9926|opera|nosmn|NO
NOHFJ|Hafjell, Norway|OPERA|2144.1|61.2318|10.5273|opera|nohfj|NO
NORSG|Rassegalvarri, Norway|OPERA|604.1|69.2186|23.4397|opera|norsg|NO
PLLEG|Legionowo, Poland|OPERA|215.6|52.4053|20.9611|opera|plleg|PL
PLRAM|Ramża, Poland|OPERA|678.1|50.1513|18.7251|opera|plram|PL
PLPAS|Pastewnik, Poland|OPERA|1360.8|50.8925|16.0395|opera|plpas|PL
PLRZE|Rzeszów, Poland|OPERA|452.2|50.1141|22.0370|opera|plrze|PL
PLPOZ|Poznań, Poland|OPERA|211.6|52.4133|16.7970|opera|plpoz|PL
PLSWI|Świdwin, Poland|OPERA|263.2|53.7958|15.8368|opera|plswi|PL
PLBRZ|Brzuchania, Poland|OPERA|817.8|50.3942|20.0832|opera|plbrz|PL
PLUZR|Uzranki, Poland|OPERA|439.0|53.8565|21.4121|opera|pluzr|PL
PTLIS|Coruche/Cruz do Leão, Portugal|OPERA|366.0|39.0714|-8.4001|opera|ptlis|PT
PTFAR|Loule/Cavalos do Caldeirão, Portugal|OPERA|1203.0|37.3041|-7.9530|opera|ptfar|PT
PTPRT|Arouca/Pico do Gralheiro, Portugal|OPERA|2144.0|40.8450|-8.2797|opera|ptprt|PT
PTTRC|Terceira/ Santa Barbara, Portugal|OPERA|2055.0|38.7302|-27.3208|opera|pttrc|PT
ROBUC|Bucuresti, Romania|OPERA|228.0|44.5127|26.0773|opera|robuc|RO
ROORA|Oradea, Romania|OPERA|456.0|47.0922|21.9429|opera|roora|RO
ROCRA|Craiova, Romania|OPERA|414.5|44.3103|23.8674|opera|rocra|RO
ROTIM|Timisoara, Romania|OPERA|248.0|45.7717|21.2577|opera|rotim|RO
ROBOB|Bobohalma, Romania|OPERA|1098.0|46.3602|24.2252|opera|robob|RO
ROBAR|Barnova, Romania|OPERA|862.0|47.0118|27.5826|opera|robar|RO
ROMED|Medgidia, Romania|OPERA|180.0|44.2434|28.2506|opera|romed|RO
RSFRG|Fruska gora, Serbia|OPERA|1026.0|45.1573|19.8109|opera|rsfrg|RS
RSSAM|Samos, Serbia|OPERA|245.1|45.1876|20.7707|opera|rssam|RS
RSJAS|Jastrebac, Serbia|OPERA|3005.4|43.3913|21.4436|opera|rsjas|RS
SKJAV|Maly Javornik, Slovakia|OPERA|1185.0|48.2556|17.1524|opera|skjav|SK
SKKOJ|Kojsovska hola, Slovakia|OPERA|2496.0|48.7827|20.9873|opera|skkoj|SK
SKKUB|Kubinska hola, Slovakia|OPERA|2830.0|49.2717|19.2494|opera|skkub|SK
SKLAZ|Spani laz, Slovakia|OPERA|1280.0|48.2404|19.2573|opera|sklaz|SK
SILIS|Lisca, Slovenia|OPERA|1894.0|46.0680|15.2850|opera|silis|SI
SIPAS|Pasja ravan, Slovenia|OPERA|2063.0|46.0980|14.2283|opera|sipas|SI
ESLID|Autilla Pino (Palencia), Spain|OPERA|1761.0|41.9956|-4.6028|opera|eslid|ES
ESMAD|Torrejon de Velasco (Madrid), Spain|OPERA|1421.0|40.1758|-3.7136|opera|esmad|ES
ESVAL|Cullera (Valencia), Spain|OPERA|481.0|39.1761|-0.2521|opera|esval|ES
ESMUR|Fortuna (Murcia), Spain|OPERA|2535.0|38.2644|-1.1897|opera|esmur|ES
ESBAR|Corbera (Barcelona), Spain|OPERA|1315.0|41.4081|1.8847|opera|esbar|ES
ESCOR|Cerceda ( La Coruna), Spain|OPERA|1203.0|43.1689|-8.5269|opera|escor|ES
ESZAR|Perdiguera (Zaragoza), Spain|OPERA|1653.0|41.7339|-0.5458|opera|eszar|ES
ESSAN|Aguión (Asturias), Spain|OPERA|1859.0|43.4625|-6.3019|opera|essan|ES
ESSSE|Baquio (Vizcaya), Spain|OPERA|1201.0|43.4033|-2.8419|opera|essse|ES
ESBAD|Sierra de Fuentes (Caceres), Spain|OPERA|1317.0|39.4289|-6.2853|opera|esbad|ES
ESMAL|Alhaurin el Grande (Malaga), Spain|OPERA|2307.0|36.6133|-4.6592|opera|esmal|ES
ESSEV|Castillo las Guardas (Sevilla), Spain|OPERA|1047.0|37.6875|-6.3344|opera|essev|ES
ESALM|Nijar (Almeria), Spain|OPERA|985.0|36.8325|-2.0822|opera|esalm|ES
ESPMA|Llucmajor (Baleares), Spain|OPERA|247.0|39.3797|2.7850|opera|espma|ES
ESLPA|Artenara (Gran Canaria), Spain|OPERA|3543.0|28.0186|-15.6144|opera|eslpa|ES
SEKRN|Kiruna, Sweden|OPERA|1287.0|67.7088|20.6178|opera|sekrn|SE
SELLA|Luleå (Rosvik), Sweden|OPERA|115.0|65.4309|21.8650|opera|sella|SE
SEOSD|Östersund, Sweden|OPERA|917.0|63.2951|14.7591|opera|seosd|SE
SEOER|Örnsköldsvik, Sweden|OPERA|1030.0|63.6395|18.4019|opera|seoer|SE
SEHUV|Hudiksvall, Sweden|OPERA|764.0|61.5771|16.7144|opera|sehuv|SE
SELEK|Leksand, Sweden|OPERA|896.0|60.7230|14.8776|opera|selek|SE
SEBAA|Bålsta, Sweden|OPERA|142.0|59.6110|17.5833|opera|sebaa|SE
SEHEM|Hemse (Ase), Sweden|OPERA|144.0|57.3035|18.4001|opera|sehem|SE
SEATV|Åtvidaberg (Vilebo), Sweden|OPERA|423.0|58.1059|15.9363|opera|seatv|SE
SEVAX|Vara, Sweden|OPERA|292.0|58.2556|12.8260|opera|sevax|SE
SEANG|Ängelholm, Sweden|OPERA|396.0|56.3675|12.8517|opera|seang|SE
SEKAA|Karlskrona, Sweden|OPERA|238.0|56.2955|15.6102|opera|sekaa|SE
CHALB|Albis, Switzerland|OPERA|1876.0|47.2843|8.5120|opera|chalb|CH
CHDOL|La Dole, Switzerland|OPERA|3364.0|46.4251|6.0994|opera|chdol|CH
CHLEM|Monte Lema, Switzerland|OPERA|3252.0|46.0408|8.8332|opera|chlem|CH
CHPPM|Plaine Monte, Switzerland|OPERA|5874.0|46.3706|7.4866|opera|chppm|CH
CHWEI|Weissfluhgiptel, Switzerland|OPERA|5700.0|46.8350|9.7945|opera|chwei|CH
UKCLE|Clee Hill, United Kingdom|OPERA|1034.0|52.3986|-2.5954|opera|ukcle|UK
UKHAM|Hameldon Hill, United Kingdom|OPERA|798.0|53.7548|-2.2892|opera|ukham|UK
UKCHE|Chenies, United Kingdom|OPERA|291.0|51.6895|-0.5302|opera|ukche|UK
UKCAS|Castor Bay, United Kingdom|OPERA|64.0|54.5020|-6.3429|opera|ukcas|UK
UKPRE|Predannack, United Kingdom|OPERA|191.0|50.0034|-5.2230|opera|ukpre|UK
UKING|Ingham, United Kingdom|OPERA|152.0|53.3347|-0.5594|opera|uking|UK
UKCYG|Crug y Gorllwyn, United Kingdom|OPERA|660.0|51.9798|-4.4446|opera|ukcyg|UK
UKJER|Jersey, United Kingdom|OPERA|152.0|49.1772|-2.2239|opera|ukjer|UK
UKDUD|Hill Of Dudwick, United Kingdom|OPERA|359.0|57.4304|-2.0367|opera|ukdud|UK
UKLEW|Druim A'Starraig, United Kingdom|OPERA|182.0|58.2117|-6.1821|opera|uklew|UK
UKCOB|Cobbacombe Cross, United Kingdom|OPERA|584.0|50.9639|-3.4521|opera|ukcob|UK
UKHHD|Holehead, United Kingdom|OPERA|1108.0|56.0180|-4.2171|opera|ukhhd|UK
UKMUN|Munduff Hill, United Kingdom|OPERA|907.0|56.2148|-3.3118|opera|ukmun|UK
UKTHU|Thurnham, United Kingdom|OPERA|419.0|51.2948|0.6043|opera|ukthu|UK
UKDEA|Dean Hill, United Kingdom|OPERA|335.0|51.0307|-1.6534|opera|ukdea|UK
UKHMY|High Moorsley, United Kingdom|OPERA|335.0|54.8034|-1.4746|opera|ukhmy|UK
)CATALOG";

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
