#include "graph.hpp"
#include "module.hpp"

namespace py = pybind11;
namespace dclpy::detail {

NativeGraphEvent::NativeGraphEvent(std::shared_ptr<NativeContext> context)
    : context_(std::move(context)),
      backing_(context_->adopt(
          unwrap(context_->operation()->create_graph_event()))) {}

dmw::Result<dmw::WaitableRegistration>
NativeGraphEvent::register_with(dmw::WaitSet &wait_set) {
  return register_backing(backing_, wait_set);
}

void NativeGraphEvent::mark_unregistered() { backing_->set_registered(false); }

std::shared_ptr<DispatchPin> NativeGraphEvent::pin_dispatch() {
  return std::make_shared<TypedDispatchPin<dmw::GraphEvent>>(
      backing_->dispatch());
}

bool NativeGraphEvent::quiescent() const noexcept {
  return backing_->quiescent();
}

std::optional<dmw::GraphChangeInfo> NativeGraphEvent::take() {
  auto operation = backing_->operation();
  dmw::GraphChangeInfo info;
  if (!unwrap(operation->take(info)))
    return std::nullopt;
  return info;
}

std::optional<dmw::GraphChangeInfo>
NativeGraphEvent::take(const std::shared_ptr<DispatchPin> &pin) {
  const auto typed =
      std::dynamic_pointer_cast<TypedDispatchPin<dmw::GraphEvent>>(pin);
  if (!typed || !typed->lease.belongs_to(backing_.get())) {
    throw MiddlewareFailure(
        dmw::Error(dmw::ErrorCode::TypeMismatch,
                   "Dispatch pin belongs to another graph event"));
  }
  if (!typed->lease)
    throw EntityClosed();
  dmw::GraphChangeInfo info;
  if (!unwrap(typed->lease->take(info)))
    return std::nullopt;
  return info;
}

void NativeGraphEvent::retire_binding() {
  if (backing_->state() != EntityState::Closed) {
    throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::Busy,
                                       "Graph event is not physically closed"));
  }
}

void bind_graph(py::module_ &module) {
  py::enum_<dmw::EndpointKind>(module, "EndpointKind")
      .value("PUBLISHER", dmw::EndpointKind::Publisher)
      .value("SUBSCRIBER", dmw::EndpointKind::Subscriber);
  py::enum_<dmw::ServiceEndpointKind>(module, "ServiceEndpointKind")
      .value("CLIENT", dmw::ServiceEndpointKind::Client)
      .value("SERVER", dmw::ServiceEndpointKind::Server);
  py::enum_<dmw::ActionEndpointKind>(module, "ActionEndpointKind")
      .value("CLIENT", dmw::ActionEndpointKind::Client)
      .value("SERVER", dmw::ActionEndpointKind::Server);
  py::class_<dmw::NodeGraphInfo>(module, "NodeGraphInfo")
      .def_readonly("node_name", &dmw::NodeGraphInfo::node_name)
      .def_readonly("node_namespace", &dmw::NodeGraphInfo::node_namespace);
  py::class_<dmw::TopicGraphInfo>(module, "TopicGraphInfo")
      .def_readonly("topic_name", &dmw::TopicGraphInfo::topic_name)
      .def_readonly("wire_types", &dmw::TopicGraphInfo::wire_types)
      .def_readonly("publisher_count", &dmw::TopicGraphInfo::publisher_count)
      .def_readonly("subscriber_count", &dmw::TopicGraphInfo::subscriber_count);
  py::class_<dmw::TopicEndpointInfo>(module, "TopicEndpointInfo")
      .def_readonly("endpoint_gid", &dmw::TopicEndpointInfo::endpoint_gid)
      .def_readonly("kind", &dmw::TopicEndpointInfo::kind)
      .def_readonly("node_name", &dmw::TopicEndpointInfo::node_name)
      .def_readonly("node_namespace", &dmw::TopicEndpointInfo::node_namespace)
      .def_readonly("topic_name", &dmw::TopicEndpointInfo::topic_name)
      .def_readonly("wire_type", &dmw::TopicEndpointInfo::wire_type)
      .def_readonly("qos", &dmw::TopicEndpointInfo::qos);
  py::class_<dmw::ServiceGraphInfo>(module, "ServiceGraphInfo")
      .def_readonly("service_name", &dmw::ServiceGraphInfo::service_name)
      .def_readonly("request_wire_types",
                    &dmw::ServiceGraphInfo::request_wire_types)
      .def_readonly("response_wire_types",
                    &dmw::ServiceGraphInfo::response_wire_types)
      .def_readonly("client_candidate_count",
                    &dmw::ServiceGraphInfo::client_candidate_count)
      .def_readonly("server_candidate_count",
                    &dmw::ServiceGraphInfo::server_candidate_count);
  py::class_<dmw::ServiceEndpointInfo>(module, "ServiceEndpointInfo")
      .def_readonly("kind", &dmw::ServiceEndpointInfo::kind)
      .def_readonly("node_name", &dmw::ServiceEndpointInfo::node_name)
      .def_readonly("node_namespace", &dmw::ServiceEndpointInfo::node_namespace)
      .def_readonly("service_name", &dmw::ServiceEndpointInfo::service_name)
      .def_readonly("request_endpoint_gid",
                    &dmw::ServiceEndpointInfo::request_endpoint_gid)
      .def_readonly("response_endpoint_gid",
                    &dmw::ServiceEndpointInfo::response_endpoint_gid)
      .def_readonly("request_wire_type",
                    &dmw::ServiceEndpointInfo::request_wire_type)
      .def_readonly("response_wire_type",
                    &dmw::ServiceEndpointInfo::response_wire_type);
  py::class_<dmw::ActionGraphInfo>(module, "ActionGraphInfo")
      .def_readonly("action_name", &dmw::ActionGraphInfo::action_name)
      .def_readonly("client_candidate_count",
                    &dmw::ActionGraphInfo::client_candidate_count)
      .def_readonly("server_candidate_count",
                    &dmw::ActionGraphInfo::server_candidate_count);
  py::class_<dmw::ActionEndpointInfo>(module, "ActionEndpointInfo")
      .def_readonly("kind", &dmw::ActionEndpointInfo::kind)
      .def_readonly("node_name", &dmw::ActionEndpointInfo::node_name)
      .def_readonly("node_namespace", &dmw::ActionEndpointInfo::node_namespace)
      .def_readonly("action_name", &dmw::ActionEndpointInfo::action_name)
      .def_readonly("endpoint_gids", &dmw::ActionEndpointInfo::endpoint_gids);
  py::class_<dmw::GraphSnapshot>(module, "GraphSnapshot")
      .def_readonly("revision", &dmw::GraphSnapshot::revision)
      .def_readonly("nodes", &dmw::GraphSnapshot::nodes)
      .def_readonly("topics", &dmw::GraphSnapshot::topics)
      .def_readonly("topic_endpoints", &dmw::GraphSnapshot::topic_endpoints)
      .def_readonly("services", &dmw::GraphSnapshot::services)
      .def_readonly("service_endpoints", &dmw::GraphSnapshot::service_endpoints)
      .def_readonly("actions", &dmw::GraphSnapshot::actions)
      .def_readonly("action_endpoints", &dmw::GraphSnapshot::action_endpoints);
  py::class_<dmw::GraphChangeInfo>(module, "GraphChangeInfo")
      .def_readonly("previous_revision",
                    &dmw::GraphChangeInfo::previous_revision)
      .def_readonly("current_revision",
                    &dmw::GraphChangeInfo::current_revision);
  auto event =
      py::class_<NativeGraphEvent, NativeWaitable,
                 std::shared_ptr<NativeGraphEvent>>(module, "_GraphEvent");
  event
      .def(py::init<std::shared_ptr<NativeContext>>(),
           py::call_guard<py::gil_scoped_release>())
      .def(
          "work_lease",
          [](NativeGraphEvent &self) -> std::shared_ptr<WorkPin> {
            return std::make_shared<TypedWorkPin<dmw::GraphEvent>>(
                self.backing()->work());
          },
          py::call_guard<py::gil_scoped_release>())
      .def("dispatch_lease", &NativeGraphEvent::pin_dispatch,
           py::call_guard<py::gil_scoped_release>())
      .def("take", py::overload_cast<>(&NativeGraphEvent::take),
           py::call_guard<py::gil_scoped_release>())
      .def("take_with_pin",
           py::overload_cast<const std::shared_ptr<DispatchPin> &>(
               &NativeGraphEvent::take),
           py::call_guard<py::gil_scoped_release>())
      .def("retire_binding", &NativeGraphEvent::retire_binding);
  bind_lifecycle(event);
}

} // namespace dclpy::detail
