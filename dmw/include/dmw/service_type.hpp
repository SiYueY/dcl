#ifndef DMW_SERVICE_TYPE_HPP_
#define DMW_SERVICE_TYPE_HPP_

#include <utility>

#include "dmw/message_type.hpp"

namespace dmw {

/// Pair of request and response message type descriptors.
class ServiceType {
public:
    ServiceType(MessageType request_type, MessageType response_type) noexcept
    : request_type_(std::move(request_type)), response_type_(std::move(response_type)) {}

    const MessageType& request_type() const noexcept { return request_type_; }

    const MessageType& response_type() const noexcept { return response_type_; }

private:
    MessageType request_type_;
    MessageType response_type_;
};

}  // namespace dmw

#endif  // DMW_SERVICE_TYPE_HPP_
