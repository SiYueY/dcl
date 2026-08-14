#ifndef DMW_IMPL__NODE_IMPL_HPP_
#define DMW_IMPL__NODE_IMPL_HPP_

#include <memory>
#include <string>

#include "dmw/node.hpp"
#include "impl/fastdds/context_state.hpp"

namespace dmw {

class Node::Impl {
public:
    Impl(
        std::shared_ptr<impl::fastdds::ContextState> context_state, std::string name,
        std::string node_namespace) noexcept
    : context_state_(std::move(context_state)),
      name_(std::move(name)),
      node_namespace_(std::move(node_namespace)) {}

    std::string_view name() const noexcept { return name_; }
    std::string_view node_namespace() const noexcept { return node_namespace_; }
    Result<std::unique_ptr<Publisher>> create_publisher(
        const MessageType&, std::string_view, const Qos&, const PublisherOptions&);
    Result<std::unique_ptr<Subscriber>> create_subscriber(
        const MessageType&, std::string_view, const Qos&, const SubscriberOptions&);
    Result<std::unique_ptr<Client>> create_client(
        const ServiceType&, std::string_view, const Qos&, const ClientOptions&);
    Result<std::unique_ptr<Server>> create_server(
        const ServiceType&, std::string_view, const Qos&, const ServerOptions&);

    std::shared_ptr<impl::fastdds::ContextState> context_state_;
    std::string name_;
    std::string node_namespace_;
};

}  // namespace dmw

#endif  // DMW_IMPL__NODE_IMPL_HPP_
