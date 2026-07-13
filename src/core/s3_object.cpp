#include "core/s3_object.hpp"

#include <algorithm>

namespace cappedweather {

bool isRadarObjectKey(const std::string& key, const std::string& radarId) {
    if (radarId.size() != 4 || key.find(radarId) == std::string::npos) {
        return false;
    }
    return key.starts_with(radarId + "/") ||
           key.find("/" + radarId + "/") != std::string::npos;
}

std::optional<S3Object> newestObject(std::vector<S3Object> objects) {
    objects.erase(std::remove_if(objects.begin(), objects.end(), [](const S3Object& object) {
        return object.key.empty() || object.size == 0;
    }), objects.end());

    if (objects.empty()) {
        return std::nullopt;
    }

    const auto result = std::max_element(objects.begin(), objects.end(), [](const S3Object& left, const S3Object& right) {
        if (left.lastModified != right.lastModified) {
            return left.lastModified < right.lastModified;
        }
        return left.key < right.key;
    });
    return *result;
}

} // namespace cappedweather
