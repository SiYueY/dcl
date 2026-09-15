#ifndef DMW_IMPL__MESSAGE_TYPE_IMPL_HPP_
#define DMW_IMPL__MESSAGE_TYPE_IMPL_HPP_

#include <string>
#include <typeindex>
#include <utility>

#include "dmw/fastdds/message_type.hpp"

namespace dmw {

class MessageType::Impl {
public:
    Impl(
        eprosima::fastdds::dds::TypeSupport type_support, std::string wire_type_name,
        std::type_index pubsub_type, fastdds::MessageTypeAdapter::ReceiveCommit receive_commit)
    : type_support_(std::move(type_support)),
      wire_type_name_(std::move(wire_type_name)),
      pubsub_type_(pubsub_type),
      receive_commit_(receive_commit) {}

    static Result<MessageType> create(
        eprosima::fastdds::dds::TypeSupport type_support, std::type_index pubsub_type,
        fastdds::MessageTypeAdapter::ReceiveCommit receive_commit);
    std::string_view wire_type_name() const noexcept { return wire_type_name_; }
    const eprosima::fastdds::dds::TypeSupport& type_support() const noexcept {
        return type_support_;
    }
    std::type_index pubsub_type() const noexcept { return pubsub_type_; }
    fastdds::MessageTypeAdapter::ReceiveCommit receive_commit() const noexcept {
        return receive_commit_;
    }

private:
    friend class MessageType;
    friend class fastdds::MessageTypeAdapter;

    eprosima::fastdds::dds::TypeSupport type_support_;
    const std::string wire_type_name_;
    const std::type_index pubsub_type_;
    const fastdds::MessageTypeAdapter::ReceiveCommit receive_commit_;
};

}  // namespace dmw

#endif  // DMW_IMPL__MESSAGE_TYPE_IMPL_HPP_
