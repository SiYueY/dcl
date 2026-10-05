#ifndef DCLPY_DETAIL_SERVICE_HPP_
#define DCLPY_DETAIL_SERVICE_HPP_

#include "context.hpp"
#include "type_support.hpp"
#include "waitable.hpp"

namespace dclpy::detail {

class NativeClient : public NativeWaitable {
public:
    NativeClient(std::shared_ptr<NativeNode> node, std::shared_ptr<const ServiceBindingHandle> type,
                 const std::string& name, const dmw::Qos& qos)
    : type_(std::move(type)), context_(node->context()),
      backing_(context_->adopt(unwrap(node->backing()->operation()->create_client(type_->type, name, qos)),
                               [](dmw::Client& client) noexcept { (void)client.interrupt_waits(); })) {}
    const std::shared_ptr<EntityBacking<dmw::Client>>& backing() const noexcept { return backing_; }
    const std::shared_ptr<const ServiceBindingHandle>& type() const noexcept { return type_; }
    dmw::Result<dmw::WaitableRegistration> register_with(dmw::WaitSet& wait_set) override {
        return register_backing(backing_, wait_set);
    }
    void mark_unregistered() override { backing_->set_registered(false); }
    bool quiescent() const noexcept override { return backing_->quiescent(); }
    std::shared_ptr<DispatchPin> pin_dispatch() override {
        return std::make_shared<TypedDispatchPin<dmw::Client>>(backing_->dispatch());
    }
    void retire_binding() {
        if (backing_->state() != EntityState::Closed)
            throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::Busy, "Client is not physically closed"));
        type_.reset();
    }
private:
    std::shared_ptr<const ServiceBindingHandle> type_;
    std::shared_ptr<NativeContext> context_;
    std::shared_ptr<EntityBacking<dmw::Client>> backing_;
};

class NativeService : public NativeWaitable {
public:
    NativeService(std::shared_ptr<NativeNode> node, std::shared_ptr<const ServiceBindingHandle> type,
                  const std::string& name, const dmw::Qos& qos, std::size_t pending_limit)
    : type_(std::move(type)), context_(node->context()),
      backing_(context_->adopt(unwrap(node->backing()->operation()->create_server(
          type_->type, name, qos, dmw::ServerOptions{pending_limit})))) {}
    const std::shared_ptr<EntityBacking<dmw::Server>>& backing() const noexcept { return backing_; }
    const std::shared_ptr<const ServiceBindingHandle>& type() const noexcept { return type_; }
    dmw::Result<dmw::WaitableRegistration> register_with(dmw::WaitSet& wait_set) override {
        return register_backing(backing_, wait_set);
    }
    void mark_unregistered() override { backing_->set_registered(false); }
    bool quiescent() const noexcept override { return backing_->quiescent(); }
    std::shared_ptr<DispatchPin> pin_dispatch() override {
        return std::make_shared<TypedDispatchPin<dmw::Server>>(backing_->dispatch());
    }
    void retire_binding() {
        if (backing_->state() != EntityState::Closed)
            throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::Busy, "Service is not physically closed"));
        type_.reset();
    }
private:
    std::shared_ptr<const ServiceBindingHandle> type_;
    std::shared_ptr<NativeContext> context_;
    std::shared_ptr<EntityBacking<dmw::Server>> backing_;
};

}  // namespace dclpy::detail
#endif  // DCLPY_DETAIL_SERVICE_HPP_
