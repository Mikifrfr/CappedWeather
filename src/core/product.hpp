#pragma once

#include <string_view>
#include <vector>

namespace cappedweather {

enum class ProductKind {
    Reflectivity,
    Velocity,
    SpectrumWidth,
    DifferentialReflectivity,
    CorrelationCoefficient,
    DifferentialPhase,
};

struct ProductDescriptor {
    ProductKind kind;
    std::string_view shortName;
    std::string_view label;
    std::string_view unit;
    double minimum;
    double maximum;
};

const ProductDescriptor& productDescriptor(ProductKind kind);
const std::vector<ProductDescriptor>& productDescriptors();

} // namespace cappedweather
