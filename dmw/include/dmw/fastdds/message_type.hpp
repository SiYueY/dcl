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
    /// Commit a received scratch sample without modifying destination on failure.
    using ReceiveCommit = Result<void> (*)(const void* scratch, void* destination) noexcept;

    static Result<MessageType> create(
        eprosima::fastdds::dds::TypeSupport type_support, std::type_index pubsub_type,
        ReceiveCommit receive_commit = nullptr);

    /// Return the immutable Fast DDS binding retained by a MessageType.
    static const eprosima::fastdds::dds::TypeSupport& type_support(
        const MessageType& message_type) noexcept;

    static std::type_index pubsub_type(const MessageType& message_type) noexcept;

    /// Return the optional transactional receive commit retained by a MessageType.
    static ReceiveCommit receive_commit(const MessageType& message_type) noexcept;
};

namespace detail {

template <class SampleT>
Result<void> transactional_receive_commit(const void* scratch, void* destination) noexcept {
    try {
        // Copy construction can fail before destination is touched.  The
        // required no-throw swap is then the linearization point.
        SampleT staged(*static_cast<const SampleT*>(scratch));
        using std::swap;
        swap(staged, *static_cast<SampleT*>(destination));
        return Result<void>::success();
    } catch (...) {
        return Result<void>::failure(
            Error(ErrorCode::DDSError, "DMW receive commit adapter failed"));
    }
}

}  // namespace detail

/// Construct a MessageType from a Fast DDS generated TopicDataType binding.
///
/// Supplying SampleT enables a transactional direct receive commit.  SampleT
/// must have a no-throw ADL swap; otherwise use the one-parameter fallback.
template <class PubSubTypeT, class SampleT = void>
Result<MessageType> create_message_type() {
    static_assert(
        std::is_base_of<eprosima::fastdds::dds::TopicDataType, PubSubTypeT>::value,
        "PubSubTypeT must derive from eprosima::fastdds::dds::TopicDataType");

    eprosima::fastdds::dds::TypeSupport type_support(new PubSubTypeT());
    if constexpr (std::is_void<SampleT>::value) {
        return MessageTypeAdapter::create(std::move(type_support), typeid(PubSubTypeT));
    } else {
        static_assert(
            std::is_copy_constructible<SampleT>::value,
            "SampleT must be copy constructible for transactional receive commit");
        static_assert(
            std::is_nothrow_swappable<SampleT>::value,
            "SampleT must be no-throw swappable for transactional receive commit");
        return MessageTypeAdapter::create(
            std::move(type_support), typeid(PubSubTypeT),
            &detail::transactional_receive_commit<SampleT>);
    }
}

}  // namespace fastdds

}  // namespace dmw

#endif  // DMW_FASTDDS__MESSAGE_TYPE_HPP_
