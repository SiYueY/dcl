#ifndef DCLPY_DETAIL_GRAPH_HPP_
#define DCLPY_DETAIL_GRAPH_HPP_

#include "context.hpp"
#include "waitable.hpp"

namespace dclpy::detail {

class NativeGraphEvent final : public NativeWaitable {
public:
  explicit NativeGraphEvent(std::shared_ptr<NativeContext> context);

  const std::shared_ptr<EntityBacking<dmw::GraphEvent>> &
  backing() const noexcept {
    return backing_;
  }
  dmw::Result<dmw::WaitableRegistration>
  register_with(dmw::WaitSet &wait_set) override;
  void mark_unregistered() override;
  std::shared_ptr<DispatchPin> pin_dispatch() override;
  bool quiescent() const noexcept override;

  std::optional<dmw::GraphChangeInfo> take();
  std::optional<dmw::GraphChangeInfo>
  take(const std::shared_ptr<DispatchPin> &pin);
  void retire_binding();

private:
  std::shared_ptr<NativeContext> context_;
  std::shared_ptr<EntityBacking<dmw::GraphEvent>> backing_;
};

} // namespace dclpy::detail

#endif // DCLPY_DETAIL_GRAPH_HPP_
