#include "action.hpp"
#include "binding_registry.hpp"
#include "module.hpp"

namespace py = pybind11;
namespace dclpy::detail {

NativeActionClient::NativeActionClient(
    std::shared_ptr<NativeNode> node,
    std::shared_ptr<const ActionBindingHandle> type, const std::string &name)
    : type_(std::move(type)), context_(node->context()),
      backing_(context_->adopt(
          unwrap(node->backing()->operation()->create_action_client(type_->type,
                                                                    name)),
          [](dmw::ActionClient &client) noexcept {
            (void)client.interrupt_waits();
          })) {}

dmw::Result<dmw::WaitableRegistration>
NativeActionClient::register_with(dmw::WaitSet &wait_set) {
  return register_backing(backing_, wait_set);
}
void NativeActionClient::mark_unregistered() {
  backing_->set_registered(false);
}
std::shared_ptr<DispatchPin> NativeActionClient::pin_dispatch() {
  return std::make_shared<TypedDispatchPin<dmw::ActionClient>>(
      backing_->dispatch());
}
std::shared_ptr<WorkPin> NativeActionClient::pin_work() {
  return std::make_shared<TypedWorkPin<dmw::ActionClient>>(backing_->work());
}
bool NativeActionClient::quiescent() const noexcept {
  return backing_->quiescent();
}
void NativeActionClient::retire_binding() {
  if (backing_->state() != EntityState::Closed)
    throw MiddlewareFailure(dmw::Error(
        dmw::ErrorCode::Busy, "Action client is not physically closed"));
  type_.reset();
}

NativeActionServer::NativeActionServer(
    std::shared_ptr<NativeNode> node,
    std::shared_ptr<const ActionBindingHandle> type, const std::string &name,
    std::chrono::nanoseconds result_timeout)
    : type_(std::move(type)), context_(node->context()) {
  dmw::ActionServerOptions options;
  options.result_timeout = result_timeout;
  backing_ =
      context_->adopt(unwrap(node->backing()->operation()->create_action_server(
          type_->type, name, options)));
}

dmw::Result<dmw::WaitableRegistration>
NativeActionServer::register_with(dmw::WaitSet &wait_set) {
  return register_backing(backing_, wait_set);
}
void NativeActionServer::mark_unregistered() {
  backing_->set_registered(false);
}
std::shared_ptr<DispatchPin> NativeActionServer::pin_dispatch() {
  return std::make_shared<TypedDispatchPin<dmw::ActionServer>>(
      backing_->dispatch());
}
std::shared_ptr<WorkPin> NativeActionServer::pin_work() {
  return std::make_shared<TypedWorkPin<dmw::ActionServer>>(backing_->work());
}
bool NativeActionServer::quiescent() const noexcept {
  return backing_->quiescent();
}
void NativeActionServer::retire_binding() {
  if (backing_->state() != EntityState::Closed)
    throw MiddlewareFailure(dmw::Error(
        dmw::ErrorCode::Busy, "Action server is not physically closed"));
  type_.reset();
}

namespace {
template <typename Reader>
py::object receive(const std::shared_ptr<const MessageBindingHandle> &type,
                   Reader &&reader) {
  auto *instance = type->binding->create_instance();
  if (!instance)
    throw py::error_already_set();
  auto object = py::reinterpret_steal<py::object>(instance);
  auto *sample = type->binding->sample_ptr(object.ptr());
  if (!sample) {
    if (PyErr_Occurred())
      throw py::error_already_set();
    throw py::type_error("Action provider created an invalid receive instance");
  }
  dmw::RequestId request_id;
  if (!unwrap(reader(sample, request_id)))
    return py::none();
  return py::make_tuple(request_id, object);
}

template <typename Reader>
py::object
receive_topic(const std::shared_ptr<const MessageBindingHandle> &type,
              Reader &&reader) {
  auto *instance = type->binding->create_instance();
  if (!instance)
    throw py::error_already_set();
  auto object = py::reinterpret_steal<py::object>(instance);
  auto *sample = type->binding->sample_ptr(object.ptr());
  if (!sample) {
    if (PyErr_Occurred())
      throw py::error_already_set();
    throw py::type_error("Action provider created an invalid receive instance");
  }
  dmw::MessageInfo info;
  if (!unwrap(reader(sample, info)))
    return py::none();
  return object;
}

template <typename T>
T &checked_pin(const std::shared_ptr<DispatchPin> &pin,
               const std::shared_ptr<EntityBacking<T>> &backing,
               const char *message) {
  auto typed = std::dynamic_pointer_cast<TypedDispatchPin<T>>(pin);
  if (!typed || !typed->lease.belongs_to(backing.get()))
    throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::TypeMismatch, message));
  if (!typed->lease)
    throw EntityClosed();
  return *typed->lease.get();
}
} // namespace

void bind_actions(py::module_ &module) {
  py::class_<dmw::GoalId>(module, "_GoalId")
      .def(py::init<>())
      .def_readwrite("data", &dmw::GoalId::data)
      .def(
          "__eq__",
          [](const dmw::GoalId &left, const dmw::GoalId &right) {
            return left == right;
          },
          py::is_operator())
      .def("__hash__",
           [](const dmw::GoalId &value) { return dmw::GoalIdHash{}(value); });
  py::class_<dmw::GoalInfo>(module, "_GoalInfo")
      .def(py::init<>())
      .def_readwrite("goal_id", &dmw::GoalInfo::goal_id)
      .def_property(
          "accepted_stamp_ns",
          [](const dmw::GoalInfo &value) {
            return value.accepted_stamp.count();
          },
          [](dmw::GoalInfo &value, std::int64_t ns) {
            value.accepted_stamp = std::chrono::nanoseconds(ns);
          });
  py::enum_<dmw::GoalState>(module, "GoalState")
      .value("UNKNOWN", dmw::GoalState::Unknown)
      .value("ACCEPTED", dmw::GoalState::Accepted)
      .value("EXECUTING", dmw::GoalState::Executing)
      .value("CANCELING", dmw::GoalState::Canceling)
      .value("SUCCEEDED", dmw::GoalState::Succeeded)
      .value("CANCELED", dmw::GoalState::Canceled)
      .value("ABORTED", dmw::GoalState::Aborted);
  py::enum_<dmw::GoalEvent>(module, "_GoalEvent")
      .value("EXECUTE", dmw::GoalEvent::Execute)
      .value("CANCEL", dmw::GoalEvent::CancelGoal)
      .value("SUCCEED", dmw::GoalEvent::Succeed)
      .value("ABORT", dmw::GoalEvent::Abort)
      .value("CANCELED", dmw::GoalEvent::Canceled);
  py::enum_<dmw::ResultRequestDisposition>(module, "_ResultRequestDisposition")
      .value("UNKNOWN_GOAL", dmw::ResultRequestDisposition::UnknownGoal)
      .value("PENDING", dmw::ResultRequestDisposition::Pending)
      .value("TERMINAL", dmw::ResultRequestDisposition::Terminal);
  py::class_<dmw::GoalTransition>(module, "GoalTransition")
      .def_readonly("previous", &dmw::GoalTransition::previous)
      .def_readonly("current", &dmw::GoalTransition::current)
      .def_readonly("became_terminal", &dmw::GoalTransition::became_terminal);
  py::class_<dmw::GoalStatusInfo>(module, "GoalStatusInfo")
      .def_readonly("goal_info", &dmw::GoalStatusInfo::goal_info)
      .def_readonly("state", &dmw::GoalStatusInfo::state);
  auto client =
      py::class_<NativeActionClient, NativeWaitable,
                 std::shared_ptr<NativeActionClient>>(module, "_ActionClient");
  client
      .def(py::init([](std::shared_ptr<NativeNode> node,
                       BindingRegistry &registry, py::object action_type,
                       const std::string &name) {
        return std::make_shared<NativeActionClient>(
            std::move(node), registry.action_handle(action_type), name);
      }))
      .def("dispatch_lease", &NativeActionClient::pin_dispatch,
           py::call_guard<py::gil_scoped_release>())
      .def("work_lease", &NativeActionClient::pin_work,
           py::call_guard<py::gil_scoped_release>())
      .def("retire_binding", &NativeActionClient::retire_binding)
      .def("receive_goal_response",
           [](NativeActionClient &self,
              const std::shared_ptr<DispatchPin> &pin) {
             auto &lease = checked_pin(pin, self.backing(),
                                       "Foreign ActionClient dispatch pin");
             return receive(self.type()->send_goal->response,
                            [&lease](void *sample, dmw::RequestId &request_id) {
                              return lease.read_goal_response(sample,
                                                              request_id);
                            });
           })
      .def("receive_cancel_response",
           [](NativeActionClient &self,
              const std::shared_ptr<DispatchPin> &pin) {
             auto &lease = checked_pin(pin, self.backing(),
                                       "Foreign ActionClient dispatch pin");
             return receive(self.type()->cancel_goal->response,
                            [&lease](void *sample, dmw::RequestId &request_id) {
                              return lease.read_cancel_response(sample,
                                                                request_id);
                            });
           })
      .def("receive_result_response",
           [](NativeActionClient &self,
              const std::shared_ptr<DispatchPin> &pin) {
             auto &lease = checked_pin(pin, self.backing(),
                                       "Foreign ActionClient dispatch pin");
             return receive(self.type()->get_result->response,
                            [&lease](void *sample, dmw::RequestId &request_id) {
                              return lease.read_result_response(sample,
                                                                request_id);
                            });
           })
      .def("receive_feedback",
           [](NativeActionClient &self,
              const std::shared_ptr<DispatchPin> &pin) {
             auto &lease = checked_pin(pin, self.backing(),
                                       "Foreign ActionClient dispatch pin");
             return receive_topic(
                 self.type()->feedback_message,
                 [&lease](void *sample, dmw::MessageInfo &info) {
                   return lease.read_feedback(sample, info);
                 });
           })
      .def("receive_status",
           [](NativeActionClient &self,
              const std::shared_ptr<DispatchPin> &pin) {
             auto &lease = checked_pin(pin, self.backing(),
                                       "Foreign ActionClient dispatch pin");
             return receive_topic(
                 self.type()->status_message,
                 [&lease](void *sample, dmw::MessageInfo &info) {
                   return lease.read_status(sample, info);
                 });
           })
      .def(
          "server_is_ready",
          [](NativeActionClient &self) {
            return unwrap(self.backing()->operation()->server_is_available());
          },
          py::call_guard<py::gil_scoped_release>())
      .def(
          "wait_for_server",
          [](NativeActionClient &self, std::optional<double> timeout) {
            const auto deadline = deadline_from_timeout(timeout);
            auto admitted = self.backing()->prepare_operation(
                [](dmw::ActionClient &resource) {
                  return unwrap(resource.prepare_availability_wait());
                });
            dmw::WaitTimeout value = dmw::WaitTimeout::infinite();
            if (deadline) {
              const auto remaining =
                  *deadline - std::chrono::steady_clock::now();
              value = remaining <= std::chrono::steady_clock::duration::zero()
                          ? dmw::WaitTimeout::poll()
                          : unwrap(dmw::WaitTimeout::finite(
                                std::chrono::duration_cast<
                                    std::chrono::nanoseconds>(remaining)));
            }
            py::gil_scoped_release release;
            return unwrap(
                admitted.first->wait_for_server(value, admitted.second));
          },
          py::arg("timeout_sec") = py::none());
  bind_lifecycle(client);
  auto server =
      py::class_<NativeActionServer, NativeWaitable,
                 std::shared_ptr<NativeActionServer>>(module, "_ActionServer");
  server
      .def(
          py::init([](std::shared_ptr<NativeNode> node,
                      BindingRegistry &registry, py::object action_type,
                      const std::string &name, std::int64_t result_timeout_ns) {
            return std::make_shared<NativeActionServer>(
                std::move(node), registry.action_handle(action_type), name,
                std::chrono::nanoseconds(result_timeout_ns));
          }))
      .def("dispatch_lease", &NativeActionServer::pin_dispatch,
           py::call_guard<py::gil_scoped_release>())
      .def("work_lease", &NativeActionServer::pin_work,
           py::call_guard<py::gil_scoped_release>())
      .def("retire_binding", &NativeActionServer::retire_binding)
      .def("receive_goal_request",
           [](NativeActionServer &self,
              const std::shared_ptr<DispatchPin> &pin) {
             auto &lease = checked_pin(pin, self.backing(),
                                       "Foreign ActionServer dispatch pin");
             return receive(self.type()->send_goal->request,
                            [&lease](void *sample, dmw::RequestId &request_id) {
                              return lease.read_goal_request(sample,
                                                             request_id);
                            });
           })
      .def("receive_cancel_request",
           [](NativeActionServer &self,
              const std::shared_ptr<DispatchPin> &pin) {
             auto &lease = checked_pin(pin, self.backing(),
                                       "Foreign ActionServer dispatch pin");
             return receive(self.type()->cancel_goal->request,
                            [&lease](void *sample, dmw::RequestId &request_id) {
                              return lease.read_cancel_request(sample,
                                                               request_id);
                            });
           })
      .def("receive_result_request",
           [](NativeActionServer &self,
              const std::shared_ptr<DispatchPin> &pin) {
             auto &lease = checked_pin(pin, self.backing(),
                                       "Foreign ActionServer dispatch pin");
             return receive(self.type()->get_result->request,
                            [&lease](void *sample, dmw::RequestId &request_id) {
                              return lease.read_result_request(sample,
                                                               request_id);
                            });
           })
      .def(
          "take_expired_goals",
          [](NativeActionServer &self) {
            return unwrap(self.backing()->operation()->take_expired_goals());
          },
          py::call_guard<py::gil_scoped_release>())
      .def(
          "goal_state",
          [](NativeActionServer &self, const dmw::GoalId &goal_id) {
            return unwrap(self.backing()->operation()->goal_state(goal_id));
          },
          py::call_guard<py::gil_scoped_release>())
      .def(
          "update_goal_state",
          [](NativeActionServer &self, const dmw::GoalId &goal_id,
             dmw::GoalEvent event) {
            return unwrap(
                self.backing()->operation()->update_goal_state(goal_id, event));
          },
          py::call_guard<py::gil_scoped_release>())
      .def(
          "status_snapshot",
          [](NativeActionServer &self) {
            return unwrap(self.backing()->operation()->status_snapshot());
          },
          py::call_guard<py::gil_scoped_release>())
      .def(
          "register_result_request",
          [](NativeActionServer &self, const dmw::GoalId &goal_id,
             const dmw::RequestId &request_id) {
            return unwrap(self.backing()->operation()->register_result_request(
                goal_id, request_id));
          },
          py::call_guard<py::gil_scoped_release>())
      .def(
          "take_pending_result_requests",
          [](NativeActionServer &self, const dmw::GoalId &goal_id) {
            return unwrap(
                self.backing()->operation()->take_pending_result_requests(goal_id));
          },
          py::call_guard<py::gil_scoped_release>())
      .def(
          "select_cancel_goals",
          [](NativeActionServer &self, const dmw::GoalId &goal_id,
             std::int64_t stamp_ns) {
            dmw::CancelGoalCriteria criteria;
            criteria.goal_id = goal_id;
            criteria.stamp = std::chrono::nanoseconds(stamp_ns);
            return unwrap(self.backing()->operation()->select_cancel_goals(criteria)).goals;
          },
          py::call_guard<py::gil_scoped_release>())
      .def(
          "discard_goal_request",
          [](NativeActionServer &self, const dmw::RequestId &request_id) {
            unwrap(self.backing()->operation()->discard_goal_request(request_id));
          },
          py::call_guard<py::gil_scoped_release>())
      .def(
          "discard_cancel_request",
          [](NativeActionServer &self, const dmw::RequestId &request_id) {
            unwrap(self.backing()->operation()->discard_cancel_request(request_id));
          },
          py::call_guard<py::gil_scoped_release>())
      .def(
          "discard_result_request",
          [](NativeActionServer &self, const dmw::RequestId &request_id) {
            unwrap(self.backing()->operation()->discard_result_request(request_id));
          },
          py::call_guard<py::gil_scoped_release>());
  bind_lifecycle(server);
}

} // namespace dclpy::detail
