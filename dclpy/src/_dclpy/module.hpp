#ifndef DCLPY_DETAIL_MODULE_HPP_
#define DCLPY_DETAIL_MODULE_HPP_

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "context.hpp"

namespace dclpy::detail {

Deadline deadline_from_timeout(std::optional<double> timeout);
void bind_errors(pybind11::module_&);
void bind_parameters(pybind11::module_&);
void bind_type_support(pybind11::module_&);
void bind_context(pybind11::module_&);
void bind_qos(pybind11::module_&);
void bind_build_info(pybind11::module_&);
void bind_publisher(pybind11::module_&);
void bind_subscription(pybind11::module_&);
void bind_services(pybind11::module_&);
void bind_native_io(pybind11::module_&);
void bind_wait_set(pybind11::module_&);
void bind_timer(pybind11::module_&);
void bind_graph(pybind11::module_&);
void bind_actions(pybind11::module_&);

template <typename Wrapper, typename... Options>
void bind_lifecycle(pybind11::class_<Wrapper, Options...>& cls) {
    namespace py = pybind11;
    cls.def("close", [](Wrapper& self) {
        py::gil_scoped_release release;
        self.backing()->close();
    });
    cls.def("wait_closed", [](Wrapper& self, std::optional<double> timeout) {
        const auto deadline = deadline_from_timeout(timeout);
        py::gil_scoped_release release;
        return self.backing()->wait_closed(deadline);
    }, py::arg("timeout") = py::none());
    cls.def_property_readonly("state", [](const Wrapper& self) { return self.backing()->state(); });
}

}  // namespace dclpy::detail

#endif  // DCLPY_DETAIL_MODULE_HPP_
