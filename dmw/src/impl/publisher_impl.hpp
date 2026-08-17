#ifndef DMW_IMPL__PUBLISHER_IMPL_HPP_
#define DMW_IMPL__PUBLISHER_IMPL_HPP_

#include <memory>
#include <string>

#include <fastdds/dds/publisher/DataWriter.hpp>

#include "dmw/publisher.hpp"
#include "impl/event_parent_state.hpp"
#include "impl/fastdds/context.hpp"

namespace dmw {

class Publisher::Impl {
public:
    Impl(
        std::shared_ptr<impl::fastdds::Context> context, eprosima::fastdds::dds::DataWriter* writer,
        std::string topic_name, MessageType type, impl::fastdds::Context::Topic topic) noexcept
    : context_(std::move(context)),
      writer_(writer),
      topic_name_(std::move(topic_name)),
      type_(std::move(type)),
      event_parent_(std::make_shared<impl::EventParentState>(context_)),
      topic_(std::move(topic)) {}
    ~Impl() noexcept;

    Result<void> write(const void* message);
    std::string_view topic_name() const noexcept { return topic_name_; }
    const MessageType& message_type() const noexcept { return type_; }
    Result<std::size_t> matched_subscriber_count() const;
    Result<std::unique_ptr<Event>> create_event(EventType type);

private:
    std::shared_ptr<impl::fastdds::Context> context_;
    eprosima::fastdds::dds::DataWriter* writer_;
    std::string topic_name_;
    MessageType type_;
    std::shared_ptr<impl::EventParentState> event_parent_;
    impl::fastdds::Context::Topic topic_;
};

}  // namespace dmw

#endif  // DMW_IMPL__PUBLISHER_IMPL_HPP_
