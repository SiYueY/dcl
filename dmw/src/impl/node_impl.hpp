#ifndef DMW_IMPL__NODE_IMPL_HPP_
#define DMW_IMPL__NODE_IMPL_HPP_

#include <memory>
#include <cstdint>
#include <string>
#include <vector>

#include "dmw/node.hpp"
#include "impl/context.hpp"
#include "impl/parameter_store.hpp"

namespace dmw {

class Node::Impl {
public:
    Impl(
        std::shared_ptr<impl::Context> context, std::string name,
        std::string node_namespace, std::vector<RemapRule> remaps,
        std::vector<ParameterOverride> parameter_overrides, bool allow_undeclared_parameters);
    ~Impl() noexcept;

    std::string_view name() const noexcept { return name_; }
    std::string_view node_namespace() const noexcept { return node_namespace_; }
    std::string_view fully_qualified_name() const noexcept { return fully_qualified_name_; }
    const impl::ParameterStoreState& parameters() const noexcept { return *parameters_; }
    impl::ParameterStoreState& parameters() noexcept { return *parameters_; }
    bool graph_registered() const noexcept { return node_id_ != 0; }
    Result<std::unique_ptr<Publisher>> create_publisher(
        const MessageType&, std::string_view, const Qos&, const PublisherOptions&);
    Result<std::unique_ptr<Subscriber>> create_subscriber(
        const MessageType&, std::string_view, const Qos&, const SubscriberOptions&);
    Result<std::unique_ptr<Client>> create_client(
        const ServiceType&, std::string_view, const Qos&, const ClientOptions&);
    Result<std::unique_ptr<Server>> create_server(
        const ServiceType&, std::string_view, const Qos&, const ServerOptions&);
    Result<std::unique_ptr<ActionClient>> create_action_client(
        const ActionType&, std::string_view, const ActionClientOptions&);
    Result<std::unique_ptr<ActionServer>> create_action_server(
        const ActionType&, std::string_view, const ActionServerOptions&);
    Result<Parameter> declare_parameter(
        std::string_view name, const ParameterValue& default_value,
        const ParameterDescriptor& descriptor, bool ignore_override);
    Result<void> undeclare_parameter(std::string_view name);
    Result<bool> has_parameter(std::string_view name) const;
    Result<Parameter> get_parameter(std::string_view name) const;
    Result<std::vector<Parameter>> get_parameters(const std::vector<std::string>& names) const;
    Result<ParameterDescriptor> describe_parameter(std::string_view name) const;
    Result<ParameterListResult> list_parameters(
        const std::vector<std::string>& prefixes, std::size_t depth) const;
    Result<void> validate_parameters(const std::vector<Parameter>& parameters) const;
    Result<ParameterChangeSet> set_parameters_atomically(
        const std::vector<Parameter>& parameters);
    Result<ParameterChangeSet> take_parameter_changes();

private:
    std::shared_ptr<impl::Context> context_;
    std::string name_;
    std::string node_namespace_;
    std::string fully_qualified_name_;
    std::vector<RemapRule> remaps_;
    std::unique_ptr<impl::ParameterStoreState> parameters_;
    std::uint64_t node_id_{0};
};

}  // namespace dmw

#endif  // DMW_IMPL__NODE_IMPL_HPP_
