#ifndef DCLPY_DETAIL_NATIVE_IO_HPP_
#define DCLPY_DETAIL_NATIVE_IO_HPP_

#include <array>
#include <condition_variable>
#include <deque>
#include <thread>
#include <unordered_map>

#include "topic.hpp"
#include "wait_set.hpp"

namespace dclpy::detail {

enum class NativeJobKind {
  Publish,
  ServiceRequest,
  ServiceResponse,
  DiscardRequest,
  GoalRequest,
  CancelRequest,
  ResultRequest,
  GoalResponse,
  CancelResponse,
  ResultResponse,
  Feedback,
  Status
};

struct CompletionRecord {
  std::uint64_t executor_id{0};
  std::uint64_t attachment_generation{0};
  std::uint64_t operation_id{0};
  std::uint64_t entity_id{0};
  NativeJobKind kind{NativeJobKind::Publish};
  bool failed{false};
  bool skipped{false};
  bool cleanup_failed{false};
  dmw::ErrorCode error_code{dmw::ErrorCode::DDSError};
  char diagnostic[256]{};
  std::optional<dmw::RequestId> request_id;
};

// Jobs own only C++ values and leases. Their completion storage is reserved in
// the admission slot before a reader can consume inbound protocol work.
class NativeJob {
public:
  virtual ~NativeJob() = default;
  virtual void run(CompletionRecord &) = 0;
  virtual unsigned max_attempts() const noexcept { return 1; }
  virtual bool retryable(const CompletionRecord &) const noexcept {
    return false;
  }
  virtual bool cleanup_pending() const noexcept { return false; }
  virtual void after_failed_delivery(CompletionRecord &) {}
};

class PublishJob final : public NativeJob {
public:
  PublishJob(WorkLease<dmw::Publisher> lease, OwnedSample sample)
      : lease_(std::move(lease)), sample_(std::move(sample)) {}
  void run(CompletionRecord &) override {
    unwrap(lease_->write(sample_.get()));
  }

private:
  WorkLease<dmw::Publisher> lease_;
  OwnedSample sample_;
};

class CompletionPort {
public:
  CompletionPort(std::uint64_t id, WorkLease<dmw::GuardCondition> wake)
      : id_(id), wake_(std::move(wake)) {}
  std::uint64_t id() const noexcept { return id_; }
  void notify() noexcept;

private:
  friend class NativeIoDispatcher;
  const std::uint64_t id_;
  WorkLease<dmw::GuardCondition> wake_;
  std::mutex mutex_;
  std::array<std::size_t, 256> completions_{};
  std::size_t head_{0}, count_{0};
  std::size_t outstanding_{0}; // Protected by dispatcher admission mutex.
  bool accepting_{true};
};

class NativeIoDispatcher {
public:
  explicit NativeIoDispatcher(std::shared_ptr<NativeContext>);
  ~NativeIoDispatcher() noexcept;
  NativeIoDispatcher(const NativeIoDispatcher &) = delete;
  NativeIoDispatcher &operator=(const NativeIoDispatcher &) = delete;

  std::shared_ptr<CompletionPort>
  open_route(const std::shared_ptr<NativeWaitSet> &);
  void stop_route(const std::shared_ptr<CompletionPort> &);
  void retire_route(const std::shared_ptr<CompletionPort> &);
  std::size_t reserve(const std::shared_ptr<CompletionPort> &, CompletionRecord,
                      bool control);
  void install(std::size_t, std::shared_ptr<NativeJob>);
  void enqueue_prepared(std::size_t);
  void enqueue(std::size_t, std::shared_ptr<NativeJob>);
  bool cancel_queued(std::size_t);
  void abandon(std::size_t);
  std::optional<std::pair<std::size_t, CompletionRecord>>
  peek(const std::shared_ptr<CompletionPort> &);
  void acknowledge(const std::shared_ptr<CompletionPort> &, std::size_t);
  std::size_t outstanding(const std::shared_ptr<CompletionPort> &);
  void shutdown();

private:
  enum class SlotState {
    Free,
    Reserved,
    Queued,
    Running,
    RetryWaiting,
    Completed
  };
  struct Slot {
    std::size_t ticket{0};
    SlotState state{SlotState::Free};
    bool control{false};
    bool priority{false};
    unsigned retries{0};
    std::chrono::steady_clock::time_point retry_due;
    std::size_t next{256};
    CompletionRecord completion;
    std::shared_ptr<CompletionPort> port;
    std::shared_ptr<NativeJob> job;
  };
  struct Queue {
    std::size_t head{256}, tail{256};
  };
  void push(Queue &, std::size_t) noexcept;
  std::size_t pop(Queue &) noexcept;
  void worker() noexcept;
  std::size_t locate(std::size_t);
  void validate_route(const std::shared_ptr<CompletionPort> &);
  std::shared_ptr<NativeJob> release_slot(std::size_t);
  std::shared_ptr<NativeContext> context_;
  std::mutex shutdown_mutex_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::array<Slot, 256> slots_;
  Queue control_queue_, user_queue_;
  std::unordered_map<std::uint64_t, std::shared_ptr<CompletionPort>> routes_;
  std::size_t used_{0}, ordinary_used_{0};
  std::size_t next_ticket_{1};
  std::uint64_t next_owner_{1};
  bool stopping_{false};
  std::thread worker_;
};

} // namespace dclpy::detail
#endif // DCLPY_DETAIL_NATIVE_IO_HPP_
