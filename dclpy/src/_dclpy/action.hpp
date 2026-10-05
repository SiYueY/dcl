#ifndef DCLPY_DETAIL_ACTION_HPP_
#define DCLPY_DETAIL_ACTION_HPP_

#include "context.hpp"
#include "type_support.hpp"
#include "waitable.hpp"

namespace dclpy::detail {

class NativeActionClient final : public NativeWaitable {
public:
  NativeActionClient(std::shared_ptr<NativeNode> node,
                     std::shared_ptr<const ActionBindingHandle> type,
                     const std::string &name);
  const std::shared_ptr<EntityBacking<dmw::ActionClient>> &
  backing() const noexcept {
    return backing_;
  }
  const std::shared_ptr<const ActionBindingHandle> &type() const noexcept {
    return type_;
  }
  dmw::Result<dmw::WaitableRegistration> register_with(dmw::WaitSet &) override;
  void mark_unregistered() override;
  std::shared_ptr<DispatchPin> pin_dispatch() override;
  std::shared_ptr<WorkPin> pin_work();
  bool quiescent() const noexcept override;
  void retire_binding();

private:
  std::shared_ptr<const ActionBindingHandle> type_;
  std::shared_ptr<NativeContext> context_;
  std::shared_ptr<EntityBacking<dmw::ActionClient>> backing_;
};

class NativeActionServer final : public NativeWaitable {
public:
  NativeActionServer(std::shared_ptr<NativeNode> node,
                     std::shared_ptr<const ActionBindingHandle> type,
                     const std::string &name,
                     std::chrono::nanoseconds result_timeout);
  const std::shared_ptr<EntityBacking<dmw::ActionServer>> &
  backing() const noexcept {
    return backing_;
  }
  const std::shared_ptr<const ActionBindingHandle> &type() const noexcept {
    return type_;
  }
  dmw::Result<dmw::WaitableRegistration> register_with(dmw::WaitSet &) override;
  void mark_unregistered() override;
  std::shared_ptr<DispatchPin> pin_dispatch() override;
  std::shared_ptr<WorkPin> pin_work();
  bool quiescent() const noexcept override;
  void retire_binding();

private:
  std::shared_ptr<const ActionBindingHandle> type_;
  std::shared_ptr<NativeContext> context_;
  std::shared_ptr<EntityBacking<dmw::ActionServer>> backing_;
};

} // namespace dclpy::detail

#endif // DCLPY_DETAIL_ACTION_HPP_
