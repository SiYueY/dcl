#include <cassert>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>

#include "entity.hpp"

using namespace std::chrono_literals;
using namespace dclpy::detail;

struct Resource {
    explicit Resource(std::atomic<unsigned>& destroyed) : destroyed(destroyed) {}
    ~Resource() noexcept { ++destroyed; }
    std::atomic<unsigned>& destroyed;
};

int main() {
    auto admission = std::make_shared<ContextAdmission>();
    std::atomic<unsigned> destroyed{0};
    auto backing = std::make_shared<EntityBacking<Resource>>(
        admission, std::make_unique<Resource>(destroyed));
    backing->set_registered(true);
    auto operation = backing->operation();
    auto dispatch = backing->dispatch();
    auto work = backing->work();
    backing->close();
    assert(backing->state() == EntityState::Closing);
    bool rejected = false;
    try {
        (void)backing->operation();
    } catch (const EntityClosed&) {
        rejected = true;
    }
    assert(rejected);
    operation.reset();
    dispatch.reset();
    auto cleanup = work.derive_cleanup();
    work.reset();
    assert(destroyed == 0);
    assert(!backing->wait_closed(std::chrono::steady_clock::now()));
    cleanup.reset();
    assert(destroyed == 0);  // registration still owns native access
    backing->set_registered(false);
    assert(backing->wait_closed(std::chrono::steady_clock::now() + 1s));
    assert(destroyed == 1);

    auto second = std::make_shared<EntityBacking<Resource>>(
        admission, std::make_unique<Resource>(destroyed));
    auto accepted = second->work();
    {
        std::lock_guard lock(admission->mutex);
        admission->open = false;
    }
    rejected = false;
    try {
        (void)second->work();
    } catch (const ContextShutdown&) {
        rejected = true;
    }
    assert(rejected);
    second->close();
    auto continuation = accepted.derive_cleanup();
    accepted.reset();
    assert(destroyed == 1);
    continuation.reset();
    assert(destroyed == 2);

    // CLOSED must not become visible until the destructor finishes.
    struct DestructorBarrier {
        std::promise<void>& entered;
        std::shared_future<void> release;
        DestructorBarrier(std::promise<void>& entered, std::shared_future<void> release)
        : entered(entered), release(std::move(release)) {}
        DestructorBarrier(const DestructorBarrier&) = delete;
        ~DestructorBarrier() noexcept {
            entered.set_value();
            release.wait();
        }
    };
    auto third_admission = std::make_shared<ContextAdmission>();
    std::promise<void> entered;
    std::promise<void> release;
    auto entered_future = entered.get_future();
    auto barrier = std::make_unique<DestructorBarrier>(entered, release.get_future().share());
    auto third = std::make_shared<EntityBacking<DestructorBarrier>>(
        third_admission, std::move(barrier));
    auto closer = std::async(std::launch::async, [third] { third->close(); });
    assert(entered_future.wait_for(2s) == std::future_status::ready);
    assert(third->state() == EntityState::Closing);
    assert(!third->wait_closed(std::chrono::steady_clock::now()));
    release.set_value();
    closer.get();
    assert(third->state() == EntityState::Closed);
}
