#ifndef DCLPY_DETAIL_CONTEXT_HPP_
#define DCLPY_DETAIL_CONTEXT_HPP_

#include <memory>
#include <algorithm>
#include <mutex>
#include <vector>

#include "dmw/context.hpp"
#include "entity.hpp"

namespace dclpy::detail {

using Deadline = std::optional<std::chrono::steady_clock::time_point>;

class NativeContext : public std::enable_shared_from_this<NativeContext> {
public:
    explicit NativeContext(const dmw::ContextOptions& options)
    : admission_(std::make_shared<ContextAdmission>()),
      backing_(std::make_shared<EntityBacking<dmw::Context>>(
          admission_, unwrap(dmw::Context::create(options)))),
      control_(backing_->work()) {}

    ~NativeContext() noexcept {
        backing_->close();
        control_.reset();
    }

    const std::shared_ptr<ContextAdmission>& admission() const noexcept { return admission_; }
    OperationLease<dmw::Context> operation() { return backing_->operation(); }

    template <typename T>
    std::shared_ptr<EntityBacking<T>> adopt(
        std::unique_ptr<T> resource, typename EntityBacking<T>::Interrupt interrupt = nullptr) {
        auto child = std::make_shared<EntityBacking<T>>(
            admission_, std::move(resource), interrupt, shared_from_this());
        bool closing;
        {
        std::lock_guard admission_lock(admission_->mutex);
        std::lock_guard children_lock(children_mutex_);
        children_.erase(std::remove_if(children_.begin(), children_.end(),
                                       [](const auto& weak) { return weak.expired(); }),
                        children_.end());
        children_.push_back(child);
        closing = !admission_->open;
        }
        if (closing) child->close();
        return child;
    }

    void stop_admission() noexcept {
        std::lock_guard lock(admission_->mutex);
        admission_->open = false;
    }

    bool is_accepting() const noexcept {
        std::lock_guard lock(admission_->mutex);
        return admission_->open;
    }

    void close_children() {
        std::vector<std::shared_ptr<EntityBackingBase>> children;
        {
            std::lock_guard lock(children_mutex_);
            children.reserve(children_.size());
            for (const auto& weak : children_) {
                if (auto child = weak.lock()) children.push_back(std::move(child));
            }
        }
        for (const auto& child : children) child->close();
    }

    // The Python coordinator calls this only after all owners have retired
    // control producers and drained their jobs/tasks/mailboxes. No user loop
    // thread blocks here. Native Context shutdown is the final native step.
    bool finish_shutdown(Deadline deadline) {
        std::lock_guard shutdown_lock(shutdown_mutex_);
        if (backing_->state() == EntityState::Closed) return true;
        if (is_accepting()) throw std::logic_error("Shutdown admission has not been closed");
        if (!backing_->wait_operations(deadline)) return false;
        std::vector<std::shared_ptr<EntityBackingBase>> children;
        {
            std::lock_guard lock(children_mutex_);
            children.reserve(children_.size());
            for (const auto& weak : children_) {
                if (auto child = weak.lock()) children.push_back(std::move(child));
            }
        }
        for (const auto& child : children) {
            child->close();
            if (!child->wait_closed(deadline)) return false;
        }
        unwrap(control_->shutdown());
        backing_->close();
        control_.reset();
        return backing_->wait_closed(deadline);
    }

private:
    std::shared_ptr<ContextAdmission> admission_;
    std::shared_ptr<EntityBacking<dmw::Context>> backing_;
    WorkLease<dmw::Context> control_;
    std::mutex children_mutex_;
    std::vector<std::weak_ptr<EntityBackingBase>> children_;
    std::mutex shutdown_mutex_;
};

class NativeNode {
public:
    NativeNode(std::shared_ptr<NativeContext> context, const dmw::NodeOptions& options)
    : context_(std::move(context)),
      backing_(context_->adopt(unwrap(context_->operation()->create_node(options)))) {}
    const std::shared_ptr<EntityBacking<dmw::Node>>& backing() const noexcept { return backing_; }
    const std::shared_ptr<NativeContext>& context() const noexcept { return context_; }
private:
    std::shared_ptr<NativeContext> context_;
    std::shared_ptr<EntityBacking<dmw::Node>> backing_;
};

class NativeClock {
public:
    NativeClock(std::shared_ptr<NativeContext> context, dmw::ClockType type)
    : context_(std::move(context)),
      backing_(context_->adopt(unwrap(context_->operation()->create_clock(type)))) {}
    const std::shared_ptr<EntityBacking<dmw::Clock>>& backing() const noexcept { return backing_; }
    const std::shared_ptr<NativeContext>& context() const noexcept { return context_; }
private:
    std::shared_ptr<NativeContext> context_;
    std::shared_ptr<EntityBacking<dmw::Clock>> backing_;
};

}  // namespace dclpy::detail

#endif  // DCLPY_DETAIL_CONTEXT_HPP_
