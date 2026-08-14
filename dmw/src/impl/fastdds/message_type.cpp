#include "impl/message_type_impl.hpp"

#include "dmw/error.hpp"
#include "impl/message_type_impl.hpp"

namespace dmw {

Result<MessageType> MessageType::Impl::create(
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

}  // namespace dmw
