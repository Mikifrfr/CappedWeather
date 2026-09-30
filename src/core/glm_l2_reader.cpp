#include "core/glm_l2_reader.hpp"

#include <cstdio>
#include <mutex>

#ifdef CAPPEDWEATHER_HAS_NETCDF
#include <netcdf.h>
#endif

namespace cappedweather {

namespace {

// days-from-civil (Howard Hinnant): days since 1970-01-01.
std::int64_t daysFromCivil(std::int64_t y, const unsigned m, const unsigned d) {
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

} // namespace

std::string glmLcfaPrefix(const int year, const int dayOfYear, const int hour) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "GLM-L2-LCFA/%04d/%03d/%02d/", year, dayOfYear, hour);
    return buffer;
}

std::optional<std::int64_t> parseGlmLcfaStartTime(const std::string& fileName) {
    if (fileName.find("GLM-L2-LCFA") == std::string::npos) {
        return std::nullopt;
    }
    // Scan tokens split by '_' for "s" + 14 digits: YYYY JJJ HH MM SS t(enths).
    std::size_t pos = 0;
    while (pos < fileName.size()) {
        const std::size_t next = fileName.find('_', pos);
        const std::string token = fileName.substr(pos, next == std::string::npos ? next : next - pos);
        if (token.size() == 15 && token[0] == 's' &&
            token.find_first_not_of("0123456789", 1) == std::string::npos) {
            // Digits-only checked above, so stoi cannot throw; guard anyway
            // against future format drift.
            try {
                const int year = std::stoi(token.substr(1, 4));
                const int doy = std::stoi(token.substr(5, 3));
                const int hour = std::stoi(token.substr(8, 2));
                const int minute = std::stoi(token.substr(10, 2));
                const int second = std::stoi(token.substr(12, 2));
                if (doy < 1 || doy > 366 || hour > 23 || minute > 59 || second > 60) {
                    return std::nullopt;
                }
                const std::int64_t days = daysFromCivil(year, 1, 1) + (doy - 1);
                return days * 86400 + hour * 3600 + minute * 60 + second;
            } catch (...) {
                return std::nullopt;
            }
        }
        if (next == std::string::npos) {
            break;
        }
        pos = next + 1;
    }
    return std::nullopt;
}

bool glmL2ReaderAvailable() {
#ifdef CAPPEDWEATHER_HAS_NETCDF
    return true;
#else
    return false;
#endif
}

#ifdef CAPPEDWEATHER_HAS_NETCDF
namespace {

// Reads a 1-D variable as double, applying scale_factor/add_offset attributes.
// Returns false when the variable is missing.
bool readVarScaled(const int ncid, const char* name, const std::size_t count,
                   std::vector<double>& out, double& fillValue) {
    int varid = -1;
    if (nc_inq_varid(ncid, name, &varid) != NC_NOERR) {
        return false;
    }
    out.assign(count, 0.0);
    if (nc_get_var_double(ncid, varid, out.data()) != NC_NOERR) {
        return false;
    }
    double scale = 1.0;
    double offset = 0.0;
    fillValue = 0.0;
    bool hasFill = false;
    if (nc_get_att_double(ncid, varid, "scale_factor", &scale) != NC_NOERR) {
        scale = 1.0;
    }
    if (nc_get_att_double(ncid, varid, "add_offset", &offset) != NC_NOERR) {
        offset = 0.0;
    }
    if (nc_get_att_double(ncid, varid, "_FillValue", &fillValue) == NC_NOERR) {
        hasFill = true;
    }
    const double rawFill = hasFill ? fillValue : 0.0;
    for (double& v : out) {
        // Exact compare is intentional: _FillValue sentinels are small integers
        // exactly representable as doubles, not computed floats.
        if (hasFill && v == rawFill) {
            v = fillValue; // keep raw sentinel for the caller to filter
            continue;
        }
        v = v * scale + offset;
    }
    return true;
}

} // namespace
#endif

std::optional<std::vector<LightningFlash>> readGlmL2Lcfa(const std::string& path) {
#ifdef CAPPEDWEATHER_HAS_NETCDF
    // netCDF builds vary in thread safety (same reason odim_reader serializes
    // HDF5). GLM currently decodes on a 1-thread pool, but guard the core
    // reader itself so a future pool widening cannot corrupt library state.
    static std::mutex glmNetcdfMutex;
    const std::lock_guard<std::mutex> lock(glmNetcdfMutex);
    int ncid = -1;
    if (nc_open(path.c_str(), NC_NOWRITE, &ncid) != NC_NOERR) {
        return std::nullopt;
    }

    std::optional<std::vector<LightningFlash>> result;
    std::size_t flashCount = 0;
    double productTime = 0.0;
    int productTimeId = -1;
    int flashDim = -1;
    int flashIdVar = -1;

    const bool dimsOk =
        nc_inq_dimid(ncid, "number_of_flashes", &flashDim) == NC_NOERR &&
        nc_inq_dimlen(ncid, flashDim, &flashCount) == NC_NOERR &&
        nc_inq_varid(ncid, "product_time", &productTimeId) == NC_NOERR &&
        nc_get_var_double(ncid, productTimeId, &productTime) == NC_NOERR &&
        nc_inq_varid(ncid, "flash_id", &flashIdVar) == NC_NOERR;

    if (dimsOk) {
        std::vector<double> timeOffset, lat, lon, energy, area;
        double fillT = 0.0, fillLat = 0.0, fillLon = 0.0, fillE = 0.0, fillA = 0.0;
        const bool varsOk =
            readVarScaled(ncid, "flash_time_offset_of_first_event", flashCount, timeOffset, fillT) &&
            readVarScaled(ncid, "flash_lat", flashCount, lat, fillLat) &&
            readVarScaled(ncid, "flash_lon", flashCount, lon, fillLon) &&
            readVarScaled(ncid, "flash_energy", flashCount, energy, fillE) &&
            readVarScaled(ncid, "flash_area", flashCount, area, fillA);
        if (varsOk) {
            std::vector<LightningFlash> flashes;
            flashes.reserve(flashCount);
            std::vector<int> ids(flashCount, 0);
            if (nc_get_var_int(ncid, flashIdVar, ids.data()) != NC_NOERR) {
                nc_close(ncid);
                return std::nullopt;
            }
            for (std::size_t i = 0; i < flashCount; ++i) {
                if (lat[i] == fillLat || lon[i] == fillLon || timeOffset[i] == fillT) {
                    continue; // sentinel / missing geometry
                }
                LightningFlash flash;
                flash.id = ids[i];
                flash.timeEpochSec = static_cast<double>(kGoesEpochUnixSec) + productTime + timeOffset[i];
                flash.latitudeDeg = lat[i];
                flash.longitudeDeg = lon[i];
                flash.energyJ = (energy[i] == fillE) ? 0.0 : energy[i];
                flash.areaKm2 = (area[i] == fillA) ? 0.0 : area[i];
                flashes.push_back(flash);
            }
            result = std::move(flashes);
        }
    }

    nc_close(ncid);
    return result;
#else
    (void)path;
    return std::nullopt;
#endif
}

} // namespace cappedweather
