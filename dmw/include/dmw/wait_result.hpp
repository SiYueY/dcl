#ifndef DMW_WAIT_RESULT_HPP_
#define DMW_WAIT_RESULT_HPP_

#include <cstdint>
#include <exception>
#include <utility>
#include <vector>

#include "dmw/waitable_registration.hpp"

namespace dmw {

class WaitSet;

enum class WaitStatus { Ready, Timeout };

/// Detail bit used by every non-Action registration.
inline constexpr std::uint32_t kWaitableReadyBit = 1U << 0U;

/// ActionClient aggregate sub-channel bits, indexed by constituent channel.
inline constexpr std::uint32_t kActionGoalResponseBit = 1U << 0U;
inline constexpr std::uint32_t kActionCancelResponseBit = 1U << 1U;
inline constexpr std::uint32_t kActionResultResponseBit = 1U << 2U;
inline constexpr std::uint32_t kActionFeedbackBit = 1U << 3U;
inline constexpr std::uint32_t kActionStatusBit = 1U << 4U;

/// ActionServer aggregate sub-channel bits, indexed by constituent channel.
inline constexpr std::uint32_t kActionGoalRequestBit = 1U << 0U;
inline constexpr std::uint32_t kActionCancelRequestBit = 1U << 1U;
inline constexpr std::uint32_t kActionResultRequestBit = 1U << 2U;
inline constexpr std::uint32_t kActionGoalExpiredBit = 1U << 3U;

/// One ready registration plus the sub-channels that were ready for it.
struct ReadyWaitable {
    WaitableRegistration registration;
    WaitableKind kind{WaitableKind::Subscriber};
    std::uint32_t detail_mask{0};
};

/// Immutable readiness snapshot returned by WaitSet::wait().
///
/// The detail mask is captured while the snapshot is formed, so an Executor
/// never has to re-query a waitable to learn why it was reported ready.
class WaitResult {
public:
    WaitStatus status() const noexcept { return status_; }

    const std::vector<ReadyWaitable>& ready() const noexcept { return ready_; }

private:
    friend class WaitSet;

    static WaitResult timeout() { return WaitResult(WaitStatus::Timeout, {}); }

    static WaitResult ready(std::vector<ReadyWaitable> registrations) {
        if (registrations.empty()) {
            std::terminate();
        }
        return WaitResult(WaitStatus::Ready, std::move(registrations));
    }

    WaitResult(WaitStatus status, std::vector<ReadyWaitable> ready)
    : status_(status), ready_(std::move(ready)) {}

    WaitStatus status_;
    std::vector<ReadyWaitable> ready_;
};

}  // namespace dmw

#endif  // DMW_WAIT_RESULT_HPP_
