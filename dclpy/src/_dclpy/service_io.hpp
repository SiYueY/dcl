#ifndef DCLPY_DETAIL_SERVICE_IO_HPP_
#define DCLPY_DETAIL_SERVICE_IO_HPP_

#include "native_io.hpp"
#include <cstdio>
#include "service.hpp"

namespace dclpy::detail {

class ClientRequestWork {
public:
    ClientRequestWork(NativeClient& client, const OwnedSample& sample)
    : lease_(client.backing()->work()), sample_(sample.clone()) {
        if (sample.binding()->binding != client.type()->request->binding)
            throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::TypeMismatch, "Foreign service request descriptor"));
    }
    void set_ticket(const std::shared_ptr<NativeIoDispatcher>& dispatcher, std::size_t ticket) {
        std::lock_guard lock(mutex_);
        dispatcher_ = dispatcher;
        ticket_ = ticket;
    }
    void cancel() {
        cancel_requested_.store(true, std::memory_order_release);
        std::lock_guard lock(mutex_);
        if (auto dispatcher = dispatcher_.lock()) {
            if (ticket_) (void)dispatcher->cancel_queued(ticket_);
        }
    }
    bool send_started() const noexcept { return send_started_.load(std::memory_order_acquire); }
    void send(CompletionRecord& completion) {
        if (cancel_requested_.load(std::memory_order_acquire)) {
            completion.skipped = true;
            return;
        }
        send_started_.store(true, std::memory_order_release);
        completion.request_id = unwrap(lease_->write_request(sample_.get()));
    }
private:
    WorkLease<dmw::Client> lease_;
    OwnedSample sample_;
    std::atomic<bool> send_started_{false}, cancel_requested_{false};
    std::mutex mutex_;
    std::weak_ptr<NativeIoDispatcher> dispatcher_;
    std::size_t ticket_{0};
};

class ServiceSendJob final : public NativeJob {
public:
    explicit ServiceSendJob(std::shared_ptr<ClientRequestWork> work) : work_(std::move(work)) {}
    void run(CompletionRecord& record) override { work_->send(record); }
private:
    std::shared_ptr<ClientRequestWork> work_;
};


class ServiceDeliveryJob final : public NativeJob {
public:
    ServiceDeliveryJob(WorkLease<dmw::Server> lease, std::shared_ptr<const ServiceBindingHandle> type)
    : lease_(std::move(lease)), type_(std::move(type)) {}
    bool belongs_to(const EntityBacking<dmw::Server>* backing) const noexcept { return lease_.belongs_to(backing); }
    void set_request(dmw::RequestId id) noexcept { request_id_ = id; }
    void response(const OwnedSample& sample) {
        if (sample.binding()->binding != type_->response->binding)
            throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::TypeMismatch, "Foreign service response descriptor"));
        response_.emplace(sample.clone());
        discard_ = false;
    }
    void discard() noexcept { discard_ = true; }
    void run(CompletionRecord& record) override {
        cleanup_pending_ = false;
        if (discard_) {
            discard_request(record);
            if (failure_) record = *failure_;
        }
        else unwrap(lease_->write_response(request_id_, response_->get()));
    }
    unsigned max_attempts() const noexcept override { return 3; }
    bool retryable(const CompletionRecord& record) const noexcept override {
        return discard_ ? cleanup_pending_ : (record.error_code == dmw::ErrorCode::DDSError ||
                                              record.error_code == dmw::ErrorCode::Busy);
    }
    bool cleanup_pending() const noexcept override { return cleanup_pending_; }
    void after_failed_delivery(CompletionRecord& record) override {
        if (!discard_) failure_ = record;
        discard_ = true;
        discard_request(record);
    }
private:
    void discard_request(CompletionRecord& record) {
        auto result = lease_->discard_request(request_id_);
        if (result || result.error().code() == dmw::ErrorCode::NotFound) return;
        cleanup_pending_ = result.error().code() == dmw::ErrorCode::Busy;
        record.cleanup_failed = !cleanup_pending_;
        unwrap(std::move(result));
    }
    WorkLease<dmw::Server> lease_;
    std::shared_ptr<const ServiceBindingHandle> type_;
    dmw::RequestId request_id_;
    std::optional<OwnedSample> response_;
    std::optional<CompletionRecord> failure_;
    bool discard_{true}, cleanup_pending_{false};
};

class ServiceRequestWork {
public:
    ServiceRequestWork(std::shared_ptr<NativeIoDispatcher> dispatcher, std::shared_ptr<CompletionPort> port,
                       CompletionRecord record, NativeService& service)
    : dispatcher_(std::move(dispatcher)), job_(std::make_shared<ServiceDeliveryJob>(
          service.backing()->work(), service.type())) {
        record.kind = NativeJobKind::ServiceResponse;
        ticket_ = dispatcher_->reserve(port, record, false);
        try { dispatcher_->install(ticket_, job_); }
        catch (...) { dispatcher_->abandon(ticket_); throw; }
    }
    ~ServiceRequestWork() noexcept {
        try { finish_task(); } catch (...) {
            // The durable reserved slot still owns its lease. A failed cleanup
            // cannot force resource destruction or silently retire the ticket.
            std::fputs("DCLPY service cleanup failed; retaining its native ticket and lease\n", stderr);
        }
    }
    std::size_t ticket() const noexcept { return ticket_; }
    void validate_take(NativeService& service) {
        std::lock_guard lock(mutex_);
        if (accepted_ || submitted_ || !job_)
            throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::InvalidState, "Service work already consumed its request"));
        if (!job_->belongs_to(service.backing().get()))
            throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::TypeMismatch, "Service work belongs to another server"));
    }
    void accepted(dmw::RequestId id) noexcept {
        job_->set_request(id);
        accepted_ = true;
    }
    void respond(const OwnedSample& sample) {
        std::lock_guard lock(mutex_);
        if (submitted_ || !accepted_)
            throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::InvalidState, "Service work is not awaiting its response"));
        job_->response(sample);
        dispatcher_->enqueue_prepared(ticket_);
        submitted_ = true;
    }
    void discard() {
        std::lock_guard lock(mutex_);
        submit_discard();
    }
    void finish_task() {
        std::lock_guard lock(mutex_);
        if (!job_) return;
        if (!accepted_) {
            dispatcher_->abandon(ticket_);
            submitted_ = true;
        } else submit_discard();
        job_.reset();
    }
private:
    void submit_discard() {
        if (submitted_) return;
        if (!accepted_)
            throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::InvalidState, "Service request has not been taken"));
        job_->discard();
        dispatcher_->enqueue_prepared(ticket_);
        submitted_ = true;
    }
    std::shared_ptr<NativeIoDispatcher> dispatcher_;
    std::shared_ptr<ServiceDeliveryJob> job_;
    std::size_t ticket_{0};
    std::mutex mutex_;
    bool accepted_{false}, submitted_{false};
};

}  // namespace dclpy::detail
#endif  // DCLPY_DETAIL_SERVICE_IO_HPP_
