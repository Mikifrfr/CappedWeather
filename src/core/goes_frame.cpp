#include "core/goes_frame.hpp"
#include "core/hdf5_serial.hpp"

#ifdef CAPPEDWEATHER_HAS_NETCDF
#include <netcdf.h>
#endif

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace cappedweather {

bool goesReaderAvailable() {
#ifdef CAPPEDWEATHER_HAS_NETCDF
    return true;
#else
    return false;
#endif
}

std::optional<GoesFrame> readGoesRad(const std::string& path) {
#ifdef CAPPEDWEATHER_HAS_NETCDF
    // netCDF-4 rides on HDF5 global state (see hdf5_serial.hpp): serialize
    // against worker-pool ODIM/GLM decodes. The vcpkg HDF5 has no threadsafety.
    const Hdf5SerialLock hdf5Lock;
    int ncid = -1;
    if (nc_open(path.c_str(), NC_NOWRITE, &ncid) != NC_NOERR) return std::nullopt;
    const auto closeFail = [&] {
        nc_close(ncid);
        return std::optional<GoesFrame>{};
    };

    int xDim = -1, yDim = -1;
    std::size_t nx = 0, ny = 0;
    if (nc_inq_dimid(ncid, "x", &xDim) != NC_NOERR ||
        nc_inq_dimid(ncid, "y", &yDim) != NC_NOERR ||
        nc_inq_dimlen(ncid, xDim, &nx) != NC_NOERR ||
        nc_inq_dimlen(ncid, yDim, &ny) != NC_NOERR || nx == 0 || ny == 0 || nx > 4096 ||
        ny > 4096) {
        return closeFail();
    }
    int radVar = -1, xVar = -1, yVar = -1, tVar = -1, projVar = -1, bandVar = -1;
    if (nc_inq_varid(ncid, "Rad", &radVar) != NC_NOERR ||
        nc_inq_varid(ncid, "x", &xVar) != NC_NOERR ||
        nc_inq_varid(ncid, "y", &yVar) != NC_NOERR ||
        nc_inq_varid(ncid, "t", &tVar) != NC_NOERR ||
        nc_inq_varid(ncid, "goes_imager_projection", &projVar) != NC_NOERR ||
        nc_inq_varid(ncid, "band_id", &bandVar) != NC_NOERR) {
        return closeFail();
    }

    GoesFixedGrid grid;
    double lonOrigin = 0.0;
    if (nc_get_att_double(ncid, projVar, "perspective_point_height",
                          &grid.perspectiveHeightMeters) != NC_NOERR ||
        nc_get_att_double(ncid, projVar, "semi_major_axis", &grid.semiMajorMeters) != NC_NOERR ||
        nc_get_att_double(ncid, projVar, "semi_minor_axis", &grid.semiMinorMeters) != NC_NOERR ||
        nc_get_att_double(ncid, projVar, "longitude_of_projection_origin", &lonOrigin) !=
            NC_NOERR) {
        return closeFail();
    }
    grid.subpointLongitudeDeg = lonOrigin;
    if (!(grid.perspectiveHeightMeters > 0.0) || !(grid.semiMajorMeters > 0.0) ||
        !(grid.semiMinorMeters > 0.0) || !std::isfinite(lonOrigin))
        return closeFail();

    // Fixed-grid axes arrive scaled integers: apply scale/offset (default 1/0).
    double xScale = 1.0, xOffset = 0.0, yScale = 1.0, yOffset = 0.0;
    nc_get_att_double(ncid, xVar, "scale_factor", &xScale);
    nc_get_att_double(ncid, xVar, "add_offset", &xOffset);
    nc_get_att_double(ncid, yVar, "scale_factor", &yScale);
    nc_get_att_double(ncid, yVar, "add_offset", &yOffset);
    std::vector<double> xRaw(nx, 0.0), yRaw(ny, 0.0);
    if (nc_get_var_double(ncid, xVar, xRaw.data()) != NC_NOERR ||
        nc_get_var_double(ncid, yVar, yRaw.data()) != NC_NOERR || nx < 2 || ny < 2)
        return closeFail();
    std::vector<double> x(nx, 0.0), y(ny, 0.0);
    for (std::size_t i = 0; i < nx; ++i) x[i] = xRaw[i] * xScale + xOffset;
    for (std::size_t i = 0; i < ny; ++i) y[i] = yRaw[i] * yScale + yOffset;
    const double dx = x[1] - x[0];
    const double dy = y[1] - y[0];
    if (!std::isfinite(x[0]) || !std::isfinite(y[0]) || dx == 0.0 || dy == 0.0)
        return closeFail();
    for (std::size_t i = 2; i < nx; ++i) {
        if (std::abs((x[i] - x[i - 1]) - dx) > 1e-12) return closeFail();
    }
    for (std::size_t i = 2; i < ny; ++i) {
        if (std::abs((y[i] - y[i - 1]) - dy) > 1e-12) return closeFail();
    }

    // Radiance scale + Planck coefficients for the BT conversion. PUG L1b
    // files carry Planck terms as scalar variables (not attributes).
    double radScale = 1.0, radOffset = 0.0;
    nc_get_att_double(ncid, radVar, "scale_factor", &radScale);
    nc_get_att_double(ncid, radVar, "add_offset", &radOffset);
    const auto readScalarVar = [&](const char* name, double& out) {
        int varid = -1;
        if (nc_inq_varid(ncid, name, &varid) != NC_NOERR) return false;
        return nc_get_var_double(ncid, varid, &out) == NC_NOERR;
    };
    double fk1 = 0.0, fk2 = 0.0, bc1 = 0.0, bc2 = 0.0;
    if (!readScalarVar("planck_fk1", fk1) || !readScalarVar("planck_fk2", fk2) ||
        !readScalarVar("planck_bc1", bc1) || !readScalarVar("planck_bc2", bc2))
        return closeFail();
    if (!(fk1 > 0.0) || !(fk2 > 0.0)) return closeFail();

    // Scan-start time: t in seconds since 2000-01-01 12:00:00Z (J2000).
    // t is scalar; units carry the epoch.
    char timeUnits[256] = {};
    double tValue = 0.0;
    if (nc_get_att_text(ncid, tVar, "units", timeUnits) != NC_NOERR ||
        nc_get_var_double(ncid, tVar, &tValue) != NC_NOERR || !std::isfinite(tValue) ||
        tValue < 0.0)
        return closeFail();
    long long validEpoch = 0;
    {
        // J2000 epoch = 946728000 UTC.
        static constexpr long long kJ2000Epoch = 946728000LL;
        const std::string units(timeUnits);
        if (units.find("2000-01-01") == std::string::npos) return closeFail();
        validEpoch = kJ2000Epoch + static_cast<long long>(tValue);
        if (validEpoch <= kJ2000Epoch) return closeFail();
    }

    int channel = 0;
    {
        // band_id is a length-1 integer array; nc_get_var_int converts
        // byte/short/int forms alike.
        int bandInt = 0;
        if (nc_get_var_int(ncid, bandVar, &bandInt) != NC_NOERR) return closeFail();
        channel = bandInt;
    }

    // Rad payload: integer counts scale to radiance, float payloads already
    // are radiance (trimmed fixtures keep native types either way).
    nc_type radType = NC_NAT;
    if (nc_inq_vartype(ncid, radVar, &radType) != NC_NOERR) return closeFail();
    const bool radIsFloat = (radType == NC_FLOAT || radType == NC_DOUBLE);
    std::vector<double> radiance(nx * ny, 0.0);
    if (radIsFloat) {
        std::vector<float> rawFloat(nx * ny, 0.0F);
        std::size_t start[2] = {0, 0};
        std::size_t count[2] = {ny, nx};
        if (nc_get_vara_float(ncid, radVar, start, count, rawFloat.data()) != NC_NOERR)
            return closeFail();
        for (std::size_t i = 0; i < nx * ny; ++i)
            radiance[i] = static_cast<double>(rawFloat[i]);
    } else {
        std::vector<int> raw(nx * ny, 0);
        std::size_t start[2] = {0, 0};
        std::size_t count[2] = {ny, nx};
        if (nc_get_vara_int(ncid, radVar, start, count, raw.data()) != NC_NOERR)
            return closeFail();
        double fillValue = 0.0;
        const bool hasFill = nc_get_att_double(ncid, radVar, "_FillValue", &fillValue) == NC_NOERR;
        for (std::size_t i = 0; i < nx * ny; ++i) {
            if (hasFill && static_cast<double>(raw[i]) == fillValue) {
                radiance[i] = std::numeric_limits<double>::quiet_NaN();
                continue;
            }
            radiance[i] = static_cast<double>(raw[i]) * radScale + radOffset;
        }
    }
    // DQF mask when present: 0 good, 1 conditionally usable, >=2 unusable.
    {
        int dqfVar = -1;
        if (nc_inq_varid(ncid, "DQF", &dqfVar) == NC_NOERR) {
            std::vector<int> dqf(nx * ny, 0);
            std::size_t start[2] = {0, 0};
            std::size_t count[2] = {ny, nx};
            if (nc_get_vara_int(ncid, dqfVar, start, count, dqf.data()) == NC_NOERR) {
                for (std::size_t i = 0; i < nx * ny; ++i) {
                    if (dqf[i] > 1)
                        radiance[i] = std::numeric_limits<double>::quiet_NaN();
                }
            }
        }
    }
    nc_close(ncid);
    std::vector<float> bt(nx * ny, 0.0F);
    for (std::size_t i = 0; i < nx * ny; ++i) {
        const double rad = radiance[i];
        if (!std::isfinite(rad) || rad <= 0.0) {
            bt[i] = std::numeric_limits<float>::quiet_NaN();
            continue;
        }
        const double kelvin = (fk2 / std::log(fk1 / rad + 1.0) - bc1) / bc2;
        if (!std::isfinite(kelvin)) {
            bt[i] = std::numeric_limits<float>::quiet_NaN();
            continue;
        }
        bt[i] = static_cast<float>(kelvin - 273.15);
    }

    GoesFrame frame;
    frame.channel = channel;
    frame.validEpochSeconds = validEpoch;
    frame.nx = nx;
    frame.ny = ny;
    frame.x0Radians = x[0];
    frame.y0Radians = y[0];
    frame.dxRadians = dx;
    frame.dyRadians = dy;
    frame.grid = grid;
    frame.brightnessCelsius = std::move(bt);
    return frame;
#else
    (void)path;
    return std::nullopt;
#endif
}

}  // namespace cappedweather
