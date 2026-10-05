#ifndef DMW_IMPL__LOGICAL_WAIT_OBSERVER_HPP_
#define DMW_IMPL__LOGICAL_WAIT_OBSERVER_HPP_

namespace dmw::impl {

// Internal wake-only observer. It may neither call application code nor
// acquire the notifying registry's mutex.
class LogicalWaitObserver {
public:
    virtual ~LogicalWaitObserver() = default;
    virtual void wake() noexcept = 0;
};

}  // namespace dmw::impl

#endif  // DMW_IMPL__LOGICAL_WAIT_OBSERVER_HPP_
