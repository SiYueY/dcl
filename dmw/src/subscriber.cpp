#include "dmw/subscriber.hpp"

#include <utility>

#include "impl/subscriber_impl.hpp"
#include "impl/event_impl.hpp"

namespace dmw {
Subscriber::Subscriber(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Subscriber::~Subscriber() noexcept = default;
Result<TakeStatus> Subscriber::take(void* message, MessageInfo& info) {
    return impl_->take(message, info);
}
std::string_view Subscriber::topic_name() const noexcept { return impl_->topic_name(); }
const MessageType& Subscriber::message_type() const noexcept { return impl_->message_type(); }
Result<std::size_t> Subscriber::matched_publisher_count() const {
    return impl_->matched_publisher_count();
}
Result<std::unique_ptr<Event>> Subscriber::create_event(EventType type) {
    return impl_->create_event(type);
}
}  // namespace dmw
