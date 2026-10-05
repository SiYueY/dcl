#ifndef DCLPY_DETAIL_TIMER_HPP_
#define DCLPY_DETAIL_TIMER_HPP_

#include "context.hpp"
#include "waitable.hpp"

namespace dclpy::detail {

class NativeTimer final : public NativeWaitable {
public:
  NativeTimer(std::shared_ptr<NativeClock> clock,
              std::chrono::nanoseconds period, bool autostart);

  const std::shared_ptr<EntityBacking<dmw::Timer>> &backing() const noexcept {
    return backing_;
  }
  dmw::Result<dmw::WaitableRegistration>
  register_with(dmw::WaitSet &wait_set) override;
  void mark_unregistered() override;
  std::shared_ptr<DispatchPin> pin_dispatch() override;
  bool quiescent() const noexcept override;

  std::optional<dmw::TimerInfo>
  consume(const std::shared_ptr<DispatchPin> &pin);
  void retire_binding();

private:
  std::shared_ptr<NativeClock> clock_;
  std::shared_ptr<NativeContext> context_;
  std::shared_ptr<EntityBacking<dmw::Timer>> backing_;
};

} // namespace dclpy::detail

#endif // DCLPY_DETAIL_TIMER_HPP_
