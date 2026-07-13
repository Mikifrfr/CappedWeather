#include "core/product.hpp"

#include <stdexcept>

namespace cappedweather {

namespace {
const std::vector<ProductDescriptor> kProducts{
    {ProductKind::Reflectivity, "REF", "Reflectivity", "dBZ", -32.0, 95.0},
    {ProductKind::Velocity, "VEL", "Radial velocity", "m/s", -80.0, 80.0},
    {ProductKind::SpectrumWidth, "SW", "Spectrum width", "m/s", 0.0, 40.0},
    {ProductKind::DifferentialReflectivity, "ZDR", "Differential reflectivity", "dB", -8.0, 8.0},
    {ProductKind::CorrelationCoefficient, "RHOHV", "Correlation coefficient", "", 0.0, 1.05},
    {ProductKind::DifferentialPhase, "PHIDP", "Differential phase", "deg", -180.0, 180.0},
};
}

const ProductDescriptor& productDescriptor(const ProductKind kind) {
    for (const auto& product : kProducts) {
        if (product.kind == kind) {
            return product;
        }
    }
    throw std::invalid_argument("unknown NEXRAD product kind");
}

const std::vector<ProductDescriptor>& productDescriptors() {
    return kProducts;
}

} // namespace cappedweather
