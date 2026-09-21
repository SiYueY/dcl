#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "dmw/action_client.hpp"
#include "dmw/action_common.hpp"
#include "dmw/action_server.hpp"
#include "dmw/action_type.hpp"
#include "dmw/context.hpp"
#include "dmw/fastdds/message_type.hpp"
#include "dmw/node.hpp"
#include "dmw/service_type.hpp"
#include "dmw/wait_set.hpp"

namespace {

using namespace std::chrono_literals;

/// Wire payload shared by every endpoint of the test Action.
struct ActionPayload {
    std::uint8_t goal_id[16];
    std::int32_t value;
};

class ActionPayloadType : public eprosima::fastdds::dds::TopicDataType {
public:
    ActionPayloadType() {
        m_typeSize = sizeof(ActionPayload);
        setName("dmw.test.ActionPayload");
    }

    bool serialize(void* data, eprosima::fastrtps::rtps::SerializedPayload_t* payload) override {
        payload->length = sizeof(ActionPayload);
        std::memcpy(payload->data, data, sizeof(ActionPayload));
        return true;
    }

    bool deserialize(eprosima::fastrtps::rtps::SerializedPayload_t* payload, void* data) override {
        if (payload->length != sizeof(ActionPayload)) return false;
        std::memcpy(data, payload->data, sizeof(ActionPayload));
        return true;
    }

    std::function<std::uint32_t()> getSerializedSizeProvider(void*) override {
        return [] { return static_cast<std::uint32_t>(sizeof(ActionPayload)); };
    }

    void* createData() override { return new ActionPayload{}; }

    void deleteData(void* data) override { delete static_cast<ActionPayload*>(data); }

    bool getKey(void*, eprosima::fastrtps::rtps::InstanceHandle_t*, bool) override {
        return false;
    }
};

ActionPayload make_payload(std::uint8_t seed, std::int32_t value) {
    ActionPayload payload{};
    payload.goal_id[0] = seed;
    payload.goal_id[15] = seed;
    payload.value = value;
    return payload;
}

dmw::GoalId goal_id_of(const ActionPayload& payload) {
    dmw::GoalId goal_id;
    std::memcpy(goal_id.data.data(), payload.goal_id, goal_id.data.size());
    return goal_id;
}

}  // namespace

int main() {
    if (std::getenv("DMW_ENABLE_DDS_INTEGRATION") == nullptr) return 0;

    const auto message_type = dmw::fastdds::create_message_type<ActionPayloadType, ActionPayload>();
    assert(message_type);
    const dmw::ServiceType service_type(message_type.value(), message_type.value());
    const dmw::ActionType action_type(
        service_type, service_type, service_type, message_type.value(), message_type.value());

    auto server_context = dmw::Context::create({});
    assert(server_context);
    dmw::NodeOptions server_node_options;
    server_node_options.node_name = "action_server";
    auto server_node = server_context.value()->create_node(server_node_options);
    assert(server_node);

    auto client_context = dmw::Context::create({});
    assert(client_context);
    dmw::NodeOptions client_node_options;
    client_node_options.node_name = "action_client";
    auto client_node = client_context.value()->create_node(client_node_options);
    assert(client_node);

    dmw::ActionServerOptions server_options;
    server_options.result_timeout = 200ms;
    auto server = server_node.value()->create_action_server(action_type, "/dmw/move", server_options);
    assert(server);
    assert(server.value()->action_name() == "/dmw/move");

    auto client = client_node.value()->create_action_client(action_type, "/dmw/move");
    assert(client);
    assert(client.value()->action_name() == "/dmw/move");

    // The five constituent endpoints must appear in the public graph as one
    // Action candidate per side.
    {
        const auto snapshot = client_context.value()->graph_snapshot();
        assert(snapshot);
        bool saw_client_candidate = false;
        for (const auto& action : snapshot.value().actions) {
            if (action.action_name != "/dmw/move") continue;
            // The client Context always contributes one candidate; once the
            // peer's endpoints are discovered its own Client composition is a
            // second, equally valid candidate.
            assert(action.client_candidate_count >= 1);
            assert(action.client_candidate_count <= 2);
            saw_client_candidate = true;
        }
        assert(saw_client_candidate);
        bool saw_client_endpoint = false;
        for (const auto& endpoint : snapshot.value().action_endpoints) {
            if (endpoint.action_name != "/dmw/move") continue;
            assert(endpoint.endpoint_gids.size() == 5);
            if (endpoint.kind != dmw::ActionEndpointKind::Client) continue;
            saw_client_endpoint = true;
            assert(endpoint.node_name == "action_client");
        }
        assert(saw_client_endpoint);
    }

    // Action availability uses one participant-consistent composition of all
    // five endpoints.  Discovery is asynchronous, so wait for it rather than
    // assuming the graph is already complete.
    const auto wait_available = dmw::WaitTimeout::finite(5s);
    assert(wait_available);
    const auto available = client.value()->wait_for_server(wait_available.value());
    assert(available && available.value());
    assert(client.value()->server_is_available().value());

    // --- a rejected goal leaves no GoalRecord ---------------------------------
    {
        const auto request = make_payload(1, 11);
        const auto written = client.value()->write_goal_request(&request);
        assert(written);
        ActionPayload raw{};
        dmw::RequestId request_id;
        const auto read = server.value()->read_goal_request(&raw, request_id);
        assert(read && read.value());
        assert(goal_id_of(raw) == goal_id_of(request));
        const auto rejected = make_payload(1, -1);
        assert(server.value()->write_goal_response(request_id, &rejected));
        const auto state = server.value()->goal_state(goal_id_of(raw));
        assert(!state);
        assert(state.error().code() == dmw::ErrorCode::NotFound);
        ActionPayload response{};
        dmw::RequestId response_id;
        const auto response_read = client.value()->read_goal_response(&response, response_id);
        assert(response_read && response_read.value());
        assert(response.value == -1);
    }

    // --- accepted goal, feedback, status, result -----------------------------
    const auto goal_payload = make_payload(2, 22);
    const auto goal_id = goal_id_of(goal_payload);
    {
        const auto written = client.value()->write_goal_request(&goal_payload);
        assert(written);
        ActionPayload raw{};
        dmw::RequestId request_id;
        const auto read = server.value()->read_goal_request(&raw, request_id);
        assert(read && read.value());

        dmw::GoalInfo goal_info;
        goal_info.goal_id = goal_id_of(raw);
        goal_info.accepted_stamp = 100ns;
        const auto accepted_payload = make_payload(2, 1);
        const auto accepted = server.value()->accept_goal(
            request_id, goal_info, &accepted_payload, dmw::GoalAcceptMode::Execute);
        assert(accepted);
        assert(accepted.value().current == dmw::GoalState::Executing);
        assert(server.value()->goal_state(goal_id).value() == dmw::GoalState::Executing);

        ActionPayload response{};
        dmw::RequestId response_id;
        const auto response_read = client.value()->read_goal_response(&response, response_id);
        assert(response_read && response_read.value());
        assert(response.value == 1);
        assert(response_id == written.value());
    }

    // Duplicate accept of the same GoalId is rejected while it is committed.
    {
        const auto duplicate = make_payload(2, 22);
        const auto written = client.value()->write_goal_request(&duplicate);
        assert(written);
        ActionPayload raw{};
        dmw::RequestId request_id;
        assert(server.value()->read_goal_request(&raw, request_id).value());
        dmw::GoalInfo goal_info;
        goal_info.goal_id = goal_id_of(raw);
        const auto accepted_payload = make_payload(2, 1);
        const auto second = server.value()->accept_goal(
            request_id, goal_info, &accepted_payload, dmw::GoalAcceptMode::Defer);
        assert(!second);
        assert(second.error().code() == dmw::ErrorCode::AlreadyExists);
        // The reservation is dropped, so the request itself stays answerable.
        const auto rejected = make_payload(2, -2);
        assert(server.value()->write_goal_response(request_id, &rejected));
        ActionPayload response{};
        dmw::RequestId response_id;
        assert(client.value()->read_goal_response(&response, response_id).value());
        assert(response.value == -2);
    }

    // Feedback and status flow through the two topic endpoints.
    {
        const auto feedback = make_payload(2, 42);
        assert(server.value()->publish_feedback(&feedback));
        ActionPayload received{};
        dmw::MessageInfo info;
        bool got_feedback = false;
        for (int attempt = 0; attempt < 100 && !got_feedback; ++attempt) {
            const auto read = client.value()->read_feedback(&received, info);
            assert(read);
            got_feedback = read.value();
            if (!got_feedback) std::this_thread::sleep_for(10ms);
        }
        assert(got_feedback);
        assert(received.value == 42);

        const auto snapshot = server.value()->status_snapshot();
        assert(snapshot);
        assert(snapshot.value().size() == 1);
        assert(snapshot.value().front().state == dmw::GoalState::Executing);
        const auto status = make_payload(2, 7);
        assert(server.value()->publish_status(&status));
        ActionPayload received_status{};
        bool got_status = false;
        for (int attempt = 0; attempt < 100 && !got_status; ++attempt) {
            const auto read = client.value()->read_status(&received_status, info);
            assert(read);
            got_status = read.value();
            if (!got_status) std::this_thread::sleep_for(10ms);
        }
        assert(got_status);
        assert(received_status.value == 7);
    }

    // GetResult while the goal is active is parked, then handed over when the
    // goal becomes terminal.
    {
        const auto result_request = make_payload(2, 0);
        const auto written = client.value()->write_result_request(&result_request);
        assert(written);
        ActionPayload raw{};
        dmw::RequestId request_id;
        assert(server.value()->read_result_request(&raw, request_id).value());
        const auto parked = server.value()->register_result_request(
            goal_id_of(raw), request_id);
        assert(parked && parked.value() == dmw::ResultRequestDisposition::Pending);

        const auto succeed = server.value()->update_goal_state(goal_id, dmw::GoalEvent::Succeed);
        assert(succeed);
        assert(succeed.value().became_terminal);
        assert(server.value()->goal_state(goal_id).value() == dmw::GoalState::Succeeded);

        const auto pending = server.value()->take_pending_result_requests(goal_id);
        assert(pending && pending.value().size() == 1);
        const auto result_payload = make_payload(2, 99);
        assert(server.value()->write_result_response(
            pending.value().front(), &result_payload));
        ActionPayload response{};
        dmw::RequestId response_id;
        assert(client.value()->read_result_response(&response, response_id).value());
        assert(response.value == 99);

        // Late GetResult after terminal is answered from the typed cache by the
        // Client Library; DMW reports the Terminal disposition.
        const auto late_request = make_payload(2, 0);
        assert(client.value()->write_result_request(&late_request));
        assert(server.value()->read_result_request(&raw, request_id).value());
        const auto late = server.value()->register_result_request(goal_id_of(raw), request_id);
        assert(late && late.value() == dmw::ResultRequestDisposition::Terminal);
    }

    // Cancel selection only offers cancelable goals.
    {
        dmw::CancelGoalCriteria criteria;
        const auto selection = server.value()->select_cancel_goals(criteria);
        assert(selection);
        assert(selection.value().goals.empty());
    }

    // Aggregate readiness: one token, driven by the feedback channel.
    {
        auto wait_set = client_context.value()->create_wait_set();
        assert(wait_set);
        const auto registration = wait_set.value()->add(*client.value());
        assert(registration);
        assert(registration.value().kind() == dmw::WaitableKind::ActionClient);

        const auto ping = make_payload(2, 77);
        assert(server.value()->publish_feedback(&ping));
        const auto signalled = wait_set.value()->wait(dmw::WaitTimeout::infinite());
        assert(signalled);
        assert(signalled.value().status() == dmw::WaitStatus::Ready);
        assert(signalled.value().ready().size() == 1);
        const auto ready_waitable = signalled.value().ready().front();
        assert(ready_waitable.kind == dmw::WaitableKind::ActionClient);
        // The snapshot tells the Executor exactly which sub-channel fired.
        const auto detail = dmw::action_client_ready_set(ready_waitable.detail_mask);
        assert(detail.feedback);
        assert(detail.any());
        const auto readiness = client.value()->readiness();
        assert(readiness && readiness.value().feedback);
        assert(wait_set.value()->remove(registration.value()));
    }

    // One ActionServer token reports its own sub-channels, including the
    // logical goal_expired channel that no reader can express.
    {
        auto wait_set = server_context.value()->create_wait_set();
        assert(wait_set);
        const auto registration = wait_set.value()->add(*server.value());
        assert(registration);
        assert(registration.value().kind() == dmw::WaitableKind::ActionServer);

        const auto active_goal = make_payload(3, 33);
        assert(client.value()->write_goal_request(&active_goal));
        const auto signalled = wait_set.value()->wait(dmw::WaitTimeout::infinite());
        assert(signalled);
        const auto waitable = signalled.value().ready().front();
        assert(waitable.kind == dmw::WaitableKind::ActionServer);
        assert(dmw::action_server_ready_set(waitable.detail_mask).goal_request);

        // Accept and drive the goal to a terminal state, then wait for the
        // retention deadline to surface through the same token.
        ActionPayload raw{};
        dmw::RequestId request_id;
        assert(server.value()->read_goal_request(&raw, request_id).value());
        dmw::GoalInfo goal_info;
        goal_info.goal_id = goal_id_of(raw);
        const auto accepted_payload = make_payload(3, 1);
        assert(server.value()
                   ->accept_goal(
                       request_id, goal_info, &accepted_payload, dmw::GoalAcceptMode::Execute)
                   .value()
                   .current == dmw::GoalState::Executing);
        assert(server.value()->update_goal_state(goal_info.goal_id, dmw::GoalEvent::Succeed));

        const auto expired_wait = wait_set.value()->wait(dmw::WaitTimeout::infinite());
        assert(expired_wait);
        const auto expired_waitable = expired_wait.value().ready().front();
        assert(dmw::action_server_ready_set(expired_waitable.detail_mask).goal_expired);
        assert(!server.value()->take_expired_goals().value().empty());
        assert(wait_set.value()->remove(registration.value()));
    }

    // Result expiry prunes terminal goals after the retention timeout.
    {
        std::this_thread::sleep_for(250ms);
        const auto expired = server.value()->take_expired_goals();
        assert(expired);
        assert(!server.value()->goal_state(goal_id));
        assert(server.value()->status_snapshot().value().empty());
    }

    assert(server_context.value()->shutdown());
    const auto state_after_shutdown = server.value()->goal_state(goal_id);
    assert(!state_after_shutdown);
    assert(state_after_shutdown.error().code() == dmw::ErrorCode::ContextShutdown);
    const auto status_after_shutdown = server.value()->status_snapshot();
    assert(!status_after_shutdown);
    assert(status_after_shutdown.error().code() == dmw::ErrorCode::ContextShutdown);
    const auto readiness_after_shutdown = server.value()->readiness();
    assert(!readiness_after_shutdown);
    assert(readiness_after_shutdown.error().code() == dmw::ErrorCode::ContextShutdown);
    dmw::CancelGoalCriteria invalid_cancel;
    invalid_cancel.stamp = -1ns;
    const auto invalid_cancel_after_shutdown =
        server.value()->select_cancel_goals(invalid_cancel);
    assert(!invalid_cancel_after_shutdown);
    assert(invalid_cancel_after_shutdown.error().code() == dmw::ErrorCode::InvalidArgument);
    const auto cancel_after_shutdown =
        server.value()->select_cancel_goals(dmw::CancelGoalCriteria{});
    assert(!cancel_after_shutdown);
    assert(cancel_after_shutdown.error().code() == dmw::ErrorCode::ContextShutdown);

    assert(client_context.value()->shutdown());
    assert(!client.value()->write_goal_request(&goal_payload));
    const auto client_readiness_after_shutdown = client.value()->readiness();
    assert(!client_readiness_after_shutdown);
    assert(client_readiness_after_shutdown.error().code() == dmw::ErrorCode::ContextShutdown);
    return 0;
}
