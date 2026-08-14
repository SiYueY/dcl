#ifndef DMW_IMPL__SUBSCRIBER_IMPL_HPP_
#define DMW_IMPL__SUBSCRIBER_IMPL_HPP_

#include <memory>
#include <string>

#include <fastdds/dds/subscriber/DataReader.hpp>

#include "dmw/subscriber.hpp"
#include "impl/endpoint_state.hpp"

namespace dmw {

class Subscriber::Impl {
public:
    Impl(
        std::shared_ptr<impl::fastdds::ContextState> state,
        eprosima::fastdds::dds::DataReader* reader, std::string topic_name, MessageType type,
        impl::fastdds::ContextState::TopicLease topic_lease) noexcept
    : state_(state),
      reader_(reader),
      topic_name_(std::move(topic_name)),
      type_(std::move(type)),
      wait_state_(std::make_shared<impl::ReaderWaitState>(std::move(state), reader)),
      event_parent_(std::make_shared<impl::EventParentState>(state_)),
      topic_lease_(std::move(topic_lease)) {}
    ~Impl() noexcept;

    Result<TakeStatus> take(void* message, MessageInfo& info);
    std::string_view topic_name() const noexcept { return topic_name_; }
    const MessageType& message_type() const noexcept { return type_; }
    Result<std::size_t> matched_publisher_count() const;
    Result<std::unique_ptr<Event>> create_event(EventType type);

    std::shared_ptr<impl::fastdds::ContextState> state_;
    eprosima::fastdds::dds::DataReader* reader_;
    std::string topic_name_;
    MessageType type_;
    std::shared_ptr<impl::ReaderWaitState> wait_state_;
    std::shared_ptr<impl::EventParentState> event_parent_;
    impl::fastdds::ContextState::TopicLease topic_lease_;
};

}  // namespace dmw

#endif  // DMW_IMPL__SUBSCRIBER_IMPL_HPP_
