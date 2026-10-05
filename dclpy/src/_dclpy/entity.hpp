#ifndef DCLPY_DETAIL_ENTITY_HPP_
#define DCLPY_DETAIL_ENTITY_HPP_

#include <array>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <type_traits>

#include "error.hpp"

namespace dclpy::detail {

// All user admission follows this mutex -> backing mutex order. Cleanup from
// an accepted WorkLease bypasses admission, but cannot introduce user work.
struct ContextAdmission {
    std::mutex mutex;
    bool open{true};
};

enum class EntityState { Open, Closing, Closed };
enum class LeaseKind : std::size_t { Operation, Dispatch, Work };

template <typename T>
class EntityBacking;

template <typename T, LeaseKind Kind>
class EntityLease {
public:
    EntityLease() noexcept = default;
    EntityLease(const EntityLease&) = delete;
    EntityLease& operator=(const EntityLease&) = delete;
    EntityLease(EntityLease&& other) noexcept
    : backing_(std::move(other.backing_)), resource_(std::exchange(other.resource_, nullptr)) {}
    EntityLease& operator=(EntityLease&& other) noexcept {
        if (this != &other) {
            reset();
            backing_ = std::move(other.backing_);
            resource_ = std::exchange(other.resource_, nullptr);
        }
        return *this;
    }
    ~EntityLease() noexcept { reset(); }

    T* get() const noexcept { return resource_; }
    bool belongs_to(const EntityBacking<T>* backing) const noexcept { return backing_.get() == backing; }
    T* operator->() const noexcept { return resource_; }
    explicit operator bool() const noexcept { return resource_ != nullptr; }

    // The source retains its reference until the derived cleanup lease exists.
    // Moving a lease to a job/completion transfers without decrementing refs.
    template <LeaseKind K = Kind, std::enable_if_t<K == LeaseKind::Work, int> = 0>
    EntityLease derive_cleanup() const {
        if (!backing_) throw EntityClosed();
        return backing_->derive_work();
    }

    void reset() noexcept {
        if (!backing_) return;
        auto backing = std::move(backing_);
        resource_ = nullptr;
        backing->release(Kind);
    }

private:
    friend class EntityBacking<T>;
    EntityLease(std::shared_ptr<EntityBacking<T>> backing, T* resource) noexcept
    : backing_(std::move(backing)), resource_(resource) {}
    std::shared_ptr<EntityBacking<T>> backing_;
    T* resource_{nullptr};
};

template <typename T>
using OperationLease = EntityLease<T, LeaseKind::Operation>;
template <typename T>
using DispatchLease = EntityLease<T, LeaseKind::Dispatch>;
template <typename T>
using WorkLease = EntityLease<T, LeaseKind::Work>;

class EntityBackingBase {
public:
    virtual ~EntityBackingBase() = default;
    virtual void close() noexcept = 0;
    virtual EntityState state() const noexcept = 0;
    virtual bool wait_closed(std::optional<std::chrono::steady_clock::time_point>) = 0;
};

template <typename T>
class EntityBacking final : public EntityBackingBase,
                            public std::enable_shared_from_this<EntityBacking<T>> {
public:
    using Interrupt = void (*)(T&) noexcept;
    EntityBacking(std::shared_ptr<ContextAdmission> admission, std::unique_ptr<T> resource,
                  Interrupt interrupt = nullptr, std::shared_ptr<void> owner = {})
    : admission_(std::move(admission)), resource_(std::move(resource)), interrupt_(interrupt), owner_(std::move(owner)) {
        if (!admission_ || !resource_) throw std::invalid_argument("Invalid entity backing");
    }

    OperationLease<T> operation() { return acquire<LeaseKind::Operation>(); }
    DispatchLease<T> dispatch() { return acquire<LeaseKind::Dispatch>(); }
    WorkLease<T> work() { return acquire<LeaseKind::Work>(); }

    // Used for availability waits: token capture and OperationLease creation
    // happen under the same backing lock that close uses for interruption.
    template <typename Prepare>
    auto prepare_operation(Prepare&& prepare) {
        std::lock_guard admission_lock(admission_->mutex);
        if (!admission_->open) throw ContextShutdown();
        std::lock_guard backing_lock(mutex_);
        if (state_ != EntityState::Open) throw EntityClosed();
        auto token = prepare(*resource_);
        auto self = this->shared_from_this();
        ++refs_[index(LeaseKind::Operation)];
        return std::make_pair(OperationLease<T>(std::move(self), resource_.get()), std::move(token));
    }

    void set_registered(bool value) {
        {
            std::lock_guard lock(mutex_);
            if (value && state_ != EntityState::Open) throw EntityClosed();
            if (value && executor_registered_)
                throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::AlreadyRegistered, "Entity already belongs to a WaitSet"));
            executor_registered_ = value;
        }
        try_finalize();
    }

    bool quiescent() const noexcept {
        std::lock_guard lock(mutex_);
        return refs_ == std::array<std::size_t, 3>{};
    }

    void close() noexcept override {
        {
            std::lock_guard lock(mutex_);
            if (state_ != EntityState::Open) return;
            state_ = EntityState::Closing;
            // Only wake-only native hooks are allowed here. The resource is
            // stable, so interruption cannot race its physical destruction.
            if (interrupt_) interrupt_(*resource_);
        }
        try_finalize();
    }

    EntityState state() const noexcept override {
        std::lock_guard lock(mutex_);
        return state_;
    }

    bool wait_operations(std::optional<std::chrono::steady_clock::time_point> deadline) {
        std::unique_lock lock(mutex_);
        const auto idle = [this] { return refs_[index(LeaseKind::Operation)] == 0; };
        if (!deadline) { cv_.wait(lock, idle); return true; }
        return cv_.wait_until(lock, *deadline, idle);
    }

    bool wait_closed(std::optional<std::chrono::steady_clock::time_point> deadline) override {
        std::unique_lock lock(mutex_);
        const auto closed = [this] { return state_ == EntityState::Closed; };
        if (!deadline) {
            cv_.wait(lock, closed);
            return true;
        }
        return cv_.wait_until(lock, *deadline, closed);
    }

private:
    template <typename U, LeaseKind K>
    friend class EntityLease;
    static constexpr std::size_t index(LeaseKind kind) noexcept {
        return static_cast<std::size_t>(kind);
    }

    template <LeaseKind Kind>
    EntityLease<T, Kind> acquire() {
        std::lock_guard admission_lock(admission_->mutex);
        if (!admission_->open) throw ContextShutdown();
        std::lock_guard backing_lock(mutex_);
        if (state_ != EntityState::Open) throw EntityClosed();
        auto self = this->shared_from_this();
        ++refs_[index(Kind)];
        return EntityLease<T, Kind>(std::move(self), resource_.get());
    }

    WorkLease<T> derive_work() {
        std::lock_guard lock(mutex_);
        if (refs_[index(LeaseKind::Work)] == 0 || destroy_started_) throw EntityClosed();
        auto self = this->shared_from_this();
        ++refs_[index(LeaseKind::Work)];
        return WorkLease<T>(std::move(self), resource_.get());
    }

    void release(LeaseKind kind) noexcept {
        {
            std::lock_guard lock(mutex_);
            auto& count = refs_[index(kind)];
            if (count == 0) std::terminate();
            --count;
        }
        cv_.notify_all();
        try_finalize();
    }

    void try_finalize() noexcept {
        std::unique_ptr<T> resource;
        {
            std::lock_guard lock(mutex_);
            if (state_ != EntityState::Closing || destroy_started_ || executor_registered_ ||
                refs_ != std::array<std::size_t, 3>{})
                return;
            destroy_started_ = true;
            resource = std::move(resource_);
        }
        resource.reset();
        {
            std::lock_guard lock(mutex_);
            state_ = EntityState::Closed;
        }
        cv_.notify_all();
    }

    std::shared_ptr<ContextAdmission> admission_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    EntityState state_{EntityState::Open};
    std::unique_ptr<T> resource_;
    std::array<std::size_t, 3> refs_{};
    bool executor_registered_{false};
    bool destroy_started_{false};
    Interrupt interrupt_;
    std::shared_ptr<void> owner_;
};

}  // namespace dclpy::detail

#endif  // DCLPY_DETAIL_ENTITY_HPP_
