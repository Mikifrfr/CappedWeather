#include "core/hrrr_frame.hpp"
#include "core/hdf5_serial.hpp"

#ifdef CAPPEDWEATHER_HAS_NETCDF
#include <netcdf.h>
#endif

#include <cmath>
#include <cstring>
#include <limits>

namespace cappedweather {

bool hrrrReaderAvailable() {
#ifdef CAPPEDWEATHER_HAS_NETCDF
    return true;
#else
    return false;
#endif
}

std::optional<HrrrFrame> readHrrrRefc(const std::string& path) {
#ifdef CAPPEDWEATHER_HAS_NETCDF
    // Same lock as the HDF5-backed readers: netCDF-C shares process-global
    // state across formats, and a worker-pool ODIM/GLM decode may be inside
    // hdf5.dll while this runs (see hdf5_serial.hpp). TDS subsets arrive
    // NetCDF-3 classic, so plain open/close pairing on every exit path
    // (closeFail) still suffices for the handle itself.
    const Hdf5SerialLock hdf5Lock;
    int ncid = -1;
    if (nc_open(path.c_str(), NC_NOWRITE, &ncid) != NC_NOERR) return std::nullopt;
    const auto closeFail = [&] {
        nc_close(ncid);
        return std::optional<HrrrFrame>{};
    };

    int xDim = -1, yDim = -1, timeDim = -1;
    std::size_t nx = 0, ny = 0;
    if (nc_inq_dimid(ncid, "x", &xDim) != NC_NOERR ||
        nc_inq_dimid(ncid, "y", &yDim) != NC_NOERR ||
        nc_inq_dimid(ncid, "time", &timeDim) != NC_NOERR ||
        nc_inq_dimlen(ncid, xDim, &nx) != NC_NOERR ||
        nc_inq_dimlen(ncid, yDim, &ny) != NC_NOERR || nx == 0 || ny == 0 || nx > 4096 ||
        ny > 4096) {
        return closeFail();
    }

    int refcVar = -1, xVar = -1, yVar = -1, timeVar = -1;
    if (nc_inq_varid(ncid, "Composite_reflectivity_entire_atmosphere", &refcVar) != NC_NOERR ||
        nc_inq_varid(ncid, "x", &xVar) != NC_NOERR ||
        nc_inq_varid(ncid, "y", &yVar) != NC_NOERR ||
        nc_inq_varid(ncid, "time", &timeVar) != NC_NOERR) {
        return closeFail();
    }

    // Grid mapping record: CF Lambert Conformal Conic. Standard parallels
    // may arrive scalar or pair; tangent (equal) cones are the HRRR norm.
    char mappingName[NC_MAX_NAME + 1] = {};
    if (nc_get_att_text(ncid, refcVar, "grid_mapping", mappingName) != NC_NOERR)
        return closeFail();
    int mappingVar = -1;
    if (nc_inq_varid(ncid, mappingName, &mappingVar) != NC_NOERR) return closeFail();
    LambertConformal projection;
    double parallels[2] = {25.0, 25.0};
    std::size_t parallelLen = 0;
    if (nc_inq_attlen(ncid, mappingVar, "standard_parallel", &parallelLen) != NC_NOERR ||
        parallelLen == 0 || parallelLen > 2)
        return closeFail();
    if (nc_get_att_double(ncid, mappingVar, "standard_parallel", parallels) != NC_NOERR)
        return closeFail();
    if (nc_get_att_double(ncid, mappingVar, "latitude_of_projection_origin",
                          &projection.latitudeOriginDeg) != NC_NOERR ||
        nc_get_att_double(ncid, mappingVar, "longitude_of_central_meridian",
                          &projection.longitudeMeridianEastDeg) != NC_NOERR ||
        nc_get_att_double(ncid, mappingVar, "earth_radius",
                          &projection.earthRadiusMeters) != NC_NOERR)
        return closeFail();
    projection.firstParallelDeg = parallels[0];
    projection.secondParallelDeg = parallelLen > 1 ? parallels[1] : parallels[0];
    if (!std::isfinite(projection.latitudeOriginDeg) ||
        !std::isfinite(projection.longitudeMeridianEastDeg) ||
        !std::isfinite(projection.firstParallelDeg) ||
        !std::isfinite(projection.secondParallelDeg) ||
        !(projection.earthRadiusMeters > 0.0))
        return closeFail();

    // Axis spacing: uniform grids only (CDL subsets of Lambert grids are).
    std::vector<double> x(nx, 0.0), y(ny, 0.0);
    if (nc_get_var_double(ncid, xVar, x.data()) != NC_NOERR ||
        nc_get_var_double(ncid, yVar, y.data()) != NC_NOERR || nx < 2 || ny < 2)
        return closeFail();
    const double dx = x[1] - x[0];
    const double dy = y[1] - y[0];
    if (!std::isfinite(x[0]) || !std::isfinite(y[0]) || !std::isfinite(dx) ||
        !std::isfinite(dy) || dx == 0.0 || dy == 0.0)
        return closeFail();
    for (std::size_t i = 2; i < nx; ++i) {
        // 1 m absolute: subset axes arrive float32, so spacing carries
        // quantization far above any relative epsilon. Grids that are
        // genuinely non-uniform miss by kilometres, not millimetres.
        if (std::abs((x[i] - x[i - 1]) - dx) > 1e-3) return closeFail();
    }
    for (std::size_t i = 2; i < ny; ++i) {
        if (std::abs((y[i] - y[i - 1]) - dy) > 1e-3) return closeFail();
    }

    // Valid time: "<unit> since <ISO>" on the time variable, first step.
    char timeUnits[256] = {};
    if (nc_get_att_text(ncid, timeVar, "units", timeUnits) != NC_NOERR) return closeFail();
    double timeValue = 0.0;
    {
        std::size_t start = 0, count = 1;
        if (nc_get_vara_double(ncid, timeVar, &start, &count, &timeValue) != NC_NOERR)
            return closeFail();
    }
    // "Hour since 2026-10-07T00:00:00Z" (also Hours/minutes/seconds/days).
    double secondsPerUnit = 0.0;
    std::string baseIso;
    {
        const std::string units(timeUnits);
        const auto since = units.find(" since ");
        if (since == std::string::npos) return closeFail();
        const std::string unit = units.substr(0, since);
        if (unit == "seconds" || unit == "second" || unit == "sec" || unit == "secs" ||
            unit == "s")
            secondsPerUnit = 1.0;
        else if (unit == "minutes" || unit == "minute" || unit == "min")
            secondsPerUnit = 60.0;
        else if (unit == "hours" || unit == "hour" || unit == "Hour" || unit == "hr")
            secondsPerUnit = 3600.0;
        else if (unit == "days" || unit == "day")
            secondsPerUnit = 86400.0;
        else
            return closeFail();
        baseIso = units.substr(since + 7);
    }
    int baseYear = 0, baseMonth = 0, baseDay = 0, baseHour = 0, baseMinute = 0, baseSecond = 0;
    // sscanf-free ISO date parse (warn-clean on MSVC): strict digit runs,
    // fail closed on anything else.
    {
        const auto takeNumber = [&](std::size_t pos, std::size_t width, int& out) {
            if (pos + width > baseIso.size()) return false;
            int value = 0;
            for (std::size_t i = 0; i < width; ++i) {
                const char digit = baseIso[pos + i];
                if (digit < '0' || digit > '9') return false;
                value = value * 10 + (digit - '0');
            }
            out = value;
            return true;
        };
        // Minimum "YYYY-MM-DD"; time part optional (defaults midnight).
        if (!takeNumber(0, 4, baseYear) || baseIso.size() < 10 || baseIso[4] != '-' ||
            !takeNumber(5, 2, baseMonth) || baseIso[7] != '-' || !takeNumber(8, 2, baseDay))
            return closeFail();
        if (baseIso.size() > 10) {
            if (baseIso.size() < 19 || baseIso[10] != 'T' || !takeNumber(11, 2, baseHour) ||
                baseIso[13] != ':' || !takeNumber(14, 2, baseMinute) || baseIso[16] != ':' ||
                !takeNumber(17, 2, baseSecond))
                return closeFail();
        }
    }
    // Civil-to-epoch via days-from-civil (Howard Hinnant), UTC, no libc TZ.
    const auto daysFromCivil = [](int y, unsigned m, unsigned d) {
        y -= m <= 2 ? 1 : 0;
        const long long era = (y >= 0 ? y : y - 399) / 400;
        const unsigned yoe = static_cast<unsigned>(y - era * 400);
        const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + static_cast<long long>(doe) - 719468;
    };
    if (baseMonth < 1 || baseMonth > 12 || baseDay < 1 || baseDay > 31 || baseHour < 0 ||
        baseHour > 23 || baseMinute < 0 || baseMinute > 59 || baseSecond < 0 || baseSecond > 60)
        return closeFail();
    const long long validEpoch =
        daysFromCivil(baseYear, static_cast<unsigned>(baseMonth),
                      static_cast<unsigned>(baseDay)) *
            86400LL +
        static_cast<long long>(baseHour) * 3600LL + static_cast<long long>(baseMinute) * 60LL +
        static_cast<long long>(baseSecond) +
        static_cast<long long>(timeValue * secondsPerUnit);
    if (validEpoch <= 0 || !std::isfinite(timeValue)) return closeFail();

    // REFC field, first time step, float native.
    std::vector<float> values(nx * ny, 0.0F);
    {
        std::size_t start[3] = {0, 0, 0};
        std::size_t count[3] = {1, ny, nx};
        if (nc_get_vara_float(ncid, refcVar, start, count, values.data()) != NC_NOERR)
            return closeFail();
    }
    float fillValue = std::numeric_limits<float>::quiet_NaN();
    bool hasFill = false;
    float candidate = 0.0F;
    if (nc_get_att_float(ncid, refcVar, "_FillValue", &candidate) == NC_NOERR) {
        fillValue = candidate;
        hasFill = true;
    } else if (nc_get_att_float(ncid, refcVar, "missing_value", &candidate) == NC_NOERR) {
        fillValue = candidate;
        hasFill = true;
    }
    for (float& value : values) {
        if (!std::isfinite(value) || (hasFill && value == fillValue)) {
            value = std::numeric_limits<float>::quiet_NaN();
        }
    }
    nc_close(ncid);

    HrrrFrame frame;
    frame.validEpochSeconds = validEpoch;
    frame.nx = nx;
    frame.ny = ny;
    frame.x0Km = x[0];
    frame.y0Km = y[0];
    frame.dxKm = dx;
    frame.dyKm = dy;
    frame.projection = projection;
    frame.reflectivityDbz = std::move(values);
    return frame;
#else
    (void)path;
    return std::nullopt;
#endif
}

}  // namespace cappedweather
