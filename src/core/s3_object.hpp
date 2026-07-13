#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cappedweather {

struct S3Object {
    std::string key;
    std::string etag;
    std::string lastModified;
    std::uint64_t size = 0;
};

bool isRadarObjectKey(const std::string& key, const std::string& radarId);
std::optional<S3Object> newestObject(std::vector<S3Object> objects);

} // namespace cappedweather
