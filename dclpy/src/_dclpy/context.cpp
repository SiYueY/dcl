#include "module.hpp"
#include <pybind11/stl.h>

namespace py = pybind11;
namespace dclpy::detail {
void bind_context(py::module_& module) {
    py::enum_<EntityState>(module, "_EntityState")
        .value("OPEN", EntityState::Open).value("CLOSING", EntityState::Closing)
        .value("CLOSED", EntityState::Closed);
    py::enum_<dmw::RuntimeMode>(module, "RuntimeMode")
        .value("DDS", dmw::RuntimeMode::DDS).value("ROS2", dmw::RuntimeMode::ROS2);
    py::class_<dmw::Arguments>(module, "_Arguments");
    module.def("_parse_arguments", [](const std::vector<std::string>& arguments) {
        return unwrap(dmw::parse_arguments(arguments));
    });
    py::class_<dmw::ContextOptions>(module, "_ContextOptions")
        .def(py::init<>()).def_readwrite("domain_id", &dmw::ContextOptions::domain_id)
        .def_readwrite("participant_name", &dmw::ContextOptions::participant_name)
        .def_readwrite("runtime_mode", &dmw::ContextOptions::runtime_mode)
        .def_readwrite("arguments", &dmw::ContextOptions::arguments);
    py::class_<NativeContext, std::shared_ptr<NativeContext>>(module, "_Context")
        .def(py::init<const dmw::ContextOptions&>(), py::call_guard<py::gil_scoped_release>())
        .def("stop_admission", &NativeContext::stop_admission,
             py::call_guard<py::gil_scoped_release>())
        .def("close_children", &NativeContext::close_children,
             py::call_guard<py::gil_scoped_release>())
        .def("finish_shutdown", [](NativeContext& self, std::optional<double> timeout) {
            const auto deadline = deadline_from_timeout(timeout);
            py::gil_scoped_release release;
            return self.finish_shutdown(deadline);
        }, py::arg("timeout") = py::none())
        .def_property_readonly("accepting", &NativeContext::is_accepting)
        .def("graph_revision", [](NativeContext& self) {
            return unwrap(self.operation()->graph_revision());
        }, py::call_guard<py::gil_scoped_release>())
        .def("graph_snapshot", [](NativeContext& self) {
            return unwrap(self.operation()->graph_snapshot());
        }, py::call_guard<py::gil_scoped_release>());
    py::class_<dmw::NodeOptions>(module, "_NodeOptions")
        .def(py::init<>()).def_readwrite("node_name", &dmw::NodeOptions::node_name)
        .def_readwrite("namespace", &dmw::NodeOptions::node_namespace)
        .def_readwrite("arguments", &dmw::NodeOptions::arguments)
        .def_readwrite("use_global_arguments", &dmw::NodeOptions::use_global_arguments)
        .def_readwrite("allow_undeclared_parameters", &dmw::NodeOptions::allow_undeclared_parameters);
    auto node = py::class_<NativeNode, std::shared_ptr<NativeNode>>(module, "_Node");
    node.def(py::init<std::shared_ptr<NativeContext>, const dmw::NodeOptions&>(),
             py::call_guard<py::gil_scoped_release>())
        .def_property_readonly("name", [](NativeNode& self) {
            return std::string(self.backing()->operation()->name());
        })
        .def_property_readonly("namespace", [](NativeNode& self) {
            return std::string(self.backing()->operation()->node_namespace());
        })
        .def_property_readonly("fully_qualified_name", [](NativeNode& self) {
            return std::string(self.backing()->operation()->fully_qualified_name());
        });
    bind_lifecycle(node);
    py::enum_<dmw::ClockType>(module, "ClockType")
        .value("SYSTEM_TIME", dmw::ClockType::System)
        .value("STEADY_TIME", dmw::ClockType::Steady)
        .value("ROS_TIME", dmw::ClockType::Ros);
    py::class_<dmw::TimePoint>(module, "Time")
        .def(py::init<>()).def_readwrite("nanoseconds", &dmw::TimePoint::nanoseconds)
        .def_readwrite("clock_type", &dmw::TimePoint::clock_type);
    auto clock = py::class_<NativeClock, std::shared_ptr<NativeClock>>(module, "_Clock");
    clock.def(py::init<std::shared_ptr<NativeContext>, dmw::ClockType>(),
              py::call_guard<py::gil_scoped_release>())
        .def("now", [](NativeClock& self) { return unwrap(self.backing()->operation()->now()); },
             py::call_guard<py::gil_scoped_release>())
        .def("enable_ros_time_override", [](NativeClock& self, bool enabled) {
            unwrap(self.backing()->operation()->enable_ros_time_override(enabled));
        }, py::call_guard<py::gil_scoped_release>())
        .def("set_ros_time", [](NativeClock& self, dmw::TimePoint time) {
            unwrap(self.backing()->operation()->set_ros_time(time));
        }, py::call_guard<py::gil_scoped_release>())
        .def("ros_time_override_enabled", [](NativeClock& self) {
            return unwrap(self.backing()->operation()->ros_time_override_enabled());
        }, py::call_guard<py::gil_scoped_release>());
    bind_lifecycle(clock);
}

}  // namespace dclpy::detail
