#include "dmw/error.hpp"
#include "dmw/fastdds/message_type.hpp"
#include "impl/message_type_impl.hpp"

namespace dmw {

Result<MessageType> MessageType::Impl::create(
    eprosima::fastdds::dds::TypeSupport type_support, std::type_index pubsub_type) {
    if (!type_support) {
        return Result<MessageType>::failure(
            Error(ErrorCode::InvalidArgument, "Fast DDS type support must not be null"));
    }

    std::string wire_type_name(type_support->getName());
    if (wire_type_name.empty()) {
        return Result<MessageType>::failure(
            Error(ErrorCode::InvalidArgument, "Fast DDS wire type name must not be empty"));
    }

    auto impl = std::make_shared<MessageType::Impl>(
        std::move(type_support), std::move(wire_type_name), pubsub_type);
    return Result<MessageType>::success(MessageType(std::move(impl)));
}

namespace fastdds {

Result<MessageType> MessageTypeAdapter::create(
    eprosima::fastdds::dds::TypeSupport support, std::type_index type) {
    return MessageType::Impl::create(std::move(support), type);
}

const eprosima::fastdds::dds::TypeSupport& MessageTypeAdapter::type_support(
    const MessageType& type) noexcept {
    return type.impl_->type_support();
}

std::type_index MessageTypeAdapter::pubsub_type(const MessageType& type) noexcept {
    return type.impl_->pubsub_type();
}

}  // namespace fastdds
}  // namespace dmw
