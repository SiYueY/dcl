#ifndef DMW_FASTDDS__MESSAGE_TYPE_HPP_
#define DMW_FASTDDS__MESSAGE_TYPE_HPP_

#include <memory>
#include <type_traits>
#include <typeindex>
#include <utility>

#include <fastdds/dds/topic/TopicDataType.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>

#include "dmw/message_type.hpp"
#include "dmw/result.hpp"
#include "dmw/visibility_control.hpp"

namespace dmw {

namespace fastdds {

/// Internal bridge from the Fast DDS binding header to MessageType's private constructor.
class DMW_PUBLIC MessageTypeAdapter {
public:
    static Result<MessageType> create(
        eprosima::fastdds::dds::TypeSupport type_support, std::type_index pubsub_type);

    /// Return the immutable Fast DDS binding retained by a MessageType.
    static const eprosima::fastdds::dds::TypeSupport& type_support(
        const MessageType& message_type) noexcept;

    static std::type_index pubsub_type(const MessageType& message_type) noexcept;
};

/// Construct a MessageType from a Fast DDS generated TopicDataType binding.
template <class PubSubTypeT>
Result<MessageType> create_message_type() {
    static_assert(
        std::is_base_of<eprosima::fastdds::dds::TopicDataType, PubSubTypeT>::value,
        "PubSubTypeT must derive from eprosima::fastdds::dds::TopicDataType");

    eprosima::fastdds::dds::TypeSupport type_support(new PubSubTypeT());
    return MessageTypeAdapter::create(std::move(type_support), typeid(PubSubTypeT));
}

}  // namespace fastdds

}  // namespace dmw

#endif  // DMW_FASTDDS__MESSAGE_TYPE_HPP_
