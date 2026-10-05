#include "timer.hpp"
#include "module.hpp"

namespace py = pybind11;
namespace dclpy::detail {

NativeTimer::NativeTimer(std::shared_ptr<NativeClock> clock,
                         std::chrono::nanoseconds period, bool autostart)
    : clock_(std::move(clock)), context_(clock_->context()) {
  dmw::TimerOptions options;
  options.period = period;
  options.autostart = autostart;
  auto context_operation = context_->operation();
  auto clock_operation = clock_->backing()->operation();
  backing_ = context_->adopt(
      unwrap(context_operation->create_timer(*clock_operation.get(), options)));
}

dmw::Result<dmw::WaitableRegistration>
NativeTimer::register_with(dmw::WaitSet &wait_set) {
  return register_backing(backing_, wait_set);
}

void NativeTimer::mark_unregistered() { backing_->set_registered(false); }

std::shared_ptr<DispatchPin> NativeTimer::pin_dispatch() {
  return std::make_shared<TypedDispatchPin<dmw::Timer>>(backing_->dispatch());
}

bool NativeTimer::quiescent() const noexcept { return backing_->quiescent(); }

std::optional<dmw::TimerInfo>
NativeTimer::consume(const std::shared_ptr<DispatchPin> &pin) {
  const auto typed =
      std::dynamic_pointer_cast<TypedDispatchPin<dmw::Timer>>(pin);
  if (!typed || !typed->lease.belongs_to(backing_.get())) {
    throw MiddlewareFailure(dmw::Error(
        dmw::ErrorCode::TypeMismatch, "Dispatch pin belongs to another timer"));
  }
  if (!typed->lease)
    throw EntityClosed();
  dmw::TimerInfo info;
  if (!unwrap(typed->lease->consume(info)))
    return std::nullopt;
  return info;
}

void NativeTimer::retire_binding() {
  if (backing_->state() != EntityState::Closed) {
    throw MiddlewareFailure(
        dmw::Error(dmw::ErrorCode::Busy, "Timer is not physically closed"));
  }
  clock_.reset();
}

void bind_timer(py::module_ &module) {
  py::class_<dmw::TimerInfo>(module, "TimerInfo")
      .def_readonly("expected_call_time", &dmw::TimerInfo::expected_call_time)
      .def_readonly("actual_call_time", &dmw::TimerInfo::actual_call_time)
      .def_property_readonly("elapsed_since_last_call_ns",
                             [](const dmw::TimerInfo &value) {
                               return value.elapsed_since_last_call.count();
                             });
  auto timer =
      py::class_<NativeTimer, NativeWaitable, std::shared_ptr<NativeTimer>>(
          module, "_Timer");
  timer
      .def(py::init([](std::shared_ptr<NativeClock> clock,
                       std::int64_t period_ns, bool autostart) {
             return std::make_shared<NativeTimer>(
                 std::move(clock), std::chrono::nanoseconds(period_ns),
                 autostart);
           }),
           py::arg("clock"), py::arg("period_ns"), py::arg("autostart") = true,
           py::call_guard<py::gil_scoped_release>())
      .def(
          "work_lease",
          [](NativeTimer &self) -> std::shared_ptr<WorkPin> {
            return std::make_shared<TypedWorkPin<dmw::Timer>>(
                self.backing()->work());
          },
          py::call_guard<py::gil_scoped_release>())
      .def("dispatch_lease", &NativeTimer::pin_dispatch,
           py::call_guard<py::gil_scoped_release>())
      .def("consume", &NativeTimer::consume,
           py::call_guard<py::gil_scoped_release>())
      .def(
          "cancel",
          [](NativeTimer &self) {
            unwrap(self.backing()->operation()->cancel());
          },
          py::call_guard<py::gil_scoped_release>())
      .def(
          "reset",
          [](NativeTimer &self) {
            unwrap(self.backing()->operation()->reset());
          },
          py::call_guard<py::gil_scoped_release>())
      .def(
          "is_canceled",
          [](NativeTimer &self) {
            return unwrap(self.backing()->operation()->is_canceled());
          },
          py::call_guard<py::gil_scoped_release>())
      .def(
          "is_ready",
          [](NativeTimer &self) {
            return unwrap(self.backing()->operation()->is_ready());
          },
          py::call_guard<py::gil_scoped_release>())
      .def_property_readonly(
          "period_ns",
          [](NativeTimer &self) {
            return self.backing()->operation()->period().count();
          })
      .def(
          "exchange_period",
          [](NativeTimer &self, std::int64_t period_ns) {
            return unwrap(self.backing()->operation()->exchange_period(
                              std::chrono::nanoseconds(period_ns)))
                .count();
          },
          py::call_guard<py::gil_scoped_release>())
      .def(
          "time_until_next_call",
          [](NativeTimer &self) {
            return unwrap(self.backing()->operation()->time_until_next_call())
                .count();
          },
          py::call_guard<py::gil_scoped_release>())
      .def("retire_binding", &NativeTimer::retire_binding);
  bind_lifecycle(timer);
}

} // namespace dclpy::detail
