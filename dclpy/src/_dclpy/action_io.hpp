#ifndef DCLPY_DETAIL_ACTION_IO_HPP_
#define DCLPY_DETAIL_ACTION_IO_HPP_

#include <pybind11/pybind11.h>

#include "action.hpp"
#include "native_io.hpp"

namespace dclpy::detail {

enum class ActionRequestKind { Goal, Cancel, Result };

class ActionRequestWork {
public:
  ActionRequestWork(NativeActionClient &client, const OwnedSample &sample,
                    ActionRequestKind kind)
      : lease_(client.backing()->work()), sample_(sample.clone()), kind_(kind) {
    const auto &expected = kind == ActionRequestKind::Goal
                               ? client.type()->send_goal->request
                           : kind == ActionRequestKind::Cancel
                               ? client.type()->cancel_goal->request
                               : client.type()->get_result->request;
    if (sample.binding()->binding != expected->binding)
      throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::TypeMismatch,
                                         "Foreign Action request descriptor"));
  }
  void set_ticket(const std::shared_ptr<NativeIoDispatcher> &dispatcher,
                  std::size_t ticket) {
    std::lock_guard lock(mutex_);
    dispatcher_ = dispatcher;
    ticket_ = ticket;
  }
  void cancel() {
    cancel_requested_.store(true, std::memory_order_release);
    std::lock_guard lock(mutex_);
    if (const auto dispatcher = dispatcher_.lock(); dispatcher && ticket_)
      (void)dispatcher->cancel_queued(ticket_);
  }
  bool send_started() const noexcept {
    return send_started_.load(std::memory_order_acquire);
  }
  void send(CompletionRecord &completion) {
    if (cancel_requested_.load(std::memory_order_acquire)) {
      completion.skipped = true;
      return;
    }
    send_started_.store(true, std::memory_order_release);
    switch (kind_) {
    case ActionRequestKind::Goal:
      completion.request_id = unwrap(lease_->write_goal_request(sample_.get()));
      break;
    case ActionRequestKind::Cancel:
      completion.request_id =
          unwrap(lease_->write_cancel_request(sample_.get()));
      break;
    case ActionRequestKind::Result:
      completion.request_id =
          unwrap(lease_->write_result_request(sample_.get()));
      break;
    }
  }

private:
  WorkLease<dmw::ActionClient> lease_;
  OwnedSample sample_;
  ActionRequestKind kind_;
  std::atomic<bool> send_started_{false}, cancel_requested_{false};
  std::mutex mutex_;
  std::weak_ptr<NativeIoDispatcher> dispatcher_;
  std::size_t ticket_{0};
};

class ActionSendJob final : public NativeJob {
public:
  explicit ActionSendJob(std::shared_ptr<ActionRequestWork> work)
      : work_(std::move(work)) {}
  void run(CompletionRecord &record) override { work_->send(record); }

private:
  std::shared_ptr<ActionRequestWork> work_;
};

enum class ActionServerRequestKind { Goal, Cancel, Result };

class ActionResultPayload {
public:
  ActionResultPayload(std::shared_ptr<const ActionBindingHandle> type,
                      pybind11::handle response)
      : sample_(OwnedSample::freeze(type->get_result->response, response)) {}
  const OwnedSample &sample() const noexcept { return sample_; }
private:
  OwnedSample sample_;
};

class ActionServerDeliveryJob final : public NativeJob {
public:
  ActionServerDeliveryJob(WorkLease<dmw::ActionServer> lease,
                          std::shared_ptr<const ActionBindingHandle> type,
                          ActionServerRequestKind kind)
      : lease_(std::move(lease)), type_(std::move(type)), kind_(kind) {}

  bool belongs_to(const EntityBacking<dmw::ActionServer> *backing) const noexcept {
    return lease_.belongs_to(backing);
  }
  void set_request(dmw::RequestId value) noexcept { request_id_ = value; }
  const dmw::RequestId &request_id() const noexcept { return request_id_; }
  void response(const OwnedSample &sample) {
    const auto &expected = kind_ == ActionServerRequestKind::Goal
                               ? type_->send_goal->response
                           : kind_ == ActionServerRequestKind::Cancel
                               ? type_->cancel_goal->response
                               : type_->get_result->response;
    if (sample.binding()->binding != expected->binding)
      throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::TypeMismatch,
                                         "Foreign Action response descriptor"));
    response_.emplace(sample.clone());
    accepted_goal_.reset();
    discard_ = false;
  }
  void accept(const OwnedSample &sample, dmw::GoalInfo info, dmw::GoalAcceptMode mode) {
    if (kind_ != ActionServerRequestKind::Goal)
      throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::InvalidState,
                                         "Only Goal requests may be accepted"));
    if (sample.binding()->binding != type_->send_goal->response->binding)
      throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::TypeMismatch,
                                         "Foreign Action Goal response descriptor"));
    response_.emplace(sample.clone());
    accepted_goal_.emplace(std::move(info), mode);
    discard_ = false;
  }
  void discard() noexcept { discard_ = true; }
  void run(CompletionRecord &record) override {
    cleanup_pending_ = false;
    if (discard_) {
      discard_request(record);
      if (failure_) record = *failure_;
      return;
    }
    if (!response_)
      throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::InvalidState,
                                         "Action request has no response payload"));
    if (accepted_goal_) {
      unwrap(lease_->accept_goal(request_id_, accepted_goal_->first, response_->get(),
                                 accepted_goal_->second));
      return;
    }
    switch (kind_) {
    case ActionServerRequestKind::Goal:
      unwrap(lease_->write_goal_response(request_id_, response_->get()));
      break;
    case ActionServerRequestKind::Cancel:
      unwrap(lease_->write_cancel_response(request_id_, response_->get()));
      break;
    case ActionServerRequestKind::Result:
      unwrap(lease_->write_result_response(request_id_, response_->get()));
      break;
    }
  }
  unsigned max_attempts() const noexcept override { return 3; }
  bool retryable(const CompletionRecord &record) const noexcept override {
    return discard_ ? cleanup_pending_ :
        (record.error_code == dmw::ErrorCode::DDSError || record.error_code == dmw::ErrorCode::Busy);
  }
  bool cleanup_pending() const noexcept override { return cleanup_pending_; }
  void after_failed_delivery(CompletionRecord &record) override {
    if (!discard_) failure_ = record;
    discard_ = true;
    discard_request(record);
  }

private:
  void discard_request(CompletionRecord &record) {
    dmw::Result<void> result = [&] {
      switch (kind_) {
      case ActionServerRequestKind::Goal: return lease_->discard_goal_request(request_id_);
      case ActionServerRequestKind::Cancel: return lease_->discard_cancel_request(request_id_);
      case ActionServerRequestKind::Result: return lease_->discard_result_request(request_id_);
      }
      std::terminate();
    }();
    if (result || result.error().code() == dmw::ErrorCode::NotFound) return;
    cleanup_pending_ = result.error().code() == dmw::ErrorCode::Busy;
    record.cleanup_failed = !cleanup_pending_;
    unwrap(std::move(result));
  }
  WorkLease<dmw::ActionServer> lease_;
  std::shared_ptr<const ActionBindingHandle> type_;
  ActionServerRequestKind kind_;
  dmw::RequestId request_id_;
  std::optional<OwnedSample> response_;
  std::optional<std::pair<dmw::GoalInfo, dmw::GoalAcceptMode>> accepted_goal_;
  std::optional<CompletionRecord> failure_;
  bool discard_{true}, cleanup_pending_{false};
};

class ActionServerRequestWork {
public:
  ActionServerRequestWork(std::shared_ptr<NativeIoDispatcher> dispatcher,
                          std::shared_ptr<CompletionPort> port,
                          CompletionRecord record, NativeActionServer &server,
                          ActionServerRequestKind kind)
      : dispatcher_(std::move(dispatcher)), kind_(kind),
        job_(std::make_shared<ActionServerDeliveryJob>(server.backing()->work(),
                                                       server.type(), kind)) {
    record.kind = kind == ActionServerRequestKind::Goal ? NativeJobKind::GoalResponse :
                  kind == ActionServerRequestKind::Cancel ? NativeJobKind::CancelResponse :
                                                            NativeJobKind::ResultResponse;
    ticket_ = dispatcher_->reserve(port, record, kind != ActionServerRequestKind::Result);
    try { dispatcher_->install(ticket_, job_); }
    catch (...) { dispatcher_->abandon(ticket_); throw; }
  }
  ~ActionServerRequestWork() noexcept {
    try { finish_task(); } catch (...) {}
  }
  std::size_t ticket() const noexcept { return ticket_; }
  ActionServerRequestKind kind() const noexcept { return kind_; }
  const dmw::RequestId &request_id() const {
    if (!accepted_)
      throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::InvalidState,
                                         "Action request has not been taken"));
    return job_->request_id();
  }
  void validate_take(NativeActionServer &server) {
    std::lock_guard lock(mutex_);
    if (accepted_ || submitted_ || !job_)
      throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::InvalidState,
                                         "Action request work was already consumed"));
    if (!job_->belongs_to(server.backing().get()))
      throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::TypeMismatch,
                                         "Action request work belongs to another server"));
  }
  void accepted(dmw::RequestId id) noexcept { job_->set_request(id); accepted_ = true; }
  void respond(const OwnedSample &sample) {
    std::lock_guard lock(mutex_);
    require_accepted();
    job_->response(sample);
    dispatcher_->enqueue_prepared(ticket_);
    submitted_ = true;
  }
  void respond_payload(const ActionResultPayload &payload) {
    if (kind_ != ActionServerRequestKind::Result)
      throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::InvalidState,
                                         "Only GetResult accepts a ResultPayload"));
    respond(payload.sample());
  }
  void accept(const OwnedSample &sample, dmw::GoalInfo info, dmw::GoalAcceptMode mode) {
    std::lock_guard lock(mutex_);
    require_accepted();
    job_->accept(sample, std::move(info), mode);
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
    if (!accepted_) dispatcher_->abandon(ticket_);
    else submit_discard();
    job_.reset();
  }
private:
  void require_accepted() const {
    if (submitted_ || !accepted_)
      throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::InvalidState,
                                         "Action request is not awaiting a response"));
  }
  void submit_discard() {
    if (submitted_) return;
    if (!accepted_)
      throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::InvalidState,
                                         "Action request has not been taken"));
    job_->discard();
    dispatcher_->enqueue_prepared(ticket_);
    submitted_ = true;
  }
  std::shared_ptr<NativeIoDispatcher> dispatcher_;
  ActionServerRequestKind kind_;
  std::shared_ptr<ActionServerDeliveryJob> job_;
  std::size_t ticket_{0};
  std::mutex mutex_;
  bool accepted_{false}, submitted_{false};
};

class ActionPublishJob final : public NativeJob {
public:
  ActionPublishJob(WorkLease<dmw::ActionServer> lease,
                   std::shared_ptr<const MessageBindingHandle> type,
                   const OwnedSample &sample, bool feedback)
      : lease_(std::move(lease)), type_(std::move(type)), sample_(sample.clone()),
        feedback_(feedback) {
    if (sample.binding()->binding != type_->binding)
      throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::TypeMismatch,
                                         "Foreign Action publication descriptor"));
  }
  void run(CompletionRecord &) override {
    if (feedback_) unwrap(lease_->publish_feedback(sample_.get()));
    else unwrap(lease_->publish_status(sample_.get()));
  }
  unsigned max_attempts() const noexcept override { return 3; }
  bool retryable(const CompletionRecord &record) const noexcept override {
    return record.error_code == dmw::ErrorCode::DDSError ||
           record.error_code == dmw::ErrorCode::Busy;
  }
private:
  WorkLease<dmw::ActionServer> lease_;
  std::shared_ptr<const MessageBindingHandle> type_;
  OwnedSample sample_;
  bool feedback_;
};

} // namespace dclpy::detail

#endif // DCLPY_DETAIL_ACTION_IO_HPP_
