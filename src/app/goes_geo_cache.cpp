#include "app/goes_geo_cache.hpp"

#include <QElapsedTimer>
#include <QtGlobal>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>

namespace cappedweather {
namespace {

// Exact bitwise identity: same file (or same grid encoding) shares,
// anything else rebuilds. Doubles compare exactly because copies carry
// identical bits; a re-encoded grid that differs by one ulp rebuilds,
// which is safe, never wrong.
bool bytesEqual(double a, double b) {
    return a == b;
}

std::size_t hashCombine(std::size_t seed, std::size_t value) {
    return seed ^ (value + 0x9e3779b9 + (seed << 6) + (seed >> 2));
}

std::atomic<int>& buildCounter() {
    static std::atomic<int> counter{0};
    return counter;
}

}  // namespace

GoesGeoIdentity GoesGeoIdentity::from(const GoesFrame& frame) {
    // Canonicalize signed zeros: -0.0 == 0.0 must also hash equal, or the
    // map misses identical grids. NaNs stay bitwise (never equal anyway).
    const auto canon = [](double value) { return value + 0.0; };
    GoesGeoIdentity identity;
    identity.subpointLonDeg = canon(frame.grid.subpointLongitudeDeg);
    identity.perspectiveHeightMeters = canon(frame.grid.perspectiveHeightMeters);
    identity.semiMajorMeters = canon(frame.grid.semiMajorMeters);
    identity.semiMinorMeters = canon(frame.grid.semiMinorMeters);
    identity.nx = frame.nx;
    identity.ny = frame.ny;
    identity.x0Radians = canon(frame.x0Radians);
    identity.y0Radians = canon(frame.y0Radians);
    identity.dxRadians = canon(frame.dxRadians);
    identity.dyRadians = canon(frame.dyRadians);
    return identity;
}

bool GoesGeoIdentity::operator==(const GoesGeoIdentity& other) const {
    return bytesEqual(subpointLonDeg, other.subpointLonDeg) &&
           bytesEqual(perspectiveHeightMeters, other.perspectiveHeightMeters) &&
           bytesEqual(semiMajorMeters, other.semiMajorMeters) &&
           bytesEqual(semiMinorMeters, other.semiMinorMeters) && nx == other.nx && ny == other.ny &&
           bytesEqual(x0Radians, other.x0Radians) && bytesEqual(y0Radians, other.y0Radians) &&
           bytesEqual(dxRadians, other.dxRadians) && bytesEqual(dyRadians, other.dyRadians);
}

std::size_t qHash(const GoesGeoIdentity& key, const std::size_t seed) {
    std::size_t hash = seed;
    hash = hashCombine(hash, std::hash<double>{}(key.subpointLonDeg));
    hash = hashCombine(hash, std::hash<double>{}(key.perspectiveHeightMeters));
    hash = hashCombine(hash, std::hash<double>{}(key.semiMajorMeters));
    hash = hashCombine(hash, std::hash<double>{}(key.semiMinorMeters));
    hash = hashCombine(hash, key.nx);
    hash = hashCombine(hash, key.ny);
    hash = hashCombine(hash, std::hash<double>{}(key.x0Radians));
    hash = hashCombine(hash, std::hash<double>{}(key.y0Radians));
    hash = hashCombine(hash, std::hash<double>{}(key.dxRadians));
    hash = hashCombine(hash, std::hash<double>{}(key.dyRadians));
    return hash;
}

std::shared_ptr<const GoesGeoData> GoesGeoCache::find(const GoesGeoIdentity& identity) {
    QMutexLocker lock(&mutex());
    const auto it = store().constFind(identity);
    if (it == store().cend()) return nullptr;
    return it.value().lock();
}

std::shared_ptr<const GoesGeoData> GoesGeoCache::build(const GoesGeoIdentity& identity) {
    ++buildCounter();
    const bool profiling = qEnvironmentVariableIsSet("CAPPEDWEATHER_PAINT_PROFILE");
    QElapsedTimer clock;
    if (profiling) clock.start();
    GoesFixedGrid grid;
    grid.subpointLongitudeDeg = identity.subpointLonDeg;
    grid.perspectiveHeightMeters = identity.perspectiveHeightMeters;
    grid.semiMajorMeters = identity.semiMajorMeters;
    grid.semiMinorMeters = identity.semiMinorMeters;
    auto data = std::make_shared<GoesGeoData>();
    const std::size_t count = identity.nx * identity.ny;
    data->lonDeg.assign(count, std::numeric_limits<float>::quiet_NaN());
    data->latDeg.assign(count, std::numeric_limits<float>::quiet_NaN());
    for (std::size_t iy = 0; iy < identity.ny; ++iy) {
        for (std::size_t ix = 0; ix < identity.nx; ++ix) {
            const double xRad = identity.x0Radians + ix * identity.dxRadians;
            const double yRad = identity.y0Radians + iy * identity.dyRadians;
            const auto lonLat = goesFixedInverse(grid, xRad, yRad);
            if (!std::isfinite(lonLat.longitudeDeg) || !std::isfinite(lonLat.latitudeDeg))
                continue;
            const std::size_t i = iy * identity.nx + ix;
            data->lonDeg[i] = static_cast<float>(lonLat.longitudeDeg);
            data->latDeg[i] = static_cast<float>(lonLat.latitudeDeg);
        }
    }
    if (profiling) {
        std::fprintf(stderr, "paintGoesGeo: %lld ms, %zu cells\n",
                     static_cast<long long>(clock.elapsed()), static_cast<std::size_t>(count));
    }
    return data;
}

void GoesGeoCache::insert(const GoesGeoIdentity& identity,
                          std::shared_ptr<const GoesGeoData> data) {
    QMutexLocker lock(&mutex());
    store().insert(identity, std::move(data));
}

void GoesGeoCache::clearForTest() {
    QMutexLocker lock(&mutex());
    store().clear();
}

void GoesGeoCache::resetBuildsForTest() {
    buildCounter().store(0);
}

int GoesGeoCache::buildsForTest() {
    return buildCounter().load();
}

QMutex& GoesGeoCache::mutex() {
    static QMutex mutex;
    return mutex;
}

QHash<GoesGeoIdentity, std::weak_ptr<const GoesGeoData>>& GoesGeoCache::store() {
    static QHash<GoesGeoIdentity, std::weak_ptr<const GoesGeoData>> store;
    return store;
}

}  // namespace cappedweather
