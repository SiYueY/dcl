#ifndef DMW_IMPL__FASTDDS__CONTEXT_HPP_
#define DMW_IMPL__FASTDDS__CONTEXT_HPP_

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <utility>

#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/publisher/qos/DataWriterQos.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/subscriber/qos/DataReaderQos.hpp>

#include "dmw/runtime_mode.hpp"
#include "dmw/message_type.hpp"
#include "dmw/qos.hpp"
#include "dmw/result.hpp"
#include "impl/lock_rank.hpp"
#include "impl/discovery_graph.hpp"
#include "impl/graph_metadata.hpp"
#include "impl/topic.hpp"

namespace dmw {

namespace impl {

class Context {
public:
    class OperationGuard {
    public:
        OperationGuard() noexcept = default;
        ~OperationGuard() noexcept;

        OperationGuard(const OperationGuard&) = delete;
        OperationGuard& operator=(const OperationGuard&) = delete;

        OperationGuard(OperationGuard&& other) noexcept;
        OperationGuard& operator=(OperationGuard&& other) noexcept;

        explicit operator bool() const noexcept { return context_ != nullptr; }

    private:
        friend class Context;

        explicit OperationGuard(Context* context) noexcept : context_(context) {}

        Context* context_{nullptr};
    };

    Context(
        eprosima::fastdds::dds::DomainParticipantFactory* factory,
        eprosima::fastdds::dds::DomainParticipant* participant,
        eprosima::fastdds::dds::Publisher* publisher,
        eprosima::fastdds::dds::Subscriber* subscriber, std::uint32_t domain_id,
        RuntimeMode runtime_mode) noexcept;
    Context(
        eprosima::fastdds::dds::DomainParticipantFactory* factory,
        eprosima::fastdds::dds::DomainParticipant* participant,
        eprosima::fastdds::dds::Publisher* publisher,
        eprosima::fastdds::dds::Subscriber* subscriber, std::uint32_t domain_id,
        RuntimeMode runtime_mode, eprosima::fastdds::dds::DataWriterQos writer_qos_baseline,
        eprosima::fastdds::dds::DataReaderQos reader_qos_baseline) noexcept;
    Context(
        eprosima::fastdds::dds::DomainParticipantFactory* factory,
        eprosima::fastdds::dds::DomainParticipant* participant,
        eprosima::fastdds::dds::Publisher* publisher,
        eprosima::fastdds::dds::Subscriber* subscriber, std::uint32_t domain_id,
        RuntimeMode runtime_mode, eprosima::fastdds::dds::DataWriterQos writer_qos_baseline,
        eprosima::fastdds::dds::DataReaderQos reader_qos_baseline,
        std::shared_ptr<DiscoveryGraph> discovery_graph) noexcept;
    ~Context() noexcept;

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    eprosima::fastdds::dds::DomainParticipant* participant() const noexcept;
    eprosima::fastdds::dds::Publisher* publisher() const noexcept;
    eprosima::fastdds::dds::Subscriber* subscriber() const noexcept;
    std::uint32_t domain_id() const noexcept;
    RuntimeMode runtime_mode() const noexcept;
    const eprosima::fastdds::dds::DataWriterQos& writer_qos_baseline() const noexcept;
    const eprosima::fastdds::dds::DataReaderQos& reader_qos_baseline() const noexcept;
    bool is_shutdown() const noexcept;
    OperationGuard try_acquire_operation() noexcept;
    void shutdown() noexcept;
    std::uint64_t register_shutdown_callback(std::function<void()> callback);
    void unregister_shutdown_callback(std::uint64_t id) noexcept;
    void adopt_discovery_listener(std::unique_ptr<DiscoveryListener> listener) noexcept {
        participant_listener_ = std::move(listener);
    }
    std::shared_ptr<DiscoveryGraph> discovery_graph() const noexcept { return discovery_graph_; }

    /// Create the ROS 2 graph metadata transport; no-op in DDS mode.
    Result<void> install_graph_metadata_transport() noexcept;
    const std::shared_ptr<GraphMetadataTransport>& graph_metadata() const noexcept {
        return graph_metadata_;
    }
    /// Unsubscribe and release the metadata transport before entity teardown.
    void close_graph_metadata_transport() noexcept;

    Result<Topic> acquire_topic(
        const MessageType& type, const std::string& dds_topic_name, const Qos& qos);

private:
    friend class Topic;
    friend class TypeRegistration;
    enum class ShutdownExecutionState { Running, RequestingChildren, Draining, Complete };
    enum class RegistryEntryPhase { Creating, Active, Retiring, Orphaned };

    struct ShutdownChild {
        explicit ShutdownChild(std::function<void()> value) : callback(std::move(value)) {}
        std::function<void()> callback;
        bool requested{false};
        bool acknowledged{false};
    };

    struct RegisteredType {
        RegisteredType(MessageType descriptor, std::type_index identity) noexcept
        : type(std::move(descriptor)), pubsub_type(identity) {}

        MessageType type;
        std::type_index pubsub_type;
        RegistryEntryPhase phase{RegistryEntryPhase::Creating};
        std::size_t endpoint_reference_count{0};
    };

    struct RegisteredTopic {
        explicit RegisteredTopic(std::string type_name) : wire_type_name(std::move(type_name)) {}

        std::string wire_type_name;
        eprosima::fastdds::dds::Topic* topic{nullptr};
        RegistryEntryPhase phase{RegistryEntryPhase::Creating};
        std::size_t endpoint_reference_count{0};
    };

    bool release_topic(std::string topic_name) noexcept;
    Result<TypeRegistration> acquire_type(const MessageType& type);
    void release_type(std::string type_name) noexcept;

    eprosima::fastdds::dds::DomainParticipantFactory* factory_;
    eprosima::fastdds::dds::DomainParticipant* participant_;
    eprosima::fastdds::dds::Publisher* publisher_;
    eprosima::fastdds::dds::Subscriber* subscriber_;
    const std::uint32_t domain_id_;
    const RuntimeMode runtime_mode_;
    const eprosima::fastdds::dds::DataWriterQos writer_qos_baseline_;
    const eprosima::fastdds::dds::DataReaderQos reader_qos_baseline_;
    std::atomic<bool> shutdown_{false};
    bool shutdown_complete_{false};
    RankedMutex<LockRank::ContextRuntime> operation_mutex_;
    std::condition_variable_any operation_cv_;
    std::size_t active_operations_{0};
    RankedMutex<LockRank::ChildRegistry> shutdown_children_mutex_;
    std::uint64_t next_shutdown_callback_id_{1};
    std::unordered_map<std::uint64_t, std::shared_ptr<ShutdownChild>> shutdown_children_;
    ShutdownExecutionState shutdown_execution_state_{ShutdownExecutionState::Running};
    RankedMutex<LockRank::TypeRegistry> type_registry_mutex_;
    std::condition_variable_any type_registry_cv_;
    RankedMutex<LockRank::TopicRegistry> topic_registry_mutex_;
    std::condition_variable_any topic_registry_cv_;
    bool topic_registry_degraded_{false};
    std::unordered_map<std::string, RegisteredType> registered_types_;
    std::unordered_map<std::string, RegisteredTopic> topics_;
    std::shared_ptr<DiscoveryGraph> discovery_graph_{std::make_shared<DiscoveryGraph>()};
    std::unique_ptr<DiscoveryListener> participant_listener_;
    std::shared_ptr<GraphMetadataTransport> graph_metadata_;
    DiscoveryGraph::Subscription graph_metadata_subscription_;
};

}  // namespace impl
}  // namespace dmw

#endif  // DMW_IMPL__FASTDDS__CONTEXT_HPP_
