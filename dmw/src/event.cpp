#include "dmw/event.hpp"

#include <utility>

#include "impl/event_impl.hpp"

namespace dmw {
Event::Event(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Event::~Event() noexcept = default;
EventType Event::type() const noexcept { return impl_->type(); }
Result<TakeStatus> Event::take(EventInfo& info) { return impl_->take(info); }
}  // namespace dmw
