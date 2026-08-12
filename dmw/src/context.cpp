// SPDX-License-Identifier: Apache-2.0

#include "dmw/context.hpp"

#include <memory>
#include <string>
#include <utility>

#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/domain/qos/DomainParticipantQos.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>

#include "dmw/error.hpp"
#include "impl/context_impl.hpp"
#include "dmw/fastdds/message_type.hpp"
#include "impl/fastdds/context_state.hpp"
#include "impl/guard_condition_impl.hpp"
#include "impl/name.hpp"
#include "impl/node_impl.hpp"

namespace dmw {

namespace {

class ParticipantCreationGuard {
public:
    ParticipantCreationGuard(
        eprosima::fastdds::dds::DomainParticipantFactory* factory,
        eprosima::fastdds::dds::DomainParticipant* participant) noexcept
    : factory_(factory), participant_(participant) {}

    ~ParticipantCreationGuard() noexcept {
        if (participant_ != nullptr) {
            try {
                participant_->delete_contained_entities();
                factory_->delete_participant(participant_);
            } catch (...) {
                // Factory rollback is best effort.  The original creation
                // exception remains the observable failure.
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
namespace fastdds {

ContextState::ContextState(
    eprosima::fastdds::dds::DomainParticipantFactory* factory,
    eprosima::fastdds::dds::DomainParticipant* participant,
    eprosima::fastdds::dds::Publisher* publisher, eprosima::fastdds::dds::Subscriber* subscriber,
    std::uint32_t domain_id, CompatibilityProfile compatibility_profile) noexcept
: factory_(factory),
  participant_(participant),
  publisher_(publisher),
  subscriber_(subscriber),
  domain_id_(domain_id),
  compatibility_profile_(compatibility_profile) {}

ContextState::~ContextState() noexcept {
    if (participant_ != nullptr) {
        try {
            participant_->delete_contained_entities();
        } catch (...) {
            // Destruction is best effort.  A later participant deletion may
            // still release resources that were not individually deleted.
        }
        try {
            factory_->delete_participant(participant_);
        } catch (...) {
            // A failed Fast DDS cleanup must not escape a noexcept destructor.
        }
    }
}

eprosima::fastdds::dds::DomainParticipant* ContextState::participant() const noexcept {
    return participant_;
}

eprosima::fastdds::dds::Publisher* ContextState::publisher() const noexcept { return publisher_; }

eprosima::fastdds::dds::Subscriber* ContextState::subscriber() const noexcept {
    return subscriber_;
}

std::uint32_t ContextState::domain_id() const noexcept { return domain_id_; }

CompatibilityProfile ContextState::compatibility_profile() const noexcept {
    return compatibility_profile_;
}

ContextState::TopicLease::~TopicLease() noexcept { reset(); }

ContextState::TypeLease::~TypeLease() noexcept { reset(); }

ContextState::TypeLease::TypeLease(TypeLease&& other) noexcept
: context_(std::exchange(other.context_, nullptr)), type_name_(std::move(other.type_name_)) {}

ContextState::TypeLease& ContextState::TypeLease::operator=(TypeLease&& other) noexcept {
    if (this != &other) {
        reset();
        context_ = std::exchange(other.context_, nullptr);
        type_name_ = std::move(other.type_name_);
    }
    return *this;
}

void ContextState::TypeLease::reset() noexcept {
    if (context_ != nullptr) {
        context_->release_type(std::move(type_name_));
        context_ = nullptr;
    }
}

void ContextState::TypeLease::disarm() noexcept {
    context_ = nullptr;
    type_name_.clear();
}

ContextState::TopicLease::TopicLease(TopicLease&& other) noexcept
: context_(std::exchange(other.context_, nullptr)),
  topic_(std::exchange(other.topic_, nullptr)),
  topic_name_(std::move(other.topic_name_)),
  type_lease_(std::move(other.type_lease_)) {}

ContextState::TopicLease& ContextState::TopicLease::operator=(TopicLease&& other) noexcept {
    if (this != &other) {
        reset();
        context_ = std::exchange(other.context_, nullptr);
        topic_ = std::exchange(other.topic_, nullptr);
        topic_name_ = std::move(other.topic_name_);
        type_lease_ = std::move(other.type_lease_);
    }
    return *this;
}

void ContextState::TopicLease::reset() noexcept {
    if (context_ != nullptr) {
        const bool topic_deleted = context_->release_topic(std::move(topic_name_));
        context_ = nullptr;
        topic_ = nullptr;
        if (!topic_deleted) {
            // An unresolved DDS Topic may still refer to its TypeSupport.
            // Keep the registry reference as a deliberate safety retention.
            type_lease_.disarm();
        }
    }
    type_lease_.reset();
}

bool ContextState::is_shutdown() const noexcept {
    return shutdown_.load(std::memory_order_acquire);
}

ContextState::OperationGuard::~OperationGuard() noexcept {
    if (state_ != nullptr) {
        std::lock_guard<std::mutex> lock(state_->operation_mutex_);
        --state_->active_operations_;
        if (state_->active_operations_ == 0) {
            state_->operation_cv_.notify_all();
        }
    }
}

ContextState::OperationGuard::OperationGuard(OperationGuard&& other) noexcept
: state_(std::exchange(other.state_, nullptr)) {}

ContextState::OperationGuard& ContextState::OperationGuard::operator=(
    OperationGuard&& other) noexcept {
    if (this != &other) {
        if (state_ != nullptr) {
            std::lock_guard<std::mutex> lock(state_->operation_mutex_);
            --state_->active_operations_;
            if (state_->active_operations_ == 0) {
                state_->operation_cv_.notify_all();
            }
        }
        state_ = std::exchange(other.state_, nullptr);
    }
    return *this;
}

ContextState::OperationGuard ContextState::try_acquire_operation() noexcept {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    if (shutdown_.load(std::memory_order_acquire)) {
        return OperationGuard{};
    }
    ++active_operations_;
    return OperationGuard(this);
}

void ContextState::shutdown() noexcept {
    {
        std::lock_guard<std::mutex> lock(operation_mutex_);
        if (shutdown_.load(std::memory_order_acquire)) {
            return;
        }
        shutdown_.store(true, std::memory_order_release);
    }

    decltype(shutdown_callbacks_) callbacks;
    {
        std::lock_guard<std::mutex> lock(shutdown_callbacks_mutex_);
        callbacks.swap(shutdown_callbacks_);
    }
    for (auto& callback : callbacks) {
        try {
            callback.second();
        } catch (...) {
            // Shutdown is noexcept. A best-effort notification must not prevent
            // later waitables from observing the terminal Context state.
        }
    }
    std::unique_lock<std::mutex> lock(operation_mutex_);
    operation_cv_.wait(lock, [this] { return active_operations_ == 0; });
}

std::uint64_t ContextState::register_shutdown_callback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(shutdown_callbacks_mutex_);
    if (shutdown_.load(std::memory_order_acquire) || next_shutdown_callback_id_ == 0) {
        return 0;
    }
    const auto id = next_shutdown_callback_id_++;
    shutdown_callbacks_.emplace(id, std::move(callback));
    return id;
}

void ContextState::unregister_shutdown_callback(std::uint64_t id) noexcept {
    if (id == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(shutdown_callbacks_mutex_);
    shutdown_callbacks_.erase(id);
}

Result<ContextState::TypeLease> ContextState::acquire_type(const MessageType& type) {
    const std::string type_name(type.type_name());
    const auto binding_type = detail::MessageTypeAccess::binding_type(type);
    {
        std::unique_lock<std::mutex> lock(registry_mutex_);
        while (true) {
            const auto type_it = registered_types_.find(type_name);
            if (type_it == registered_types_.end()) {
                registered_types_.emplace(type_name, RegisteredType(type, binding_type));
                break;
            }
            if (type_it->second.binding_type != binding_type) {
                return Result<TypeLease>::failure(
                    Error(ErrorCode::TypeMismatch, "DDS wire type name has a different binding"));
            }
            if (type_it->second.phase == RegistryEntryPhase::Active) {
                ++type_it->second.endpoint_reference_count;
                return Result<TypeLease>::success(TypeLease(this, type_name));
            }
            if (type_it->second.phase == RegistryEntryPhase::Orphaned) {
                return Result<TypeLease>::failure(
                    Error(ErrorCode::DdsError, "DDS type registration state is indeterminate"));
            }
            registry_cv_.wait(lock);
        }
    }

    eprosima::fastrtps::types::ReturnCode_t result;
    try {
        result = participant_->register_type(detail::MessageTypeAccess::type_support(type));
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(registry_mutex_);
            const auto type_it = registered_types_.find(type_name);
            if (type_it != registered_types_.end()) {
                type_it->second.phase = RegistryEntryPhase::Orphaned;
            }
        }
        registry_cv_.notify_all();
        throw;
    }

    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        const auto type_it = registered_types_.find(type_name);
        if (result == eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK) {
            type_it->second.phase = RegistryEntryPhase::Active;
            type_it->second.endpoint_reference_count = 1;
        } else {
            registered_types_.erase(type_it);
        }
    }
    registry_cv_.notify_all();
    if (result != eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK) {
        return Result<TypeLease>::failure(
            Error(ErrorCode::DdsError, "Fast DDS failed to register the message type"));
    }
    return Result<TypeLease>::success(TypeLease(this, type_name));
}

Result<ContextState::TopicLease> ContextState::acquire_topic(
    const MessageType& type, const std::string& dds_topic_name) {
    const std::string type_name(type.type_name());
    auto type_lease = acquire_type(type);
    if (!type_lease) return Result<TopicLease>::failure(std::move(type_lease.error()));

    bool create_topic = false;
    {
        std::unique_lock<std::mutex> lock(registry_mutex_);
        while (true) {
            const auto topic_it = topics_.find(dds_topic_name);
            if (topic_it == topics_.end()) {
                topics_.emplace(dds_topic_name, RegisteredTopic(type_name));
                create_topic = true;
                break;
            }
            if (topic_it->second.wire_type_name != type_name) {
                return Result<TopicLease>::failure(
                    Error(ErrorCode::TypeMismatch, "DDS topic already has a different wire type"));
            }
            if (topic_it->second.phase == RegistryEntryPhase::Active) {
                ++topic_it->second.endpoint_reference_count;
                return Result<TopicLease>::success(TopicLease(
                    this, topic_it->second.topic, dds_topic_name, std::move(type_lease.value())));
            }
            if (topic_it->second.phase == RegistryEntryPhase::Orphaned) {
                return Result<TopicLease>::failure(
                    Error(ErrorCode::DdsError, "DDS topic creation state is indeterminate"));
            }
            registry_cv_.wait(lock);
        }
    }

    if (create_topic) {
        eprosima::fastdds::dds::Topic* topic = nullptr;
        try {
            topic = participant_->create_topic(
                dds_topic_name, type_name, eprosima::fastdds::dds::TOPIC_QOS_DEFAULT);
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(registry_mutex_);
                const auto topic_it = topics_.find(dds_topic_name);
                if (topic_it != topics_.end()) {
                    topic_it->second.phase = RegistryEntryPhase::Orphaned;
                }
            }
            registry_cv_.notify_all();
            // A throwing Fast DDS create call can leave an indeterminate DDS
            // side effect.  Retain its TypeSupport until Context teardown.
            type_lease.value().disarm();
            throw;
        }

        {
            std::lock_guard<std::mutex> lock(registry_mutex_);
            const auto topic_it = topics_.find(dds_topic_name);
            if (topic == nullptr) {
                topics_.erase(topic_it);
            } else {
                topic_it->second.topic = topic;
                topic_it->second.phase = RegistryEntryPhase::Active;
                topic_it->second.endpoint_reference_count = 1;
            }
        }
        registry_cv_.notify_all();
        if (topic == nullptr) {
            return Result<TopicLease>::failure(
                Error(ErrorCode::DdsError, "Fast DDS failed to create the topic"));
        }
        return Result<TopicLease>::success(
            TopicLease(this, topic, dds_topic_name, std::move(type_lease.value())));
    }

    return Result<TopicLease>::failure(
        Error(ErrorCode::DdsError, "DDS topic creation did not complete"));
}

bool ContextState::release_topic(std::string topic_name) noexcept {
    eprosima::fastdds::dds::Topic* topic = nullptr;
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
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
            std::lock_guard<std::mutex> lock(registry_mutex_);
            const auto topic_it = topics_.find(topic_name);
            if (topic_it != topics_.end()) {
                topic_it->second.phase = RegistryEntryPhase::Orphaned;
            }
        }
        registry_cv_.notify_all();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        const auto topic_it = topics_.find(topic_name);
        if (topic_it != topics_.end()) {
            topics_.erase(topic_it);
        }
    }
    registry_cv_.notify_all();
    return true;
}

void ContextState::release_type(std::string type_name) noexcept {
    bool unregister_type = false;
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
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
        std::lock_guard<std::mutex> lock(registry_mutex_);
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
    registry_cv_.notify_all();
}

}  // namespace fastdds
}  // namespace impl

Result<std::unique_ptr<Context>> Context::create(const ContextOptions& options) {
    auto* factory = eprosima::fastdds::dds::DomainParticipantFactory::get_instance();
    eprosima::fastdds::dds::DomainParticipantQos qos;
    if (!options.participant_name.empty()) {
        qos.name(options.participant_name);
    }

    auto* participant = factory->create_participant(options.domain_id, qos);
    if (participant == nullptr) {
        return Result<std::unique_ptr<Context>>::failure(
            Error(ErrorCode::DdsError, "Fast DDS failed to create a DomainParticipant"));
    }
    ParticipantCreationGuard participant_guard(factory, participant);

    auto* publisher = participant->create_publisher(eprosima::fastdds::dds::PUBLISHER_QOS_DEFAULT);
    auto* subscriber =
        participant->create_subscriber(eprosima::fastdds::dds::SUBSCRIBER_QOS_DEFAULT);
    if (publisher == nullptr || subscriber == nullptr) {
        return Result<std::unique_ptr<Context>>::failure(
            Error(ErrorCode::DdsError, "Fast DDS failed to create a Context container"));
    }

    auto state = std::make_shared<impl::fastdds::ContextState>(
        factory, participant, publisher, subscriber, options.domain_id,
        options.compatibility_profile);
    auto impl = std::make_unique<Impl>(std::move(state));
    participant_guard.release();
    return Result<std::unique_ptr<Context>>::success(
        std::unique_ptr<Context>(new Context(std::move(impl))));
}

Context::Context(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Context::~Context() noexcept {
    if (impl_) {
        shutdown();
    }
}

std::uint32_t Context::domain_id() const noexcept { return impl_->state_->domain_id(); }

bool Context::is_shutdown() const noexcept { return impl_->state_->is_shutdown(); }

Result<void> Context::shutdown() {
    impl_->state_->shutdown();
    return Result<void>::success();
}

Result<std::unique_ptr<Node>> Context::create_node(const NodeOptions& options) {
    if (options.name.empty() || impl::has_invalid_name_syntax(options.name) ||
        options.name.find('/') != std::string::npos) {
        return Result<std::unique_ptr<Node>>::failure(
            Error(ErrorCode::InvalidName, "Node name contains unsupported syntax"));
    }
    auto ns = impl::normalize_namespace(options.ns);
    if (!ns) {
        return Result<std::unique_ptr<Node>>::failure(std::move(ns.error()));
    }
    const auto operation = impl_->state_->try_acquire_operation();
    if (!operation) {
        return Result<std::unique_ptr<Node>>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }

    auto node_impl =
        std::make_unique<Node::Impl>(impl_->state_, options.name, std::move(ns.value()));
    return Result<std::unique_ptr<Node>>::success(
        std::unique_ptr<Node>(new Node(std::move(node_impl))));
}

Result<std::unique_ptr<GuardCondition>> Context::create_guard_condition(
    const GuardConditionOptions&) {
    const auto operation = impl_->state_->try_acquire_operation();
    if (!operation) {
        return Result<std::unique_ptr<GuardCondition>>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    auto guard_impl = std::make_unique<GuardCondition::Impl>(impl_->state_);
    return Result<std::unique_ptr<GuardCondition>>::success(
        std::unique_ptr<GuardCondition>(new GuardCondition(std::move(guard_impl))));
}

}  // namespace dmw
