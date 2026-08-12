// SPDX-License-Identifier: Apache-2.0

#include "dmw/node.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>

#include "dmw/error.hpp"
#include "impl/endpoint_impl.hpp"
#include "impl/fastdds/qos.hpp"
#include "impl/name.hpp"
#include "impl/node_impl.hpp"
#include "impl/service_impl.hpp"
#include "impl/service_match_state.hpp"

namespace dmw {

Node::Node(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Node::~Node() noexcept = default;

std::string_view Node::name() const noexcept { return impl_->name_; }

std::string_view Node::namespace_() const noexcept { return impl_->ns_; }

namespace {

void delete_writer_noexcept(
    impl::fastdds::ContextState& state, eprosima::fastdds::dds::DataWriter* writer) noexcept {
    if (writer == nullptr) return;
    try {
        state.publisher()->delete_datawriter(writer);
    } catch (...) {
        // The Context container is the final ownership barrier for a writer
        // whose individual deletion cannot be confirmed during rollback.
    }
}

void delete_reader_noexcept(
    impl::fastdds::ContextState& state, eprosima::fastdds::dds::DataReader* reader) noexcept {
    if (reader == nullptr) return;
    try {
        state.subscriber()->delete_datareader(reader);
    } catch (...) {
        // See delete_writer_noexcept().
    }
}

std::string dds_topic_name(
    const impl::fastdds::ContextState& state, const std::string& logical_name) {
    const std::string path = logical_name.substr(1);
    if (state.compatibility_profile() == CompatibilityProfile::Ros2FastDdsHumble) {
        return "rt/" + path;
    }
    return "dmw/t/" + path;
}

std::string service_topic_name(
    const impl::fastdds::ContextState& state, const std::string& logical_name, bool request) {
    const std::string path = logical_name.substr(1);
    if (state.compatibility_profile() == CompatibilityProfile::Ros2FastDdsHumble) {
        return (request ? "rq/" : "rr/") + path + (request ? "Request" : "Reply");
    }
    return (request ? "dmw/rq/" : "dmw/rr/") + path;
}

}  // namespace

Result<std::unique_ptr<Publisher>> Node::create_publisher(
    const MessageType& type, std::string_view topic_name, const Qos& qos, const PublisherOptions&) {
    auto logical_name = impl::resolve_name(impl_->ns_, topic_name);
    if (!logical_name) {
        return Result<std::unique_ptr<Publisher>>::failure(std::move(logical_name.error()));
    }
    auto writer_qos =
        impl::fastdds::make_writer_qos(qos, impl_->context_state_->compatibility_profile());
    if (!writer_qos)
        return Result<std::unique_ptr<Publisher>>::failure(std::move(writer_qos.error()));
    const auto operation = impl_->context_state_->try_acquire_operation();
    if (!operation) {
        return Result<std::unique_ptr<Publisher>>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    auto topic = impl_->context_state_->acquire_topic(
        type, dds_topic_name(*impl_->context_state_, logical_name.value()));
    if (!topic) {
        return Result<std::unique_ptr<Publisher>>::failure(std::move(topic.error()));
    }
    auto* writer = impl_->context_state_->publisher()->create_datawriter(
        topic.value().get(), writer_qos.value());
    if (writer == nullptr) {
        return Result<std::unique_ptr<Publisher>>::failure(
            Error(ErrorCode::DdsError, "Fast DDS failed to create a DataWriter"));
    }
    std::unique_ptr<Publisher::Impl> endpoint_impl;
    try {
        endpoint_impl = std::make_unique<Publisher::Impl>(
            impl_->context_state_, writer, std::move(logical_name.value()), type,
            std::move(topic.value()));
    } catch (...) {
        delete_writer_noexcept(*impl_->context_state_, writer);
        throw;
    }
    return Result<std::unique_ptr<Publisher>>::success(
        std::unique_ptr<Publisher>(new Publisher(std::move(endpoint_impl))));
}

Result<std::unique_ptr<Subscriber>> Node::create_subscriber(
    const MessageType& type, std::string_view topic_name, const Qos& qos,
    const SubscriberOptions&) {
    auto logical_name = impl::resolve_name(impl_->ns_, topic_name);
    if (!logical_name) {
        return Result<std::unique_ptr<Subscriber>>::failure(std::move(logical_name.error()));
    }
    auto reader_qos =
        impl::fastdds::make_reader_qos(qos, impl_->context_state_->compatibility_profile());
    if (!reader_qos)
        return Result<std::unique_ptr<Subscriber>>::failure(std::move(reader_qos.error()));
    const auto operation = impl_->context_state_->try_acquire_operation();
    if (!operation) {
        return Result<std::unique_ptr<Subscriber>>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    auto topic = impl_->context_state_->acquire_topic(
        type, dds_topic_name(*impl_->context_state_, logical_name.value()));
    if (!topic) {
        return Result<std::unique_ptr<Subscriber>>::failure(std::move(topic.error()));
    }
    auto* reader = impl_->context_state_->subscriber()->create_datareader(
        topic.value().get(), reader_qos.value());
    if (reader == nullptr) {
        return Result<std::unique_ptr<Subscriber>>::failure(
            Error(ErrorCode::DdsError, "Fast DDS failed to create a DataReader"));
    }
    std::unique_ptr<Subscriber::Impl> endpoint_impl;
    try {
        endpoint_impl = std::make_unique<Subscriber::Impl>(
            impl_->context_state_, reader, std::move(logical_name.value()), type,
            std::move(topic.value()));
    } catch (...) {
        delete_reader_noexcept(*impl_->context_state_, reader);
        throw;
    }
    return Result<std::unique_ptr<Subscriber>>::success(
        std::unique_ptr<Subscriber>(new Subscriber(std::move(endpoint_impl))));
}

Result<std::unique_ptr<Client>> Node::create_client(
    const ServiceType& type, std::string_view service_name, const Qos& qos, const ClientOptions&) {
    auto logical_name = impl::resolve_name(impl_->ns_, service_name);
    if (!logical_name)
        return Result<std::unique_ptr<Client>>::failure(std::move(logical_name.error()));
    auto writer_qos =
        impl::fastdds::make_writer_qos(qos, impl_->context_state_->compatibility_profile());
    if (!writer_qos) return Result<std::unique_ptr<Client>>::failure(std::move(writer_qos.error()));
    auto reader_qos =
        impl::fastdds::make_reader_qos(qos, impl_->context_state_->compatibility_profile());
    if (!reader_qos) return Result<std::unique_ptr<Client>>::failure(std::move(reader_qos.error()));
    const auto operation = impl_->context_state_->try_acquire_operation();
    if (!operation)
        return Result<std::unique_ptr<Client>>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    auto request_topic = impl_->context_state_->acquire_topic(
        type.request_type(),
        service_topic_name(*impl_->context_state_, logical_name.value(), true));
    if (!request_topic)
        return Result<std::unique_ptr<Client>>::failure(std::move(request_topic.error()));
    auto response_topic = impl_->context_state_->acquire_topic(
        type.response_type(),
        service_topic_name(*impl_->context_state_, logical_name.value(), false));
    if (!response_topic)
        return Result<std::unique_ptr<Client>>::failure(std::move(response_topic.error()));
    auto match_state = std::make_shared<impl::ServiceMatchState>();
    auto request_listener =
        std::make_unique<impl::RequestWriterMatchListener>(std::weak_ptr(match_state));
    auto response_listener =
        std::make_unique<impl::ResponseReaderMatchListener>(std::weak_ptr(match_state));
    auto* writer = impl_->context_state_->publisher()->create_datawriter(
        request_topic.value().get(), writer_qos.value(), request_listener.get());
    if (writer == nullptr)
        return Result<std::unique_ptr<Client>>::failure(
            Error(ErrorCode::DdsError, "Fast DDS failed to create request writer"));
    auto* reader = impl_->context_state_->subscriber()->create_datareader(
        response_topic.value().get(), reader_qos.value(), response_listener.get());
    if (reader == nullptr) {
        delete_writer_noexcept(*impl_->context_state_, writer);
        return Result<std::unique_ptr<Client>>::failure(
            Error(ErrorCode::DdsError, "Fast DDS failed to create response reader"));
    }
    std::unique_ptr<Client::Impl> client_impl;
    try {
        client_impl = std::make_unique<Client::Impl>(
            impl_->context_state_, writer, reader, std::move(logical_name.value()),
            type.response_type(), std::move(match_state), std::move(request_listener),
            std::move(response_listener), std::move(request_topic.value()),
            std::move(response_topic.value()));
    } catch (...) {
        delete_reader_noexcept(*impl_->context_state_, reader);
        delete_writer_noexcept(*impl_->context_state_, writer);
        throw;
    }
    return Result<std::unique_ptr<Client>>::success(
        std::unique_ptr<Client>(new Client(std::move(client_impl))));
}

Result<std::unique_ptr<Server>> Node::create_server(
    const ServiceType& type, std::string_view service_name, const Qos& qos,
    const ServerOptions& options) {
    if (options.max_pending_requests == 0)
        return Result<std::unique_ptr<Server>>::failure(
            Error(ErrorCode::InvalidArgument, "max_pending_requests must be greater than zero"));
    auto logical_name = impl::resolve_name(impl_->ns_, service_name);
    if (!logical_name)
        return Result<std::unique_ptr<Server>>::failure(std::move(logical_name.error()));
    auto reader_qos =
        impl::fastdds::make_reader_qos(qos, impl_->context_state_->compatibility_profile());
    if (!reader_qos) return Result<std::unique_ptr<Server>>::failure(std::move(reader_qos.error()));
    auto writer_qos =
        impl::fastdds::make_writer_qos(qos, impl_->context_state_->compatibility_profile());
    if (!writer_qos) return Result<std::unique_ptr<Server>>::failure(std::move(writer_qos.error()));
    const auto operation = impl_->context_state_->try_acquire_operation();
    if (!operation)
        return Result<std::unique_ptr<Server>>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    auto request_topic = impl_->context_state_->acquire_topic(
        type.request_type(),
        service_topic_name(*impl_->context_state_, logical_name.value(), true));
    if (!request_topic)
        return Result<std::unique_ptr<Server>>::failure(std::move(request_topic.error()));
    auto response_topic = impl_->context_state_->acquire_topic(
        type.response_type(),
        service_topic_name(*impl_->context_state_, logical_name.value(), false));
    if (!response_topic)
        return Result<std::unique_ptr<Server>>::failure(std::move(response_topic.error()));
    auto response_match_state = std::make_shared<impl::ResponseWriterMatchState>();
    auto response_match_listener =
        std::make_unique<impl::ResponseWriterMatchListener>(std::weak_ptr(response_match_state));
    auto* reader = impl_->context_state_->subscriber()->create_datareader(
        request_topic.value().get(), reader_qos.value());
    if (reader == nullptr)
        return Result<std::unique_ptr<Server>>::failure(
            Error(ErrorCode::DdsError, "Fast DDS failed to create request reader"));
    auto* writer = impl_->context_state_->publisher()->create_datawriter(
        response_topic.value().get(), writer_qos.value(), response_match_listener.get());
    if (writer == nullptr) {
        delete_reader_noexcept(*impl_->context_state_, reader);
        return Result<std::unique_ptr<Server>>::failure(
            Error(ErrorCode::DdsError, "Fast DDS failed to create response writer"));
    }
    std::unique_ptr<Server::Impl> server_impl;
    try {
        server_impl = std::make_unique<Server::Impl>(
            impl_->context_state_, reader, writer, std::move(logical_name.value()),
            options.max_pending_requests, type.request_type(), std::move(response_match_state),
            std::move(response_match_listener), std::move(request_topic.value()),
            std::move(response_topic.value()));
    } catch (...) {
        delete_writer_noexcept(*impl_->context_state_, writer);
        delete_reader_noexcept(*impl_->context_state_, reader);
        throw;
    }
    return Result<std::unique_ptr<Server>>::success(
        std::unique_ptr<Server>(new Server(std::move(server_impl))));
}

}  // namespace dmw
