// SPDX-License-Identifier: Apache-2.0

#ifndef DMW__FASTDDS__MESSAGE_TYPE_HPP_
#define DMW__FASTDDS__MESSAGE_TYPE_HPP_

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

namespace detail {

/// Internal bridge from the Fast DDS binding header to MessageType's private constructor.
class DMW_PUBLIC MessageTypeAccess {
public:
    static Result<MessageType> create(
        eprosima::fastdds::dds::TypeSupport type_support, std::type_index binding_type);

    /// Return the immutable Fast DDS binding retained by a MessageType.
    static const eprosima::fastdds::dds::TypeSupport& type_support(
        const MessageType& message_type) noexcept;

    static std::type_index binding_type(const MessageType& message_type) noexcept;
};

}  // namespace detail

namespace fastdds {

/// Construct a MessageType from a Fast DDS generated TopicDataType binding.
template <class PubSubTypeT>
Result<MessageType> make_message_type() {
    static_assert(
        std::is_base_of<eprosima::fastdds::dds::TopicDataType, PubSubTypeT>::value,
        "PubSubTypeT must derive from eprosima::fastdds::dds::TopicDataType");

    eprosima::fastdds::dds::TypeSupport type_support(new PubSubTypeT());
    return detail::MessageTypeAccess::create(std::move(type_support), typeid(PubSubTypeT));
}

}  // namespace fastdds

}  // namespace dmw

#endif  // DMW__FASTDDS__MESSAGE_TYPE_HPP_
