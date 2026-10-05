#include "binding_registry.hpp"

#include <cstring>
#include "dclpy/compatibility.hpp"

namespace py = pybind11;
namespace dclpy::detail {
namespace {

void check_gil() {
    if (!PyGILState_Check()) throw std::logic_error("Binding registry access requires the GIL");
}

void check_header(const DclpyBindingHeaderV1& header, std::size_t required) {
    if (header.abi_version != DCLPY_INTERFACE_BINDING_ABI || header.struct_size < required ||
        !header.compatibility_id ||
        std::strcmp(header.compatibility_id, interface_compatibility_id()) != 0)
        throw py::import_error("Incompatible DCLPY interface binding ABI");
}

template <typename T>
const T* capsule(py::handle type, const char* attribute, const char* name) {
    check_gil();
    if (!PyType_Check(type.ptr()) || !py::hasattr(type, attribute))
        throw py::import_error("Interface type does not carry a DCLPY binding capsule");
    auto object = type.attr(attribute);
    if (!PyCapsule_IsValid(object.ptr(), name))
        throw py::import_error("Invalid DCLPY interface capsule name");
    auto* binding = static_cast<const T*>(PyCapsule_GetPointer(object.ptr(), name));
    if (!binding) throw py::error_already_set();
    check_header(binding->header, sizeof(T));
    if (!binding->python_qualified_name || !*binding->python_qualified_name)
        throw py::import_error("Missing interface qualified name");
    return binding;
}

void check_binding(const DclpyMessageBindingV1* binding) {
    if (!binding) throw py::import_error("Missing constituent message descriptor");
    check_header(binding->header, sizeof(*binding));
    if (!binding->message_type || !binding->is_instance || !binding->sample_ptr ||
        !binding->create_instance || !binding->clone_sample || !binding->destroy_sample ||
        !binding->message_type())
        throw py::import_error("Incomplete message descriptor");
    if (!dmw::fastdds::MessageTypeAdapter::receive_commit(*binding->message_type()))
        throw py::import_error("Message descriptor requires a transactional receive adapter");
}

[[noreturn]] void adapter_failure(const DclpyBindingErrorV1& error) {
    const auto code = [&] {
        switch (error.code) {
            case DclpyBindingErrorCodeV1::InvalidArgument: return dmw::ErrorCode::InvalidArgument;
            case DclpyBindingErrorCodeV1::TypeMismatch: return dmw::ErrorCode::TypeMismatch;
            case DclpyBindingErrorCodeV1::ResourceExhausted: return dmw::ErrorCode::ResourceExhausted;
            default: return dmw::ErrorCode::DDSError;
        }
    }();
    const auto length = ::strnlen(error.message, sizeof(error.message));
    throw MiddlewareFailure(dmw::Error(code, std::string(error.message, length)));
}

bool same_message_type(const dmw::MessageType& left, const dmw::MessageType& right) {
    return left.type_name() == right.type_name() &&
           dmw::fastdds::MessageTypeAdapter::pubsub_type(left) ==
               dmw::fastdds::MessageTypeAdapter::pubsub_type(right);
}

void check_service(const DclpyServiceBindingV1* binding) {
    if (!binding) throw py::import_error("Missing constituent service descriptor");
    check_header(binding->header, sizeof(*binding));
    if (!binding->service_type || !binding->service_type())
        throw py::import_error("Missing service type descriptor");
    check_binding(binding->request);
    check_binding(binding->response);
    if (!same_message_type(binding->service_type()->request_type(), *binding->request->message_type()) ||
        !same_message_type(binding->service_type()->response_type(), *binding->response->message_type()))
        throw py::import_error("Service constituent wire types disagree");
}
}

OwnedSample OwnedSample::clone() const {
    if (!sample_) throw std::logic_error("Cannot clone a moved OwnedSample");
    DclpyBindingErrorV1 error{};
    auto* clone = binding_->binding->clone_sample(sample_, &error);
    if (!clone || error.code != DclpyBindingErrorCodeV1::None) {
        if (clone) binding_->binding->destroy_sample(clone);
        adapter_failure(error);
    }
    return OwnedSample(binding_, clone);
}

OwnedSample OwnedSample::freeze(std::shared_ptr<const MessageBindingHandle> handle, py::handle message) {
    check_gil();
    if (!handle->binding->is_instance(message.ptr()))
        throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::TypeMismatch, "Message has the wrong generated type"));
    auto* source = handle->binding->sample_ptr(message.ptr());
    if (!source) throw py::import_error("Message binding rejected its own instance");
    DclpyBindingErrorV1 error{};
    auto* sample = handle->binding->clone_sample(source, &error);
    if (!sample || error.code != DclpyBindingErrorCodeV1::None) {
        if (sample) handle->binding->destroy_sample(sample);
        adapter_failure(error);
    }
    return OwnedSample(std::move(handle), sample);
}

py::object OwnedSample::materialize() const {
    check_gil();
    auto instance = py::reinterpret_steal<py::object>(binding_->binding->create_instance());
    if (!instance) throw py::error_already_set();
    auto* destination = binding_->binding->sample_ptr(instance.ptr());
    if (!destination) throw py::import_error("Message factory returned an invalid instance");
    unwrap(dmw::fastdds::MessageTypeAdapter::receive_commit(binding_->type)(sample_, destination));
    return instance;
}

void BindingRegistry::retain_provider(py::handle type) {
    auto provider = py::module_::import(py::str(type.attr("__module__")).cast<std::string>().c_str());
    for (const auto& entry : providers_) {
        if (entry.is(provider)) return;
    }
    if (py::hasattr(provider, "__dclpy_dependencies__")) {
        for (const auto& dependency : provider.attr("__dclpy_dependencies__")) {
            if (!PyModule_Check(dependency.ptr()))
                throw py::import_error("Interface dependencies must be imported modules");
            providers_.push_back(py::reinterpret_borrow<py::object>(dependency));
        }
    }
    providers_.push_back(provider);
    providers_.push_back(py::reinterpret_borrow<py::object>(type));
}

std::shared_ptr<const MessageBindingHandle> BindingRegistry::message(py::handle type) {
    const auto* binding = capsule<DclpyMessageBindingV1>(type, "__dclpy_message_binding__", "dclpy.MessageBindingV1");
    check_binding(binding);
    auto instance = py::reinterpret_steal<py::object>(binding->create_instance());
    if (!instance) throw py::error_already_set();
    const auto matches = PyObject_IsInstance(instance.ptr(), type.ptr());
    if (matches < 0) throw py::error_already_set();
    if (!matches || !binding->is_instance(instance.ptr()) || !binding->sample_ptr(instance.ptr()))
        throw py::import_error("Message class disagrees with its factory adapter");
    retain_provider(type);
    const auto found = messages_.find(binding);
    if (found != messages_.end()) return found->second;
    auto handle = std::make_shared<MessageBindingHandle>(MessageBindingHandle{binding, *binding->message_type()});
    messages_.emplace(binding, handle);
    return handle;
}

const DclpyServiceBindingV1* BindingRegistry::service(py::handle type) {
    const auto* binding = capsule<DclpyServiceBindingV1>(type, "__dclpy_service_binding__", "dclpy.ServiceBindingV1");
    check_service(binding);
    if (!py::hasattr(type, "Request") || !py::hasattr(type, "Response"))
        throw py::import_error("Service class is missing Request or Response");
    if (message(type.attr("Request"))->binding != binding->request ||
        message(type.attr("Response"))->binding != binding->response)
        throw py::import_error("Service classes disagree with constituent capsules");
    retain_provider(type);
    return binding;
}

std::shared_ptr<const ServiceBindingHandle> BindingRegistry::service_handle(py::handle type) {
    const auto* binding = service(type);
    return std::make_shared<ServiceBindingHandle>(ServiceBindingHandle{
        binding, *binding->service_type(), message(type.attr("Request")), message(type.attr("Response"))});
}

std::shared_ptr<const ActionBindingHandle> BindingRegistry::action_handle(py::handle type) {
    const auto* binding = action(type);
    auto impl = type.attr("Impl");
    return std::make_shared<ActionBindingHandle>(ActionBindingHandle{
        binding, *binding->action_type(), message(type.attr("Goal")), message(type.attr("Result")),
        message(type.attr("Feedback")), service_handle(impl.attr("SendGoalService")),
        service_handle(impl.attr("CancelGoalService")), service_handle(impl.attr("GetResultService")),
        message(impl.attr("FeedbackMessage")), message(impl.attr("GoalStatusMessage"))});
}

const DclpyActionBindingV1* BindingRegistry::action(py::handle type) {
    const auto* binding = capsule<DclpyActionBindingV1>(type, "__dclpy_action_binding__", "dclpy.ActionBindingV1");
    if (!binding->action_type || !binding->action_type())
        throw py::import_error("Missing Action type descriptor");
    for (const auto* attribute : {"Goal", "Result", "Feedback", "Impl"}) {
        if (!py::hasattr(type, attribute)) throw py::import_error("Action class is incomplete");
    }
    if (message(type.attr("Goal"))->binding != binding->goal ||
        message(type.attr("Result"))->binding != binding->result ||
        message(type.attr("Feedback"))->binding != binding->feedback)
        throw py::import_error("Action classes disagree with constituent capsules");
    auto impl = type.attr("Impl");
    for (const auto* attribute : {"SendGoalService", "CancelGoalService", "GetResultService",
                                  "FeedbackMessage", "GoalStatusMessage"}) {
        if (!py::hasattr(impl, attribute)) throw py::import_error("Action.Impl metadata is incomplete");
    }
    if (service(impl.attr("SendGoalService")) != binding->send_goal ||
        service(impl.attr("CancelGoalService")) != binding->cancel_goal ||
        service(impl.attr("GetResultService")) != binding->get_result ||
        message(impl.attr("FeedbackMessage"))->binding != binding->feedback_message ||
        message(impl.attr("GoalStatusMessage"))->binding != binding->status_message)
        throw py::import_error("Action.Impl classes disagree with envelope capsules");
    const auto& action = *binding->action_type();
    if (!same_message_type(action.send_goal_type().request_type(), binding->send_goal->service_type()->request_type()) ||
        !same_message_type(action.send_goal_type().response_type(), binding->send_goal->service_type()->response_type()) ||
        !same_message_type(action.cancel_goal_type().request_type(), binding->cancel_goal->service_type()->request_type()) ||
        !same_message_type(action.cancel_goal_type().response_type(), binding->cancel_goal->service_type()->response_type()) ||
        !same_message_type(action.get_result_type().request_type(), binding->get_result->service_type()->request_type()) ||
        !same_message_type(action.get_result_type().response_type(), binding->get_result->service_type()->response_type()) ||
        !same_message_type(action.feedback_type(), *binding->feedback_message->message_type()) ||
        !same_message_type(action.status_type(), *binding->status_message->message_type()))
        throw py::import_error("Action native descriptor disagrees with its envelope wire types");
    retain_provider(type);
    return binding;
}

void BindingRegistry::clear() {
    check_gil();
    // Handles retained outside the registry prove that the teardown barrier has
    // not finished; unloading a provider here would invalidate native hooks.
    for (const auto& entry : messages_) {
        if (entry.second.use_count() != 1)
            throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::Busy, "Native samples still retain interface bindings"));
    }
    messages_.clear();
    providers_.clear();
}

void bind_type_support(py::module_& module) {
    py::class_<MessageBindingHandle, std::shared_ptr<MessageBindingHandle>>(module, "_MessageBinding");
    py::class_<ServiceBindingHandle, std::shared_ptr<ServiceBindingHandle>>(module, "_ServiceBinding");
    py::class_<OwnedSample>(module, "_OwnedSample")
        .def("clone", &OwnedSample::clone, py::call_guard<py::gil_scoped_release>())
        .def("materialize", &OwnedSample::materialize);
    py::class_<BindingRegistry>(module, "_BindingRegistry")
        .def(py::init<>()).def("message", &BindingRegistry::message)
        .def("service", &BindingRegistry::service_handle)
        .def("validate_service", [](BindingRegistry& self, py::handle type) { (void)self.service(type); })
        .def("validate_action", [](BindingRegistry& self, py::handle type) { (void)self.action(type); })
        .def("snapshot", [](BindingRegistry& self, py::handle type, py::handle value) {
            return OwnedSample::freeze(self.message(type), value);
        }).def("clear", &BindingRegistry::clear);
    module.def("_interface_compatibility_id", &interface_compatibility_id);
}

}  // namespace dclpy::detail
