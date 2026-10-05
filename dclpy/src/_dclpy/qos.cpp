#include "module.hpp"
#include <pybind11/stl.h>

namespace py = pybind11;
namespace dclpy::detail {
void bind_qos(py::module_& module) {
    py::enum_<dmw::HistoryPolicy>(module, "HistoryPolicy")
        .value("SYSTEM_DEFAULT", dmw::HistoryPolicy::SystemDefault)
        .value("KEEP_LAST", dmw::HistoryPolicy::KeepLast)
        .value("KEEP_ALL", dmw::HistoryPolicy::KeepAll);
    py::enum_<dmw::ReliabilityPolicy>(module, "ReliabilityPolicy")
        .value("SYSTEM_DEFAULT", dmw::ReliabilityPolicy::SystemDefault)
        .value("RELIABLE", dmw::ReliabilityPolicy::Reliable)
        .value("BEST_EFFORT", dmw::ReliabilityPolicy::BestEffort);
    py::enum_<dmw::DurabilityPolicy>(module, "DurabilityPolicy")
        .value("SYSTEM_DEFAULT", dmw::DurabilityPolicy::SystemDefault)
        .value("VOLATILE", dmw::DurabilityPolicy::Volatile)
        .value("TRANSIENT_LOCAL", dmw::DurabilityPolicy::TransientLocal);
    py::enum_<dmw::LivelinessPolicy>(module, "LivelinessPolicy")
        .value("SYSTEM_DEFAULT", dmw::LivelinessPolicy::SystemDefault)
        .value("AUTOMATIC", dmw::LivelinessPolicy::Automatic)
        .value("MANUAL_BY_TOPIC", dmw::LivelinessPolicy::ManualByTopic);
    py::enum_<dmw::QosDuration::Kind>(module, "QosDurationKind")
        .value("SYSTEM_DEFAULT", dmw::QosDuration::Kind::SystemDefault)
        .value("INFINITE", dmw::QosDuration::Kind::Infinite)
        .value("FINITE", dmw::QosDuration::Kind::Finite);
    py::class_<dmw::QosDuration>(module, "QosDuration")
        .def_static("system_default", &dmw::QosDuration::system_default)
        .def_static("infinite", &dmw::QosDuration::infinite)
        .def_static("finite", [](std::int64_t nanoseconds) {
            return unwrap(dmw::QosDuration::finite(std::chrono::nanoseconds(nanoseconds)));
        })
        .def_property_readonly("kind", &dmw::QosDuration::kind)
        .def_property_readonly("nanoseconds", [](const dmw::QosDuration& value) {
            if (value.kind() != dmw::QosDuration::Kind::Finite)
                throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::InvalidState, "QoS duration is not finite"));
            return value.value().count();
        });
    py::class_<dmw::Qos>(module, "_Qos")
        .def(py::init<>())
        .def_static("ros2_default", &dmw::Qos::ros2_default)
        .def_static("services_default", &dmw::Qos::ros2_services_default)
        .def_static("sensor_data", &dmw::Qos::ros2_sensor_data)
        .def_static("parameters", &dmw::Qos::ros2_parameters)
        .def_static("parameter_events", &dmw::Qos::ros2_parameter_events)
        .def_static("action_status", &dmw::Qos::ros2_action_status_default)
        .def("keep_last", [](dmw::Qos& self, std::size_t depth) { unwrap(self.keep_last(depth)); })
        .def("keep_all", &dmw::Qos::keep_all, py::return_value_policy::reference_internal)
        .def("reliable", &dmw::Qos::reliable, py::return_value_policy::reference_internal)
        .def("best_effort", &dmw::Qos::best_effort, py::return_value_policy::reference_internal)
        .def("volatile", &dmw::Qos::volatile_, py::return_value_policy::reference_internal)
        .def("transient_local", &dmw::Qos::transient_local, py::return_value_policy::reference_internal)
        .def("history_system_default", &dmw::Qos::history_system_default, py::return_value_policy::reference_internal)
        .def("reliability_system_default", &dmw::Qos::reliability_system_default, py::return_value_policy::reference_internal)
        .def("durability_system_default", &dmw::Qos::durability_system_default, py::return_value_policy::reference_internal)
        .def("set_deadline", py::overload_cast<dmw::QosDuration>(&dmw::Qos::deadline), py::return_value_policy::reference_internal)
        .def("set_lifespan", py::overload_cast<dmw::QosDuration>(&dmw::Qos::lifespan), py::return_value_policy::reference_internal)
        .def("set_liveliness", py::overload_cast<dmw::LivelinessPolicy>(&dmw::Qos::liveliness), py::return_value_policy::reference_internal)
        .def("set_liveliness_lease_duration", py::overload_cast<dmw::QosDuration>(&dmw::Qos::liveliness_lease_duration), py::return_value_policy::reference_internal)
        .def_property_readonly("deadline", py::overload_cast<>(&dmw::Qos::deadline, py::const_))
        .def_property_readonly("lifespan", py::overload_cast<>(&dmw::Qos::lifespan, py::const_))
        .def_property_readonly("liveliness", py::overload_cast<>(&dmw::Qos::liveliness, py::const_))
        .def_property_readonly("liveliness_lease_duration", py::overload_cast<>(&dmw::Qos::liveliness_lease_duration, py::const_))
        .def_property_readonly("history", &dmw::Qos::history)
        .def_property_readonly("depth", &dmw::Qos::depth)
        .def_property_readonly("reliability", &dmw::Qos::reliability)
        .def_property_readonly("durability", &dmw::Qos::durability);
}


}  // namespace dclpy::detail
