#include "dmw/fastdds/message_type.hpp"

#include "impl/message_type_impl.hpp"

namespace dmw {
std::string_view MessageType::type_name() const noexcept { return impl_->wire_type_name(); }
namespace fastdds {
Result<MessageType> MessageTypeAccess::create(
    eprosima::fastdds::dds::TypeSupport support, std::type_index type) {
    return MessageType::Impl::create(std::move(support), type);
}
const eprosima::fastdds::dds::TypeSupport& MessageTypeAccess::type_support(
    const MessageType& type) noexcept {
    return type.impl_->type_support();
}
std::type_index MessageTypeAccess::binding_type(const MessageType& type) noexcept {
    return type.impl_->binding_type();
}
}  // namespace fastdds
}  // namespace dmw
