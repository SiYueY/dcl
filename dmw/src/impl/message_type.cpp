#include "dmw/error.hpp"
#include "dmw/fastdds/message_type.hpp"
#include "impl/message_type_impl.hpp"

namespace dmw {

namespace {

// Fast DDS 2.6 does not unwind an allocated writer payload when a serializer
// throws. Translate callback failures before they cross the DDS callback
// boundary, so DDS can return its payload to the pool normally.
class SafeTopicDataType final : public eprosima::fastdds::dds::TopicDataType {
public:
    explicit SafeTopicDataType(eprosima::fastdds::dds::TypeSupport support)
    : TopicDataType(*support.get()), support_(std::move(support)) {}

    bool serialize(void* data, eprosima::fastrtps::rtps::SerializedPayload_t* payload) override {
        try {
            return support_->serialize(data, payload);
        } catch (...) {
            return false;
        }
    }
    bool deserialize(eprosima::fastrtps::rtps::SerializedPayload_t* payload, void* data) override {
        try {
            return support_->deserialize(payload, data);
        } catch (...) {
            return false;
        }
    }
    std::function<std::uint32_t()> getSerializedSizeProvider(void* data) override {
        return support_->getSerializedSizeProvider(data);
    }
    void* createData() override { return support_->createData(); }
    void deleteData(void* data) override { support_->deleteData(data); }
    bool getKey(void* data, eprosima::fastrtps::rtps::InstanceHandle_t* handle,
                bool force_md5 = false) override {
        return support_->getKey(data, handle, force_md5);
    }
    bool is_bounded() const override { return support_->is_bounded(); }
    bool is_plain() const override { return support_->is_plain(); }
    bool construct_sample(void* memory) const override { return support_->construct_sample(memory); }

private:
    eprosima::fastdds::dds::TypeSupport support_;
};

}  // namespace

Result<MessageType> MessageType::Impl::create(
    eprosima::fastdds::dds::TypeSupport type_support, std::type_index pubsub_type,
    fastdds::MessageTypeAdapter::ReceiveCommit receive_commit) {
    if (!type_support) {
        return Result<MessageType>::failure(
            Error(ErrorCode::InvalidArgument, "Fast DDS type support must not be null"));
    }

    std::string wire_type_name(type_support->getName());
    if (wire_type_name.empty()) {
        return Result<MessageType>::failure(
            Error(ErrorCode::InvalidArgument, "Fast DDS wire type name must not be empty"));
    }

    eprosima::fastdds::dds::TypeSupport safe_support(new SafeTopicDataType(std::move(type_support)));
    auto impl = std::make_shared<MessageType::Impl>(
        std::move(safe_support), std::move(wire_type_name), pubsub_type, receive_commit);
    return Result<MessageType>::success(MessageType(std::move(impl)));
}

namespace fastdds {

Result<MessageType> MessageTypeAdapter::create(
    eprosima::fastdds::dds::TypeSupport support, std::type_index type,
    ReceiveCommit receive_commit) {
    return MessageType::Impl::create(std::move(support), type, receive_commit);
}

const eprosima::fastdds::dds::TypeSupport& MessageTypeAdapter::type_support(
    const MessageType& type) noexcept {
    return type.impl_->type_support();
}

std::type_index MessageTypeAdapter::pubsub_type(const MessageType& type) noexcept {
    return type.impl_->pubsub_type();
}

MessageTypeAdapter::ReceiveCommit MessageTypeAdapter::receive_commit(
    const MessageType& type) noexcept {
    return type.impl_->receive_commit();
}

}  // namespace fastdds
}  // namespace dmw
