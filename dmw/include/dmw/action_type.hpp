#ifndef DMW_ACTION_TYPE_HPP_
#define DMW_ACTION_TYPE_HPP_

#include <utility>

#include "dmw/message_type.hpp"
#include "dmw/service_type.hpp"

namespace dmw {

/// Wire/runtime descriptor of one Action: three services plus two topics.
///
/// ActionType describes only the constituent endpoint types.  It carries no
/// ActionT template or Python class information and never parses an
/// action-specific object layout.
class ActionType {
public:
    ActionType(
        ServiceType send_goal_type, ServiceType cancel_goal_type, ServiceType get_result_type,
        MessageType feedback_type, MessageType status_type) noexcept
    : send_goal_type_(std::move(send_goal_type)),
      cancel_goal_type_(std::move(cancel_goal_type)),
      get_result_type_(std::move(get_result_type)),
      feedback_type_(std::move(feedback_type)),
      status_type_(std::move(status_type)) {}

    const ServiceType& send_goal_type() const noexcept { return send_goal_type_; }
    const ServiceType& cancel_goal_type() const noexcept { return cancel_goal_type_; }
    const ServiceType& get_result_type() const noexcept { return get_result_type_; }
    const MessageType& feedback_type() const noexcept { return feedback_type_; }
    const MessageType& status_type() const noexcept { return status_type_; }

private:
    ServiceType send_goal_type_;
    ServiceType cancel_goal_type_;
    ServiceType get_result_type_;
    MessageType feedback_type_;
    MessageType status_type_;
};

}  // namespace dmw

#endif  // DMW_ACTION_TYPE_HPP_
