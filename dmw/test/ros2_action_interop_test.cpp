#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <typeindex>
#include <vector>

#include "action_msgs/msg/goal_status.hpp"
#include "action_msgs/msg/goal_status_array.hpp"
#include "action_msgs/srv/cancel_goal.hpp"
#include "builtin_interfaces/msg/time.hpp"
// The Fibonacci action definition comes from example_interfaces when the ROS 2
// installation under test ships it, and from the workspace-local
// dmw_action_test_interfaces package otherwise (for example on Rolling).
#ifndef DMW_ROS_ACTION_INTERFACE_HEADER
#define DMW_ROS_ACTION_INTERFACE_HEADER "example_interfaces/action/fibonacci.hpp"
#endif
#ifndef DMW_ROS_ACTION_INTERFACE_NAMESPACE
#define DMW_ROS_ACTION_INTERFACE_NAMESPACE example_interfaces::action
#endif
#ifndef DMW_ROS_ACTION_TYPE_NAME_PREFIX
#define DMW_ROS_ACTION_TYPE_NAME_PREFIX "example_interfaces::action"
#endif

#include DMW_ROS_ACTION_INTERFACE_HEADER
#include "fastcdr/Cdr.h"
#include "fastcdr/FastBuffer.h"
#include "fastcdr/config.h"
#include "rclcpp/rclcpp.hpp"
#include "rclcpp_action/rclcpp_action.hpp"
#include "unique_identifier_msgs/msg/uuid.hpp"

#include "dmw/action_client.hpp"
#include "dmw/action_common.hpp"
#include "dmw/action_server.hpp"
#include "dmw/action_type.hpp"
#include "dmw/context.hpp"
#include "dmw/fastdds/message_type.hpp"
#include "dmw/node.hpp"
#include "dmw/service_type.hpp"
#include "dmw/wait_set.hpp"
#include "dmw/wait_timeout.hpp"

namespace {

using namespace std::chrono_literals;
using Fibonacci = DMW_ROS_ACTION_INTERFACE_NAMESPACE::Fibonacci;
using Cdr = eprosima::fastcdr::Cdr;

constexpr std::uint32_t kMaxPayload = 4096U;

// Goal status values from action_msgs/msg/GoalStatus.
constexpr std::int8_t kStatusExecuting = 2;
constexpr std::int8_t kStatusSucceeded = 4;
constexpr std::int8_t kStatusCanceled = 5;
constexpr std::int8_t kStatusAborted = 6;
// CancelGoal service return codes.
constexpr std::int8_t kCancelNone = 0;

#if FASTCDR_VERSION_MAJOR >= 2
constexpr auto kRosCdrVersion = eprosima::fastcdr::DDS_CDR;
#else
constexpr auto kRosCdrVersion = eprosima::fastcdr::Cdr::DDS_CDR;
#endif

void write_uuid(Cdr& cdr, const unique_identifier_msgs::msg::UUID& value) {
    for (const auto byte : value.uuid) cdr << byte;
}

void read_uuid(Cdr& cdr, unique_identifier_msgs::msg::UUID& value) {
    for (auto& byte : value.uuid) cdr >> byte;
}

void write_time(Cdr& cdr, const builtin_interfaces::msg::Time& value) {
    cdr << value.sec;
    cdr << value.nanosec;
}

void read_time(Cdr& cdr, builtin_interfaces::msg::Time& value) {
    cdr >> value.sec;
    cdr >> value.nanosec;
}

void write_sequence(Cdr& cdr, const std::vector<std::int32_t>& value) {
    cdr << static_cast<std::uint32_t>(value.size());
    for (const auto element : value) cdr << element;
}

void read_sequence(Cdr& cdr, std::vector<std::int32_t>& value) {
    std::uint32_t size = 0;
    cdr >> size;
    value.clear();
    value.reserve(size);
    for (std::uint32_t index = 0; index < size; ++index) {
        std::int32_t element = 0;
        cdr >> element;
        value.push_back(element);
    }
}

void write_goal_info(Cdr& cdr, const action_msgs::msg::GoalInfo& value) {
    write_uuid(cdr, value.goal_id);
    write_time(cdr, value.stamp);
}

void read_goal_info(Cdr& cdr, action_msgs::msg::GoalInfo& value) {
    read_uuid(cdr, value.goal_id);
    read_time(cdr, value.stamp);
}

/// CDR codec for every message carried by the five Fibonacci endpoints.
template <typename Message>
struct CdrCodec;

template <>
struct CdrCodec<Fibonacci::Impl::SendGoalService::Request> {
    static void write(Cdr& cdr, const Fibonacci::Impl::SendGoalService::Request& value) {
        write_uuid(cdr, value.goal_id);
        cdr << value.goal.order;
    }
    static void read(Cdr& cdr, Fibonacci::Impl::SendGoalService::Request& value) {
        read_uuid(cdr, value.goal_id);
        cdr >> value.goal.order;
    }
};

template <>
struct CdrCodec<Fibonacci::Impl::SendGoalService::Response> {
    static void write(Cdr& cdr, const Fibonacci::Impl::SendGoalService::Response& value) {
        cdr << value.accepted;
        write_time(cdr, value.stamp);
    }
    static void read(Cdr& cdr, Fibonacci::Impl::SendGoalService::Response& value) {
        cdr >> value.accepted;
        read_time(cdr, value.stamp);
    }
};

template <>
struct CdrCodec<action_msgs::srv::CancelGoal::Request> {
    static void write(Cdr& cdr, const action_msgs::srv::CancelGoal::Request& value) {
        write_goal_info(cdr, value.goal_info);
    }
    static void read(Cdr& cdr, action_msgs::srv::CancelGoal::Request& value) {
        read_goal_info(cdr, value.goal_info);
    }
};

template <>
struct CdrCodec<action_msgs::srv::CancelGoal::Response> {
    static void write(Cdr& cdr, const action_msgs::srv::CancelGoal::Response& value) {
        cdr << value.return_code;
        cdr << static_cast<std::uint32_t>(value.goals_canceling.size());
        for (const auto& goal : value.goals_canceling) write_goal_info(cdr, goal);
    }
    static void read(Cdr& cdr, action_msgs::srv::CancelGoal::Response& value) {
        cdr >> value.return_code;
        std::uint32_t size = 0;
        cdr >> size;
        value.goals_canceling.clear();
        value.goals_canceling.resize(size);
        for (auto& goal : value.goals_canceling) read_goal_info(cdr, goal);
    }
};

template <>
struct CdrCodec<Fibonacci::Impl::GetResultService::Request> {
    static void write(Cdr& cdr, const Fibonacci::Impl::GetResultService::Request& value) {
        write_uuid(cdr, value.goal_id);
    }
    static void read(Cdr& cdr, Fibonacci::Impl::GetResultService::Request& value) {
        read_uuid(cdr, value.goal_id);
    }
};

template <>
struct CdrCodec<Fibonacci::Impl::GetResultService::Response> {
    static void write(Cdr& cdr, const Fibonacci::Impl::GetResultService::Response& value) {
        cdr << value.status;
        write_sequence(cdr, value.result.sequence);
    }
    static void read(Cdr& cdr, Fibonacci::Impl::GetResultService::Response& value) {
        cdr >> value.status;
        read_sequence(cdr, value.result.sequence);
    }
};

template <>
struct CdrCodec<Fibonacci::Impl::FeedbackMessage> {
    static void write(Cdr& cdr, const Fibonacci::Impl::FeedbackMessage& value) {
        write_uuid(cdr, value.goal_id);
        write_sequence(cdr, value.feedback.sequence);
    }
    static void read(Cdr& cdr, Fibonacci::Impl::FeedbackMessage& value) {
        read_uuid(cdr, value.goal_id);
        read_sequence(cdr, value.feedback.sequence);
    }
};

template <>
struct CdrCodec<action_msgs::msg::GoalStatusArray> {
    static void write(Cdr& cdr, const action_msgs::msg::GoalStatusArray& value) {
        cdr << static_cast<std::uint32_t>(value.status_list.size());
        for (const auto& status : value.status_list) {
            write_goal_info(cdr, status.goal_info);
            cdr << status.status;
        }
    }
    static void read(Cdr& cdr, action_msgs::msg::GoalStatusArray& value) {
        std::uint32_t size = 0;
        cdr >> size;
        value.status_list.clear();
        value.status_list.resize(size);
        for (auto& status : value.status_list) {
            read_goal_info(cdr, status.goal_info);
            cdr >> status.status;
        }
    }
};

/// Fast DDS binding that speaks the ROS 2 CDR wire format for one message.
template <typename Message>
class RosActionTypeSupport final : public eprosima::fastdds::dds::TopicDataType {
public:
    explicit RosActionTypeSupport(const char* type_name) {
        m_typeSize = kMaxPayload;
        m_isGetKeyDefined = false;
        setName(type_name);
    }

    bool serialize(void* data, eprosima::fastrtps::rtps::SerializedPayload_t* payload) override {
        eprosima::fastcdr::FastBuffer buffer(
            reinterpret_cast<char*>(payload->data), payload->max_size);
        Cdr cdr(buffer, Cdr::DEFAULT_ENDIAN, kRosCdrVersion);
        cdr.serialize_encapsulation();
        CdrCodec<Message>::write(cdr, *static_cast<Message*>(data));
#if FASTCDR_VERSION_MAJOR >= 2
        payload->length = static_cast<std::uint32_t>(cdr.get_serialized_data_length());
#else
        payload->length = static_cast<std::uint32_t>(cdr.getSerializedDataLength());
#endif
        return true;
    }

    bool deserialize(
        eprosima::fastrtps::rtps::SerializedPayload_t* payload, void* data) override {
        eprosima::fastcdr::FastBuffer buffer(
            reinterpret_cast<char*>(payload->data), static_cast<std::size_t>(payload->length));
        Cdr cdr(buffer, Cdr::DEFAULT_ENDIAN, kRosCdrVersion);
        cdr.read_encapsulation();
        CdrCodec<Message>::read(cdr, *static_cast<Message*>(data));
        return true;
    }

    std::function<std::uint32_t()> getSerializedSizeProvider(void*) override {
        return [] { return kMaxPayload; };
    }

    void* createData() override { return new Message(); }

    void deleteData(void* data) override { delete static_cast<Message*>(data); }

    bool getKey(void*, eprosima::fastrtps::rtps::InstanceHandle_t*, bool) override {
        return false;
    }
};

template <typename Message>
dmw::MessageType ros_message_type(const char* type_name) {
    eprosima::fastdds::dds::TypeSupport support(new RosActionTypeSupport<Message>(type_name));
    auto type = dmw::fastdds::MessageTypeAdapter::create(
        std::move(support), std::type_index(typeid(RosActionTypeSupport<Message>)));
    assert(type);
    return type.value();
}

dmw::ActionType make_action_type() {
    using SendGoal = Fibonacci::Impl::SendGoalService;
    using GetResult = Fibonacci::Impl::GetResultService;
    using CancelGoal = action_msgs::srv::CancelGoal;
    return dmw::ActionType(
        dmw::ServiceType(
            ros_message_type<SendGoal::Request>(
                DMW_ROS_ACTION_TYPE_NAME_PREFIX "::dds_::Fibonacci_SendGoal_Request_"),
            ros_message_type<SendGoal::Response>(
                DMW_ROS_ACTION_TYPE_NAME_PREFIX "::dds_::Fibonacci_SendGoal_Response_")),
        dmw::ServiceType(
            ros_message_type<CancelGoal::Request>("action_msgs::srv::dds_::CancelGoal_Request_"),
            ros_message_type<CancelGoal::Response>("action_msgs::srv::dds_::CancelGoal_Response_")),
        dmw::ServiceType(
            ros_message_type<GetResult::Request>(
                DMW_ROS_ACTION_TYPE_NAME_PREFIX "::dds_::Fibonacci_GetResult_Request_"),
            ros_message_type<GetResult::Response>(
                DMW_ROS_ACTION_TYPE_NAME_PREFIX "::dds_::Fibonacci_GetResult_Response_")),
        ros_message_type<Fibonacci::Impl::FeedbackMessage>(
            DMW_ROS_ACTION_TYPE_NAME_PREFIX "::dds_::Fibonacci_FeedbackMessage_"),
        ros_message_type<action_msgs::msg::GoalStatusArray>(
            "action_msgs::msg::dds_::GoalStatusArray_"));
}

std::uint32_t ros_domain_id() {
    const char* value = std::getenv("ROS_DOMAIN_ID");
    assert(value != nullptr);
    char* end = nullptr;
    const auto parsed = std::strtoul(value, &end, 10);
    assert(*value != '\0' && *end == '\0' && parsed <= 232U);
    return static_cast<std::uint32_t>(parsed);
}

std::vector<std::int32_t> fibonacci(std::int32_t order) {
    std::vector<std::int32_t> sequence;
    if (order <= 0) return sequence;
    sequence.push_back(0);
    if (order == 1) return sequence;
    sequence.push_back(1);
    for (std::int32_t index = 2; index < order; ++index) {
        sequence.push_back(sequence[static_cast<std::size_t>(index) - 1] +
                           sequence[static_cast<std::size_t>(index) - 2]);
    }
    return sequence;
}

unique_identifier_msgs::msg::UUID make_uuid(std::uint8_t seed) {
    unique_identifier_msgs::msg::UUID uuid{};
    uuid.uuid[0] = seed;
    uuid.uuid[15] = seed;
    return uuid;
}

dmw::GoalId to_goal_id(const unique_identifier_msgs::msg::UUID& uuid) {
    dmw::GoalId goal_id;
    std::copy(uuid.uuid.begin(), uuid.uuid.end(), goal_id.data.begin());
    return goal_id;
}

/// Goals handled by the DMW side of the interop test.
struct DmwGoalState {
    std::mutex mutex;
    bool active{false};
    unique_identifier_msgs::msg::UUID active_goal{};
    std::int32_t active_order{0};
    int feedback_rounds{0};
    std::vector<std::int32_t> result;
    std::int8_t terminal_status{kStatusSucceeded};
    std::atomic<int> feedback_sent{0};
};

/// Real action servers publish periodic feedback, so the test server does too.
constexpr int kFeedbackRounds = 4;
/// Orders with special meanings used by this interop scenario.
constexpr std::int32_t kRejectOrder = -1;
constexpr std::int32_t kAbortOrder = 0;
constexpr std::int32_t kCancelOrder = 100;

void publish_goal_status(
    dmw::ActionServer& server, const unique_identifier_msgs::msg::UUID& uuid,
    std::int8_t status) {
    action_msgs::msg::GoalStatusArray array{};
    action_msgs::msg::GoalStatus entry{};
    entry.goal_info.goal_id = uuid;
    entry.status = status;
    array.status_list.push_back(entry);
    const auto published = server.publish_status(&array);
    assert(published);
}

}  // namespace

/// Scenario markers keep a flaky interop failure attributable to one phase.
#define DMW_PHASE(name) std::fprintf(stderr, "[phase] %s\n", name)

int main() {
    rclcpp::init(0, nullptr);
    auto ros_node = std::make_shared<rclcpp::Node>("dmw_ros2_action_interop");
    // The ROS side is pumped from the thread that also issues the client calls
    // (spin_some), which is the canonical rclcpp_action pattern.  Newer
    // rclcpp_action releases abort with "nothing is ready" when a multi-threaded
    // executor races the action client against concurrent application calls
    // (rclcpp_action's client is not thread safe).
    auto pump_until = [&ros_node](const std::function<bool()>& predicate) {
        const auto deadline = std::chrono::steady_clock::now() + 15s;
        while (std::chrono::steady_clock::now() < deadline) {
            rclcpp::spin_some(ros_node);
            if (predicate()) return true;
        }
        rclcpp::spin_some(ros_node);
        return predicate();
    };

    dmw::ContextOptions options;
    options.runtime_mode = dmw::RuntimeMode::ROS2;
    options.domain_id = ros_domain_id();
    options.participant_name = "dmw-ros2-action-interop";
    auto context = dmw::Context::create(options);
    assert(context);

    dmw::NodeOptions node_options;
    node_options.node_name = "dmw_action_interop";
    auto node = context.value()->create_node(node_options);
    assert(node);

    const auto action_type = make_action_type();
    DmwGoalState dmw_state;

    auto dmw_server = node.value()->create_action_server(
        action_type, "/dmw_ros2_action", dmw::ActionServerOptions{});
    assert(dmw_server);
    auto dmw_client = node.value()->create_action_client(action_type, "/ros2_action");
    assert(dmw_client);

    // --- availability contract -------------------------------------------------
    auto ros_client = rclcpp_action::create_client<Fibonacci>(ros_node, "/dmw_ros2_action");
    assert(ros_client->wait_for_action_server(15s));

    std::atomic<int> ros_goals{0};
    auto ros_server = rclcpp_action::create_server<Fibonacci>(
        ros_node, "/ros2_action",
        [](const rclcpp_action::GoalUUID&, std::shared_ptr<const Fibonacci::Goal>) {
            return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
        },
        [](const std::shared_ptr<rclcpp_action::ServerGoalHandle<Fibonacci>>&) {
            return rclcpp_action::CancelResponse::ACCEPT;
        },
        [&ros_goals](const std::shared_ptr<rclcpp_action::ServerGoalHandle<Fibonacci>>& handle) {
            ros_goals.fetch_add(1);
            auto feedback = std::make_shared<Fibonacci::Feedback>();
            feedback->sequence = {0, 1};
            handle->publish_feedback(feedback);
            auto result = std::make_shared<Fibonacci::Result>();
            result->sequence = fibonacci(handle->get_goal()->order);
            handle->succeed(result);
        });
    assert(ros_server);

    const auto availability_timeout = dmw::WaitTimeout::finite(15s);
    assert(availability_timeout);
    const auto available = dmw_client.value()->wait_for_server(availability_timeout.value());
    assert(available && available.value());

    // --- ROS 2 action client -> DMW ActionServer -------------------------------
    auto server_wait_set = context.value()->create_wait_set();
    assert(server_wait_set);
    const auto server_token = server_wait_set.value()->add(*dmw_server.value());
    assert(server_token);
    std::atomic<bool> stop_server{false};
    std::thread dmw_server_thread([&] {
        std::vector<dmw::RequestId> pending_result_requests;
        const auto respond_result = [&](const dmw::GoalId& goal_id,
                                        const dmw::RequestId& request_id) {
            Fibonacci::Impl::GetResultService::Response response{};
            response.status = kStatusSucceeded;
            {
                std::lock_guard lock(dmw_state.mutex);
                response.result.sequence = dmw_state.result;
                response.status = dmw_state.terminal_status;
            }
            (void)dmw_server.value()->write_result_response(request_id, &response);
        };
        while (!stop_server.load()) {
            // A real Action server publishes periodic feedback during
            // execution; do the same so the ROS 2 client observes feedback
            // after it has processed the goal response.
            enum class FinishKind { None, Succeed, Abort };
            FinishKind finish_kind = FinishKind::None;
            unique_identifier_msgs::msg::UUID finished_goal{};
            {
                std::lock_guard lock(dmw_state.mutex);
                if (dmw_state.active && dmw_state.feedback_rounds < kFeedbackRounds) {
                    Fibonacci::Impl::FeedbackMessage feedback{};
                    feedback.goal_id = dmw_state.active_goal;
                    feedback.feedback.sequence = {0, 1};
                    assert(dmw_server.value()->publish_feedback(&feedback));
                    dmw_state.feedback_sent.fetch_add(1);
                    ++dmw_state.feedback_rounds;
                } else if (dmw_state.active &&
                           (dmw_state.active_order != kCancelOrder || stop_server.load())) {
                    // kCancelOrder stays executing until a CancelGoal arrives.
                    dmw_state.result = fibonacci(dmw_state.active_order);
                    finish_kind = dmw_state.active_order == kAbortOrder ? FinishKind::Abort
                                                                        : FinishKind::Succeed;
                    dmw_state.terminal_status =
                        finish_kind == FinishKind::Abort ? kStatusAborted : kStatusSucceeded;
                    finished_goal = dmw_state.active_goal;
                    dmw_state.active = false;
                }
            }
            if (finish_kind != FinishKind::None) {
                const std::int8_t terminal_status = finish_kind == FinishKind::Abort
                                                        ? kStatusAborted
                                                        : kStatusSucceeded;
                assert(dmw_server.value()->update_goal_state(
                    to_goal_id(finished_goal), finish_kind == FinishKind::Abort
                                                  ? dmw::GoalEvent::Abort
                                                  : dmw::GoalEvent::Succeed));
                publish_goal_status(*dmw_server.value(), finished_goal, terminal_status);
                // Parked GetResult requests are handed over now that the goal is
                // terminal, exactly as the DMW contract describes.
                const auto pending =
                    dmw_server.value()->take_pending_result_requests(to_goal_id(finished_goal));
                if (pending) {
                    for (const auto& request_id : pending.value()) {
                        respond_result(to_goal_id(finished_goal), request_id);
                    }
                }
            }
            const auto timeout = dmw::WaitTimeout::finite(100ms);
            if (!timeout) break;
            const auto signalled = server_wait_set.value()->wait(timeout.value());
            if (!signalled) break;
            if (signalled.value().status() != dmw::WaitStatus::Ready) continue;
            for (const auto& ready : signalled.value().ready()) {
                if (ready.kind != dmw::WaitableKind::ActionServer) continue;
                const auto detail = dmw::action_server_ready_set(ready.detail_mask);

                if (detail.goal_request) {
                    {
                        std::lock_guard lock(dmw_state.mutex);
                        // This test server handles one goal at a time; a second
                        // request stays pending in DMW until the first finishes.
                        if (dmw_state.active) continue;
                    }
                    Fibonacci::Impl::SendGoalService::Request request{};
                    dmw::RequestId request_id;
                    const auto read = dmw_server.value()->read_goal_request(&request, request_id);
                    if (!read || !read.value()) continue;
                    Fibonacci::Impl::SendGoalService::Response response{};
                    // Rejected goals never create a GoalRecord: DMW answers the
                    // raw response path and no state is committed.
                    if (request.goal.order == kRejectOrder) {
                        response.accepted = false;
                        assert(dmw_server.value()->write_goal_response(request_id, &response));
                        continue;
                    }
                    response.accepted = true;
                    dmw::GoalInfo goal_info;
                    goal_info.goal_id = to_goal_id(request.goal_id);
                    goal_info.accepted_stamp = 1ns;
                    const auto accepted = dmw_server.value()->accept_goal(
                        request_id, goal_info, &response, dmw::GoalAcceptMode::Execute);
                    if (!accepted) {
                        response.accepted = false;
                        (void)dmw_server.value()->write_goal_response(request_id, &response);
                        continue;
                    }
                    publish_goal_status(*dmw_server.value(), request.goal_id, kStatusExecuting);
                    {
                        std::lock_guard lock(dmw_state.mutex);
                        dmw_state.active = true;
                        dmw_state.active_goal = request.goal_id;
                        dmw_state.active_order = request.goal.order;
                        dmw_state.feedback_rounds = 0;
                    }
                }

                if (detail.cancel_request) {
                    action_msgs::srv::CancelGoal::Request request{};
                    dmw::RequestId request_id;
                    const auto read =
                        dmw_server.value()->read_cancel_request(&request, request_id);
                    if (!read || !read.value()) continue;
                    action_msgs::srv::CancelGoal::Response response{};
                    dmw::CancelGoalCriteria criteria;
                    criteria.goal_id = to_goal_id(request.goal_info.goal_id);
                    const auto selection =
                        dmw_server.value()->select_cancel_goals(criteria);
                    if (selection && !selection.value().goals.empty()) {
                        response.return_code = kCancelNone;
                        for (const auto& goal : selection.value().goals) {
                            action_msgs::msg::GoalInfo info{};
                            std::copy(
                                goal.goal_id.data.begin(), goal.goal_id.data.end(),
                                info.goal_id.uuid.begin());
                            response.goals_canceling.push_back(info);
                        }
                    }
                    // Real ROS 2 action servers answer the CancelGoal request
                    // before the goal reaches its terminal state: the client
                    // needs that response to know the goal is canceling, and
                    // only then can it consume a terminal status and result.
                    (void)dmw_server.value()->write_cancel_response(request_id, &response);
                    if (response.return_code == kCancelNone) {
                        for (const auto& goal : selection.value().goals) {
                            (void)dmw_server.value()->update_goal_state(
                                goal.goal_id, dmw::GoalEvent::CancelGoal);
                            (void)dmw_server.value()->update_goal_state(
                                goal.goal_id, dmw::GoalEvent::Canceled);
                        }
                        publish_goal_status(
                            *dmw_server.value(), request.goal_info.goal_id, kStatusCanceled);
                        {
                            std::lock_guard lock(dmw_state.mutex);
                            dmw_state.terminal_status = kStatusCanceled;
                            dmw_state.active = false;
                            dmw_state.result.clear();
                        }
                        // Parked GetResult requests are answered with the CANCELED
                        // terminal status through the same DMW handover path.
                        const auto parked = dmw_server.value()->take_pending_result_requests(
                            to_goal_id(request.goal_info.goal_id));
                        if (parked) {
                            for (const auto& parked_id : parked.value()) {
                                respond_result(
                                    to_goal_id(request.goal_info.goal_id), parked_id);
                            }
                        }
                    }
                }

                if (detail.result_request) {
                    Fibonacci::Impl::GetResultService::Request request{};
                    dmw::RequestId request_id;
                    const auto read =
                        dmw_server.value()->read_result_request(&request, request_id);
                    if (!read || !read.value()) continue;
                    const auto goal_id = to_goal_id(request.goal_id);
                    const auto disposition =
                        dmw_server.value()->register_result_request(goal_id, request_id);
                    if (!disposition) continue;
                    if (disposition.value() == dmw::ResultRequestDisposition::Terminal) {
                        respond_result(goal_id, request_id);
                    } else if (disposition.value() == dmw::ResultRequestDisposition::Pending) {
                        pending_result_requests.push_back(request_id);
                    }
                }
            }
        }
    });

    {
        auto goal = Fibonacci::Goal();
        goal.order = 6;
        std::atomic<int> client_feedback{0};
        rclcpp_action::Client<Fibonacci>::SendGoalOptions client_options;
        client_options.feedback_callback = [&client_feedback](
                                               rclcpp_action::ClientGoalHandle<Fibonacci>::SharedPtr,
                                               const std::shared_ptr<const Fibonacci::Feedback>) {
            client_feedback.fetch_add(1);
        };
        auto goal_future = ros_client->async_send_goal(goal, client_options);
        assert(pump_until([&] { return goal_future.wait_for(0s) == std::future_status::ready; }));
        auto goal_handle = goal_future.get();
        assert(goal_handle);
        auto result_future = ros_client->async_get_result(goal_handle);
        assert(pump_until([&] { return result_future.wait_for(0s) == std::future_status::ready; }));
        const auto wrapped = result_future.get();
        assert(wrapped.code == rclcpp_action::ResultCode::SUCCEEDED);
        assert(wrapped.result);
        const std::vector<std::int32_t> expected{0, 1, 1, 2, 3, 5};
        assert(wrapped.result->sequence == expected);
        assert(client_feedback.load() >= 1);
        assert(dmw_state.feedback_sent.load() >= 1);
    }

    DMW_PHASE("succeed");
    // --- rejected goal: no GoalRecord is created on the DMW side ---------------
    {
        auto goal = Fibonacci::Goal();
        goal.order = kRejectOrder;
        auto goal_future = ros_client->async_send_goal(goal);
        assert(pump_until([&] { return goal_future.wait_for(0s) == std::future_status::ready; }));
        assert(goal_future.get() == nullptr);
    }

    DMW_PHASE("rejected");
    // --- aborted goal: DMW drives Executing -> Aborted --------------------------
    {
        auto goal = Fibonacci::Goal();
        goal.order = kAbortOrder;
        auto goal_future = ros_client->async_send_goal(goal);
        assert(pump_until([&] { return goal_future.wait_for(0s) == std::future_status::ready; }));
        auto handle = goal_future.get();
        assert(handle);
        auto result_future = ros_client->async_get_result(handle);
        assert(pump_until([&] { return result_future.wait_for(0s) == std::future_status::ready; }));
        const auto wrapped = result_future.get();
        assert(wrapped.code == rclcpp_action::ResultCode::ABORTED);
    }

    DMW_PHASE("aborted");
    // --- canceled goal: ROS 2 client cancel -> DMW Canceling -> Canceled --------
    {
        auto goal = Fibonacci::Goal();
        goal.order = kCancelOrder;
        auto goal_future = ros_client->async_send_goal(goal);
        assert(pump_until([&] { return goal_future.wait_for(0s) == std::future_status::ready; }));
        auto handle = goal_future.get();
        assert(handle);
        // Cancel first, then ask for the result: the client only requests a
        // result once the goal is terminal, and a GetResult request that the
        // client parks before canceling may be discarded on cancel by newer
        // rclcpp_action releases.  The parked-request path itself is covered by
        // the succeed scenario above.
        auto cancel_future = ros_client->async_cancel_goal(handle);
        assert(pump_until([&] { return cancel_future.wait_for(0s) == std::future_status::ready; }));
        const auto cancel_response = cancel_future.get();
        assert(cancel_response != nullptr);
        assert(cancel_response->return_code == 0);  // ERROR_NONE
        assert(cancel_response->goals_canceling.size() == 1);
        auto result_future = ros_client->async_get_result(handle);
        assert(pump_until([&] { return result_future.wait_for(0s) == std::future_status::ready; }));
        const auto wrapped = result_future.get();
        assert(wrapped.code == rclcpp_action::ResultCode::CANCELED);
    }

    DMW_PHASE("canceled");
    // --- concurrent goals from two ROS 2 clients --------------------------------
    {
        auto second_client = rclcpp_action::create_client<Fibonacci>(ros_node, "/dmw_ros2_action");
        assert(second_client->wait_for_action_server(15s));
        auto first_goal = Fibonacci::Goal();
        first_goal.order = 4;
        auto second_goal = Fibonacci::Goal();
        second_goal.order = 5;
        auto first_future = ros_client->async_send_goal(first_goal);
        auto second_future = second_client->async_send_goal(second_goal);
        assert(pump_until([&] {
            return first_future.wait_for(0s) == std::future_status::ready &&
                   second_future.wait_for(0s) == std::future_status::ready;
        }));
        auto first_handle = first_future.get();
        auto second_handle = second_future.get();
        assert(first_handle && second_handle);
        auto first_result = ros_client->async_get_result(first_handle);
        auto second_result = second_client->async_get_result(second_handle);
        assert(pump_until([&] {
            return first_result.wait_for(0s) == std::future_status::ready &&
                   second_result.wait_for(0s) == std::future_status::ready;
        }));
        const auto first_wrapped = first_result.get();
        const auto second_wrapped = second_result.get();
        assert(first_wrapped.code == rclcpp_action::ResultCode::SUCCEEDED);
        assert(second_wrapped.code == rclcpp_action::ResultCode::SUCCEEDED);
        const std::vector<std::int32_t> first_expected{0, 1, 1, 2};
        const std::vector<std::int32_t> second_expected{0, 1, 1, 2, 3};
        assert(first_wrapped.result->sequence == first_expected);
        assert(second_wrapped.result->sequence == second_expected);
    }

    // --- DMW ActionClient -> ROS 2 action server -------------------------------
    {
        auto client_wait_set = context.value()->create_wait_set();
        assert(client_wait_set);
        const auto client_token = client_wait_set.value()->add(*dmw_client.value());
        assert(client_token);

        Fibonacci::Impl::SendGoalService::Request request{};
        request.goal_id = make_uuid(7);
        request.goal.order = 5;
        const auto written = dmw_client.value()->write_goal_request(&request);
        assert(written);

        bool goal_accepted = false;
        Fibonacci::Impl::SendGoalService::Response goal_response{};
        for (int attempt = 0; attempt < 100 && !goal_accepted; ++attempt) {
            // The ROS 2 action server runs on the ROS node, so both sides must
            // be driven from this thread.
            rclcpp::spin_some(ros_node);
            const auto timeout = dmw::WaitTimeout::finite(50ms);
            assert(timeout);
            const auto signalled = client_wait_set.value()->wait(timeout.value());
            assert(signalled);
            if (signalled.value().status() != dmw::WaitStatus::Ready) continue;
            for (const auto& ready : signalled.value().ready()) {
                if (ready.kind != dmw::WaitableKind::ActionClient) continue;
                const auto detail = dmw::action_client_ready_set(ready.detail_mask);
                if (!detail.goal_response) continue;
                dmw::RequestId response_id;
                const auto read =
                    dmw_client.value()->read_goal_response(&goal_response, response_id);
                assert(read);
                if (read.value()) goal_accepted = true;
            }
        }
        assert(goal_accepted);
        assert(goal_response.accepted);
        // The accepted callback runs on the ROS executor, so allow it to catch up.
        assert(pump_until([&] { return ros_goals.load() >= 1; }));
        assert(ros_goals.load() == 1);

        Fibonacci::Impl::GetResultService::Request result_request{};
        result_request.goal_id = request.goal_id;

        bool got_result = false;
        Fibonacci::Impl::GetResultService::Response result_response{};
        // A volatile GetResult request can be written before the ROS 2 server's
        // response writer is matched, so a real client re-sends it.  Do the
        // same here until one terminal result comes back.
        for (int attempt = 0; attempt < 20 && !got_result; ++attempt) {
            assert(dmw_client.value()->write_result_request(&result_request));
            for (int inner = 0; inner < 20 && !got_result; ++inner) {
                rclcpp::spin_some(ros_node);
                const auto timeout = dmw::WaitTimeout::finite(50ms);
                assert(timeout);
                const auto signalled = client_wait_set.value()->wait(timeout.value());
                assert(signalled);
                if (signalled.value().status() != dmw::WaitStatus::Ready) continue;
                for (const auto& ready : signalled.value().ready()) {
                    if (ready.kind != dmw::WaitableKind::ActionClient) continue;
                    const auto detail = dmw::action_client_ready_set(ready.detail_mask);
                    if (!detail.result_response) continue;
                    dmw::RequestId response_id;
                    const auto read =
                        dmw_client.value()->read_result_response(&result_response, response_id);
                    assert(read);
                    if (read.value() && result_response.status == kStatusSucceeded) {
                        got_result = true;
                    }
                }
            }
        }
        assert(got_result);
        assert(result_response.status == kStatusSucceeded);
        const std::vector<std::int32_t> expected{0, 1, 1, 2, 3};
        assert(result_response.result.sequence == expected);
        assert(client_wait_set.value()->remove(client_token.value()));
    }

    DMW_PHASE("concurrent");
    stop_server.store(true);
    dmw_server_thread.join();
    assert(server_wait_set.value()->remove(server_token.value()));

    assert(context.value()->shutdown());
    rclcpp::shutdown();
    return 0;
}
