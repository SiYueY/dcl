#ifndef DCLPY_DETAIL_WAITABLE_HPP_
#define DCLPY_DETAIL_WAITABLE_HPP_

#include <atomic>
#include <cstdint>
#include <limits>
#include "entity.hpp"
#include "dmw/wait_set.hpp"

namespace dclpy::detail {

inline std::uint64_t next_entity_id() {
    static std::atomic<std::uint64_t> next{1};
    auto value = next.load(std::memory_order_relaxed);
    while (true) {
        if (value == std::numeric_limits<std::uint64_t>::max())
            throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::ResourceExhausted, "Entity identities are exhausted"));
        if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) return value;
    }
}

class DispatchPin {
public:
    virtual ~DispatchPin() = default;
    virtual void release() noexcept = 0;
};

template <typename T>
class TypedDispatchPin final : public DispatchPin {
public:
    explicit TypedDispatchPin(DispatchLease<T> value) : lease(std::move(value)) {}
    void release() noexcept override { lease.reset(); }
    DispatchLease<T> lease;
};

class WorkPin {
public:
    virtual ~WorkPin() = default;
    virtual void release() noexcept = 0;
};
template <typename T>
class TypedWorkPin final : public WorkPin {
public:
    explicit TypedWorkPin(WorkLease<T> value) : lease(std::move(value)) {}
    void release() noexcept override { lease.reset(); }
    WorkLease<T> lease;
};

class NativeWaitable {
public:
    virtual ~NativeWaitable() = default;
    std::uint64_t entity_id() const noexcept { return entity_id_; }
    virtual dmw::Result<dmw::WaitableRegistration> register_with(dmw::WaitSet&) = 0;
    virtual void mark_unregistered() = 0;
    virtual std::shared_ptr<DispatchPin> pin_dispatch() = 0;
    virtual bool quiescent() const noexcept = 0;
private:
    const std::uint64_t entity_id_{next_entity_id()};
};

// Registration claim is distinct from its DDS token. An add failure restores
// only the claim acquired by this call, never another owner's registration.
template <typename T>
dmw::Result<dmw::WaitableRegistration> register_backing(
    const std::shared_ptr<EntityBacking<T>>& backing, dmw::WaitSet& wait_set) {
    auto operation = backing->operation();
    backing->set_registered(true);
    try {
        auto result = wait_set.add(*operation.get());
        if (!result) backing->set_registered(false);
        return result;
    } catch (...) {
        backing->set_registered(false);
        throw;
    }
}

}  // namespace dclpy::detail

#endif  // DCLPY_DETAIL_WAITABLE_HPP_
