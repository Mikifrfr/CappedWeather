#pragma once

#include <cstddef>
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
    EchoTop,
    VerticallyIntegratedLiquid,
};

inline constexpr std::size_t kProductCount = 8;

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
constexpr bool isDerivedProduct(const ProductKind kind) {
    return kind == ProductKind::EchoTop || kind == ProductKind::VerticallyIntegratedLiquid;
}

} // namespace cappedweather
