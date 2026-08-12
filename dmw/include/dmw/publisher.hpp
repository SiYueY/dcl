// SPDX-License-Identifier: Apache-2.0

#ifndef DMW__PUBLISHER_HPP_
#define DMW__PUBLISHER_HPP_

#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>

#include "dmw/event.hpp"
#include "dmw/message_type.hpp"
#include "dmw/result.hpp"
#include "dmw/visibility_control.hpp"

namespace dmw {

class Node;

/// Endpoint-local publisher options; compatibility is inherited from the parent Context.
struct PublisherOptions {};

/// Type-erased topic writer.
class DMW_PUBLIC Publisher {
public:
    ~Publisher() noexcept;

    Publisher(const Publisher&) = delete;
    Publisher& operator=(const Publisher&) = delete;
    Publisher(Publisher&&) = delete;
    Publisher& operator=(Publisher&&) = delete;

    /// Publish a non-null object matching message_type().
    Result<void> publish(const void* message);

    std::string_view topic_name() const noexcept;
    const MessageType& message_type() const noexcept;
    Result<std::size_t> matched_subscriber_count() const;

    /// Create one of the publisher EventType values defined by the DMW contract.
    Result<std::unique_ptr<Event>> create_event(EventType type);

private:
    friend class Node;

    class Impl;

    explicit Publisher(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}  // namespace dmw

#endif  // DMW__PUBLISHER_HPP_
