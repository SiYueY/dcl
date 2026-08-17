#ifndef DMW_IMPL__LOCK_RANK_HPP_
#define DMW_IMPL__LOCK_RANK_HPP_

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <mutex>

namespace dmw::impl {

/// The fixed lock hierarchy from dmw_fastdds.md §10.1.  Process quarantine
/// locks deliberately do not participate: they may only be entered with no
/// ordinary DMW lock held.
enum class LockRank : unsigned char {
    ContextRuntime = 1,
    ChildRegistry = 2,
    TypeRegistry = 3,
    TopicRegistry = 4,
    DiscoveryGraph = 5,
    RequestState = 7,
    TargetReader = 8,
    OrphanedEndpoint = 9,
    RetiredWaitSet = 10,
    EndpointRuntime = 11,
    WaitSetTopology = 12,
    WaitSetReconciliation = 13,
    Registration = 14,
    WaitableLocal = 15,
    PendingRequest = 16,
    ListenerState = 17,
};

class LockRankTracker {
public:
    static void acquire(LockRank rank) noexcept {
#ifndef NDEBUG
        assert(depth_ < held_.size());
        if (depth_ != 0) {
            // Same-rank locking is forbidden by default.  The only permitted
            // exceptions must use a dedicated ordered multi-lock primitive.
            if (static_cast<unsigned>(held_[depth_ - 1]) >= static_cast<unsigned>(rank)) {
                std::fprintf(
                    stderr, "DMW lock-rank inversion: held=%u requested=%u depth=%zu\n",
                    static_cast<unsigned>(held_[depth_ - 1]), static_cast<unsigned>(rank), depth_);
                assert(false && "DMW lock-rank inversion");
            }
        }
        held_[depth_++] = rank;
#else
        (void)rank;
#endif
    }

    static void release(LockRank rank) noexcept {
#ifndef NDEBUG
        assert(depth_ != 0);
        assert(held_[depth_ - 1] == rank);
        --depth_;
#else
        (void)rank;
#endif
    }

private:
#ifndef NDEBUG
    inline static thread_local std::array<LockRank, 32> held_{};
    inline static thread_local std::size_t depth_{0};
#endif
};

template <LockRank Rank>
class RankedMutex {
public:
    void lock() {
        mutex_.lock();
        LockRankTracker::acquire(Rank);
    }

    bool try_lock() {
        if (!mutex_.try_lock()) return false;
        LockRankTracker::acquire(Rank);
        return true;
    }

    void unlock() noexcept {
        LockRankTracker::release(Rank);
        mutex_.unlock();
    }

private:
    std::mutex mutex_;
};

}  // namespace dmw::impl

#endif  // DMW_IMPL__LOCK_RANK_HPP_
