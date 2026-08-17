#include "impl/context_impl.hpp"

#include <memory>
#include <string>
#include <utility>

#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/domain/qos/DomainParticipantQos.hpp>
#include <fastdds/rtps/attributes/HistoryAttributes.h>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>

#include "dmw/error.hpp"
#include "impl/context_impl.hpp"
#include "dmw/fastdds/message_type.hpp"
#include "impl/context.hpp"
#include "impl/process_lifetime.hpp"
#include "impl/return_code.hpp"
#include "impl/guard_condition_impl.hpp"
#include "impl/name.hpp"
#include "impl/node_impl.hpp"

namespace dmw {

namespace {

bool same_duration(QosDuration left, QosDuration right) noexcept {
    return left.kind() == right.kind() &&
           (left.kind() != QosDuration::Kind::Finite || left.value() == right.value());
}

bool same_topic_qos(const Qos& left, const Qos& right) noexcept {
    return left.history() == right.history() && left.depth() == right.depth() &&
           left.reliability() == right.reliability() && left.durability() == right.durability() &&
           left.liveliness() == right.liveliness() &&
           same_duration(left.deadline(), right.deadline()) &&
           same_duration(left.lifespan(), right.lifespan()) &&
           same_duration(left.liveliness_lease_duration(), right.liveliness_lease_duration());
}

class ParticipantCreationGuard {
public:
    ParticipantCreationGuard(
        eprosima::fastdds::dds::DomainParticipantFactory* factory,
        eprosima::fastdds::dds::DomainParticipant* participant) noexcept
    : factory_(factory), participant_(participant) {}

    ~ParticipantCreationGuard() noexcept {
        if (participant_ != nullptr) {
            bool deleted = false;
            try {
                const auto contained = participant_->delete_contained_entities();
                const auto participant =
                    contained == eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK
                        ? factory_->delete_participant(participant_)
                        : contained;
                deleted = participant == eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK;
            } catch (...) {
                deleted = false;
            }
            if (!deleted) {
                impl::ProcessLifetime::instance().retain_participant(factory_, participant_);
            }
        }
    }

    ParticipantCreationGuard(const ParticipantCreationGuard&) = delete;
    ParticipantCreationGuard& operator=(const ParticipantCreationGuard&) = delete;

    void release() noexcept { participant_ = nullptr; }

private:
    eprosima::fastdds::dds::DomainParticipantFactory* factory_;
    eprosima::fastdds::dds::DomainParticipant* participant_;
};

}  // namespace

namespace impl {

Context::Context(
    eprosima::fastdds::dds::DomainParticipantFactory* factory,
    eprosima::fastdds::dds::DomainParticipant* participant,
    eprosima::fastdds::dds::Publisher* publisher, eprosima::fastdds::dds::Subscriber* subscriber,
    std::uint32_t domain_id, RuntimeMode runtime_mode) noexcept
: factory_(factory),
  participant_(participant),
  publisher_(publisher),
  subscriber_(subscriber),
  domain_id_(domain_id),
  runtime_mode_(runtime_mode) {}

Context::~Context() noexcept {
    if (participant_ != nullptr) {
        bool listener_safe_to_destroy = participant_listener_ == nullptr;
        if (participant_listener_) {
            try {
                listener_safe_to_destroy = participant_->set_listener(nullptr) ==
                                           eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK;
            } catch (...) {
                listener_safe_to_destroy = false;
            }
            if (listener_safe_to_destroy) participant_listener_->close_and_drain();
        }
        if (!listener_safe_to_destroy) {
            // An unconfirmed detach leaves Fast DDS free to enter the
            // listener.  Do not attempt contained-entity or participant
            // deletion in that context: retain both at the process barrier.
            ProcessLifetime::instance().retain_participant(
                factory_, participant_, std::move(participant_listener_));
            participant_ = nullptr;
            publisher_ = nullptr;
            subscriber_ = nullptr;
            return;
        }
        bool deleted = false;
        try {
            const auto contained = participant_->delete_contained_entities();
            if (contained == eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK) {
                deleted = factory_->delete_participant(participant_) ==
                          eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK;
            }
        } catch (...) {
        }
        if (!deleted) {
            ProcessLifetime::instance().retain_participant(
                factory_, participant_, std::move(participant_listener_));
        } else {
            participant_listener_.reset();
        }
        participant_ = nullptr;
        publisher_ = nullptr;
        subscriber_ = nullptr;
    }
}

Result<void> Context::install_discovery_listener() noexcept {
    std::unique_ptr<DiscoveryListener> listener;
    try {
        listener = std::make_unique<DiscoveryListener>(discovery_graph_);
        const auto result = participant_->set_listener(listener.get());
        if (result != eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK) {
            ProcessLifetime::instance().retain_participant_listener(std::move(listener));
            return Result<void>::failure(
                to_error(result, "Fast DDS failed to install discovery listener"));
        }
        participant_listener_ = std::move(listener);
        return Result<void>::success();
    } catch (...) {
        // A throwing listener install has no reliable attachment evidence.
        // Keep any already-created listener alive for the Fast DDS process
        // barrier.  If construction threw, listener is simply null.
        ProcessLifetime::instance().retain_participant_listener(std::move(listener));
        return Result<void>::failure(
            Error(ErrorCode::DDSError, "Fast DDS discovery listener setup failed"));
    }
}

eprosima::fastdds::dds::DomainParticipant* Context::participant() const noexcept {
    return participant_;
}

eprosima::fastdds::dds::Publisher* Context::publisher() const noexcept { return publisher_; }

eprosima::fastdds::dds::Subscriber* Context::subscriber() const noexcept { return subscriber_; }

std::uint32_t Context::domain_id() const noexcept { return domain_id_; }

RuntimeMode Context::runtime_mode() const noexcept { return runtime_mode_; }

Topic::~Topic() noexcept { reset(); }

TypeRegistration::~TypeRegistration() noexcept { reset(); }

TypeRegistration::TypeRegistration(TypeRegistration&& other) noexcept
: context_(std::exchange(other.context_, nullptr)), type_name_(std::move(other.type_name_)) {}

TypeRegistration& TypeRegistration::operator=(TypeRegistration&& other) noexcept {
    if (this != &other) {
        reset();
        context_ = std::exchange(other.context_, nullptr);
        type_name_ = std::move(other.type_name_);
    }
    return *this;
}

void TypeRegistration::reset() noexcept {
    if (context_ != nullptr) {
        context_->release_type(std::move(type_name_));
        context_ = nullptr;
    }
}

void TypeRegistration::disarm() noexcept {
    context_ = nullptr;
    type_name_.clear();
}

Topic::Topic(Topic&& other) noexcept
: context_(std::exchange(other.context_, nullptr)),
  topic_(std::exchange(other.topic_, nullptr)),
  topic_name_(std::move(other.topic_name_)),
  type_registration_(std::move(other.type_registration_)) {}

Topic& Topic::operator=(Topic&& other) noexcept {
    if (this != &other) {
        reset();
        context_ = std::exchange(other.context_, nullptr);
        topic_ = std::exchange(other.topic_, nullptr);
        topic_name_ = std::move(other.topic_name_);
        type_registration_ = std::move(other.type_registration_);
    }
    return *this;
}

void Topic::reset() noexcept {
    if (context_ != nullptr) {
        const bool topic_deleted = context_->release_topic(std::move(topic_name_));
        context_ = nullptr;
        topic_ = nullptr;
        if (!topic_deleted) {
            // An unresolved DDS Topic may still refer to its TypeSupport.
            // Keep the registry reference as a deliberate safety retention.
            type_registration_.disarm();
        }
    }
    type_registration_.reset();
}

bool Context::is_shutdown() const noexcept { return shutdown_.load(std::memory_order_acquire); }

Context::OperationGuard::~OperationGuard() noexcept {
    if (context_ != nullptr) {
        std::lock_guard lock(context_->operation_mutex_);
        --context_->active_operations_;
        if (context_->active_operations_ == 0) {
            context_->operation_cv_.notify_all();
        }
    }
}

Context::OperationGuard::OperationGuard(OperationGuard&& other) noexcept
: context_(std::exchange(other.context_, nullptr)) {}

Context::OperationGuard& Context::OperationGuard::operator=(OperationGuard&& other) noexcept {
    if (this != &other) {
        if (context_ != nullptr) {
            std::lock_guard lock(context_->operation_mutex_);
            --context_->active_operations_;
            if (context_->active_operations_ == 0) {
                context_->operation_cv_.notify_all();
            }
        }
        context_ = std::exchange(other.context_, nullptr);
    }
    return *this;
}

Context::OperationGuard Context::try_acquire_operation() noexcept {
    std::lock_guard lock(operation_mutex_);
    if (shutdown_.load(std::memory_order_acquire)) {
        return OperationGuard{};
    }
    ++active_operations_;
    return OperationGuard(this);
}

void Context::shutdown() noexcept {
    {
        std::unique_lock lock(operation_mutex_);
        if (shutdown_.load(std::memory_order_acquire)) {
            operation_cv_.wait(lock, [this] { return shutdown_complete_; });
            return;
        }
        shutdown_.store(true, std::memory_order_release);
    }

    decltype(shutdown_children_) children;
    {
        std::lock_guard lock(shutdown_children_mutex_);
        shutdown_execution_state_ = ShutdownExecutionState::RequestingChildren;
        for (const auto& entry : shutdown_children_) {
            entry.second->requested = true;
        }
        children.swap(shutdown_children_);
    }
    // A child acknowledges the shutdown request by returning from its
    // callback.  Holding a shared record means concurrent unregistration
    // cannot cancel a request already committed to this shutdown attempt.
    for (const auto& entry : children) {
        const auto& child = entry.second;
        try {
            child->callback();
        } catch (...) {
            // Shutdown is noexcept. A best-effort notification must not prevent
            // later waitables from observing the terminal Context context.
        }
        {
            std::lock_guard lock(shutdown_children_mutex_);
            child->acknowledged = true;
        }
    }
    {
        std::lock_guard lock(shutdown_children_mutex_);
        shutdown_execution_state_ = ShutdownExecutionState::Draining;
        shutdown_execution_state_ = ShutdownExecutionState::Complete;
    }
    std::unique_lock lock(operation_mutex_);
    operation_cv_.wait(lock, [this] { return active_operations_ == 0; });
    shutdown_complete_ = true;
    lock.unlock();
    operation_cv_.notify_all();
}

std::uint64_t Context::register_shutdown_callback(std::function<void()> callback) {
    std::lock_guard lock(shutdown_children_mutex_);
    if (shutdown_.load(std::memory_order_acquire) || next_shutdown_callback_id_ == 0) {
        return 0;
    }
    const auto id = next_shutdown_callback_id_++;
    shutdown_children_.emplace(id, std::make_shared<ShutdownChild>(std::move(callback)));
    return id;
}

void Context::unregister_shutdown_callback(std::uint64_t id) noexcept {
    if (id == 0) {
        return;
    }
    std::lock_guard lock(shutdown_children_mutex_);
    shutdown_children_.erase(id);
}

Result<TypeRegistration> Context::acquire_type(const MessageType& type) {
    const std::string type_name(type.type_name());
    const auto pubsub_type = dmw::fastdds::MessageTypeAdapter::pubsub_type(type);
    {
        std::unique_lock lock(type_registry_mutex_);
        while (true) {
            const auto type_it = registered_types_.find(type_name);
            if (type_it == registered_types_.end()) {
                registered_types_.emplace(type_name, RegisteredType(type, pubsub_type));
                break;
            }
            if (type_it->second.pubsub_type != pubsub_type) {
                return Result<TypeRegistration>::failure(
                    Error(ErrorCode::TypeMismatch, "DDS wire type name has a different binding"));
            }
            if (type_it->second.phase == RegistryEntryPhase::Active) {
                ++type_it->second.endpoint_reference_count;
                return Result<TypeRegistration>::success(TypeRegistration(this, type_name));
            }
            if (type_it->second.phase == RegistryEntryPhase::Orphaned) {
                return Result<TypeRegistration>::failure(
                    Error(ErrorCode::DDSError, "DDS type registration context is indeterminate"));
            }
            type_registry_cv_.wait(lock);
        }
    }

    eprosima::fastrtps::types::ReturnCode_t result;
    try {
        result = participant_->register_type(dmw::fastdds::MessageTypeAdapter::type_support(type));
    } catch (...) {
        {
            std::lock_guard lock(type_registry_mutex_);
            const auto type_it = registered_types_.find(type_name);
            if (type_it != registered_types_.end()) {
                type_it->second.phase = RegistryEntryPhase::Orphaned;
            }
        }
        type_registry_cv_.notify_all();
        throw;
    }

    {
        std::lock_guard lock(type_registry_mutex_);
        const auto type_it = registered_types_.find(type_name);
        if (result == eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK) {
            type_it->second.phase = RegistryEntryPhase::Active;
            type_it->second.endpoint_reference_count = 1;
        } else {
            registered_types_.erase(type_it);
        }
    }
    type_registry_cv_.notify_all();
    if (result != eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK) {
        return Result<TypeRegistration>::failure(
            to_error(result, "Fast DDS failed to register the message type"));
    }
    return Result<TypeRegistration>::success(TypeRegistration(this, type_name));
}

Result<Topic> Context::acquire_topic(
    const MessageType& type, const std::string& dds_topic_name, const Qos& qos) {
    const std::string type_name(type.type_name());
    auto type_registration = acquire_type(type);
    if (!type_registration) return Result<Topic>::failure(std::move(type_registration.error()));

    bool create_topic = false;
    {
        std::unique_lock lock(topic_registry_mutex_);
        while (true) {
            if (topic_registry_degraded_) {
                return Result<Topic>::failure(
                    Error(ErrorCode::DDSError, "DDS topic registry is degraded"));
            }
            const auto topic_it = topics_.find(dds_topic_name);
            if (topic_it == topics_.end()) {
                topics_.emplace(dds_topic_name, RegisteredTopic(type_name, qos));
                create_topic = true;
                break;
            }
            if (topic_it->second.wire_type_name != type_name) {
                return Result<Topic>::failure(
                    Error(ErrorCode::TypeMismatch, "DDS topic already has a different wire type"));
            }
            if (!same_topic_qos(topic_it->second.qos, qos)) {
                topic_registry_degraded_ = true;
                return Result<Topic>::failure(
                    Error(ErrorCode::DDSError, "DDS topic has conflicting canonical QoS"));
            }
            if (topic_it->second.phase == RegistryEntryPhase::Active) {
                ++topic_it->second.endpoint_reference_count;
                return Result<Topic>::success(Topic(
                    this, topic_it->second.topic, dds_topic_name,
                    std::move(type_registration.value())));
            }
            if (topic_it->second.phase == RegistryEntryPhase::Orphaned) {
                return Result<Topic>::failure(
                    Error(ErrorCode::DDSError, "DDS topic creation context is indeterminate"));
            }
            topic_registry_cv_.wait(lock);
        }
    }

    if (create_topic) {
        eprosima::fastdds::dds::Topic* topic = nullptr;
        try {
            topic = participant_->create_topic(
                dds_topic_name, type_name, eprosima::fastdds::dds::TOPIC_QOS_DEFAULT);
        } catch (...) {
            {
                std::lock_guard lock(topic_registry_mutex_);
                const auto topic_it = topics_.find(dds_topic_name);
                if (topic_it != topics_.end()) {
                    topic_it->second.phase = RegistryEntryPhase::Orphaned;
                }
            }
            topic_registry_cv_.notify_all();
            // A throwing Fast DDS create call can leave an indeterminate DDS
            // side effect.  Retain its TypeSupport until Context teardown.
            type_registration.value().disarm();
            throw;
        }

        {
            std::lock_guard lock(topic_registry_mutex_);
            const auto topic_it = topics_.find(dds_topic_name);
            if (topic == nullptr) {
                topics_.erase(topic_it);
            } else {
                topic_it->second.topic = topic;
                topic_it->second.phase = RegistryEntryPhase::Active;
                topic_it->second.endpoint_reference_count = 1;
            }
        }
        topic_registry_cv_.notify_all();
        if (topic == nullptr) {
            return Result<Topic>::failure(
                Error(ErrorCode::DDSError, "Fast DDS failed to create the topic"));
        }
        return Result<Topic>::success(
            Topic(this, topic, dds_topic_name, std::move(type_registration.value())));
    }

    return Result<Topic>::failure(
        Error(ErrorCode::DDSError, "DDS topic creation did not complete"));
}

bool Context::release_topic(std::string topic_name) noexcept {
    eprosima::fastdds::dds::Topic* topic = nullptr;
    {
        std::lock_guard lock(topic_registry_mutex_);
        const auto topic_it = topics_.find(topic_name);
        if (topic_it == topics_.end() || topic_it->second.endpoint_reference_count == 0) {
            return false;
        }
        --topic_it->second.endpoint_reference_count;
        if (topic_it->second.endpoint_reference_count != 0 ||
            topic_it->second.phase != RegistryEntryPhase::Active) {
            return topic_it->second.phase == RegistryEntryPhase::Active;
        }
        topic_it->second.phase = RegistryEntryPhase::Retiring;
        topic = topic_it->second.topic;
    }

    bool deleted = false;
    try {
        deleted = participant_->delete_topic(topic) ==
                  eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK;
    } catch (...) {
        deleted = false;
    }
    if (!deleted) {
        {
            std::lock_guard lock(topic_registry_mutex_);
            const auto topic_it = topics_.find(topic_name);
            if (topic_it != topics_.end()) {
                topic_it->second.phase = RegistryEntryPhase::Orphaned;
            }
        }
        topic_registry_cv_.notify_all();
        return false;
    }

    {
        std::lock_guard lock(topic_registry_mutex_);
        const auto topic_it = topics_.find(topic_name);
        if (topic_it != topics_.end()) {
            topics_.erase(topic_it);
        }
    }
    topic_registry_cv_.notify_all();
    return true;
}

void Context::release_type(std::string type_name) noexcept {
    bool unregister_type = false;
    {
        std::lock_guard lock(type_registry_mutex_);
        const auto type_it = registered_types_.find(type_name);
        if (type_it == registered_types_.end() || type_it->second.endpoint_reference_count == 0) {
            return;
        }
        --type_it->second.endpoint_reference_count;
        if (type_it->second.endpoint_reference_count != 0 ||
            type_it->second.phase != RegistryEntryPhase::Active) {
            return;
        }
        type_it->second.phase = RegistryEntryPhase::Retiring;
        unregister_type = true;
    }
    if (!unregister_type) return;

    bool unregistered = false;
    try {
        unregistered = participant_->unregister_type(type_name) ==
                       eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK;
    } catch (...) {
        unregistered = false;
    }
    {
        std::lock_guard lock(type_registry_mutex_);
        const auto type_it = registered_types_.find(type_name);
        if (type_it == registered_types_.end()) {
            return;
        }
        if (unregistered) {
            registered_types_.erase(type_it);
        } else {
            type_it->second.phase = RegistryEntryPhase::Orphaned;
        }
    }
    type_registry_cv_.notify_all();
}

}  // namespace impl

Result<std::unique_ptr<Context>> Context::Impl::create(const ContextOptions& options) {
    auto* factory = eprosima::fastdds::dds::DomainParticipantFactory::get_instance();
    eprosima::fastdds::dds::DomainParticipantQos qos;
    if (options.runtime_mode == RuntimeMode::ROS2) {
        qos.wire_protocol().builtin.readerHistoryMemoryPolicy =
            eprosima::fastrtps::rtps::PREALLOCATED_WITH_REALLOC_MEMORY_MODE;
        qos.wire_protocol().builtin.writerHistoryMemoryPolicy =
            eprosima::fastrtps::rtps::PREALLOCATED_WITH_REALLOC_MEMORY_MODE;
    }
    if (!options.participant_name.empty()) {
        qos.name(options.participant_name);
    }

    auto* participant = factory->create_participant(options.domain_id, qos);
    if (participant == nullptr) {
        return Result<std::unique_ptr<Context>>::failure(
            Error(ErrorCode::DDSError, "Fast DDS failed to create a DomainParticipant"));
    }
    ParticipantCreationGuard participant_guard(factory, participant);

    auto* publisher = participant->create_publisher(eprosima::fastdds::dds::PUBLISHER_QOS_DEFAULT);
    auto* subscriber =
        participant->create_subscriber(eprosima::fastdds::dds::SUBSCRIBER_QOS_DEFAULT);
    if (publisher == nullptr || subscriber == nullptr) {
        return Result<std::unique_ptr<Context>>::failure(
            Error(ErrorCode::DDSError, "Fast DDS failed to create a Context container"));
    }

    auto context = std::make_shared<impl::Context>(
        factory, participant, publisher, subscriber, options.domain_id, options.runtime_mode);
    // From this point Context is the sole owner, including listener
    // installation failure paths.
    participant_guard.release();
    auto discovery = context->install_discovery_listener();
    if (!discovery) return Result<std::unique_ptr<Context>>::failure(std::move(discovery.error()));
    auto impl = std::make_unique<Impl>(std::move(context));
    return Result<std::unique_ptr<Context>>::success(
        std::unique_ptr<Context>(new Context(std::move(impl))));
}

std::uint32_t Context::Impl::domain_id() const noexcept { return context_->domain_id(); }
bool Context::Impl::is_shutdown() const noexcept { return context_->is_shutdown(); }
Result<void> Context::Impl::shutdown() {
    context_->shutdown();
    return Result<void>::success();
}

Result<std::unique_ptr<Node>> Context::Impl::create_node(const NodeOptions& options) {
    if (options.node_name.empty() || impl::has_invalid_name_syntax(options.node_name) ||
        options.node_name.find('/') != std::string::npos) {
        return Result<std::unique_ptr<Node>>::failure(
            Error(ErrorCode::InvalidName, "Node name contains unsupported syntax"));
    }
    auto node_namespace = impl::normalize_namespace(options.node_namespace);
    if (!node_namespace) {
        return Result<std::unique_ptr<Node>>::failure(std::move(node_namespace.error()));
    }
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<std::unique_ptr<Node>>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }

    auto node_impl = std::make_unique<Node::Impl>(
        context_, options.node_name, std::move(node_namespace.value()));
    return Result<std::unique_ptr<Node>>::success(
        std::unique_ptr<Node>(new Node(std::move(node_impl))));
}

Result<std::unique_ptr<GuardCondition>> Context::Impl::create_guard_condition(
    const GuardConditionOptions&) {
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<std::unique_ptr<GuardCondition>>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    auto guard_impl = std::make_unique<GuardCondition::Impl>(context_);
    return Result<std::unique_ptr<GuardCondition>>::success(
        std::unique_ptr<GuardCondition>(new GuardCondition(std::move(guard_impl))));
}

}  // namespace dmw
