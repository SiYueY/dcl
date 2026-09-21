#ifndef DMW_IMPL__GRAPH_IMPL_HPP_
#define DMW_IMPL__GRAPH_IMPL_HPP_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

#include <fastdds/rtps/common/Guid.h>

#include "dmw/graph.hpp"
#include "dmw/graph_event.hpp"
#include "impl/context.hpp"
#include "impl/discovery_graph.hpp"
#include "impl/lock_rank.hpp"

namespace dmw {

namespace impl {

struct Registration;


/// Level-triggered graph revision cursor behind one public GraphEvent.
class GraphEventState : public std::enable_shared_from_this<GraphEventState> {
public:
    explicit GraphEventState(std::shared_ptr<impl::Context> context) noexcept
    : context_(std::move(context)) {}

    ~GraphEventState() noexcept {
        if (subscription_) subscription_.close_and_drain();
    }

    /// Cursor starts at the creation-time revision, so pre-creation history is
    /// never replayed.
    void initialize() {
        cursor_ = context_->discovery_graph()->revision();
        const std::weak_ptr<GraphEventState> weak = weak_from_this();
        subscription_ = context_->discovery_graph()->subscribe([weak](std::uint64_t) {
            if (const auto state = weak.lock()) state->notify_wait_set_noexcept();
        });
    }

    bool logically_ready() const noexcept {
        if (closing.load(std::memory_order_acquire) || context_->is_shutdown()) return false;
        std::lock_guard lock(state_mutex_);
        return context_->discovery_graph()->revision() > cursor_;
    }

    const std::shared_ptr<impl::Context>& context() const noexcept { return context_; }

    Result<bool> take(GraphChangeInfo& info) {
        if (closing.load(std::memory_order_acquire)) {
            return Result<bool>::failure(Error(ErrorCode::ParentDestroyed, "GraphEvent is closing"));
        }
        const auto operation = context_->try_acquire_operation();
        if (!operation) {
            return Result<bool>::failure(Error(ErrorCode::ContextShutdown, "Context is shut down"));
        }
        {
            std::lock_guard lock(state_mutex_);
            const auto current = context_->discovery_graph()->revision();
            if (current == cursor_) return Result<bool>::success(false);
            GraphChangeInfo change;
            change.previous_revision = cursor_;
            change.current_revision = current;
            cursor_ = current;
            info = change;
        }
        return Result<bool>::success(true);
    }

    Result<void> notify_wait_set() {
        std::lock_guard lock(callback_mutex);
        if (wake_callback) return wake_callback();
        return Result<void>::success();
    }

    void notify_wait_set_noexcept() noexcept {
        try {
            (void)notify_wait_set();
        } catch (...) {
        }
    }

    void detach_wait_set() noexcept {
        std::function<void()> callback;
        {
            std::lock_guard lock(callback_mutex);
            callback = detach_callback;
        }
        if (callback) callback();
    }

    void close() noexcept {
        closing.store(true, std::memory_order_release);
        detach_wait_set();
        notify_wait_set_noexcept();
    }

    std::atomic<bool> closing{false};
    std::atomic<std::uint64_t> wait_set_id{0};
    std::atomic<std::uint64_t> registration_id{0};
    impl::RankedMutex<impl::LockRank::WaitableLocal> callback_mutex;
    std::function<Result<void>()> wake_callback;
    std::function<void()> detach_callback;

private:
    friend struct impl::Registration;

    std::shared_ptr<impl::Context> context_;
    mutable std::mutex state_mutex_;
    std::uint64_t cursor_{0};
    DiscoveryGraph::Subscription subscription_;
};

}  // namespace impl

class GraphEvent::Impl {
public:
    explicit Impl(std::shared_ptr<impl::Context> context) {
        state_ = std::make_shared<impl::GraphEventState>(std::move(context));
        state_->initialize();
    }

    ~Impl() noexcept { state_->close(); }

    Result<bool> take(GraphChangeInfo& info) { return state_->take(info); }

    const std::shared_ptr<impl::GraphEventState>& wait_state() const noexcept { return state_; }

private:
    std::shared_ptr<impl::GraphEventState> state_;
};

}  // namespace dmw

#endif  // DMW_IMPL__GRAPH_IMPL_HPP_
