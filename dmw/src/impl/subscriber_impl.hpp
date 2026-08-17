#ifndef DMW_IMPL__SUBSCRIBER_IMPL_HPP_
#define DMW_IMPL__SUBSCRIBER_IMPL_HPP_

#include <memory>
#include <string>

#include <fastdds/dds/subscriber/DataReader.hpp>

#include "dmw/subscriber.hpp"
#include "impl/event_parent_state.hpp"
#include "impl/context.hpp"
#include "impl/reader_wait_state.hpp"

namespace dmw {

class Subscriber::Impl {
public:
    Impl(
        std::shared_ptr<impl::Context> context, eprosima::fastdds::dds::DataReader* reader,
        std::string topic_name, MessageType type, impl::Topic topic) noexcept
    : context_(context),
      reader_(reader),
      topic_name_(std::move(topic_name)),
      type_(std::move(type)),
      wait_state_(std::make_shared<impl::ReaderWaitState>(std::move(context), reader)),
      event_parent_(std::make_shared<impl::EventParentState>(context_)),
      topic_(std::move(topic)) {}
    ~Impl() noexcept;

    Result<bool> read(void* message, MessageInfo& info);
    std::string_view topic_name() const noexcept { return topic_name_; }
    const MessageType& message_type() const noexcept { return type_; }
    Result<std::size_t> matched_publisher_count() const;
    Result<std::unique_ptr<Event>> create_event(EventType type);
    const std::shared_ptr<impl::ReaderWaitState>& wait_state() const noexcept {
        return wait_state_;
    }

private:
    std::shared_ptr<impl::Context> context_;
    eprosima::fastdds::dds::DataReader* reader_;
    std::string topic_name_;
    MessageType type_;
    std::shared_ptr<impl::ReaderWaitState> wait_state_;
    std::shared_ptr<impl::EventParentState> event_parent_;
    impl::Topic topic_;
};

}  // namespace dmw

#endif  // DMW_IMPL__SUBSCRIBER_IMPL_HPP_
