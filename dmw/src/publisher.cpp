#include "dmw/publisher.hpp"

#include <utility>

#include "impl/publisher_impl.hpp"
#include "impl/event_impl.hpp"

namespace dmw {

Publisher::Publisher(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Publisher::~Publisher() noexcept = default;

Result<void> Publisher::publish(const void* message) { return impl_->publish(message); }

std::string_view Publisher::topic_name() const noexcept { return impl_->topic_name(); }

const MessageType& Publisher::message_type() const noexcept { return impl_->message_type(); }

Result<std::size_t> Publisher::matched_subscriber_count() const {
    return impl_->matched_subscriber_count();
}

Result<std::unique_ptr<Event>> Publisher::create_event(EventType type) {
    return impl_->create_event(type);
}

}  // namespace dmw
