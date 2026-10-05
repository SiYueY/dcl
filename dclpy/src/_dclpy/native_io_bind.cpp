#include "action_io.hpp"
#include "module.hpp"
#include "native_io.hpp"
#include "service_io.hpp"
#include <pybind11/stl.h>
namespace py = pybind11;

namespace dclpy::detail {
void bind_native_io(py::module_ &module) {
  py::enum_<NativeJobKind>(module, "_JobKind")
      .value("PUBLISH", NativeJobKind::Publish)
      .value("SERVICE_REQUEST", NativeJobKind::ServiceRequest)
      .value("SERVICE_RESPONSE", NativeJobKind::ServiceResponse)
      .value("DISCARD_REQUEST", NativeJobKind::DiscardRequest)
      .value("GOAL_REQUEST", NativeJobKind::GoalRequest)
      .value("CANCEL_REQUEST", NativeJobKind::CancelRequest)
      .value("RESULT_REQUEST", NativeJobKind::ResultRequest)
      .value("GOAL_RESPONSE", NativeJobKind::GoalResponse)
      .value("CANCEL_RESPONSE", NativeJobKind::CancelResponse)
      .value("RESULT_RESPONSE", NativeJobKind::ResultResponse)
      .value("FEEDBACK", NativeJobKind::Feedback)
      .value("STATUS", NativeJobKind::Status);
  py::class_<CompletionRecord>(module, "_Completion")
      .def(py::init<>())
      .def_readwrite("attachment_generation",
                     &CompletionRecord::attachment_generation)
      .def_readwrite("operation_id", &CompletionRecord::operation_id)
      .def_readwrite("entity_id", &CompletionRecord::entity_id)
      .def_readwrite("kind", &CompletionRecord::kind)
      .def_readonly("cleanup_failed", &CompletionRecord::cleanup_failed)
      .def_readonly("skipped", &CompletionRecord::skipped)
      .def_readonly("request_id", &CompletionRecord::request_id)
      .def_readonly("executor_id", &CompletionRecord::executor_id)
      .def("check", [](const CompletionRecord &self) {
        if (self.failed)
          throw MiddlewareFailure(dmw::Error(self.error_code, self.diagnostic));
      });
  py::class_<ServiceRequestWork, std::shared_ptr<ServiceRequestWork>>(
      module, "_ServiceRequestWork")
      .def(py::init<std::shared_ptr<NativeIoDispatcher>,
                    std::shared_ptr<CompletionPort>, CompletionRecord,
                    NativeService &>(),
           py::call_guard<py::gil_scoped_release>())
      .def_property_readonly("ticket", &ServiceRequestWork::ticket)
      .def("respond", &ServiceRequestWork::respond,
           py::call_guard<py::gil_scoped_release>())
      .def("discard", &ServiceRequestWork::discard,
           py::call_guard<py::gil_scoped_release>())
      .def("release", &ServiceRequestWork::finish_task,
           py::call_guard<py::gil_scoped_release>())
      .def("receive",
           [](ServiceRequestWork &work, NativeService &service,
              const std::shared_ptr<DispatchPin> &pin) -> py::object {
             work.validate_take(service);
             auto typed =
                 std::dynamic_pointer_cast<TypedDispatchPin<dmw::Server>>(pin);
             if (!typed || !typed->lease.belongs_to(service.backing().get()))
               throw MiddlewareFailure(
                   dmw::Error(dmw::ErrorCode::TypeMismatch,
                              "Foreign service dispatch pin"));
             if (!typed->lease)
               throw EntityClosed();
             const auto *binding = service.type()->request->binding;
             auto *instance = binding->create_instance();
             if (!instance)
               throw py::error_already_set();
             auto object = py::reinterpret_steal<py::object>(instance);
             auto *sample = binding->sample_ptr(object.ptr());
             if (!sample) {
               if (PyErr_Occurred())
                 throw py::error_already_set();
               throw py::type_error("Invalid private request instance");
             }
             dmw::RequestId request_id;
             if (!unwrap(typed->lease->read_request(sample, request_id)))
               return py::none();
             work.accepted(request_id);
             return object;
           });
  py::class_<ClientRequestWork, std::shared_ptr<ClientRequestWork>>(
      module, "_ClientRequestWork")
      .def(py::init<NativeClient &, const OwnedSample &>(),
           py::call_guard<py::gil_scoped_release>())
      .def("cancel", &ClientRequestWork::cancel,
           py::call_guard<py::gil_scoped_release>())
      .def_property_readonly("send_started", &ClientRequestWork::send_started);
  py::enum_<ActionRequestKind>(module, "_ActionRequestKind")
      .value("GOAL", ActionRequestKind::Goal)
      .value("CANCEL", ActionRequestKind::Cancel)
      .value("RESULT", ActionRequestKind::Result);
  py::enum_<ActionServerRequestKind>(module, "_ActionServerRequestKind")
      .value("GOAL", ActionServerRequestKind::Goal)
      .value("CANCEL", ActionServerRequestKind::Cancel)
      .value("RESULT", ActionServerRequestKind::Result);
  py::class_<ActionResultPayload, std::shared_ptr<ActionResultPayload>>(
      module, "_ActionResultPayload")
      .def(py::init([](NativeActionServer &server, py::handle response) {
        return std::make_shared<ActionResultPayload>(server.type(), response);
      }));
  py::class_<ActionServerRequestWork, std::shared_ptr<ActionServerRequestWork>>(
      module, "_ActionServerRequestWork")
      .def(py::init<std::shared_ptr<NativeIoDispatcher>, std::shared_ptr<CompletionPort>,
                    CompletionRecord, NativeActionServer &, ActionServerRequestKind>(),
           py::call_guard<py::gil_scoped_release>())
      .def_property_readonly("ticket", &ActionServerRequestWork::ticket)
      .def_property_readonly("request_id", &ActionServerRequestWork::request_id,
                             py::return_value_policy::copy)
      .def("receive", [](ActionServerRequestWork &work, NativeActionServer &server,
                          const std::shared_ptr<DispatchPin> &pin) -> py::object {
        work.validate_take(server);
        auto typed = std::dynamic_pointer_cast<TypedDispatchPin<dmw::ActionServer>>(pin);
        if (!typed || !typed->lease.belongs_to(server.backing().get()))
          throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::TypeMismatch,
                                             "Foreign ActionServer dispatch pin"));
        if (!typed->lease) throw EntityClosed();
        const auto *binding = work.kind() == ActionServerRequestKind::Goal
                                  ? server.type()->send_goal->request->binding
                              : work.kind() == ActionServerRequestKind::Cancel
                                  ? server.type()->cancel_goal->request->binding
                                  : server.type()->get_result->request->binding;
        auto *instance = binding->create_instance();
        if (!instance) throw py::error_already_set();
        auto object = py::reinterpret_steal<py::object>(instance);
        auto *sample = binding->sample_ptr(object.ptr());
        if (!sample) {
          if (PyErr_Occurred()) throw py::error_already_set();
          throw py::type_error("Action provider created an invalid request instance");
        }
        dmw::RequestId request_id;
        const auto taken = work.kind() == ActionServerRequestKind::Goal
                               ? typed->lease->read_goal_request(sample, request_id)
                           : work.kind() == ActionServerRequestKind::Cancel
                               ? typed->lease->read_cancel_request(sample, request_id)
                               : typed->lease->read_result_request(sample, request_id);
        if (!unwrap(taken)) return py::none();
        work.accepted(request_id);
        return object;
      })
      .def("respond", [](ActionServerRequestWork &work, NativeActionServer &server,
                          py::handle response) {
        const auto type = work.kind() == ActionServerRequestKind::Goal
                              ? server.type()->send_goal->response
                          : work.kind() == ActionServerRequestKind::Cancel
                              ? server.type()->cancel_goal->response
                              : server.type()->get_result->response;
        work.respond(OwnedSample::freeze(type, response));
      })
      .def("respond_payload", &ActionServerRequestWork::respond_payload)
      .def("accept", [](ActionServerRequestWork &work, NativeActionServer &server,
                         py::handle response, const dmw::GoalInfo &info, bool execute) {
        work.accept(OwnedSample::freeze(server.type()->send_goal->response, response), info,
                    execute ? dmw::GoalAcceptMode::Execute : dmw::GoalAcceptMode::Defer);
      })
      .def("discard", &ActionServerRequestWork::discard,
           py::call_guard<py::gil_scoped_release>())
      .def("release", &ActionServerRequestWork::finish_task,
           py::call_guard<py::gil_scoped_release>());
  py::class_<ActionRequestWork, std::shared_ptr<ActionRequestWork>>(
      module, "_ActionRequestWork")
      .def(py::init<NativeActionClient &, const OwnedSample &,
                    ActionRequestKind>(),
           py::call_guard<py::gil_scoped_release>())
      .def("cancel", &ActionRequestWork::cancel,
           py::call_guard<py::gil_scoped_release>())
      .def_property_readonly("send_started", &ActionRequestWork::send_started);
  py::class_<CompletionPort, std::shared_ptr<CompletionPort>>(module,
                                                              "_CompletionPort")
      .def_property_readonly("executor_id", &CompletionPort::id);
  py::class_<NativeIoDispatcher, std::shared_ptr<NativeIoDispatcher>>(
      module, "_NativeIoDispatcher")
      .def(py::init<std::shared_ptr<NativeContext>>(),
           py::call_guard<py::gil_scoped_release>())
      .def("open_route", &NativeIoDispatcher::open_route,
           py::call_guard<py::gil_scoped_release>())
      .def("stop_route", &NativeIoDispatcher::stop_route,
           py::call_guard<py::gil_scoped_release>())
      .def("retire_route", &NativeIoDispatcher::retire_route,
           py::call_guard<py::gil_scoped_release>())
      .def("outstanding", &NativeIoDispatcher::outstanding,
           py::call_guard<py::gil_scoped_release>())
      .def("peek", &NativeIoDispatcher::peek,
           py::call_guard<py::gil_scoped_release>())
      .def("acknowledge", &NativeIoDispatcher::acknowledge,
           py::call_guard<py::gil_scoped_release>())
      .def("reserve", &NativeIoDispatcher::reserve,
           py::call_guard<py::gil_scoped_release>())
      .def("abandon", &NativeIoDispatcher::abandon,
           py::call_guard<py::gil_scoped_release>())
      .def("shutdown", &NativeIoDispatcher::shutdown,
           py::call_guard<py::gil_scoped_release>())
      .def(
          "send_request",
          [](std::shared_ptr<NativeIoDispatcher> self,
             const std::shared_ptr<CompletionPort> &port,
             CompletionRecord record,
             const std::shared_ptr<ClientRequestWork> &work) {
            record.kind = NativeJobKind::ServiceRequest;
            const auto ticket = self->reserve(port, record, false);
            try {
              auto job = std::make_unique<ServiceSendJob>(work);
              work->set_ticket(self, ticket);
              self->enqueue(ticket, std::move(job));
            } catch (...) {
              self->abandon(ticket);
              throw;
            }
            return ticket;
          },
          py::call_guard<py::gil_scoped_release>())
      .def(
          "send_action_request",
          [](std::shared_ptr<NativeIoDispatcher> self,
             const std::shared_ptr<CompletionPort> &port,
             CompletionRecord record,
             const std::shared_ptr<ActionRequestWork> &work,
             ActionRequestKind kind) {
            record.kind = kind == ActionRequestKind::Goal
                              ? NativeJobKind::GoalRequest
                          : kind == ActionRequestKind::Cancel
                              ? NativeJobKind::CancelRequest
                              : NativeJobKind::ResultRequest;
            const auto ticket = self->reserve(port, record, false);
            try {
              work->set_ticket(self, ticket);
              self->enqueue(ticket, std::make_shared<ActionSendJob>(work));
            } catch (...) {
              self->abandon(ticket);
              throw;
            }
            return ticket;
          },
          py::call_guard<py::gil_scoped_release>())
      .def(
          "publish",
          [](NativeIoDispatcher &self,
             const std::shared_ptr<CompletionPort> &port,
             CompletionRecord record, NativePublisher &publisher,
             const OwnedSample &sample) {
            auto lease = publisher.backing()->work();
            if (sample.binding()->binding != publisher.type()->binding)
              throw MiddlewareFailure(
                  dmw::Error(dmw::ErrorCode::TypeMismatch,
                             "Publisher sample has a foreign descriptor"));
            record.kind = NativeJobKind::Publish;
            const auto ticket = self.reserve(port, record, false);
            try {
              auto job = std::make_unique<PublishJob>(std::move(lease),
                                                      sample.clone());
              self.enqueue(ticket, std::move(job));
            } catch (...) {
              self.abandon(ticket);
              throw;
            }
            return ticket;
          },
          py::call_guard<py::gil_scoped_release>())
      .def(
          "publish_action",
          [](NativeIoDispatcher &self, const std::shared_ptr<CompletionPort> &port,
             CompletionRecord record, NativeActionServer &server, py::handle message,
             bool feedback) {
            const auto type = feedback ? server.type()->feedback_message : server.type()->status_message;
            auto sample = OwnedSample::freeze(type, message);
            record.kind = feedback ? NativeJobKind::Feedback : NativeJobKind::Status;
            const auto ticket = self.reserve(port, record, false);
            try {
              self.enqueue(ticket, std::make_shared<ActionPublishJob>(
                  server.backing()->work(), type, sample, feedback));
            } catch (...) {
              self.abandon(ticket);
              throw;
            }
            return ticket;
          });
}
} // namespace dclpy::detail
