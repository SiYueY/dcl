#ifndef DMW_NODE_HPP_
#define DMW_NODE_HPP_

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "dmw/client.hpp"
#include "dmw/arguments.hpp"
#include "dmw/action_client.hpp"
#include "dmw/action_server.hpp"
#include "dmw/action_type.hpp"
#include "dmw/message_type.hpp"
#include "dmw/parameter.hpp"
#include "dmw/parameter_change_set.hpp"
#include "dmw/parameter_descriptor.hpp"
#include "dmw/publisher.hpp"
#include "dmw/qos.hpp"
#include "dmw/result.hpp"
#include "dmw/server.hpp"
#include "dmw/service_type.hpp"
#include "dmw/subscriber.hpp"
#include "dmw/visibility_control.hpp"

namespace dmw {

class Context;

struct NodeOptions {
    std::string node_name;
    std::string node_namespace{"/"};
    Arguments arguments;
    bool use_global_arguments{true};
    bool allow_undeclared_parameters{false};
};

/// Logical communication identity owned by one Context.
class DMW_PUBLIC Node {
public:
    ~Node() noexcept;

    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;
    Node(Node&&) = delete;
    Node& operator=(Node&&) = delete;

    std::string_view name() const noexcept;
    std::string_view node_namespace() const noexcept;
    /// Normalized `/<namespace>/<name>` identity used by graph consumers.
    std::string_view fully_qualified_name() const noexcept;

    /// Frozen override table selected for this Node's identity.
    const std::vector<Parameter>& parameter_overrides() const noexcept;

    /// Declare one parameter, applying a matching override unless ignored.
    Result<Parameter> declare_parameter(
        std::string_view name, const ParameterValue& default_value,
        const ParameterDescriptor& descriptor = {}, bool ignore_override = false);

    /// Remove a declared parameter and record it in the pending change set.
    Result<void> undeclare_parameter(std::string_view name);

    Result<bool> has_parameter(std::string_view name) const;

    Result<Parameter> get_parameter(std::string_view name) const;

    Result<std::vector<Parameter>> get_parameters(const std::vector<std::string>& names) const;

    Result<ParameterDescriptor> describe_parameter(std::string_view name) const;

    Result<ParameterListResult> list_parameters(
        const std::vector<std::string>& prefixes = {}, std::size_t depth = 0) const;

    /// Validate a whole request without mutating the store.
    Result<void> validate_parameters(const std::vector<Parameter>& parameters) const;

    /// Atomically commit a parameter request and return its delta.
    Result<ParameterChangeSet> set_parameters_atomically(
        const std::vector<Parameter>& parameters);

    /// Return and clear the accumulated new/changed/deleted delta.
    Result<ParameterChangeSet> take_parameter_changes();

    /// Transactionally create a complete Publisher or return an Error.
    Result<std::unique_ptr<Publisher>> create_publisher(
        const MessageType& type, std::string_view topic_name, const Qos& qos,
        const PublisherOptions& options = {});

    /// Transactionally create a complete Subscriber or return an Error.
    Result<std::unique_ptr<Subscriber>> create_subscriber(
        const MessageType& type, std::string_view topic_name, const Qos& qos,
        const SubscriberOptions& options = {});

    /// Transactionally create both Client DDS endpoints or return an Error.
    Result<std::unique_ptr<Client>> create_client(
        const ServiceType& type, std::string_view service_name, const Qos& qos,
        const ClientOptions& options = {});

    /// Transactionally create both Server DDS endpoints or return an Error.
    Result<std::unique_ptr<Server>> create_server(
        const ServiceType& type, std::string_view service_name, const Qos& qos,
        const ServerOptions& options = {});

    /// Create all five ActionClient constituents or return an Error.
    Result<std::unique_ptr<ActionClient>> create_action_client(
        const ActionType& type, std::string_view action_name,
        const ActionClientOptions& options = {});

    /// Create all five ActionServer constituents or return an Error.
    Result<std::unique_ptr<ActionServer>> create_action_server(
        const ActionType& type, std::string_view action_name,
        const ActionServerOptions& options = {});

private:
    friend class Context;

    class Impl;

    explicit Node(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}  // namespace dmw

#endif  // DMW_NODE_HPP_
