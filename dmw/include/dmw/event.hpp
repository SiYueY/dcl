// SPDX-License-Identifier: Apache-2.0

#ifndef DMW__EVENT_HPP_
#define DMW__EVENT_HPP_

#include <memory>
#include <utility>

#include "dmw/event_info.hpp"
#include "dmw/result.hpp"
#include "dmw/take_status.hpp"
#include "dmw/visibility_control.hpp"

namespace dmw {

class Publisher;
class Subscriber;

/// Persistent endpoint-bound status event.
class DMW_PUBLIC Event {
public:
    ~Event() noexcept;

    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
    Event(Event&&) = delete;
    Event& operator=(Event&&) = delete;

    EventType type() const noexcept;

    /// Take the current event status without invoking user callbacks.
    ///
    /// Returns ParentDestroyed when the parent is gone and the Context remains active.
    Result<TakeStatus> take(EventInfo& info);

private:
    friend class Publisher;
    friend class Subscriber;

    class Impl;

    explicit Event(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}  // namespace dmw

#endif  // DMW__EVENT_HPP_
