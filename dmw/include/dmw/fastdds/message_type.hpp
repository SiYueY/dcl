// SPDX-License-Identifier: Apache-2.0

#ifndef DMW__FASTDDS__MESSAGE_TYPE_HPP_
#define DMW__FASTDDS__MESSAGE_TYPE_HPP_

#include <memory>
#include <type_traits>
#include <typeindex>
#include <utility>

#include <fastdds/dds/topic/TopicDataType.hpp>

#include "dmw/message_type.hpp"
#include "dmw/result.hpp"
#include "dmw/visibility_control.hpp"

namespace dmw {

namespace detail {

/// Internal bridge from the Fast DDS binding header to MessageType's private constructor.
class DMW_PUBLIC MessageTypeAccess {
public:
    static Result<MessageType> create(
        std::shared_ptr<eprosima::fastdds::dds::TopicDataType> type_support,
        std::type_index binding_type);
};

}  // namespace detail

namespace fastdds {

/// Construct a MessageType from a Fast DDS generated TopicDataType binding.
template <class PubSubTypeT>
Result<MessageType> make_message_type() {
    static_assert(
        std::is_base_of<eprosima::fastdds::dds::TopicDataType, PubSubTypeT>::value,
        "PubSubTypeT must derive from eprosima::fastdds::dds::TopicDataType");

    auto type_support = std::make_shared<PubSubTypeT>();
    return detail::MessageTypeAccess::create(std::move(type_support), typeid(PubSubTypeT));
}

}  // namespace fastdds

}  // namespace dmw

#endif  // DMW__FASTDDS__MESSAGE_TYPE_HPP_
