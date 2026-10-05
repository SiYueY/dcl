#include "native_io.hpp"

#include <cassert>
#include <future>
#include <string>
#include <vector>

using namespace dclpy::detail;
namespace {
class FaultJob final : public NativeJob {
public:
    explicit FaultJob(int fault) : fault_(fault) {}
    void run(CompletionRecord&) override {
        if (fault_ == 0) throw std::bad_alloc();
        if (fault_ == 1) throw std::runtime_error(std::string(252, 'x') + "中文");
        if (fault_ == 2) throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::ProtocolFault, "post-commit failure"));
        throw 17;
    }
private:
    int fault_;
};
class RetryJob final : public NativeJob {
public:
    RetryJob(int& attempts, int& cleanup, bool protocol_fault)
    : attempts_(attempts), cleanup_(cleanup), protocol_fault_(protocol_fault) {}
    void run(CompletionRecord&) override {
        ++attempts_;
        throw MiddlewareFailure(dmw::Error(protocol_fault_ ? dmw::ErrorCode::ProtocolFault : dmw::ErrorCode::DDSError,
                                          "delivery failed"));
    }
    unsigned max_attempts() const noexcept override { return 4; }
    bool retryable(const CompletionRecord&) const noexcept override { return true; }
    void after_failed_delivery(CompletionRecord&) override { ++cleanup_; }
private:
    int& attempts_;
    int& cleanup_;
    bool protocol_fault_;
};
class OrderedJob final : public NativeJob {
public:
    OrderedJob(int id, std::vector<int>& output, std::shared_future<void> gate = {},
               std::promise<void>* started = nullptr)
    : id_(id), output_(output), gate_(std::move(gate)), started_(started) {}
    void run(CompletionRecord&) override {
        if (started_) started_->set_value();
        if (gate_.valid()) gate_.wait();
        output_.push_back(id_);
    }
private:
    int id_;
    std::vector<int>& output_;
    std::shared_future<void> gate_;
    std::promise<void>* started_;
};
CompletionRecord await_one(NativeIoDispatcher& dispatcher, const std::shared_ptr<CompletionPort>& port,
                           NativeWaitSet& wait_set) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto entry = dispatcher.peek(port)) {
            assert(dispatcher.peek(port)->first == entry->first);  // Durable until owner ACK.
            dispatcher.acknowledge(port, entry->first);
            return entry->second;
        }
        wait_set.wait(unwrap(dmw::WaitTimeout::finite(std::chrono::milliseconds(10)))).release();
    }
    throw std::runtime_error("Native completion timed out");
}
}

int main() {
    dmw::ContextOptions options;
    options.domain_id = 208;
    auto context = std::make_shared<NativeContext>(options);
    auto wait_set = std::make_shared<NativeWaitSet>(context);
    NativeIoDispatcher dispatcher(context);
    auto port = dispatcher.open_route(wait_set);
    CompletionRecord record;
    record.attachment_generation = 71;
    record.operation_id = 19;
    record.entity_id = 31;
    for (int fault = 0; fault < 4; ++fault) {
        auto ticket = dispatcher.reserve(port, record, false);
        dispatcher.enqueue(ticket, std::make_unique<FaultJob>(fault));
        auto completion = await_one(dispatcher, port, *wait_set);
        assert(completion.failed);
        assert(completion.executor_id == port->id());
        assert(completion.attachment_generation == 71 && completion.operation_id == 19 && completion.entity_id == 31);
        if (fault == 0) assert(completion.error_code == dmw::ErrorCode::ResourceExhausted);
        if (fault == 1) assert(std::string(completion.diagnostic) == std::string(252, 'x') + "中");
        if (fault == 2) assert(completion.error_code == dmw::ErrorCode::ProtocolFault);
        if (fault == 3) assert(completion.error_code == dmw::ErrorCode::DDSError);
        assert(dispatcher.outstanding(port) == 0);
        assert(!dispatcher.peek(port));
    }
    std::vector<int> order;
    order.reserve(3);
    std::promise<void> start, release;
    auto started = start.get_future();
    auto gate = release.get_future().share();
    auto first = dispatcher.reserve(port, record, false);
    dispatcher.enqueue(first, std::make_unique<OrderedJob>(1, order, gate, &start));
    assert(started.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    auto second = dispatcher.reserve(port, record, false);
    dispatcher.enqueue(second, std::make_unique<OrderedJob>(2, order));
    record.kind = NativeJobKind::CancelResponse;
    auto critical = dispatcher.reserve(port, record, true);
    dispatcher.enqueue(critical, std::make_unique<OrderedJob>(3, order));
    release.set_value();
    for (int index = 0; index < 3; ++index) assert(!await_one(dispatcher, port, *wait_set).failed);
    assert((order == std::vector<int>{1, 3, 2}));
    int attempts = 0, cleanup = 0;
    auto retry = dispatcher.reserve(port, record, true);
    dispatcher.enqueue(retry, std::make_unique<RetryJob>(attempts, cleanup, false));
    assert(await_one(dispatcher, port, *wait_set).failed);
    assert(attempts == 4 && cleanup == 1);  // First attempt plus three same-ticket retries.
    attempts = cleanup = 0;
    auto fault = dispatcher.reserve(port, record, true);
    dispatcher.enqueue(fault, std::make_unique<RetryJob>(attempts, cleanup, true));
    assert(await_one(dispatcher, port, *wait_set).error_code == dmw::ErrorCode::ProtocolFault);
    assert(attempts == 1 && cleanup == 1);
    dispatcher.stop_route(port);
    dispatcher.retire_route(port);
    wait_set->close();
    dispatcher.shutdown();
    context->stop_admission();
    context->close_children();
    context->finish_shutdown(std::nullopt);
}
