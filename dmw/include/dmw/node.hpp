// SPDX-License-Identifier: Apache-2.0

#ifndef DMW__NODE_HPP_
#define DMW__NODE_HPP_

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "dmw/client.hpp"
#include "dmw/message_type.hpp"
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
    std::string name;
    std::string ns{"/"};
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
    std::string_view namespace_() const noexcept;

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

private:
    friend class Context;

    class Impl;

    explicit Node(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}  // namespace dmw

#endif  // DMW__NODE_HPP_
