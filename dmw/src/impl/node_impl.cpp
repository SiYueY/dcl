#include "impl/node_impl.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/rtps/common/InstanceHandle.h>

#include "dmw/error.hpp"
#include "impl/action_impl.hpp"
#include "impl/graph_names.hpp"
#include "impl/publisher_impl.hpp"
#include "impl/subscriber_impl.hpp"
#include "impl/qos.hpp"
#include "impl/process_lifetime.hpp"
#include "impl/name.hpp"
#include "impl/client_impl.hpp"
#include "impl/server_impl.hpp"
#include "impl/response.hpp"

namespace dmw {

namespace {

void delete_writer_noexcept(
    impl::Context& context, eprosima::fastdds::dds::DataWriter* writer) noexcept {
    if (writer == nullptr) return;
    try {
        context.publisher()->delete_datawriter(writer);
    } catch (...) {
        // The Context container is the final ownership barrier for a writer
        // whose individual deletion cannot be confirmed during rollback.
    }
}

void delete_reader_noexcept(
    impl::Context& context, eprosima::fastdds::dds::DataReader* reader) noexcept {
    if (reader == nullptr) return;
    try {
        context.subscriber()->delete_datareader(reader);
    } catch (...) {
        // See delete_writer_noexcept().
    }
}

template <typename Listener>
void delete_writer_listener_noexcept(
    impl::Context& context, eprosima::fastdds::dds::DataWriter* writer,
    std::unique_ptr<Listener>& listener) noexcept {
    if (writer == nullptr) return;
    bool detached = false;
    try {
        detached =
            writer->set_listener(nullptr) == eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK;
        if (detached && listener) listener->close_and_drain();
        if (detached) context.publisher()->delete_datawriter(writer);
    } catch (...) {
        detached = false;
    }
    if (!detached) {
        impl::ProcessLifetime::instance().retain_writer_listener(std::move(listener));
    }
}

template <typename Listener>
void delete_reader_listener_noexcept(
    impl::Context& context, eprosima::fastdds::dds::DataReader* reader,
    std::unique_ptr<Listener>& listener) noexcept {
    if (reader == nullptr) return;
    bool detached = false;
    try {
        detached =
            reader->set_listener(nullptr) == eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK;
        if (detached && listener) listener->close_and_drain();
        if (detached) context.subscriber()->delete_datareader(reader);
    } catch (...) {
        detached = false;
    }
    if (!detached) {
        impl::ProcessLifetime::instance().retain_reader_listener(std::move(listener));
    }
}

std::string dds_topic_name(const impl::Context& context, const std::string& logical_name) {
    const std::string path = logical_name.substr(1);
    if (context.runtime_mode() == RuntimeMode::ROS2) {
        return "rt/" + path;
    }
    return path;
}

std::string service_topic_name(
    const impl::Context& context, const std::string& logical_name, bool request) {
    const std::string path = logical_name.substr(1);
    if (context.runtime_mode() == RuntimeMode::ROS2) {
        return (request ? "rq/" : "rr/") + path + (request ? "Request" : "Reply");
    }
    return path + (request ? "_Request" : "_Reply");
}

/// Apply the remap rules that belong to this Node.
///
/// A rule whose source is written `<node>:` `name` only applies when `<node>`
/// matches this Node's name or fully qualified name, so one Context-level
/// argument list can address several Nodes.  Every rule is matched against the
/// original requested name (rules never chain), and the last matching rule
/// wins: Node-local rules are appended after the Context-level ones, so they
/// take precedence.
Result<std::string> resolve_remapped_name(
    std::string_view node_name, std::string_view node_namespace,
    const std::vector<RemapRule>& remaps, std::string_view value) {
    auto resolved = impl::resolve_name(node_namespace, value);
    if (!resolved) return resolved;
    const std::string original = resolved.value();
    std::string result = original;
    std::string_view unqualified_node = node_name;
    if (!unqualified_node.empty() && unqualified_node.front() == '/') {
        unqualified_node.remove_prefix(1);
    }
    std::string_view unqualified_namespace = node_namespace;
    if (!unqualified_namespace.empty() && unqualified_namespace.front() == '/') {
        unqualified_namespace.remove_prefix(1);
    }
    const std::string node_fqn =
        unqualified_namespace.empty()
            ? std::string(unqualified_node)
            : std::string(unqualified_namespace) + "/" + std::string(unqualified_node);
    for (const auto& remap : remaps) {
        std::string_view rule_source = remap.from;
        const auto separator = rule_source.find(':');
        if (separator != std::string_view::npos) {
            auto scope = rule_source.substr(0, separator);
            rule_source = rule_source.substr(separator + 1);
            if (!scope.empty() && scope.front() == '/') scope.remove_prefix(1);
            if (scope != unqualified_node && scope != node_fqn) continue;
        }
        auto source = impl::resolve_name(node_namespace, rule_source);
        if (!source) return Result<std::string>::failure(std::move(source.error()));
        if (source.value() != original) continue;
        auto target = impl::resolve_name(node_namespace, remap.to);
        if (!target) return Result<std::string>::failure(std::move(target.error()));
        result = std::move(target.value());
    }
    return Result<std::string>::success(std::move(result));
}

}  // namespace

#define impl_ this

Node::Impl::Impl(
    std::shared_ptr<impl::Context> context, std::string name, std::string node_namespace,
    std::vector<RemapRule> remaps, std::vector<ParameterOverride> parameter_overrides,
    bool allow_undeclared_parameters)
: context_(std::move(context)),
  name_(std::move(name)),
  node_namespace_(std::move(node_namespace)),
  fully_qualified_name_(node_namespace_ == "/" ? "/" + name_ : node_namespace_ + "/" + name_),
  remaps_(std::move(remaps)),
  parameters_(std::make_unique<impl::ParameterStoreState>(
      allow_undeclared_parameters,
      impl::select_parameter_overrides(parameter_overrides, name_, fully_qualified_name_))),
  node_id_(context_->discovery_graph()->add_local_node(name_, node_namespace_)) {}

Node::Impl::~Impl() noexcept {
    context_->discovery_graph()->release_local_node(node_id_);
}

Result<Parameter> Node::Impl::declare_parameter(
    std::string_view name, const ParameterValue& default_value,
    const ParameterDescriptor& descriptor, bool ignore_override) {
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<Parameter>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    return parameters_->declare(name, default_value, descriptor, ignore_override);
}

Result<void> Node::Impl::undeclare_parameter(std::string_view name) {
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<void>::failure(Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    return parameters_->undeclare(name);
}

Result<bool> Node::Impl::has_parameter(std::string_view name) const {
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<bool>::failure(Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    return parameters_->has(name);
}

Result<Parameter> Node::Impl::get_parameter(std::string_view name) const {
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<Parameter>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    return parameters_->get(name);
}

Result<std::vector<Parameter>> Node::Impl::get_parameters(
    const std::vector<std::string>& names) const {
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<std::vector<Parameter>>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    return parameters_->get_many(names);
}

Result<ParameterDescriptor> Node::Impl::describe_parameter(std::string_view name) const {
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<ParameterDescriptor>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    return parameters_->describe(name);
}

Result<ParameterListResult> Node::Impl::list_parameters(
    const std::vector<std::string>& prefixes, std::size_t depth) const {
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<ParameterListResult>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    return parameters_->list(prefixes, depth);
}

Result<void> Node::Impl::validate_parameters(const std::vector<Parameter>& parameters) const {
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<void>::failure(Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    return parameters_->validate(parameters);
}

Result<ParameterChangeSet> Node::Impl::set_parameters_atomically(
    const std::vector<Parameter>& parameters) {
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<ParameterChangeSet>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    return parameters_->set_atomically(parameters);
}

Result<ParameterChangeSet> Node::Impl::take_parameter_changes() {
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<ParameterChangeSet>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    return parameters_->take_changes();
}

namespace {

/// Bind one freshly created local DDS endpoint to its owning Node.
impl::LocalEndpointRegistration bind_endpoint_metadata(
    const std::shared_ptr<impl::Context>& context, const eprosima::fastrtps::rtps::GUID_t& guid,
    impl::EndpointKind kind, const std::string& dds_topic, std::string_view wire_type,
    const Qos& qos, std::uint64_t node_id) {
    return impl::register_local_endpoint(
        context->discovery_graph(), guid, kind, dds_topic, std::string(wire_type), qos, node_id);
}

}  // namespace

Result<std::unique_ptr<Publisher>> Node::Impl::create_publisher(
    const MessageType& type, std::string_view topic_name, const Qos& qos, const PublisherOptions&) {
    auto logical_name = resolve_remapped_name(impl_->name_, impl_->node_namespace_, impl_->remaps_, topic_name);
    if (!logical_name) {
        return Result<std::unique_ptr<Publisher>>::failure(std::move(logical_name.error()));
    }
    auto writer_qos = impl::to_writer_qos(
        qos, impl_->context_->runtime_mode(), impl_->context_->writer_qos_baseline());
    if (!writer_qos)
        return Result<std::unique_ptr<Publisher>>::failure(std::move(writer_qos.error()));
    const auto operation = impl_->context_->try_acquire_operation();
    if (!operation) {
        return Result<std::unique_ptr<Publisher>>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    const auto dds_name = dds_topic_name(*impl_->context_, logical_name.value());
    auto topic = impl_->context_->acquire_topic(type, dds_name, qos);
    if (!topic) {
        return Result<std::unique_ptr<Publisher>>::failure(std::move(topic.error()));
    }
    auto* writer =
        impl_->context_->publisher()->create_datawriter(topic.value().get(), writer_qos.value());
    if (writer == nullptr) {
        return Result<std::unique_ptr<Publisher>>::failure(
            Error(ErrorCode::DDSError, "Fast DDS failed to create a DataWriter"));
    }
    std::unique_ptr<Publisher::Impl> publisher_impl;
    try {
        auto metadata = bind_endpoint_metadata(
            impl_->context_, eprosima::fastrtps::rtps::iHandle2GUID(writer->get_instance_handle()),
            impl::EndpointKind::Writer, dds_name, type.type_name(), qos, impl_->node_id_);
        publisher_impl = std::unique_ptr<Publisher::Impl>(new Publisher::Impl(
            impl_->context_, writer, std::move(logical_name.value()), type,
            std::move(topic.value()), std::move(metadata)));
    } catch (...) {
        delete_writer_noexcept(*impl_->context_, writer);
        throw;
    }
    return Result<std::unique_ptr<Publisher>>::success(
        std::unique_ptr<Publisher>(new Publisher(std::move(publisher_impl))));
}

Result<std::unique_ptr<Subscriber>> Node::Impl::create_subscriber(
    const MessageType& type, std::string_view topic_name, const Qos& qos,
    const SubscriberOptions&) {
    auto logical_name = resolve_remapped_name(impl_->name_, impl_->node_namespace_, impl_->remaps_, topic_name);
    if (!logical_name) {
        return Result<std::unique_ptr<Subscriber>>::failure(std::move(logical_name.error()));
    }
    auto reader_qos = impl::to_reader_qos(
        qos, impl_->context_->runtime_mode(), impl_->context_->reader_qos_baseline());
    if (!reader_qos)
        return Result<std::unique_ptr<Subscriber>>::failure(std::move(reader_qos.error()));
    const auto operation = impl_->context_->try_acquire_operation();
    if (!operation) {
        return Result<std::unique_ptr<Subscriber>>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    const auto dds_name = dds_topic_name(*impl_->context_, logical_name.value());
    auto topic = impl_->context_->acquire_topic(type, dds_name, qos);
    if (!topic) {
        return Result<std::unique_ptr<Subscriber>>::failure(std::move(topic.error()));
    }
    auto* reader =
        impl_->context_->subscriber()->create_datareader(topic.value().get(), reader_qos.value());
    if (reader == nullptr) {
        return Result<std::unique_ptr<Subscriber>>::failure(
            Error(ErrorCode::DDSError, "Fast DDS failed to create a DataReader"));
    }
    std::unique_ptr<Subscriber::Impl> subscriber_impl;
    try {
        auto metadata = bind_endpoint_metadata(
            impl_->context_, eprosima::fastrtps::rtps::iHandle2GUID(reader->get_instance_handle()),
            impl::EndpointKind::Reader, dds_name, type.type_name(), qos, impl_->node_id_);
        subscriber_impl = std::unique_ptr<Subscriber::Impl>(new Subscriber::Impl(
            impl_->context_, reader, std::move(logical_name.value()), type,
            std::move(topic.value()), std::move(metadata)));
    } catch (...) {
        delete_reader_noexcept(*impl_->context_, reader);
        throw;
    }
    return Result<std::unique_ptr<Subscriber>>::success(
        std::unique_ptr<Subscriber>(new Subscriber(std::move(subscriber_impl))));
}

Result<std::unique_ptr<Client>> Node::Impl::create_client(
    const ServiceType& type, std::string_view service_name, const Qos& qos, const ClientOptions&) {
    auto logical_name = resolve_remapped_name(impl_->name_, impl_->node_namespace_, impl_->remaps_, service_name);
    if (!logical_name)
        return Result<std::unique_ptr<Client>>::failure(std::move(logical_name.error()));
    auto writer_qos = impl::to_writer_qos(
        qos, impl_->context_->runtime_mode(), impl_->context_->writer_qos_baseline());
    if (!writer_qos) return Result<std::unique_ptr<Client>>::failure(std::move(writer_qos.error()));
    auto reader_qos = impl::to_reader_qos(
        qos, impl_->context_->runtime_mode(), impl_->context_->reader_qos_baseline());
    if (!reader_qos) return Result<std::unique_ptr<Client>>::failure(std::move(reader_qos.error()));
    const auto operation = impl_->context_->try_acquire_operation();
    if (!operation)
        return Result<std::unique_ptr<Client>>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    const auto request_topic_name =
        service_topic_name(*impl_->context_, logical_name.value(), true);
    const auto response_topic_name =
        service_topic_name(*impl_->context_, logical_name.value(), false);
    auto request_topic =
        impl_->context_->acquire_topic(type.request_type(), request_topic_name, qos);
    if (!request_topic)
        return Result<std::unique_ptr<Client>>::failure(std::move(request_topic.error()));
    auto response_topic =
        impl_->context_->acquire_topic(type.response_type(), response_topic_name, qos);
    if (!response_topic)
        return Result<std::unique_ptr<Client>>::failure(std::move(response_topic.error()));
    auto request_state = std::make_shared<impl::RequestState>(
        impl_->context_->discovery_graph(), request_topic_name,
        std::string(type.request_type().type_name()), response_topic_name,
        std::string(type.response_type().type_name()));
    auto request_listener =
        std::make_unique<impl::RequestWriterListener>(std::weak_ptr(request_state));
    auto response_listener =
        std::make_unique<impl::ResponseReaderListener>(std::weak_ptr(request_state));
    auto* writer = impl_->context_->publisher()->create_datawriter(
        request_topic.value().get(), writer_qos.value(), request_listener.get());
    if (writer == nullptr)
        return Result<std::unique_ptr<Client>>::failure(
            Error(ErrorCode::DDSError, "Fast DDS failed to create request writer"));
    auto* reader = impl_->context_->subscriber()->create_datareader(
        response_topic.value().get(), reader_qos.value(), response_listener.get());
    if (reader == nullptr) {
        delete_writer_listener_noexcept(*impl_->context_, writer, request_listener);
        return Result<std::unique_ptr<Client>>::failure(
            Error(ErrorCode::DDSError, "Fast DDS failed to create response reader"));
    }
    std::unique_ptr<Client::Impl> client_impl;
    try {
        auto request_metadata = bind_endpoint_metadata(
            impl_->context_, eprosima::fastrtps::rtps::iHandle2GUID(writer->get_instance_handle()),
            impl::EndpointKind::Writer, request_topic_name, type.request_type().type_name(), qos,
            impl_->node_id_);
        auto response_metadata = bind_endpoint_metadata(
            impl_->context_, eprosima::fastrtps::rtps::iHandle2GUID(reader->get_instance_handle()),
            impl::EndpointKind::Reader, response_topic_name, type.response_type().type_name(), qos,
            impl_->node_id_);
        client_impl = std::unique_ptr<Client::Impl>(new Client::Impl(
            impl_->context_, std::move(logical_name.value()), type.response_type(),
            std::move(request_state), std::move(request_topic.value()), writer,
            std::move(request_listener), std::move(response_topic.value()), reader,
            std::move(response_listener), std::move(request_metadata),
            std::move(response_metadata)));
    } catch (...) {
        delete_reader_listener_noexcept(*impl_->context_, reader, response_listener);
        delete_writer_listener_noexcept(*impl_->context_, writer, request_listener);
        throw;
    }
    if (!client_impl->initialized()) {
        if (impl_->context_->is_shutdown()) {
            return Result<std::unique_ptr<Client>>::failure(
                Error(ErrorCode::ContextShutdown, "Context is shut down"));
        }
        return Result<std::unique_ptr<Client>>::failure(
            Error(ErrorCode::DDSError, "Client discovery wait registration failed"));
    }
    return Result<std::unique_ptr<Client>>::success(
        std::unique_ptr<Client>(new Client(std::move(client_impl))));
}

Result<std::unique_ptr<Server>> Node::Impl::create_server(
    const ServiceType& type, std::string_view service_name, const Qos& qos,
    const ServerOptions& options) {
    if (options.max_pending_requests == 0)
        return Result<std::unique_ptr<Server>>::failure(
            Error(ErrorCode::InvalidArgument, "max_pending_requests must be greater than zero"));
    auto logical_name = resolve_remapped_name(impl_->name_, impl_->node_namespace_, impl_->remaps_, service_name);
    if (!logical_name)
        return Result<std::unique_ptr<Server>>::failure(std::move(logical_name.error()));
    auto reader_qos = impl::to_reader_qos(
        qos, impl_->context_->runtime_mode(), impl_->context_->reader_qos_baseline());
    if (!reader_qos) return Result<std::unique_ptr<Server>>::failure(std::move(reader_qos.error()));
    auto writer_qos = impl::to_writer_qos(
        qos, impl_->context_->runtime_mode(), impl_->context_->writer_qos_baseline());
    if (!writer_qos) return Result<std::unique_ptr<Server>>::failure(std::move(writer_qos.error()));
    const auto operation = impl_->context_->try_acquire_operation();
    if (!operation)
        return Result<std::unique_ptr<Server>>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    auto request_topic = impl_->context_->acquire_topic(
        type.request_type(), service_topic_name(*impl_->context_, logical_name.value(), true), qos);
    if (!request_topic)
        return Result<std::unique_ptr<Server>>::failure(std::move(request_topic.error()));
    const auto request_topic_name =
        service_topic_name(*impl_->context_, logical_name.value(), true);
    const auto response_topic_name =
        service_topic_name(*impl_->context_, logical_name.value(), false);
    auto response_topic = impl_->context_->acquire_topic(
        type.response_type(), response_topic_name, qos);
    if (!response_topic)
        return Result<std::unique_ptr<Server>>::failure(std::move(response_topic.error()));
    auto response_state = std::make_shared<impl::ResponseState>(impl_->context_->discovery_graph());
    if (!response_state->subscribe_to_graph()) {
        return Result<std::unique_ptr<Server>>::failure(
            Error(ErrorCode::DDSError, "Server response discovery subscription failed"));
    }
    auto response_listener =
        std::make_unique<impl::ResponseWriterListener>(std::weak_ptr(response_state));
    auto* reader = impl_->context_->subscriber()->create_datareader(
        request_topic.value().get(), reader_qos.value());
    if (reader == nullptr)
        return Result<std::unique_ptr<Server>>::failure(
            Error(ErrorCode::DDSError, "Fast DDS failed to create request reader"));
    auto* writer = impl_->context_->publisher()->create_datawriter(
        response_topic.value().get(), writer_qos.value(), response_listener.get());
    if (writer == nullptr) {
        delete_reader_noexcept(*impl_->context_, reader);
        return Result<std::unique_ptr<Server>>::failure(
            Error(ErrorCode::DDSError, "Fast DDS failed to create response writer"));
    }
    std::unique_ptr<Server::Impl> server_impl;
    try {
        auto request_metadata = bind_endpoint_metadata(
            impl_->context_, eprosima::fastrtps::rtps::iHandle2GUID(reader->get_instance_handle()),
            impl::EndpointKind::Reader, request_topic_name, type.request_type().type_name(), qos,
            impl_->node_id_);
        auto response_metadata = bind_endpoint_metadata(
            impl_->context_, eprosima::fastrtps::rtps::iHandle2GUID(writer->get_instance_handle()),
            impl::EndpointKind::Writer, response_topic_name, type.response_type().type_name(), qos,
            impl_->node_id_);
        server_impl = std::unique_ptr<Server::Impl>(new Server::Impl(
            impl_->context_, reader, writer, std::move(logical_name.value()),
            options.max_pending_requests, type.request_type(), std::move(response_state),
            std::move(response_listener), std::move(request_topic.value()),
            std::move(response_topic.value()), std::move(request_metadata),
            std::move(response_metadata)));
    } catch (...) {
        delete_writer_listener_noexcept(*impl_->context_, writer, response_listener);
        delete_reader_noexcept(*impl_->context_, reader);
        throw;
    }
    return Result<std::unique_ptr<Server>>::success(
        std::unique_ptr<Server>(new Server(std::move(server_impl))));
}

Result<std::unique_ptr<ActionClient>> Node::Impl::create_action_client(
    const ActionType& type, std::string_view action_name, const ActionClientOptions& options) {
    auto logical_name = resolve_remapped_name(impl_->name_, impl_->node_namespace_, impl_->remaps_, action_name);
    if (!logical_name)
        return Result<std::unique_ptr<ActionClient>>::failure(std::move(logical_name.error()));
    const auto names = impl::derive_action_endpoint_names(logical_name.value());
    if (!names) {
        return Result<std::unique_ptr<ActionClient>>::failure(
            Error(ErrorCode::InvalidName, "Action name is not a valid fully qualified name"));
    }
    const auto operation = impl_->context_->try_acquire_operation();
    if (!operation) {
        return Result<std::unique_ptr<ActionClient>>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }

    // Five endpoints are created as one transaction: any failure destroys the
    // already-created constituents, so no partial Action is ever exposed.
    auto goal = impl_->create_client(
        type.send_goal_type(), names->send_goal, options.goal_service_qos, {});
    if (!goal) return Result<std::unique_ptr<ActionClient>>::failure(std::move(goal.error()));
    auto cancel = impl_->create_client(
        type.cancel_goal_type(), names->cancel_goal, options.cancel_service_qos, {});
    if (!cancel)
        return Result<std::unique_ptr<ActionClient>>::failure(std::move(cancel.error()));
    auto result = impl_->create_client(
        type.get_result_type(), names->get_result, options.result_service_qos, {});
    if (!result)
        return Result<std::unique_ptr<ActionClient>>::failure(std::move(result.error()));
    auto feedback = impl_->create_subscriber(
        type.feedback_type(), names->feedback, options.feedback_topic_qos, {});
    if (!feedback)
        return Result<std::unique_ptr<ActionClient>>::failure(std::move(feedback.error()));
    auto status = impl_->create_subscriber(
        type.status_type(), names->status, options.status_topic_qos, {});
    if (!status) return Result<std::unique_ptr<ActionClient>>::failure(std::move(status.error()));

    auto client_impl = std::make_unique<ActionClient::Impl>(
        impl_->context_, std::move(logical_name.value()), *names, impl::action_endpoint_types(type),
        std::move(goal.value()), std::move(cancel.value()), std::move(result.value()),
        std::move(feedback.value()), std::move(status.value()));
    if (!client_impl->initialized()) {
        if (impl_->context_->is_shutdown()) {
            return Result<std::unique_ptr<ActionClient>>::failure(
                Error(ErrorCode::ContextShutdown, "Context is shut down"));
        }
        return Result<std::unique_ptr<ActionClient>>::failure(
            Error(ErrorCode::DDSError, "ActionClient availability registration failed"));
    }
    if (impl_->context_->is_shutdown()) {
        return Result<std::unique_ptr<ActionClient>>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    return Result<std::unique_ptr<ActionClient>>::success(
        std::unique_ptr<ActionClient>(new ActionClient(std::move(client_impl))));
}

Result<std::unique_ptr<ActionServer>> Node::Impl::create_action_server(
    const ActionType& type, std::string_view action_name, const ActionServerOptions& options) {
    if (options.result_timeout < std::chrono::nanoseconds::zero()) {
        return Result<std::unique_ptr<ActionServer>>::failure(
            Error(ErrorCode::InvalidArgument, "result_timeout must not be negative"));
    }
    auto logical_name = resolve_remapped_name(impl_->name_, impl_->node_namespace_, impl_->remaps_, action_name);
    if (!logical_name)
        return Result<std::unique_ptr<ActionServer>>::failure(std::move(logical_name.error()));
    const auto names = impl::derive_action_endpoint_names(logical_name.value());
    if (!names) {
        return Result<std::unique_ptr<ActionServer>>::failure(
            Error(ErrorCode::InvalidName, "Action name is not a valid fully qualified name"));
    }
    const auto operation = impl_->context_->try_acquire_operation();
    if (!operation) {
        return Result<std::unique_ptr<ActionServer>>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }

    auto goal = impl_->create_server(
        type.send_goal_type(), names->send_goal, options.goal_service_qos, {});
    if (!goal) return Result<std::unique_ptr<ActionServer>>::failure(std::move(goal.error()));
    auto cancel = impl_->create_server(
        type.cancel_goal_type(), names->cancel_goal, options.cancel_service_qos, {});
    if (!cancel)
        return Result<std::unique_ptr<ActionServer>>::failure(std::move(cancel.error()));
    auto result = impl_->create_server(
        type.get_result_type(), names->get_result, options.result_service_qos, {});
    if (!result)
        return Result<std::unique_ptr<ActionServer>>::failure(std::move(result.error()));
    auto feedback = impl_->create_publisher(
        type.feedback_type(), names->feedback, options.feedback_topic_qos, {});
    if (!feedback)
        return Result<std::unique_ptr<ActionServer>>::failure(std::move(feedback.error()));
    auto status = impl_->create_publisher(
        type.status_type(), names->status, options.status_topic_qos, {});
    if (!status) return Result<std::unique_ptr<ActionServer>>::failure(std::move(status.error()));

    auto server_impl = std::make_unique<ActionServer::Impl>(
        impl_->context_, std::move(logical_name.value()), std::move(goal.value()),
        std::move(cancel.value()), std::move(result.value()), std::move(feedback.value()),
        std::move(status.value()), options.result_timeout);
    if (impl_->context_->is_shutdown()) {
        return Result<std::unique_ptr<ActionServer>>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    return Result<std::unique_ptr<ActionServer>>::success(
        std::unique_ptr<ActionServer>(new ActionServer(std::move(server_impl))));
}

#undef impl_

}  // namespace dmw
