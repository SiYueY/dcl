#include "dmw/node.hpp"

#include <utility>

#include "impl/node_impl.hpp"
#include "impl/client_impl.hpp"
#include "impl/publisher_impl.hpp"
#include "impl/server_impl.hpp"
#include "impl/subscriber_impl.hpp"

namespace dmw {

Node::Node(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Node::~Node() noexcept = default;

std::string_view Node::name() const noexcept { return impl_->name(); }

std::string_view Node::node_namespace() const noexcept { return impl_->node_namespace(); }

Result<std::unique_ptr<Publisher>> Node::create_publisher(
    const MessageType& type, std::string_view name, const Qos& qos,
    const PublisherOptions& options) {
    return impl_->create_publisher(type, name, qos, options);
}

Result<std::unique_ptr<Subscriber>> Node::create_subscriber(
    const MessageType& type, std::string_view name, const Qos& qos,
    const SubscriberOptions& options) {
    return impl_->create_subscriber(type, name, qos, options);
}

Result<std::unique_ptr<Client>> Node::create_client(
    const ServiceType& type, std::string_view name, const Qos& qos, const ClientOptions& options) {
    return impl_->create_client(type, name, qos, options);
}

Result<std::unique_ptr<Server>> Node::create_server(
    const ServiceType& type, std::string_view name, const Qos& qos, const ServerOptions& options) {
    return impl_->create_server(type, name, qos, options);
}

}  // namespace dmw
