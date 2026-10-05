#ifndef DMW_AVAILABILITY_WAIT_TOKEN_HPP_
#define DMW_AVAILABILITY_WAIT_TOKEN_HPP_

#include <cstdint>
#include <memory>
#include <utility>

namespace dmw {
namespace impl {
class AvailabilityWaitState;
}

/// Captures interruption before entering a blocking availability wait.
/// A token is valid only for the endpoint that prepared it. It does not keep
/// the endpoint alive; the caller must protect its lifetime during the wait.
class AvailabilityWaitToken {
public:
    AvailabilityWaitToken() noexcept = default;

private:
    friend class impl::AvailabilityWaitState;
    AvailabilityWaitToken(std::weak_ptr<const void> identity, std::uint64_t generation) noexcept
    : identity_(std::move(identity)), generation_(generation) {}

    std::weak_ptr<const void> identity_;
    std::uint64_t generation_{0};
};

}  // namespace dmw

#endif  // DMW_AVAILABILITY_WAIT_TOKEN_HPP_
