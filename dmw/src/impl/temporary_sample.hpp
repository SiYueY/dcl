#ifndef DMW_IMPL__TEMPORARY_SAMPLE_HPP_
#define DMW_IMPL__TEMPORARY_SAMPLE_HPP_

#include <utility>
#include <atomic>
#include <cstdint>
#include <memory>
#include <new>

#include <fastdds/rtps/common/SerializedPayload.h>

#include "dmw/error.hpp"
#include "dmw/fastdds/message_type.hpp"
#include "dmw/result.hpp"

namespace dmw {

namespace impl {

class TemporarySample {
public:
    struct AllocationCounters {
        std::uint64_t sample_creations;
        std::uint64_t payload_allocations;
    };

    static AllocationCounters allocation_counters() noexcept {
        return {
            sample_creations_.load(std::memory_order_relaxed),
            payload_allocations_.load(std::memory_order_relaxed)};
    }

    static Result<TemporarySample> create(const MessageType& type) {
        const auto support = dmw::fastdds::MessageTypeAdapter::type_support(type);
        void* data = support->createData();
        if (data == nullptr) {
            return Result<TemporarySample>::failure(
                Error(ErrorCode::DDSError, "Fast DDS type support failed to allocate a sample"));
        }
        sample_creations_.fetch_add(1, std::memory_order_relaxed);
        return Result<TemporarySample>::success(
            TemporarySample(support, data, dmw::fastdds::MessageTypeAdapter::receive_commit(type)));
    }

    TemporarySample(const TemporarySample&) = delete;
    TemporarySample& operator=(const TemporarySample&) = delete;
    TemporarySample(TemporarySample&& other) noexcept
    : support_(std::move(other.support_)),
      data_(std::exchange(other.data_, nullptr)),
      payload_(std::move(other.payload_)),
      payload_capacity_(std::exchange(other.payload_capacity_, 0)),
      receive_commit_(std::exchange(other.receive_commit_, nullptr)) {}
    TemporarySample& operator=(TemporarySample&& other) noexcept {
        if (this != &other) {
            reset();
            support_ = std::move(other.support_);
            data_ = std::exchange(other.data_, nullptr);
            payload_ = std::move(other.payload_);
            payload_capacity_ = std::exchange(other.payload_capacity_, 0);
            receive_commit_ = std::exchange(other.receive_commit_, nullptr);
        }
        return *this;
    }
    ~TemporarySample() noexcept { reset(); }

    void* data() const noexcept { return data_; }

    Result<void> commit_to(void* destination) {
        if (receive_commit_ != nullptr) return receive_commit_(data_, destination);
        const auto serialized_size = support_->getSerializedSizeProvider(data_)();
        if (!payload_ || serialized_size > payload_capacity_) {
            try {
                payload_ = std::make_unique<eprosima::fastrtps::rtps::SerializedPayload_t>(
                    serialized_size);
            } catch (const std::bad_alloc&) {
                throw;
            } catch (...) {
                return Result<void>::failure(Error(
                    ErrorCode::ResourceExhausted, "Fast DDS sample payload allocation failed"));
            }
            payload_allocations_.fetch_add(1, std::memory_order_relaxed);
            payload_capacity_ = serialized_size;
        }
        payload_->length = 0;
        if (!support_->serialize(data_, payload_.get())) {
            return Result<void>::failure(
                Error(ErrorCode::DDSError, "Fast DDS sample serialization failed"));
        }
        if (!support_->deserialize(payload_.get(), destination)) {
            return Result<void>::failure(
                Error(ErrorCode::DDSError, "Fast DDS sample deserialization failed"));
        }
        return Result<void>::success();
    }

private:
    TemporarySample(
        eprosima::fastdds::dds::TypeSupport support, void* data,
        dmw::fastdds::MessageTypeAdapter::ReceiveCommit receive_commit) noexcept
    : support_(std::move(support)), data_(data), receive_commit_(receive_commit) {}
    void reset() noexcept {
        if (data_ != nullptr) {
            try {
                support_->deleteData(data_);
            } catch (...) {
            }
            data_ = nullptr;
        }
    }
    eprosima::fastdds::dds::TypeSupport support_;
    void* data_{nullptr};
    std::unique_ptr<eprosima::fastrtps::rtps::SerializedPayload_t> payload_;
    std::uint32_t payload_capacity_{0};
    dmw::fastdds::MessageTypeAdapter::ReceiveCommit receive_commit_{nullptr};
    inline static std::atomic<std::uint64_t> sample_creations_{0};
    inline static std::atomic<std::uint64_t> payload_allocations_{0};
};

}  // namespace impl
}  // namespace dmw

#endif  // DMW_IMPL__TEMPORARY_SAMPLE_HPP_
