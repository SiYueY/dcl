#include "module.hpp"
#include "service.hpp"

namespace py = pybind11;
namespace dclpy::detail {
namespace {
py::object receive_response(NativeClient& self, const std::shared_ptr<DispatchPin>& pin) {
    auto typed = std::dynamic_pointer_cast<TypedDispatchPin<dmw::Client>>(pin);
    if (!typed || !typed->lease.belongs_to(self.backing().get()))
        throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::TypeMismatch, "Foreign client dispatch pin"));
    if (!typed->lease) throw EntityClosed();
    const auto* binding = self.type()->response->binding;
    auto* instance = binding->create_instance();
    if (!instance) throw py::error_already_set();
    auto object = py::reinterpret_steal<py::object>(instance);
    auto* sample = binding->sample_ptr(object.ptr());
    if (!sample) {
        if (PyErr_Occurred()) throw py::error_already_set();
        throw py::type_error("Invalid private response instance");
    }
    dmw::RequestId request_id;
    if (!unwrap(typed->lease->read_response(sample, request_id))) return py::none();
    return py::make_tuple(request_id, object);
}
}

void bind_services(py::module_& module) {
    py::class_<dmw::RequestId>(module, "_RequestId")
        .def_readonly("client_gid", &dmw::RequestId::client_gid)
        .def_readonly("sequence_number", &dmw::RequestId::sequence_number)
        .def("__eq__", [](const dmw::RequestId& lhs, const dmw::RequestId& rhs) { return lhs == rhs; }, py::is_operator())
        .def("__hash__", [](const dmw::RequestId& id) { return dmw::RequestIdHash{}(id); });
    auto client = py::class_<NativeClient, NativeWaitable, std::shared_ptr<NativeClient>>(module, "_Client");
    client.def(py::init<std::shared_ptr<NativeNode>, std::shared_ptr<const ServiceBindingHandle>,
                         const std::string&, const dmw::Qos&>(), py::call_guard<py::gil_scoped_release>())
        .def("receive", &receive_response)
        .def("retire_binding", &NativeClient::retire_binding)
        .def("service_is_ready", [](NativeClient& self) {
            return unwrap(self.backing()->operation()->service_is_available());
        }, py::call_guard<py::gil_scoped_release>())
        .def("wait_for_service", [](NativeClient& self, std::optional<double> timeout) {
            const auto deadline = deadline_from_timeout(timeout);
            auto admitted = self.backing()->prepare_operation([](dmw::Client& resource) {
                return unwrap(resource.prepare_availability_wait());
            });
            dmw::WaitTimeout value = dmw::WaitTimeout::infinite();
            if (deadline) {
                const auto remaining = *deadline - std::chrono::steady_clock::now();
                value = remaining <= std::chrono::steady_clock::duration::zero() ? dmw::WaitTimeout::poll() :
                    unwrap(dmw::WaitTimeout::finite(std::chrono::duration_cast<std::chrono::nanoseconds>(remaining)));
            }
            py::gil_scoped_release release;
            return unwrap(admitted.first->wait_for_service(value, admitted.second));
        }, py::arg("timeout_sec") = py::none());
    bind_lifecycle(client);
    auto service = py::class_<NativeService, NativeWaitable, std::shared_ptr<NativeService>>(module, "_Service");
    service.def(py::init<std::shared_ptr<NativeNode>, std::shared_ptr<const ServiceBindingHandle>,
                          const std::string&, const dmw::Qos&, std::size_t>(),
                py::call_guard<py::gil_scoped_release>())
        .def("retire_binding", &NativeService::retire_binding);
    bind_lifecycle(service);
}
}  // namespace dclpy::detail
