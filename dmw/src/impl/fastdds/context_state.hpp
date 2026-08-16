#ifndef DMW_IMPL__FASTDDS__CONTEXT_STATE_HPP_
#define DMW_IMPL__FASTDDS__CONTEXT_STATE_HPP_

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
#include <fastdds/dds/subscriber/Subscriber.hpp>

#include "dmw/runtime_mode.hpp"
#include "dmw/message_type.hpp"
#include "dmw/qos.hpp"
#include "dmw/result.hpp"
#include "impl/lock_rank.hpp"
#include "impl/participant_observation.hpp"

namespace dmw {
namespace impl {
namespace fastdds {

class ContextState {
public:
    class TopicLease;

    /// Non-copyable ownership of one endpoint reference to a DDS type registration.
    class TypeLease {
    public:
        TypeLease() noexcept = default;
        ~TypeLease() noexcept;

        TypeLease(const TypeLease&) = delete;
        TypeLease& operator=(const TypeLease&) = delete;

        TypeLease(TypeLease&& other) noexcept;
        TypeLease& operator=(TypeLease&& other) noexcept;

    private:
        friend class ContextState;
        friend class TopicLease;

        TypeLease(ContextState* context, std::string type_name) noexcept
        : context_(context), type_name_(std::move(type_name)) {}

        void reset() noexcept;
        void disarm() noexcept;

        ContextState* context_{nullptr};
        std::string type_name_;
    };

    /// Non-copyable ownership of one endpoint reference to a DDS Topic.
    ///
    /// A lease keeps the Topic and its wire-type registration alive until the
    /// endpoint has deleted its DataReader or DataWriter.  It is intentionally
    /// an implementation type: public entities retain it through their PImpl.
    class TopicLease {
    public:
        TopicLease() noexcept = default;
        ~TopicLease() noexcept;

        TopicLease(const TopicLease&) = delete;
        TopicLease& operator=(const TopicLease&) = delete;

        TopicLease(TopicLease&& other) noexcept;
        TopicLease& operator=(TopicLease&& other) noexcept;

        eprosima::fastdds::dds::Topic* get() const noexcept { return topic_; }

    private:
        friend class ContextState;

        TopicLease(
            ContextState* context, eprosima::fastdds::dds::Topic* topic, std::string topic_name,
            TypeLease type_lease) noexcept
        : context_(context),
          topic_(topic),
          topic_name_(std::move(topic_name)),
          type_lease_(std::move(type_lease)) {}

        void reset() noexcept;

        ContextState* context_{nullptr};
        eprosima::fastdds::dds::Topic* topic_{nullptr};
        std::string topic_name_;
        TypeLease type_lease_;
    };

    class OperationGuard {
    public:
        OperationGuard() noexcept = default;
        ~OperationGuard() noexcept;

        OperationGuard(const OperationGuard&) = delete;
        OperationGuard& operator=(const OperationGuard&) = delete;

        OperationGuard(OperationGuard&& other) noexcept;
        OperationGuard& operator=(OperationGuard&& other) noexcept;

        explicit operator bool() const noexcept { return state_ != nullptr; }

    private:
        friend class ContextState;

        explicit OperationGuard(ContextState* state) noexcept : state_(state) {}

        ContextState* state_{nullptr};
    };

    ContextState(
        eprosima::fastdds::dds::DomainParticipantFactory* factory,
        eprosima::fastdds::dds::DomainParticipant* participant,
        eprosima::fastdds::dds::Publisher* publisher,
        eprosima::fastdds::dds::Subscriber* subscriber, std::uint32_t domain_id,
        RuntimeMode runtime_mode) noexcept;
    ~ContextState() noexcept;

    ContextState(const ContextState&) = delete;
    ContextState& operator=(const ContextState&) = delete;

    eprosima::fastdds::dds::DomainParticipant* participant() const noexcept;
    eprosima::fastdds::dds::Publisher* publisher() const noexcept;
    eprosima::fastdds::dds::Subscriber* subscriber() const noexcept;
    std::uint32_t domain_id() const noexcept;
    RuntimeMode runtime_mode() const noexcept;
    bool is_shutdown() const noexcept;
    OperationGuard try_acquire_operation() noexcept;
    void shutdown() noexcept;
    std::uint64_t register_shutdown_callback(std::function<void()> callback);
    void unregister_shutdown_callback(std::uint64_t id) noexcept;
    Result<void> install_discovery_listener() noexcept;
    std::shared_ptr<ParticipantObservationRegistry> participant_observations() const noexcept {
        return participant_observations_;
    }
    std::shared_ptr<RemoteEndpointRegistry> remote_endpoints() const noexcept {
        return remote_endpoints_;
    }
    std::shared_ptr<TargetReaderObservationRegistry> target_readers() const noexcept {
        return target_readers_;
    }

    Result<TopicLease> acquire_topic(
        const MessageType& type, const std::string& dds_topic_name, const Qos& qos);

private:
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
        RegisteredTopic(std::string type_name, Qos value)
        : wire_type_name(std::move(type_name)), qos(std::move(value)) {}

        std::string wire_type_name;
        Qos qos;
        eprosima::fastdds::dds::Topic* topic{nullptr};
        RegistryEntryPhase phase{RegistryEntryPhase::Creating};
        std::size_t endpoint_reference_count{0};
    };

    bool release_topic(std::string topic_name) noexcept;
    Result<TypeLease> acquire_type(const MessageType& type);
    void release_type(std::string type_name) noexcept;

    eprosima::fastdds::dds::DomainParticipantFactory* factory_;
    eprosima::fastdds::dds::DomainParticipant* participant_;
    eprosima::fastdds::dds::Publisher* publisher_;
    eprosima::fastdds::dds::Subscriber* subscriber_;
    const std::uint32_t domain_id_;
    const RuntimeMode runtime_mode_;
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
    std::shared_ptr<ParticipantObservationRegistry> participant_observations_{
        std::make_shared<ParticipantObservationRegistry>()};
    std::shared_ptr<RemoteEndpointRegistry> remote_endpoints_{
        std::make_shared<RemoteEndpointRegistry>()};
    std::shared_ptr<TargetReaderObservationRegistry> target_readers_{
        std::make_shared<TargetReaderObservationRegistry>()};
    std::unique_ptr<ParticipantObservationListener> participant_listener_;
};

}  // namespace fastdds
}  // namespace impl
}  // namespace dmw

#endif  // DMW_IMPL__FASTDDS__CONTEXT_STATE_HPP_
