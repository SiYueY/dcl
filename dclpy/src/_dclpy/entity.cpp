#include "module.hpp"
#include <cmath>
#include <pybind11/stl.h>

namespace py = pybind11;
namespace dclpy::detail {

Deadline deadline_from_timeout(std::optional<double> timeout) {
    if (!timeout) return std::nullopt;
    if (!std::isfinite(*timeout) || *timeout < 0)
        throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::InvalidArgument,
                                           "Timeout must be finite and nonnegative"));
    const auto now = std::chrono::steady_clock::now();
    const auto available = std::chrono::duration<double>(
        std::chrono::steady_clock::time_point::max() - now).count();
    if (*timeout >= available) return std::chrono::steady_clock::time_point::max();
    return now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                     std::chrono::duration<double>(*timeout));
}

}  // namespace dclpy::detail
