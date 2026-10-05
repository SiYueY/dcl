#include "native_io.hpp"
#include "dclpy/diagnostic.hpp"

namespace dclpy::detail {
namespace {
[[noreturn]] void failure(dmw::ErrorCode code, const char* message) {
    throw MiddlewareFailure(dmw::Error(code, message));
}

template<class Function>
void execute_step(CompletionRecord& record, Function&& function) noexcept {
    try { function(); }
    catch (const MiddlewareFailure& error) {
        record.failed = true;
        record.error_code = error.code;
        copy_diagnostic(record.diagnostic, error.what());
    } catch (const std::bad_alloc&) {
        record.failed = true;
        record.error_code = dmw::ErrorCode::ResourceExhausted;
        copy_diagnostic(record.diagnostic, "Native I/O allocation failed");
    } catch (const std::exception& error) {
        record.failed = true;
        record.error_code = dmw::ErrorCode::DDSError;
        copy_diagnostic(record.diagnostic, error.what());
    } catch (...) {
        record.failed = true;
        record.error_code = dmw::ErrorCode::DDSError;
        copy_diagnostic(record.diagnostic, "Unclassified native I/O exception");
    }
}

}

void CompletionPort::notify() noexcept {
    try { (void)wake_->trigger(); } catch (...) {
        // Completion remains durable in this mailbox even if its notification
        // fails. The owner's bounded control polling still sees the record.
    }
}

NativeIoDispatcher::NativeIoDispatcher(std::shared_ptr<NativeContext> context)
: context_(std::move(context)) {
    if (!context_) failure(dmw::ErrorCode::InvalidArgument, "Missing dispatcher Context");
    auto admission = context_->operation();
    worker_ = std::thread([this] { worker(); });
}
NativeIoDispatcher::~NativeIoDispatcher() noexcept {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void NativeIoDispatcher::validate_route(const std::shared_ptr<CompletionPort>& port) {
    if (!port) failure(dmw::ErrorCode::InvalidArgument, "Missing completion route");
    auto found = routes_.find(port->id());
    if (found == routes_.end() || found->second != port)
        failure(dmw::ErrorCode::NotRegistered, "Completion route belongs to another dispatcher or has retired");
}

std::shared_ptr<CompletionPort> NativeIoDispatcher::open_route(const std::shared_ptr<NativeWaitSet>& wait_set) {
    if (!wait_set) failure(dmw::ErrorCode::InvalidArgument, "Missing owner WaitSet");
    if (wait_set->context() != context_)
        failure(dmw::ErrorCode::InvalidArgument, "Completion route belongs to a foreign Context");
    auto lease = wait_set->control_wake_lease();
    std::lock_guard lock(mutex_);
    if (stopping_) failure(dmw::ErrorCode::InvalidState, "Native dispatcher is stopping");
    if (next_owner_ == std::numeric_limits<std::uint64_t>::max())
        failure(dmw::ErrorCode::ResourceExhausted, "Executor routing identities exhausted");
    auto port = std::make_shared<CompletionPort>(next_owner_++, std::move(lease));
    routes_.emplace(port->id(), port);
    return port;
}

void NativeIoDispatcher::stop_route(const std::shared_ptr<CompletionPort>& port) {
    std::lock_guard lock(mutex_);
    validate_route(port);
    port->accepting_ = false;
}

void NativeIoDispatcher::retire_route(const std::shared_ptr<CompletionPort>& port) {
    WorkLease<dmw::GuardCondition> lease;
    {
        std::lock_guard lock(mutex_);
        validate_route(port);
        if (port->outstanding_) failure(dmw::ErrorCode::Busy, "Completion route still has accepted producers");
        std::lock_guard port_lock(port->mutex_);
        if (port->count_) failure(dmw::ErrorCode::Busy, "Completion route still contains notifications");
        port->accepting_ = false;
        routes_.erase(port->id());
        lease = std::move(port->wake_);
    }
    // Last control references may finalize DDS resources: never destroy them
    // while the dispatcher admission or mailbox mutex is held.
}

std::size_t NativeIoDispatcher::reserve(const std::shared_ptr<CompletionPort>& port,
                                       CompletionRecord record, bool control) {
    std::lock_guard lock(mutex_);
    validate_route(port);
    if (control && record.kind != NativeJobKind::GoalResponse && record.kind != NativeJobKind::CancelResponse)
        failure(dmw::ErrorCode::InvalidArgument, "Only Goal and Cancel transactions may use reserved control capacity");
    if (stopping_ || (!control && !port->accepting_))
        failure(dmw::ErrorCode::InvalidState, "Dispatcher route rejects new user work");
    if (used_ == slots_.size() || (!control && ordinary_used_ == 224))
        failure(dmw::ErrorCode::ResourceExhausted, "Native I/O admission capacity exhausted");
    if (next_ticket_ == std::numeric_limits<std::size_t>::max())
        failure(dmw::ErrorCode::ResourceExhausted, "Native I/O ticket identities exhausted");
    for (auto& slot : slots_) {
        if (slot.state != SlotState::Free) continue;
        slot.ticket = next_ticket_++;
        slot.state = SlotState::Reserved;
        slot.control = control;
        slot.priority = record.kind == NativeJobKind::ServiceResponse ||
                        record.kind == NativeJobKind::DiscardRequest ||
                        record.kind == NativeJobKind::GoalResponse ||
                        record.kind == NativeJobKind::CancelResponse ||
                        record.kind == NativeJobKind::ResultResponse;
        slot.retries = 0;
        record.executor_id = port->id();
        slot.completion = record;
        slot.port = port;
        ++used_;
        if (!control) ++ordinary_used_;
        ++port->outstanding_;
        return slot.ticket;
    }
    failure(dmw::ErrorCode::InvalidState, "Native I/O capacity accounting is inconsistent");
}

std::size_t NativeIoDispatcher::locate(std::size_t ticket) {
    for (std::size_t index = 0; index < slots_.size(); ++index)
        if (slots_[index].state != SlotState::Free && slots_[index].ticket == ticket) return index;
    failure(dmw::ErrorCode::NotFound, "Native I/O ticket has retired");
}

void NativeIoDispatcher::push(Queue& queue, std::size_t index) noexcept {
    slots_[index].next = slots_.size();
    if (queue.tail == slots_.size()) queue.head = index;
    else slots_[queue.tail].next = index;
    queue.tail = index;
}
std::size_t NativeIoDispatcher::pop(Queue& queue) noexcept {
    auto index = queue.head;
    queue.head = slots_[index].next;
    if (queue.head == slots_.size()) queue.tail = slots_.size();
    return index;
}

void NativeIoDispatcher::install(std::size_t ticket, std::shared_ptr<NativeJob> job) {
    if (!job) failure(dmw::ErrorCode::InvalidArgument, "Missing native job");
    std::lock_guard lock(mutex_);
    auto& slot = slots_[locate(ticket)];
    if (slot.state != SlotState::Reserved || slot.job)
        failure(dmw::ErrorCode::InvalidState, "Native ticket already has its prepared job");
    slot.job = std::move(job);
}

void NativeIoDispatcher::enqueue_prepared(std::size_t ticket) {
    {
        std::lock_guard lock(mutex_);
        auto index = locate(ticket);
        auto& slot = slots_[index];
        if (slot.state != SlotState::Reserved || !slot.job)
            failure(dmw::ErrorCode::InvalidState, "Native ticket has no prepared job or was already enqueued");
        slot.state = SlotState::Queued;
        push(slot.priority ? control_queue_ : user_queue_, index);
    }
    wake_.notify_one();
}

void NativeIoDispatcher::enqueue(std::size_t ticket, std::shared_ptr<NativeJob> job) {
    install(ticket, std::move(job));
    enqueue_prepared(ticket);
}

bool NativeIoDispatcher::cancel_queued(std::size_t ticket) {
    std::lock_guard lock(mutex_);
    auto index = slots_.size();
    for (std::size_t candidate = 0; candidate < slots_.size(); ++candidate)
        if (slots_[candidate].state != SlotState::Free && slots_[candidate].ticket == ticket) {
            index = candidate;
            break;
        }
    if (index == slots_.size()) return false;
    auto& slot = slots_[index];
    if (slot.state != SlotState::Queued) return false;
    auto& queue = slot.priority ? control_queue_ : user_queue_;
    std::size_t previous = slots_.size(), current = queue.head;
    while (current != index) {
        if (current == slots_.size())
            failure(dmw::ErrorCode::InvalidState, "Queued native ticket is missing from its ready list");
        previous = current;
        current = slots_[current].next;
    }
    if (previous == slots_.size()) queue.head = slot.next;
    else slots_[previous].next = slot.next;
    if (queue.tail == index) queue.tail = previous;
    slot.completion.skipped = true;
    slot.state = SlotState::Completed;
    std::lock_guard port_lock(slot.port->mutex_);
    auto tail = (slot.port->head_ + slot.port->count_) % 256;
    slot.port->completions_[tail] = index;
    ++slot.port->count_;
    slot.port->notify();
    return true;
}

std::shared_ptr<NativeJob> NativeIoDispatcher::release_slot(std::size_t index) {
    auto& slot = slots_[index];
    auto job = std::move(slot.job);
    --slot.port->outstanding_;
    --used_;
    if (!slot.control) --ordinary_used_;
    slot.port.reset();
    slot.state = SlotState::Free;
    for (const auto& route : routes_) route.second->notify();
    return job;
}

void NativeIoDispatcher::abandon(std::size_t ticket) {
    std::shared_ptr<NativeJob> job;
    {
        std::lock_guard lock(mutex_);
        auto index = locate(ticket);
        if (slots_[index].state != SlotState::Reserved)
            failure(dmw::ErrorCode::Busy, "An accepted job must retire through its completion owner");
        job = release_slot(index);
    }
}

void NativeIoDispatcher::worker() noexcept {
    while (true) {
        std::size_t index;
        {
            std::unique_lock lock(mutex_);
            while (true) {
                const auto now = std::chrono::steady_clock::now();
                std::optional<std::chrono::steady_clock::time_point> earliest;
                for (std::size_t delayed = 0; delayed < slots_.size(); ++delayed) {
                    auto& candidate = slots_[delayed];
                    if (candidate.state != SlotState::RetryWaiting) continue;
                    if (candidate.retry_due <= now) {
                        candidate.state = SlotState::Queued;
                        push(candidate.priority ? control_queue_ : user_queue_, delayed);
                    } else if (!earliest || candidate.retry_due < *earliest) {
                        earliest = candidate.retry_due;
                    }
                }
                if (control_queue_.head != slots_.size()) {
                    index = pop(control_queue_);
                    break;
                }
                if (user_queue_.head != slots_.size()) {
                    index = pop(user_queue_);
                    break;
                }
                if (stopping_ && !earliest) return;
                // A dispatcher-owned steady deadline progresses retries even
                // with every DDS user channel paused and no network traffic.
                if (earliest) wake_.wait_until(lock, *earliest);
                else wake_.wait(lock);
            }
            slots_[index].state = SlotState::Running;
        }
        auto& slot = slots_[index];
        slot.completion.failed = false;
        slot.completion.diagnostic[0] = '\0';
        execute_step(slot.completion, [&] { slot.job->run(slot.completion); });
        if (slot.completion.failed && slot.completion.error_code != dmw::ErrorCode::ProtocolFault &&
            slot.job->retryable(slot.completion) && slot.retries < 3 &&
            slot.retries + 1 < slot.job->max_attempts()) {
            std::lock_guard lock(mutex_);
            slot.retry_due = std::chrono::steady_clock::now() + std::chrono::milliseconds(10U << slot.retries);
            ++slot.retries;
            slot.state = SlotState::RetryWaiting;
            continue;
        }
        if (slot.completion.failed)
            execute_step(slot.completion, [&] { slot.job->after_failed_delivery(slot.completion); });
        if (slot.job->cleanup_pending()) {
            std::lock_guard lock(mutex_);
            slot.retry_due = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
            slot.state = SlotState::RetryWaiting;
            continue;
        }
        {
            std::lock_guard lock(mutex_);
            slot.state = SlotState::Completed;
        }
        {
            std::lock_guard lock(slot.port->mutex_);
            // At most 256 outstanding tickets exist globally, so this ring
            // cannot overflow. Publishing never allocates, including errors.
            auto tail = (slot.port->head_ + slot.port->count_) % 256;
            slot.port->completions_[tail] = index;
            ++slot.port->count_;
            slot.port->notify();
        }
    }
}

std::optional<std::pair<std::size_t, CompletionRecord>> NativeIoDispatcher::peek(
    const std::shared_ptr<CompletionPort>& port) {
    std::lock_guard lock(mutex_);
    validate_route(port);
    std::lock_guard port_lock(port->mutex_);
    if (!port->count_) return std::nullopt;
    const auto& slot = slots_[port->completions_[port->head_]];
    return std::make_pair(slot.ticket, slot.completion);
}

void NativeIoDispatcher::acknowledge(const std::shared_ptr<CompletionPort>& port, std::size_t ticket) {
    std::shared_ptr<NativeJob> job;
    {
        std::lock_guard lock(mutex_);
        validate_route(port);
        auto index = locate(ticket);
        std::lock_guard port_lock(port->mutex_);
        if (!port->count_ || port->completions_[port->head_] != index || slots_[index].port != port)
            failure(dmw::ErrorCode::InvalidState, "Completion ACK does not match the owner mailbox head");
        port->head_ = (port->head_ + 1) % 256;
        --port->count_;
        job = release_slot(index);
    }
}
std::size_t NativeIoDispatcher::outstanding(const std::shared_ptr<CompletionPort>& port) {
    std::lock_guard lock(mutex_);
    validate_route(port);
    return port->outstanding_;
}
void NativeIoDispatcher::shutdown() {
    std::lock_guard shutdown_lock(shutdown_mutex_);
    {
        std::lock_guard lock(mutex_);
        if (used_ || !routes_.empty()) failure(dmw::ErrorCode::Busy, "Native I/O routes or tickets have not retired");
        stopping_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

}  // namespace dclpy::detail
