#include "impl/event_impl.hpp"

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

Result<bool> Event::Impl::take(EventInfo& info) {
    const auto operation = parent_->context->try_acquire_operation();
    if (!operation) {
        return Result<bool>::failure(Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    if (!parent_->alive.load(std::memory_order_acquire)) {
        return Result<bool>::failure(
            Error(ErrorCode::ParentDestroyed, "Event parent is destroyed"));
    }
    if (parent_->is_exhausted()) {
        return Result<bool>::failure(Error(
            ErrorCode::ResourceExhausted, "Event generation or registration ID is exhausted"));
    }
    const auto current = parent_->snapshot(type_);
    if (current.generation == cursor_.generation) {
        return Result<bool>::success(false);
    }
    info = difference(current.info, cursor_.info);
    cursor_ = current;
    wait_state_->clear_pending();
    return Result<bool>::success(true);
}

}  // namespace dmw
