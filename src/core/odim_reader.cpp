#include "core/odim_reader.hpp"
#include "core/hdf5_serial.hpp"

#ifdef CAPPEDWEATHER_HAS_HDF5
#include <hdf5.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

namespace cappedweather {

namespace {

constexpr std::size_t kMaxElevations = 40;
constexpr std::size_t kMaxRays = 1440;
constexpr std::size_t kMaxGates = 4096;
// NEXRAD-compatible 16-bit packing so nativeValue() decodes ODIM gates
// with zero special-casing downstream.
constexpr float kReflectivityScale = 2.0F;
constexpr float kReflectivityOffset = 66.0F;
constexpr float kVelocityScale = 2.0F;
constexpr float kVelocityOffset = 129.0F;
// Dual-pol packing. Each moment gets a 16-bit code span that covers the full
// published physical range, so a real gate is never clipped into a missing
// sentinel (packGate reserves 0/1):
//   ZDR   [-32, +32] dB     - DWD's own UZDR span, 1/1024 dB per step
//   RHOHV [0, 1.05]         - product descriptor maximum, 1.6e-5 per step
//   PHIDP [-180, 360] deg   - ODIM writers publish either range, 0.0082 deg
//                             per step
constexpr float kDifferentialReflectivityScale = 1023.9375F;
constexpr float kDifferentialReflectivityOffset = 32768.0F;
constexpr float kCorrelationCoefficientScale = 62411.43F;
constexpr float kCorrelationCoefficientOffset = 2.0F;
constexpr float kDifferentialPhaseScale = 121.35555F;
constexpr float kDifferentialPhaseOffset = 21846.0F;
constexpr std::uint16_t kEncodedMissing = 0;
constexpr std::uint16_t kEncodedBelowThreshold = 1;
constexpr std::uint16_t kEncodedFirstValid = 2;
constexpr std::uint16_t kEncodedMax = 65534;

// ODIM dual-pol moments and the product slot each one feeds. The U-prefixed
// identifiers are the unfiltered variants defined by ODIM_H5 v2.4 (DWD
// publishes them today); the standard identifier wins when a writer carries
// both, mirroring DBZH-over-TH. KDP/UKDP is deliberately absent: there is no
// ProductKind for specific differential phase yet, so a KDP-only dataset is
// counted as rejected instead of being folded into another product.
struct DualPolMomentSpec {
    ProductKind kind;
    const char* quantity;
    const char* unfilteredQuantity;
    float scale;
    float offset;
};
constexpr DualPolMomentSpec kDualPolMoments[] = {
    {ProductKind::DifferentialReflectivity, "ZDR", "UZDR", kDifferentialReflectivityScale,
     kDifferentialReflectivityOffset},
    {ProductKind::CorrelationCoefficient, "RHOHV", "URHOHV", kCorrelationCoefficientScale,
     kCorrelationCoefficientOffset},
    {ProductKind::DifferentialPhase, "PHIDP", "UPHIDP", kDifferentialPhaseScale,
     kDifferentialPhaseOffset},
};

bool hasHdf5Signature(const std::span<const std::byte> bytes) {
    static constexpr std::byte kSignature[] = {std::byte{0x89}, std::byte{'H'}, std::byte{'D'},
                                               std::byte{'F'},  std::byte{'\r'}, std::byte{'\n'},
                                               std::byte{0x1a}, std::byte{'\n'}};
    if (bytes.size() < sizeof(kSignature)) return false;
    return std::memcmp(bytes.data(), kSignature, sizeof(kSignature)) == 0;
}

#ifdef CAPPEDWEATHER_HAS_HDF5

// All HDF5 close functions share the herr_t(hid_t) shape.
struct H5Handle {
    hid_t id = H5I_INVALID_HID;
    herr_t (*close)(hid_t) = nullptr;
    ~H5Handle() {
        if (close != nullptr && id >= 0) close(id);
    }
    H5Handle() = default;
    H5Handle(const H5Handle&) = delete;
    H5Handle& operator=(const H5Handle&) = delete;
    H5Handle(H5Handle&& other) noexcept : id(other.id), close(other.close) {
        other.id = H5I_INVALID_HID;
        other.close = nullptr;
    }
    H5Handle& operator=(H5Handle&& other) noexcept {
        if (this != &other) {
            if (close != nullptr && id >= 0) close(id);
            id = other.id;
            close = other.close;
            other.id = H5I_INVALID_HID;
            other.close = nullptr;
        }
        return *this;
    }
    [[nodiscard]] bool valid() const { return close != nullptr && id >= 0; }
};

H5Handle openGroup(const hid_t parent, const char* name) {
    H5Handle handle;
    if (H5Lexists(parent, name, H5P_DEFAULT) <= 0) return handle;
    handle.id = H5Gopen2(parent, name, H5P_DEFAULT);
    handle.close = H5Gclose;
    return handle;
}

bool readAttrDouble(const hid_t object, const char* name, double* out) {
    if (H5Aexists(object, name) <= 0) return false;
    H5Handle attr;
    attr.id = H5Aopen(object, name, H5P_DEFAULT);
    attr.close = H5Aclose;
    if (!attr.valid()) return false;
    // Scalar only: array attributes (per-ray startazA/stopazA) must go
    // through readAttrDoubleArray. Reading an N-element attribute into a
    // single double overflows the stack buffer (0xC0000409).
    H5Handle attrSpace;
    attrSpace.id = H5Aget_space(attr.id);
    attrSpace.close = H5Sclose;
    if (!attrSpace.valid() || H5Sget_simple_extent_ndims(attrSpace.id) != 0) return false;
    const hid_t type = H5Aget_type(attr.id);
    H5Handle typeHandle;
    typeHandle.id = type;
    typeHandle.close = H5Tclose;
    if (type < 0) return false;
    // Accept ints, floats, and doubles; writers mix them freely.
    double value = 0.0;
    const hid_t cls = H5Tget_class(type);
    herr_t status = -1;
    if (cls == H5T_INTEGER) {
        long long raw = 0;
        status = H5Aread(attr.id, H5T_NATIVE_LLONG, &raw);
        value = static_cast<double>(raw);
    } else if (cls == H5T_FLOAT) {
        status = H5Aread(attr.id, H5T_NATIVE_DOUBLE, &value);
    } else {
        return false;
    }
    if (status < 0) return false;
    *out = value;
    return true;
}

bool readAttrString(const hid_t object, const char* name, std::string* out) {
    if (H5Aexists(object, name) <= 0) return false;
    H5Handle attr;
    attr.id = H5Aopen(object, name, H5P_DEFAULT);
    attr.close = H5Aclose;
    if (!attr.valid()) return false;
    const hid_t type = H5Aget_type(attr.id);
    H5Handle typeHandle;
    typeHandle.id = type;
    typeHandle.close = H5Tclose;
    if (type < 0 || H5Tget_class(type) != H5T_STRING) return false;
    if (H5Tis_variable_str(type) > 0) {
        char* raw = nullptr;
        if (H5Aread(attr.id, type, &raw) < 0 || raw == nullptr) return false;
        *out = raw;
        H5free_memory(raw);
        return true;
    }
    const std::size_t size = H5Tget_size(type);
    if (size == 0 || size > 256) return false;
    std::string buffer(size, '\0');
    if (H5Aread(attr.id, type, buffer.data()) < 0) return false;
    *out = std::string(buffer.c_str(), strnlen(buffer.c_str(), size));
    return true;
}

// One parsed moment sweep: raw floating-point gate values per ray plus the
// geometry and calibration needed to pack them into DecodedRays.
struct OdimSweep {
    double elevationDegrees = 0.0;
    double rangeStartKm = 0.0;
    double rangeScaleM = 0.0;
    std::size_t rayCount = 0;
    std::size_t gateCount = 0;
    // Ray-center azimuths in degrees [0, 360), one per ray, from the ODIM
    // how/startazA (+stopazA midpoint) arrays when present, else uniform
    // from north. a1gate is a RANGE index (first radiated gate) and must
    // never offset azimuths (it rotated whole sweeps by 100+ degrees).
    std::vector<double> rayAzimuthDegrees;
    // Per-ray gate values in physical units; NaN marks nodata (missing).
    // undetect (below threshold) is tracked separately per gate.
    std::vector<double> values;
    std::vector<char> belowThreshold;
    // Folding Nyquist (m/s) from the dataset how/NI attribute. 0 when the
    // writer omits it: rays then fall back to the panel Nyquist downstream.
    double nyquistMetersPerSecond = 0.0;
};

// Attribute array reader: ODIM writers store per-ray startazA/stopazA as
// fixed-size numeric attribute arrays. Accepts ints/floats/doubles and
// requires exactly expectedSize elements; anything else leaves out empty.
bool readAttrDoubleArray(const hid_t object, const char* name, const std::size_t expectedSize,
                         std::vector<double>* out) {
    if (H5Aexists(object, name) <= 0) return false;
    H5Handle attr;
    attr.id = H5Aopen(object, name, H5P_DEFAULT);
    attr.close = H5Aclose;
    if (!attr.valid()) return false;
    H5Handle space;
    space.id = H5Aget_space(attr.id);
    space.close = H5Sclose;
    if (!space.valid() || H5Sget_simple_extent_ndims(space.id) != 1) return false;
    hsize_t dims[1] = {0};
    if (H5Sget_simple_extent_dims(space.id, dims, nullptr) < 0 ||
        dims[0] != static_cast<hsize_t>(expectedSize)) {
        return false;
    }
    H5Handle type;
    type.id = H5Aget_type(attr.id);
    type.close = H5Tclose;
    if (!type.valid()) return false;
    const auto cls = H5Tget_class(type.id);
    std::vector<double> values(expectedSize, 0.0);
    herr_t status = -1;
    if (cls == H5T_INTEGER) {
        std::vector<long long> raw(expectedSize, 0);
        status = H5Aread(attr.id, H5T_NATIVE_LLONG, raw.data());
        for (std::size_t i = 0; i < expectedSize; ++i) values[i] = static_cast<double>(raw[i]);
    } else if (cls == H5T_FLOAT) {
        status = H5Aread(attr.id, H5T_NATIVE_DOUBLE, values.data());
    } else {
        return false;
    }
    if (status < 0) return false;
    *out = std::move(values);
    return true;
}

double wrapAzimuth(double degrees) {
    double wrapped = std::fmod(degrees, 360.0);
    if (wrapped < 0.0) wrapped += 360.0;
    return wrapped;
}

// Nearest source ray to an azimuth, by circular distance. Within one ODIM
// dataset every moment shares where/nrays and the how/startazA arrays, so
// the caller normally matches by index; this is the fallback for writers
// that put moments on different ray grids.
std::size_t nearestRayForAzimuth(const OdimSweep& sweep, const double azimuth) {
    const double resolution = 360.0 / static_cast<double>(sweep.rayCount);
    std::size_t best = 0;
    double bestDistance = 360.0;
    for (std::size_t candidate = 0; candidate < sweep.rayCount; ++candidate) {
        const double candidateAzimuth = sweep.rayAzimuthDegrees.size() == sweep.rayCount
                                           ? sweep.rayAzimuthDegrees[candidate]
                                           : wrapAzimuth(static_cast<double>(candidate) * resolution);
        double distance = std::fabs(azimuth - candidateAzimuth);
        if (distance > 180.0) distance = 360.0 - distance;
        if (distance < bestDistance) {
            bestDistance = distance;
            best = candidate;
        }
    }
    return best;
}

bool readMomentSweep(const hid_t dataset, const char* moment, OdimSweep* sweep) {
    double elevation = 0.0;
    double nbins = 0.0;
    double nrays = 0.0;
    double rstart = 0.0;
    double rscale = 0.0;
    {
        H5Handle where = openGroup(dataset, "where");
        if (!where.valid()) return false;
        if (!readAttrDouble(where.id, "elangle", &elevation)) return false;
        if (!readAttrDouble(where.id, "nbins", &nbins)) return false;
        if (!readAttrDouble(where.id, "nrays", &nrays)) return false;
        if (!readAttrDouble(where.id, "rstart", &rstart)) return false;
        if (!readAttrDouble(where.id, "rscale", &rscale)) return false;
    }
    if (!std::isfinite(elevation) || elevation < 0.0 || elevation > 90.0) return false;
    if (nbins < 1.0 || nbins > static_cast<double>(kMaxGates) || nrays < 1.0 ||
        nrays > static_cast<double>(kMaxRays)) {
        return false;
    }
    if (!std::isfinite(rscale) || rscale <= 0.0 || rscale > 5000.0) return false;
    const auto rays = static_cast<std::size_t>(nrays);
    const auto gates = static_cast<std::size_t>(nbins);
    // Find the dataN group carrying the requested quantity.
    // Note: data groups are not guaranteed contiguous (data1+data3 with no
    // data2 happens in the wild), so a missing group is skipped, not a stop.
    for (int dataIndex = 1; dataIndex <= 8; ++dataIndex) {
        char groupName[16];
        snprintf(groupName, sizeof(groupName), "data%d", dataIndex);
        H5Handle dataGroup = openGroup(dataset, groupName);
        if (!dataGroup.valid()) continue;
        H5Handle what = openGroup(dataGroup.id, "what");
        if (!what.valid()) continue;
        std::string quantity;
        if (!readAttrString(what.id, "quantity", &quantity) || quantity != moment) continue;
        double gain = 1.0;
        double offset = 0.0;
        double nodata = 255.0;
        double undetect = 0.0;
        readAttrDouble(what.id, "gain", &gain);
        readAttrDouble(what.id, "offset", &offset);
        readAttrDouble(what.id, "nodata", &nodata);
        readAttrDouble(what.id, "undetect", &undetect);
        if (!std::isfinite(gain) || gain == 0.0 || !std::isfinite(offset)) continue;
        if (H5Lexists(dataGroup.id, "data", H5P_DEFAULT) <= 0) continue;
        H5Handle data;
        data.id = H5Dopen2(dataGroup.id, "data", H5P_DEFAULT);
        data.close = H5Dclose;
        if (!data.valid()) continue;
        H5Handle space;
        space.id = H5Dget_space(data.id);
        space.close = H5Sclose;
        if (!space.valid() || H5Sget_simple_extent_ndims(space.id) != 2) continue;
        hsize_t dims[2] = {0, 0};
        if (H5Sget_simple_extent_dims(space.id, dims, nullptr) < 0) continue;
        if (dims[0] != rays || dims[1] != gates) continue;
        H5Handle fileType;
        fileType.id = H5Dget_type(data.id);
        fileType.close = H5Tclose;
        if (!fileType.valid() || H5Tget_class(fileType.id) != H5T_INTEGER) continue;
        const std::size_t count = rays * gates;
        // Read through the widest signed integer, then compare in doubles.
        std::vector<long long> raw(count, 0);
        if (H5Dread(data.id, H5T_NATIVE_LLONG, H5S_ALL, H5S_ALL, H5P_DEFAULT, raw.data()) < 0) {
            continue;
        }
        sweep->elevationDegrees = elevation;
        sweep->rangeStartKm = rstart;
        sweep->rangeScaleM = rscale;
        // Ray azimuths: per-ray startazA/stopazA arrays when the writer
        // provides them (DWD does), scalar startazA + uniform step, else
        // uniform from north (ODIM default for a missing how group).
        sweep->rayAzimuthDegrees.assign(rays, 0.0);
        const double uniformStep = 360.0 / static_cast<double>(rays);
        {
            H5Handle how = openGroup(dataset, "how");
            std::vector<double> startAz;
            std::vector<double> stopAz;
            double scalarStart = 0.0;
            bool haveScalarStart = false;
            if (how.valid()) {
                readAttrDoubleArray(how.id, "startazA", rays, &startAz);
                readAttrDoubleArray(how.id, "stopazA", rays, &stopAz);
                haveScalarStart = readAttrDouble(how.id, "startazA", &scalarStart);
                // Folding Nyquist for the velocity moment on this scan.
                // Single-PRF C-band scans run as low as ~6 m/s; unfolding
                // those at a 25 m/s panel default leaves fold boundaries in
                // the grid, and fold boundaries pass meso gates at close
                // range (HRGOL: NI 6.1, phantom MESO at 18 km).
                double ni = 0.0;
                if (readAttrDouble(how.id, "NI", &ni) && std::isfinite(ni) && ni > 0.0 &&
                    ni < 100.0) {
                    sweep->nyquistMetersPerSecond = ni;
                }
            }
            for (std::size_t ray = 0; ray < rays; ++ray) {
                double azimuth = 0.0;
                if (!startAz.empty()) {
                    azimuth = startAz[ray];
                    if (!stopAz.empty()) {
                        // Wrap-aware midpoint: a 359.5->0.5 ray centers on 0, not 180.
                        double half = stopAz[ray] - startAz[ray];
                        half = std::fmod(half + 540.0, 360.0) - 180.0;
                        azimuth = startAz[ray] + half * 0.5;
                    }
                } else if (haveScalarStart) {
                    azimuth = scalarStart + static_cast<double>(ray) * uniformStep;
                } else {
                    azimuth = static_cast<double>(ray) * uniformStep;
                }
                sweep->rayAzimuthDegrees[ray] = wrapAzimuth(azimuth);
            }
        }
        sweep->rayCount = rays;
        sweep->gateCount = gates;
        sweep->values.resize(count);
        sweep->belowThreshold.assign(count, 0);
        for (std::size_t i = 0; i < count; ++i) {
            const double rawValue = static_cast<double>(raw[i]);
            if (rawValue == nodata) {
                sweep->values[i] = std::numeric_limits<double>::quiet_NaN();
            } else if (rawValue == undetect) {
                sweep->values[i] = 0.0;
                sweep->belowThreshold[i] = 1;
            } else {
                sweep->values[i] = rawValue * gain + offset;
            }
        }
        return true;
    }
    return false;
}

std::uint16_t packGate(const double value, const bool belowThreshold, const float scale,
                       const float offset) {
    if (belowThreshold) return kEncodedBelowThreshold;
    if (!std::isfinite(value)) return kEncodedMissing;
    const double encoded = std::round(value * scale + offset);
    if (encoded < static_cast<double>(kEncodedFirstValid)) return kEncodedBelowThreshold;
    if (encoded > static_cast<double>(kEncodedMax)) return kEncodedMax;
    return static_cast<std::uint16_t>(encoded);
}

#endif  // CAPPEDWEATHER_HAS_HDF5

}  // namespace

bool odimReaderAvailable() {
#ifdef CAPPEDWEATHER_HAS_HDF5
    return true;
#else
    return false;
#endif
}

std::optional<DecodedChunk> readOdimPvol(const std::span<const std::byte> bytes, std::string radarId,
                                         std::string* error) {
#ifdef CAPPEDWEATHER_HAS_HDF5
    // Shared process-wide HDF5 lock (see core/hdf5_serial.hpp): a separate
    // ODIM-only mutex is not enough because GLM netCDF-4 reads enter the
    // same non-threadsafe hdf5.dll from another pool.
    const Hdf5SerialLock hdf5Lock;
    (void)hdf5Lock;
    DecodedChunk chunk;
    chunk.radarId = std::move(radarId);
    if (!hasHdf5Signature(bytes)) {
        if (error != nullptr) *error = "not an HDF5 file (bad signature)";
        return std::nullopt;
    }
    // Malformed inputs are routine (truncated downloads); keep HDF5's own
    // diagnostics off stderr and report through *error instead.
    H5Eset_auto(H5E_DEFAULT, nullptr, nullptr);
    H5Handle fapl;
    fapl.id = H5Pcreate(H5P_FILE_ACCESS);
    fapl.close = H5Pclose;
    if (!fapl.valid()) {
        if (error != nullptr) *error = "HDF5 property list allocation failed";
        return std::nullopt;
    }
    // Open the network buffer as an in-memory file: no temp files, no HL lib.
    if (H5Pset_fapl_core(fapl.id, 4096, 0) < 0 ||
        H5Pset_file_image(fapl.id, const_cast<std::byte*>(bytes.data()), bytes.size()) < 0) {
        if (error != nullptr) *error = "HDF5 in-memory open failed";
        return std::nullopt;
    }
    H5Handle file;
    file.id = H5Fopen("odim_mem.h5", H5F_ACC_RDONLY, fapl.id);
    file.close = H5Fclose;
    if (!file.valid()) {
        if (error != nullptr) *error = "HDF5 file open failed (not a readable container?)";
        return std::nullopt;
    }
    struct ElevationData {
        double elevation = 0.0;
        OdimSweep reflectivity;
        bool hasReflectivity = false;
        OdimSweep velocity;
        bool hasVelocity = false;
        // Dual-pol moments keyed by ProductKind index (ZDR/CC/PHI only), and
        // only populated when the dataset actually carried that moment.
        // Absence is preserved: no field is created for a missing moment.
        std::array<OdimSweep, kProductCount> dualPol;
        std::array<bool, kProductCount> hasDualPol{};
    };
    // Preferred geometry sweep for an elevation: a base moment first, then any
    // dual-pol moment (DWD publishes one moment per object).
    const auto geometrySweep = [](const ElevationData& elevation) -> const OdimSweep* {
        if (elevation.hasReflectivity) return &elevation.reflectivity;
        if (elevation.hasVelocity) return &elevation.velocity;
        for (const auto& moment : kDualPolMoments) {
            const auto index = static_cast<std::size_t>(moment.kind);
            if (elevation.hasDualPol[index]) return &elevation.dualPol[index];
        }
        return nullptr;
    };
    std::vector<ElevationData> elevations;
    // datasetN groups; bail out past the cap rather than looping forever.
    for (int datasetIndex = 1; datasetIndex <= static_cast<int>(kMaxElevations); ++datasetIndex) {
        char groupName[16];
        snprintf(groupName, sizeof(groupName), "dataset%d", datasetIndex);
        H5Handle dataset = openGroup(file.id, groupName);
        if (!dataset.valid()) break;
        ElevationData elevation;
        OdimSweep dbzh;
        OdimSweep th;
        OdimSweep vradh;
        const bool hasDbzh = readMomentSweep(dataset.id, "DBZH", &dbzh);
        const bool hasTh = !hasDbzh && readMomentSweep(dataset.id, "TH", &th);
        const bool hasVradh = readMomentSweep(dataset.id, "VRADH", &vradh);
        bool hasAnyMoment = hasDbzh || hasTh || hasVradh;
        for (const auto& moment : kDualPolMoments) {
            const auto index = static_cast<std::size_t>(moment.kind);
            // Prefer the corrected/filtered identifier over the U-prefixed
            // unfiltered one when a writer publishes both.
            const bool found =
                readMomentSweep(dataset.id, moment.quantity, &elevation.dualPol[index]) ||
                (moment.unfilteredQuantity != nullptr &&
                 readMomentSweep(dataset.id, moment.unfilteredQuantity,
                                 &elevation.dualPol[index]));
            if (!found) continue;
            elevation.hasDualPol[index] = true;
            hasAnyMoment = true;
        }
        if (!hasAnyMoment) {
            ++chunk.rejectedBlocks;
            continue;
        }
        if (hasDbzh || hasTh) {
            elevation.reflectivity = hasDbzh ? std::move(dbzh) : std::move(th);
            elevation.hasReflectivity = true;
        }
        if (hasVradh) {
            elevation.velocity = std::move(vradh);
            elevation.hasVelocity = true;
        }
        // A dual-pol-only sweep (DWD publishes one moment per object) is still
        // a real sweep and carries the same where/how grid.
        elevation.elevation = geometrySweep(elevation)->elevationDegrees;
        elevations.push_back(std::move(elevation));
    }
    if (elevations.empty()) {
        if (error != nullptr) {
            *error = "no readable ODIM elevations (need DBZH/TH/VRADH or ZDR/RHOHV/PHIDP "
                     "scans)";
        }
        return std::nullopt;
    }
    std::sort(elevations.begin(), elevations.end(), [](const ElevationData& a, const ElevationData& b) {
        return a.elevation < b.elevation;
    });
    const auto reflectivityIndex = static_cast<std::size_t>(ProductKind::Reflectivity);
    const auto velocityIndex = static_cast<std::size_t>(ProductKind::Velocity);
    std::uint8_t elevationNumber = 0;
    for (auto& elevation : elevations) {
        ++elevationNumber;
        const OdimSweep* geometry = geometrySweep(elevation);
        const double resolution = 360.0 / static_cast<double>(geometry->rayCount);
        const auto firstGateMeters =
            static_cast<std::int16_t>(std::clamp(std::llround(geometry->rangeStartKm * 1000.0),
                                                 static_cast<long long>(std::numeric_limits<std::int16_t>::min()),
                                                 static_cast<long long>(std::numeric_limits<std::int16_t>::max())));
        const auto spacingMeters = static_cast<std::uint16_t>(
            std::clamp(std::llround(geometry->rangeScaleM), 1LL, 5000LL));
        for (std::size_t ray = 0; ray < geometry->rayCount; ++ray) {
            DecodedRay decoded;
            const double azimuth = geometry->rayAzimuthDegrees.size() == geometry->rayCount
                                       ? geometry->rayAzimuthDegrees[ray]
                                       : wrapAzimuth(static_cast<double>(ray) * resolution);
            decoded.azimuthDegrees = static_cast<float>(azimuth);
            decoded.azimuthResolutionDegrees = static_cast<float>(resolution);
            decoded.elevationNumber = elevationNumber;
            decoded.elevationDegrees = static_cast<float>(elevation.elevation);
            decoded.gateCount = static_cast<std::uint16_t>(
                std::min<std::size_t>(geometry->gateCount, std::numeric_limits<std::uint16_t>::max()));
            decoded.firstGateMeters = firstGateMeters;
            decoded.gateSpacingMeters = spacingMeters;
            decoded.nyquistMetersPerSecond = static_cast<float>(geometry->nyquistMetersPerSecond);
            if (elevation.hasReflectivity) {
                const auto& sweep = elevation.reflectivity;
                const auto gates = std::min(sweep.gateCount, static_cast<std::size_t>(decoded.gateCount));
                auto& field = decoded.fields[reflectivityIndex];
                field.reserve(gates);
                for (std::size_t gate = 0; gate < gates; ++gate) {
                    const std::size_t flat = ray * sweep.gateCount + gate;
                    field.push_back(packGate(sweep.values[flat], sweep.belowThreshold[flat] != 0,
                                             kReflectivityScale, kReflectivityOffset));
                }
                decoded.firstGateByProduct[reflectivityIndex] =
                    static_cast<std::int16_t>(std::clamp(std::llround(sweep.rangeStartKm * 1000.0),
                                                         static_cast<long long>(std::numeric_limits<std::int16_t>::min()),
                                                         static_cast<long long>(std::numeric_limits<std::int16_t>::max())));
                decoded.gateSpacingByProduct[reflectivityIndex] = static_cast<std::uint16_t>(
                    std::clamp(std::llround(sweep.rangeScaleM), 1LL, 5000LL));
                decoded.scaleByProduct[reflectivityIndex] = kReflectivityScale;
                decoded.offsetByProduct[reflectivityIndex] = kReflectivityOffset;
            }
            if (elevation.hasVelocity) {
                const auto& sweep = elevation.velocity;
                // Velocity rides its own scan table; match each geometry ray
                // to the nearest velocity ray by circular distance on the
                // velocity sweep's own azimuths, then resample gates by
                // slant range.
                const auto velocityRay = nearestRayForAzimuth(sweep, azimuth);
                auto& field = decoded.fields[velocityIndex];
                field.reserve(decoded.gateCount);
                for (std::size_t gate = 0; gate < decoded.gateCount; ++gate) {
                    const double slant =
                        static_cast<double>(firstGateMeters) + static_cast<double>(gate * spacingMeters);
                    const double sweepFirst = sweep.rangeStartKm * 1000.0;
                    long long source = 0;
                    if (sweep.rangeScaleM > 0.0) {
                        source = std::llround((slant - sweepFirst) / sweep.rangeScaleM);
                    }
                    if (source < 0 || source >= static_cast<long long>(sweep.gateCount)) {
                        field.push_back(kEncodedMissing);
                        continue;
                    }
                    const std::size_t flat =
                        velocityRay * sweep.gateCount + static_cast<std::size_t>(source);
                    field.push_back(packGate(sweep.values[flat], sweep.belowThreshold[flat] != 0,
                                             kVelocityScale, kVelocityOffset));
                }
                decoded.firstGateByProduct[velocityIndex] = firstGateMeters;
                decoded.gateSpacingByProduct[velocityIndex] = spacingMeters;
                decoded.scaleByProduct[velocityIndex] = kVelocityScale;
                decoded.offsetByProduct[velocityIndex] = kVelocityOffset;
            }
            // Dual-pol moments carried by this dataset. ODIM_H5 2.4.2 puts
            // where/{nrays,nbins,rstart,rscale} and how/startazA|stopazA on
            // the dataset, with one azimuth entry per ray sized where/nrays,
            // so every moment in a dataset shares the geometry grid: pack by
            // index. The nearest-azimuth + slant-range path is the fallback
            // for a writer that breaks that contract.
            for (const auto& moment : kDualPolMoments) {
                const auto productIndex = static_cast<std::size_t>(moment.kind);
                if (!elevation.hasDualPol[productIndex]) continue;
                const auto& sweep = elevation.dualPol[productIndex];
                auto& field = decoded.fields[productIndex];
                field.reserve(decoded.gateCount);
                if (sweep.rayCount == geometry->rayCount) {
                    const auto gates =
                        std::min(sweep.gateCount, static_cast<std::size_t>(decoded.gateCount));
                    for (std::size_t gate = 0; gate < decoded.gateCount; ++gate) {
                        if (gate >= gates) {
                            field.push_back(kEncodedMissing);
                            continue;
                        }
                        const std::size_t flat = ray * sweep.gateCount + gate;
                        field.push_back(packGate(sweep.values[flat], sweep.belowThreshold[flat] != 0,
                                                 moment.scale, moment.offset));
                    }
                } else {
                    const auto sourceRay = nearestRayForAzimuth(sweep, azimuth);
                    for (std::size_t gate = 0; gate < decoded.gateCount; ++gate) {
                        const double slant = static_cast<double>(firstGateMeters) +
                                             static_cast<double>(gate * spacingMeters);
                        const double sweepFirst = sweep.rangeStartKm * 1000.0;
                        long long source = 0;
                        if (sweep.rangeScaleM > 0.0) {
                            source = std::llround((slant - sweepFirst) / sweep.rangeScaleM);
                        }
                        if (source < 0 || source >= static_cast<long long>(sweep.gateCount)) {
                            field.push_back(kEncodedMissing);
                            continue;
                        }
                        const std::size_t flat = sourceRay * sweep.gateCount +
                                                 static_cast<std::size_t>(source);
                        field.push_back(packGate(sweep.values[flat], sweep.belowThreshold[flat] != 0,
                                                 moment.scale, moment.offset));
                    }
                }
                decoded.firstGateByProduct[productIndex] = static_cast<std::int16_t>(
                    std::clamp(std::llround(sweep.rangeStartKm * 1000.0),
                               static_cast<long long>(std::numeric_limits<std::int16_t>::min()),
                               static_cast<long long>(std::numeric_limits<std::int16_t>::max())));
                decoded.gateSpacingByProduct[productIndex] = static_cast<std::uint16_t>(
                    std::clamp(std::llround(sweep.rangeScaleM), 1LL, 5000LL));
                decoded.scaleByProduct[productIndex] = moment.scale;
                decoded.offsetByProduct[productIndex] = moment.offset;
            }
            chunk.rays.push_back(std::move(decoded));
            ++chunk.messageCount;
        }
    }
    return chunk;
#else
    (void)bytes;
    (void)radarId;
    if (error != nullptr) *error = "ODIM reader unavailable: build without HDF5";
    return std::nullopt;
#endif
}

} // namespace cappedweather
