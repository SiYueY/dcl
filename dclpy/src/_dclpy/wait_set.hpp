#ifndef DCLPY_DETAIL_WAIT_SET_HPP_
#define DCLPY_DETAIL_WAIT_SET_HPP_

#include <mutex>
#include <unordered_map>
#include <vector>
#include "context.hpp"
#include "waitable.hpp"

namespace dclpy::detail {

struct NativeReady {
    std::uint64_t registration;
    std::uint64_t attachment_generation;
    std::uint32_t detail_mask;
    std::shared_ptr<DispatchPin> pin;
};

class ReadyBatch {
public:
    explicit ReadyBatch(std::vector<NativeReady> entries) : entries_(std::move(entries)) {}
    const std::vector<NativeReady>& entries() const noexcept { return entries_; }
    void release() noexcept {
        for (auto& entry : entries_) entry.pin->release();
        entries_.clear();
    }
private:
    std::vector<NativeReady> entries_;
};

class NativeWaitSet {
public:
    explicit NativeWaitSet(std::shared_ptr<NativeContext> context)
    : context_(std::move(context)),
      backing_(context_->adopt(unwrap(context_->operation()->create_wait_set()))),
      control_(backing_->work()),
      guard_(context_->adopt(unwrap(context_->operation()->create_guard_condition()))),
      guard_control_(guard_->work()) {
        unwrap(control_->add(*guard_control_.get()));
    }
    ~NativeWaitSet() noexcept;

    std::uint64_t add(std::shared_ptr<NativeWaitable> value, std::uint64_t generation);
    void remove(std::uint64_t token);
    void set_interest(std::uint64_t token, std::uint32_t mask);
    ReadyBatch wait(dmw::WaitTimeout timeout);
    void wake();
    void close();
    const std::shared_ptr<NativeContext>& context() const noexcept { return context_; }
    WorkLease<dmw::GuardCondition> control_wake_lease() {
        std::lock_guard lock(registrations_mutex_);
        if (closed_) throw EntityClosed();
        return guard_control_.derive_cleanup();
    }

private:
    struct Entry {
        std::shared_ptr<NativeWaitable> entity;
        std::uint64_t generation;
        dmw::WaitableRegistration native;
    };
    std::shared_ptr<NativeContext> context_;
    std::shared_ptr<EntityBacking<dmw::WaitSet>> backing_;
    WorkLease<dmw::WaitSet> control_;
    std::shared_ptr<EntityBacking<dmw::GuardCondition>> guard_;
    WorkLease<dmw::GuardCondition> guard_control_;
    std::mutex registrations_mutex_;
    std::unordered_map<std::uint64_t, Entry> registrations_;
    std::uint64_t next_registration_{1};
    bool closed_{false};
    bool waiting_{false};
};

}  // namespace dclpy::detail

#endif  // DCLPY_DETAIL_WAIT_SET_HPP_
