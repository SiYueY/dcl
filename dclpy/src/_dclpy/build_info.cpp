#include "module.hpp"
#include <pybind11/stl.h>

namespace py = pybind11;
namespace dclpy::detail {
void bind_build_info(py::module_& module) {
    module.def("build_info", [] {
        py::dict info;
        info["dclpy_version"] = DCLPY_VERSION;
        info["dclpy_interface_abi"] = 1;
        info["dmw_version"] = "0.1.0";
        info["dmw_soversion"] = "0";
        info["python_version"] = PY_VERSION;
        info["python_abi"] = "cp" + std::to_string(PY_MAJOR_VERSION) + std::to_string(PY_MINOR_VERSION);
        info["compiler"] = __VERSION__;
        info["cxx_standard"] = "17";
        info["fastdds_version"] = DCLPY_FASTDDS_VERSION;
        info["fastcdr_version"] = DCLPY_FASTCDR_VERSION;
        info["ros_profile"] = DCLPY_ROS_PROFILE;
        info["build_type"] = DCLPY_BUILD_TYPE;
        return info;
    });
}

}  // namespace dclpy::detail
