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
    CompositeReflectivity,
    Cappi,
};

inline constexpr std::size_t kProductCount = 10;

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
// Non-throwing lookup for hot paths / settings restore: null when the kind
// is out of range (e.g. persisted int from an older build). Prefer this over
// productDescriptor() unless a missing descriptor is a programming error.
const ProductDescriptor* tryProductDescriptor(ProductKind kind) noexcept;
constexpr bool isDerivedProduct(const ProductKind kind) {
    return kind == ProductKind::EchoTop || kind == ProductKind::VerticallyIntegratedLiquid ||
           kind == ProductKind::CompositeReflectivity || kind == ProductKind::Cappi;
}

} // namespace cappedweather
