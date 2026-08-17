#ifndef DMW_IMPL__TEMPORARY_SAMPLE_HPP_
#define DMW_IMPL__TEMPORARY_SAMPLE_HPP_

#include <utility>

#include <fastdds/rtps/common/SerializedPayload.h>

#include "dmw/error.hpp"
#include "dmw/fastdds/message_type.hpp"
#include "dmw/result.hpp"

namespace dmw {

namespace impl {

class TemporarySample {
public:
    static Result<TemporarySample> create(const MessageType& type) {
        const auto support = dmw::fastdds::MessageTypeAdapter::type_support(type);
        void* data = support->createData();
        if (data == nullptr) {
            return Result<TemporarySample>::failure(
                Error(ErrorCode::DdsError, "Fast DDS type support failed to allocate a sample"));
        }
        return Result<TemporarySample>::success(TemporarySample(support, data));
    }

    TemporarySample(const TemporarySample&) = delete;
    TemporarySample& operator=(const TemporarySample&) = delete;
    TemporarySample(TemporarySample&& other) noexcept
    : support_(std::move(other.support_)), data_(std::exchange(other.data_, nullptr)) {}
    TemporarySample& operator=(TemporarySample&& other) noexcept {
        if (this != &other) {
            reset();
            support_ = std::move(other.support_);
            data_ = std::exchange(other.data_, nullptr);
        }
        return *this;
    }
    ~TemporarySample() noexcept { reset(); }

    void* data() const noexcept { return data_; }

    Result<void> commit_to(void* destination) const {
        eprosima::fastrtps::rtps::SerializedPayload_t payload(
            support_->getSerializedSizeProvider(data_)());
        if (!support_->serialize(data_, &payload)) {
            return Result<void>::failure(
                Error(ErrorCode::DdsError, "Fast DDS sample serialization failed"));
        }
        if (!support_->deserialize(&payload, destination)) {
            return Result<void>::failure(
                Error(ErrorCode::DdsError, "Fast DDS sample deserialization failed"));
        }
        return Result<void>::success();
    }

private:
    TemporarySample(eprosima::fastdds::dds::TypeSupport support, void* data) noexcept
    : support_(std::move(support)), data_(data) {}
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
};

}  // namespace impl
}  // namespace dmw

#endif  // DMW_IMPL__TEMPORARY_SAMPLE_HPP_
