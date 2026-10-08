#pragma once

// One lock for every library call that can reach HDF5 global state.
//
// The vcpkg HDF5 is built WITHOUT thread safety (H5_HAVE_THREADSAFE off),
// and netCDF-4 is layered on top of the same HDF5: an ODIM decode and a GLM
// netCDF read running on different pools are two threads inside hdf5.dll at
// once. Per-reader mutexes do not cover that pairing. Every H5* call and
// every netCDF-4-backed nc* call process-wide must hold this lock. Downloads
// stay parallel; only the file reads serialize.
#include <mutex>

namespace cappedweather {

inline std::mutex& hdf5SerialMutex() {
    static std::mutex mutex;
    return mutex;
}

struct Hdf5SerialLock {
    std::lock_guard<std::mutex> guard;
    Hdf5SerialLock() : guard(hdf5SerialMutex()) {}
};

} // namespace cappedweather
