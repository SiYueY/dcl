#ifndef DCLPY_COMPATIBILITY_HPP_
#define DCLPY_COMPATIBILITY_HPP_

#include <pybind11/detail/internals.h>

#ifndef DCLPY_ROS_PROFILE
#error "DCLPY_ROS_PROFILE must match the shared DCLPY runtime's interface profile"
#endif

namespace dclpy {

inline const char* interface_compatibility_id() noexcept {
    return "cp" PYBIND11_TOSTRING(PY_MAJOR_VERSION) PYBIND11_TOSTRING(PY_MINOR_VERSION)
        "-pybind" PYBIND11_TOSTRING(PYBIND11_INTERNALS_VERSION)
        PYBIND11_PLATFORM_ABI_ID
#ifdef Py_GIL_DISABLED
        "-free-threaded"
#else
        "-gil"
#endif
        "-ros=" DCLPY_ROS_PROFILE "-dmw=0.1-so0";
}

}  // namespace dclpy

#endif  // DCLPY_COMPATIBILITY_HPP_
