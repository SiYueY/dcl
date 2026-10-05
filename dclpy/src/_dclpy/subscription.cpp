#include "module.hpp"
#include "topic.hpp"
#include <pybind11/stl.h>

namespace py = pybind11;
namespace dclpy::detail {
void bind_subscription(py::module_& module) {
    py::class_<dmw::Gid>(module, "_Gid")
        .def_property_readonly("data", [](const dmw::Gid& gid) { return gid.data; });
    py::class_<dmw::MessageInfo>(module, "MessageInfo")
        .def_readonly("writer_gid", &dmw::MessageInfo::writer_gid)
        .def_readonly("writer_timestamp", &dmw::MessageInfo::writer_timestamp)
        .def_readonly("reader_timestamp", &dmw::MessageInfo::reader_timestamp)
        .def_readonly("to_writer_sequence", &dmw::MessageInfo::to_writer_sequence);
    auto subscriber = py::class_<NativeSubscription, NativeWaitable, std::shared_ptr<NativeSubscription>>(module, "_Subscription");
    subscriber.def(py::init<std::shared_ptr<NativeNode>, std::shared_ptr<const MessageBindingHandle>,
                           const std::string&, const dmw::Qos&>(),
                   py::call_guard<py::gil_scoped_release>())
        .def("work_lease", [](NativeSubscription& self) -> std::shared_ptr<WorkPin> {
            return std::make_shared<TypedWorkPin<dmw::Subscriber>>(self.backing()->work());
        }, py::call_guard<py::gil_scoped_release>())
        .def("dispatch_lease", [](NativeSubscription& self) { return self.pin_dispatch(); },
             py::call_guard<py::gil_scoped_release>())
        .def("take", &NativeSubscription::take)
        .def("receive", [](NativeSubscription& self, const std::shared_ptr<DispatchPin>& pin) -> py::object {
            auto typed = std::dynamic_pointer_cast<TypedDispatchPin<dmw::Subscriber>>(pin);
            if (!typed || !typed->lease.belongs_to(self.backing().get()))
                throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::TypeMismatch, "Foreign subscription dispatch pin"));
            if (!typed->lease) throw EntityClosed();
            const auto* binding = self.type()->binding;
            auto* instance = binding->create_instance();
            if (!instance) throw py::error_already_set();
            auto object = py::reinterpret_steal<py::object>(instance);
            auto* sample = binding->sample_ptr(object.ptr());
            if (!sample) {
                if (PyErr_Occurred()) throw py::error_already_set();
                throw py::type_error("Provider created an invalid receive instance");
            }
            dmw::MessageInfo info;
            if (!unwrap(typed->lease->read(sample, info))) return py::none();
            return py::make_tuple(object, info);
        })
        .def("retire_binding", &NativeSubscription::retire_binding)
        .def("actual_qos", [](NativeSubscription& self) {
            return unwrap(self.backing()->operation()->actual_qos());
        }, py::call_guard<py::gil_scoped_release>());
    bind_lifecycle(subscriber);
}
}  // namespace dclpy::detail
