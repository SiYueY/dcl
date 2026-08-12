// SPDX-License-Identifier: Apache-2.0

#ifndef DMW__SUBSCRIBER_HPP_
#define DMW__SUBSCRIBER_HPP_

#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>

#include "dmw/event.hpp"
#include "dmw/message_info.hpp"
#include "dmw/message_type.hpp"
#include "dmw/result.hpp"
#include "dmw/take_status.hpp"
#include "dmw/visibility_control.hpp"

namespace dmw {

class Node;

/// Endpoint-local subscriber options; compatibility is inherited from the parent Context.
struct SubscriberOptions {};

/// Type-erased topic reader and WaitSet waitable.
class DMW_PUBLIC Subscriber {
public:
    ~Subscriber() noexcept;

    Subscriber(const Subscriber&) = delete;
    Subscriber& operator=(const Subscriber&) = delete;
    Subscriber(Subscriber&&) = delete;
    Subscriber& operator=(Subscriber&&) = delete;

    /// Take into a non-null object matching message_type().
    Result<TakeStatus> take(void* message, MessageInfo& info);

    std::string_view topic_name() const noexcept;
    const MessageType& message_type() const noexcept;
    Result<std::size_t> matched_publisher_count() const;

    /// Create one of the subscriber EventType values defined by the DMW contract.
    Result<std::unique_ptr<Event>> create_event(EventType type);

private:
    friend class Node;
    friend class WaitSet;

    class Impl;

    explicit Subscriber(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}  // namespace dmw

#endif  // DMW__SUBSCRIBER_HPP_
