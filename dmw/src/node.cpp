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

std::string_view Node::fully_qualified_name() const noexcept {
    return impl_->fully_qualified_name();
}

const std::vector<Parameter>& Node::parameter_overrides() const noexcept {
    return impl_->parameters().overrides();
}

Result<Parameter> Node::declare_parameter(
    std::string_view name, const ParameterValue& default_value,
    const ParameterDescriptor& descriptor, bool ignore_override) {
    return impl_->declare_parameter(name, default_value, descriptor, ignore_override);
}

Result<void> Node::undeclare_parameter(std::string_view name) {
    return impl_->undeclare_parameter(name);
}

Result<bool> Node::has_parameter(std::string_view name) const {
    return impl_->has_parameter(name);
}

Result<Parameter> Node::get_parameter(std::string_view name) const {
    return impl_->get_parameter(name);
}

Result<std::vector<Parameter>> Node::get_parameters(const std::vector<std::string>& names) const {
    return impl_->get_parameters(names);
}

Result<ParameterDescriptor> Node::describe_parameter(std::string_view name) const {
    return impl_->describe_parameter(name);
}

Result<ParameterListResult> Node::list_parameters(
    const std::vector<std::string>& prefixes, std::size_t depth) const {
    return impl_->list_parameters(prefixes, depth);
}

Result<void> Node::validate_parameters(const std::vector<Parameter>& parameters) const {
    return impl_->validate_parameters(parameters);
}

Result<ParameterChangeSet> Node::set_parameters_atomically(
    const std::vector<Parameter>& parameters) {
    return impl_->set_parameters_atomically(parameters);
}

Result<ParameterChangeSet> Node::take_parameter_changes() {
    return impl_->take_parameter_changes();
}

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

Result<std::unique_ptr<ActionClient>> Node::create_action_client(
    const ActionType& type, std::string_view action_name, const ActionClientOptions& options) {
    return impl_->create_action_client(type, action_name, options);
}

Result<std::unique_ptr<ActionServer>> Node::create_action_server(
    const ActionType& type, std::string_view action_name, const ActionServerOptions& options) {
    return impl_->create_action_server(type, action_name, options);
}

}  // namespace dmw
