#pragma once

// Shared GOES fixed-grid georeference: one immutable lon/lat grid per exact
// grid identity, built once off the GUI thread and reused by every canvas.
// Identity covers every projection input (grid record, nx/ny, x0/y0/dx/dy);
// time and band never enter it, so frames share geometry exactly when valid
// and never otherwise. Pure computation plus a mutex-guarded weak map: no
// threads are spawned here, delivery coalescing lives in RadarMapCanvas.

#include "core/goes_frame.hpp"

#include <QHash>
#include <QMutex>

#include <cstddef>
#include <memory>
#include <vector>

namespace cappedweather {

struct GoesGeoIdentity {
    double subpointLonDeg = 0.0;
    double perspectiveHeightMeters = 0.0;
    double semiMajorMeters = 0.0;
    double semiMinorMeters = 0.0;
    std::size_t nx = 0;
    std::size_t ny = 0;
    double x0Radians = 0.0;
    double y0Radians = 0.0;
    double dxRadians = 0.0;
    double dyRadians = 0.0;

    static GoesGeoIdentity from(const GoesFrame& frame);
    bool operator==(const GoesGeoIdentity& other) const;
};

std::size_t qHash(const GoesGeoIdentity& key, std::size_t seed);

struct GoesGeoData {
    // Row-major degrees, NaN off-disc. Same layout the painters consumed
    // from per-canvas vectors: identical sampling, colors, raster path.
    std::vector<float> lonDeg;
    std::vector<float> latDeg;
};

class GoesGeoCache {
public:
    // Shared hit or null. Thread-safe.
    static std::shared_ptr<const GoesGeoData> find(const GoesGeoIdentity& identity);
    // Full-grid inverse for one identity. Pure: no shared state, safe on
    // any thread. Counts builds for the test hook below.
    static std::shared_ptr<const GoesGeoData> build(const GoesGeoIdentity& identity);
    static void insert(const GoesGeoIdentity& identity,
                       std::shared_ptr<const GoesGeoData> data);
    // Hermetic isolation plus build accounting for tests.
    static void clearForTest();
    static void resetBuildsForTest();
    static int buildsForTest();

private:
    static QMutex& mutex();
    static QHash<GoesGeoIdentity, std::weak_ptr<const GoesGeoData>>& store();
};

}  // namespace cappedweather
