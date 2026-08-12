// SPDX-License-Identifier: Apache-2.0

#include "dmw/fastdds/message_type.hpp"

#include <memory>
#include <string>
#include <typeindex>
#include <utility>

#include "dmw/error.hpp"

namespace dmw {

class MessageType::Impl {
public:
    Impl(
        eprosima::fastdds::dds::TypeSupport type_support, std::string wire_type_name,
        std::type_index binding_type)
    : type_support_(std::move(type_support)),
      wire_type_name_(std::move(wire_type_name)),
      binding_type_(binding_type) {}

private:
    friend class MessageType;
    friend class fastdds::MessageTypeAccess;

    eprosima::fastdds::dds::TypeSupport type_support_;
    const std::string wire_type_name_;
    const std::type_index binding_type_;
};

std::string_view MessageType::type_name() const noexcept { return impl_->wire_type_name_; }

namespace fastdds {

Result<MessageType> MessageTypeAccess::create(
    eprosima::fastdds::dds::TypeSupport type_support, std::type_index binding_type) {
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
        std::move(type_support), std::move(wire_type_name), binding_type);
    return Result<MessageType>::success(MessageType(std::move(impl)));
}

const eprosima::fastdds::dds::TypeSupport& MessageTypeAccess::type_support(
    const MessageType& message_type) noexcept {
    return message_type.impl_->type_support_;
}

std::type_index MessageTypeAccess::binding_type(const MessageType& message_type) noexcept {
    return message_type.impl_->binding_type_;
}

}  // namespace fastdds

}  // namespace dmw
