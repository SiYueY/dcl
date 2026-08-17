#include "dmw/wait_set.hpp"

#include <utility>

#include "impl/context_impl.hpp"
#include "impl/wait_set_impl.hpp"

namespace dmw {

WaitSet::WaitSet(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

WaitSet::~WaitSet() noexcept = default;

Result<std::unique_ptr<WaitSet>> Context::create_wait_set(const WaitSetOptions& options) {
    return impl_->create_wait_set(options);
}

Result<WaitToken> WaitSet::add(GuardCondition& value) {
    return impl_->add(value.impl_->wait_state(), WaitableKind::GuardCondition);
}

Result<WaitToken> WaitSet::add(Subscriber& value) {
    return impl_->add(value.impl_->wait_state(), WaitableKind::Subscriber);
}

Result<WaitToken> WaitSet::add(Client& value) {
    return impl_->add(value.impl_->wait_state(), WaitableKind::Client);
}

Result<WaitToken> WaitSet::add(Server& value) {
    return impl_->add(value.impl_->wait_state(), WaitableKind::Server);
}

Result<WaitToken> WaitSet::add(Event& value) {
    return impl_->add(value.impl_->wait_state(), WaitableKind::Event);
}

Result<void> WaitSet::remove(WaitToken token) { return impl_->remove(token); }

Result<WaitResult> WaitSet::wait(WaitTimeout timeout) { return impl_->wait(timeout); }

}  // namespace dmw
