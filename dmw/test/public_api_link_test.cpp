#include "dmw/client.hpp"
#include "dmw/context.hpp"
#include "dmw/event.hpp"
#include "dmw/guard_condition.hpp"
#include "dmw/node.hpp"
#include "dmw/publisher.hpp"
#include "dmw/server.hpp"
#include "dmw/subscriber.hpp"
#include "dmw/wait_set.hpp"

int main() {
    [[maybe_unused]] auto context_create = &dmw::Context::create;
    [[maybe_unused]] auto context_shutdown = &dmw::Context::shutdown;
    [[maybe_unused]] auto context_node = &dmw::Context::create_node;
    [[maybe_unused]] auto context_wait_set = &dmw::Context::create_wait_set;
    [[maybe_unused]] auto context_guard = &dmw::Context::create_guard_condition;

    [[maybe_unused]] auto node_publisher = &dmw::Node::create_publisher;
    [[maybe_unused]] auto node_subscriber = &dmw::Node::create_subscriber;
    [[maybe_unused]] auto node_client = &dmw::Node::create_client;
    [[maybe_unused]] auto node_server = &dmw::Node::create_server;

    [[maybe_unused]] auto write = &dmw::Publisher::write;
    [[maybe_unused]] auto publisher_event = &dmw::Publisher::create_event;
    [[maybe_unused]] auto read = &dmw::Subscriber::read;
    [[maybe_unused]] auto subscriber_event = &dmw::Subscriber::create_event;
    [[maybe_unused]] auto request = &dmw::Client::send_request;
    [[maybe_unused]] auto response = &dmw::Client::take_response;
    [[maybe_unused]] auto availability = &dmw::Client::service_is_available;
    [[maybe_unused]] auto server_take = &dmw::Server::take_request;
    [[maybe_unused]] auto server_response = &dmw::Server::send_response;

    [[maybe_unused]] auto guard_trigger = &dmw::GuardCondition::trigger;
    [[maybe_unused]] auto event_take = &dmw::Event::take;
    [[maybe_unused]] auto wait_subscriber =
        static_cast<dmw::Result<dmw::WaitToken> (dmw::WaitSet::*)(dmw::Subscriber&)>(
            &dmw::WaitSet::add);
    [[maybe_unused]] auto wait_client =
        static_cast<dmw::Result<dmw::WaitToken> (dmw::WaitSet::*)(dmw::Client&)>(
            &dmw::WaitSet::add);
    [[maybe_unused]] auto wait_server =
        static_cast<dmw::Result<dmw::WaitToken> (dmw::WaitSet::*)(dmw::Server&)>(
            &dmw::WaitSet::add);
    [[maybe_unused]] auto wait_event =
        static_cast<dmw::Result<dmw::WaitToken> (dmw::WaitSet::*)(dmw::Event&)>(&dmw::WaitSet::add);
    [[maybe_unused]] auto wait_guard =
        static_cast<dmw::Result<dmw::WaitToken> (dmw::WaitSet::*)(dmw::GuardCondition&)>(
            &dmw::WaitSet::add);
    [[maybe_unused]] auto wait_remove = &dmw::WaitSet::remove;
    [[maybe_unused]] auto wait = &dmw::WaitSet::wait;
    return 0;
}
