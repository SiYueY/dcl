#ifndef DMW_IMPL__AVAILABILITY_WAIT_STATE_HPP_
#define DMW_IMPL__AVAILABILITY_WAIT_STATE_HPP_

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

#include "dmw/availability_wait_token.hpp"

namespace dmw::impl {

/// Discovery and interruption share the wait mutex. Updating a predicate
/// without this mutex would lose a notification between checking and sleeping.
class AvailabilityWaitState : public std::enable_shared_from_this<AvailabilityWaitState> {
public:
    AvailabilityWaitToken prepare() {
        std::lock_guard lock(mutex);
        return AvailabilityWaitToken(shared_from_this(), interruption_generation_);
    }

    bool owns(const AvailabilityWaitToken& token) const noexcept {
        return token.identity_.lock().get() == this;
    }

    /// Caller holds mutex, including the first check before availability.
    bool interrupted(const AvailabilityWaitToken& token) const noexcept {
        return token.generation_ != interruption_generation_;
    }

    void notify_revision() {
        {
            std::lock_guard lock(mutex);
            ++revision;
        }
        cv.notify_all();
    }

    void interrupt() {
        {
            std::lock_guard lock(mutex);
            ++interruption_generation_;
            ++revision;
        }
        cv.notify_all();
    }

    std::mutex mutex;
    std::condition_variable cv;
    std::uint64_t revision{0};

private:
    std::uint64_t interruption_generation_{0};
};

}  // namespace dmw::impl

#endif  // DMW_IMPL__AVAILABILITY_WAIT_STATE_HPP_
