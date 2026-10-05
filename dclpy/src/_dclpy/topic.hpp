#ifndef DCLPY_DETAIL_TOPIC_HPP_
#define DCLPY_DETAIL_TOPIC_HPP_

#include "context.hpp"
#include "type_support.hpp"
#include "waitable.hpp"

namespace dclpy::detail {

class NativePublisher {
    const std::uint64_t entity_id_{next_entity_id()};
public:
    NativePublisher(std::shared_ptr<NativeNode> node,
                    std::shared_ptr<const MessageBindingHandle> type,
                    const std::string& name, const dmw::Qos& qos)
    : type_(std::move(type)), context_(node->context()),
      backing_(context_->adopt(unwrap(node->backing()->operation()->create_publisher(type_->type, name, qos)))) {}
    const std::shared_ptr<EntityBacking<dmw::Publisher>>& backing() const noexcept { return backing_; }
    const std::shared_ptr<const MessageBindingHandle>& type() const noexcept { return type_; }
    std::uint64_t entity_id() const noexcept { return entity_id_; }
    bool quiescent() const noexcept { return backing_->quiescent(); }
    void write(const OwnedSample& sample) {
        auto operation = backing_->operation();
        if (sample.binding()->binding != type_->binding)
            throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::TypeMismatch, "Publisher sample has the wrong descriptor"));
        unwrap(operation->write(sample.get()));
    }
    void retire_binding() {
        if (backing_->state() != EntityState::Closed)
            throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::Busy, "Publisher is not physically closed"));
        type_.reset();
    }
private:
    std::shared_ptr<const MessageBindingHandle> type_;
    std::shared_ptr<NativeContext> context_;
    std::shared_ptr<EntityBacking<dmw::Publisher>> backing_;
};

class NativeSubscription : public NativeWaitable {
public:
    NativeSubscription(std::shared_ptr<NativeNode> node,
                       std::shared_ptr<const MessageBindingHandle> type,
                       const std::string& name, const dmw::Qos& qos)
    : type_(std::move(type)), context_(node->context()),
      backing_(context_->adopt(unwrap(node->backing()->operation()->create_subscriber(type_->type, name, qos)))) {}
    const std::shared_ptr<EntityBacking<dmw::Subscriber>>& backing() const noexcept { return backing_; }
    const std::shared_ptr<const MessageBindingHandle>& type() const noexcept { return type_; }
    dmw::Result<dmw::WaitableRegistration> register_with(dmw::WaitSet& wait_set) override {
        return register_backing(backing_, wait_set);
    }
    void mark_unregistered() override { backing_->set_registered(false); }
    bool quiescent() const noexcept override { return backing_->quiescent(); }
    std::shared_ptr<DispatchPin> pin_dispatch() override {
        return std::make_shared<TypedDispatchPin<dmw::Subscriber>>(backing_->dispatch());
    }
    std::optional<dmw::MessageInfo> take(const std::shared_ptr<DispatchPin>& pin, OwnedSample& destination) {
        auto typed = std::dynamic_pointer_cast<TypedDispatchPin<dmw::Subscriber>>(pin);
        if (!typed || !typed->lease.belongs_to(backing_.get()))
            throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::TypeMismatch, "Dispatch pin belongs to another entity"));
        auto& lease = typed->lease;
        if (!lease) throw EntityClosed();
        if (destination.binding()->binding != type_->binding)
            throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::TypeMismatch, "Subscriber sample has the wrong descriptor"));
        dmw::MessageInfo info;
        if (!unwrap(lease->read(destination.get(), info))) return std::nullopt;
        return info;
    }
    void retire_binding() {
        if (backing_->state() != EntityState::Closed)
            throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::Busy, "Subscription is not physically closed"));
        type_.reset();
    }
private:
    std::shared_ptr<const MessageBindingHandle> type_;
    std::shared_ptr<NativeContext> context_;
    std::shared_ptr<EntityBacking<dmw::Subscriber>> backing_;
};

}  // namespace dclpy::detail

#endif  // DCLPY_DETAIL_TOPIC_HPP_
