// SPDX-License-Identifier: Apache-2.0

#include "dmw/event.hpp"

#include <memory>
#include <utility>

#include "dmw/error.hpp"
#include "impl/event_impl.hpp"

namespace dmw {

namespace {

EventInfo difference(const EventInfo& current, const EventInfo& cursor) {
    if (const auto* value = std::get_if<DeadlineMissedInfo>(&current)) {
        const auto& previous = std::get<DeadlineMissedInfo>(cursor);
        return DeadlineMissedInfo{
            value->total_count, value->total_count_change - previous.total_count_change};
    }
    if (const auto* value = std::get_if<LivelinessLostInfo>(&current)) {
        const auto& previous = std::get<LivelinessLostInfo>(cursor);
        return LivelinessLostInfo{
            value->total_count, value->total_count_change - previous.total_count_change};
    }
    if (const auto* value = std::get_if<LivelinessChangedInfo>(&current)) {
        const auto& previous = std::get<LivelinessChangedInfo>(cursor);
        return LivelinessChangedInfo{
            value->alive_count, value->not_alive_count,
            value->alive_count_change - previous.alive_count_change,
            value->not_alive_count_change - previous.not_alive_count_change};
    }
    if (const auto* value = std::get_if<IncompatibleQosInfo>(&current)) {
        const auto& previous = std::get<IncompatibleQosInfo>(cursor);
        return IncompatibleQosInfo{
            value->total_count, value->total_count_change - previous.total_count_change,
            value->last_policy};
    }
    const auto& value = std::get<MessageLostInfo>(current);
    const auto& previous = std::get<MessageLostInfo>(cursor);
    return MessageLostInfo{
        value.total_count, value.total_count_change - previous.total_count_change};
}

}  // namespace

Event::Event(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Event::~Event() noexcept = default;

EventType Event::type() const noexcept { return impl_->type_; }

Result<TakeStatus> Event::take(EventInfo& info) {
    const auto operation = impl_->parent_->context_state->try_acquire_operation();
    if (!operation) {
        return Result<TakeStatus>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    if (!impl_->parent_->alive.load(std::memory_order_acquire)) {
        return Result<TakeStatus>::failure(
            Error(ErrorCode::ParentDestroyed, "Event parent is destroyed"));
    }
    const auto current = impl_->parent_->snapshot(impl_->type_);
    if (current.generation == impl_->cursor_.generation) {
        return Result<TakeStatus>::success(TakeStatus::NoData);
    }
    info = difference(current.info, impl_->cursor_.info);
    impl_->cursor_ = current;
    impl_->wait_state_->pending.store(false, std::memory_order_release);
    return Result<TakeStatus>::success(TakeStatus::Taken);
}

}  // namespace dmw
