#ifndef DMW_IMPL__PUBLISHER_IMPL_HPP_
#define DMW_IMPL__PUBLISHER_IMPL_HPP_

#include <memory>
#include <string>

#include <fastdds/dds/publisher/DataWriter.hpp>

#include "dmw/publisher.hpp"
#include "impl/endpoint_state.hpp"

namespace dmw {

class Publisher::Impl {
public:
    Impl(
        std::shared_ptr<impl::fastdds::ContextState> state,
        eprosima::fastdds::dds::DataWriter* writer, std::string topic_name, MessageType type,
        impl::fastdds::ContextState::TopicLease topic_lease) noexcept
    : state_(std::move(state)),
      writer_(writer),
      topic_name_(std::move(topic_name)),
      type_(std::move(type)),
      event_parent_(std::make_shared<impl::EventParentState>(state_)),
      topic_lease_(std::move(topic_lease)) {}
    ~Impl() noexcept;

    Result<void> publish(const void* message);
    std::string_view topic_name() const noexcept { return topic_name_; }
    const MessageType& message_type() const noexcept { return type_; }
    Result<std::size_t> matched_subscriber_count() const;
    Result<std::unique_ptr<Event>> create_event(EventType type);

    std::shared_ptr<impl::fastdds::ContextState> state_;
    eprosima::fastdds::dds::DataWriter* writer_;
    std::string topic_name_;
    MessageType type_;
    std::shared_ptr<impl::EventParentState> event_parent_;
    impl::fastdds::ContextState::TopicLease topic_lease_;
};

}  // namespace dmw

#endif  // DMW_IMPL__PUBLISHER_IMPL_HPP_
