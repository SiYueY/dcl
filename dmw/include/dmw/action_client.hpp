#ifndef DMW_ACTION_CLIENT_HPP_
#define DMW_ACTION_CLIENT_HPP_

#include <memory>
#include <string_view>
#include <utility>

#include "dmw/action_common.hpp"
#include "dmw/action_type.hpp"
#include "dmw/message_info.hpp"
#include "dmw/qos.hpp"
#include "dmw/request_id.hpp"
#include "dmw/result.hpp"
#include "dmw/visibility_control.hpp"
#include "dmw/wait_timeout.hpp"

namespace dmw {

class Node;
class WaitSet;

/// Per-endpoint QoS of the five ActionClient constituents.
struct ActionClientOptions {
    Qos goal_service_qos{Qos::ros2_services_default()};
    Qos cancel_service_qos{Qos::ros2_services_default()};
    Qos result_service_qos{Qos::ros2_services_default()};
    Qos feedback_topic_qos{Qos::ros2_default()};
    Qos status_topic_qos{Qos::ros2_action_status_default()};
};

/// Aggregate Action client primitive.
///
/// The three internal service clients and two topic readers stay private:
/// callers only see this one aggregate and one WaitSet registration token.
/// RequestId -> Future mapping stays in the Client Library.
class DMW_PUBLIC ActionClient {
public:
    ~ActionClient() noexcept;

    ActionClient(const ActionClient&) = delete;
    ActionClient& operator=(const ActionClient&) = delete;
    ActionClient(ActionClient&&) = delete;
    ActionClient& operator=(ActionClient&&) = delete;

    Result<RequestId> write_goal_request(const void* request);
    Result<bool> read_goal_response(void* response, RequestId& request_id);

    Result<RequestId> write_cancel_request(const void* request);
    Result<bool> read_cancel_response(void* response, RequestId& request_id);

    Result<RequestId> write_result_request(const void* request);
    Result<bool> read_result_response(void* response, RequestId& request_id);

    Result<bool> read_feedback(void* feedback, MessageInfo& info);
    Result<bool> read_status(void* status, MessageInfo& info);

    Result<bool> server_is_available() const;
    Result<bool> wait_for_server(WaitTimeout timeout) const;

    /// Current-state readiness observer; a WaitResult snapshot stays
    /// authoritative for one wait() cycle.
    Result<ActionClientReadySet> readiness() const;

    std::string_view action_name() const noexcept;

private:
    friend class Node;
    friend class WaitSet;

    class Impl;

    explicit ActionClient(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}  // namespace dmw

#endif  // DMW_ACTION_CLIENT_HPP_
