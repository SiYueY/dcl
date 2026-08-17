#ifndef DMW_IMPL__FASTDDS__PROCESS_RUNTIME_HPP_
#define DMW_IMPL__FASTDDS__PROCESS_RUNTIME_HPP_

#include <memory>
#include <mutex>
#include <vector>

#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/domain/DomainParticipantListener.hpp>
#include <fastdds/dds/publisher/DataWriterListener.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>

#include "impl/discovery_graph.hpp"

namespace dmw {

namespace impl {

/// Process-lifetime owner for DDS objects whose deletion was not confirmed.
///
/// Fast DDS can retain callbacks and endpoint bindings after a failed delete.
/// Releasing the corresponding C++ listener in that state would turn a
/// recoverable DDS failure into a use-after-free.  The retention owner intentionally
/// has process lifetime: quarantined objects are only safe to release at the
/// Fast DDS process termination barrier.
class ProcessLifetime final {
public:
    static ProcessLifetime& instance() noexcept {
        static auto* lifetime = new ProcessLifetime();
        return *lifetime;
    }

    void retain_participant(
        eprosima::fastdds::dds::DomainParticipantFactory* factory,
        eprosima::fastdds::dds::DomainParticipant* participant,
        std::unique_ptr<DiscoveryListener> listener = {}) noexcept {
        if (participant == nullptr) return;
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            participants_.push_back(
                QuarantinedParticipant{factory, participant, std::move(listener)});
        } catch (...) {
            // The terminal fallback deliberately leaks all three objects.
            // This preserves callback and DDS binding validity even if the
            // quarantine bookkeeping itself cannot allocate.
            (void)factory;
            (void)participant;
            (void)listener.release();
        }
    }

    std::size_t quarantined_participant_count() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return participants_.size();
    }

    void retain_writer_listener(
        std::unique_ptr<eprosima::fastdds::dds::DataWriterListener> listener) noexcept {
        if (!listener) return;
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            writer_listeners_.push_back(std::move(listener));
        } catch (...) {
            (void)listener.release();
        }
    }

    void retain_reader_listener(
        std::unique_ptr<eprosima::fastdds::dds::DataReaderListener> listener) noexcept {
        if (!listener) return;
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            reader_listeners_.push_back(std::move(listener));
        } catch (...) {
            (void)listener.release();
        }
    }

    void retain_participant_listener(
        std::unique_ptr<eprosima::fastdds::dds::DomainParticipantListener> listener) noexcept {
        if (!listener) return;
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            participant_listeners_.push_back(std::move(listener));
        } catch (...) {
            (void)listener.release();
        }
    }

private:
    struct QuarantinedParticipant {
        eprosima::fastdds::dds::DomainParticipantFactory* factory;
        eprosima::fastdds::dds::DomainParticipant* participant;
        std::unique_ptr<DiscoveryListener> listener;
    };

    ProcessLifetime() = default;

    mutable std::mutex mutex_;
    std::vector<QuarantinedParticipant> participants_;
    std::vector<std::unique_ptr<eprosima::fastdds::dds::DataWriterListener>> writer_listeners_;
    std::vector<std::unique_ptr<eprosima::fastdds::dds::DataReaderListener>> reader_listeners_;
    std::vector<std::unique_ptr<eprosima::fastdds::dds::DomainParticipantListener>>
        participant_listeners_;
};

}  // namespace impl
}  // namespace dmw

#endif  // DMW_IMPL__FASTDDS__PROCESS_RUNTIME_HPP_
