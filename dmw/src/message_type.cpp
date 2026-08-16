#include "impl/message_type_impl.hpp"

namespace dmw {

std::string_view MessageType::type_name() const noexcept { return impl_->wire_type_name(); }

}  // namespace dmw
