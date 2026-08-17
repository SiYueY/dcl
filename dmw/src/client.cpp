#include "dmw/client.hpp"

#include <utility>

#include "impl/client_impl.hpp"

namespace dmw {

Client::Client(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Client::~Client() noexcept = default;

std::string_view Client::service_name() const noexcept { return impl_->service_name(); }

Result<RequestId> Client::send_request(const void* request) { return impl_->send_request(request); }

Result<TakeStatus> Client::take_response(void* response, RequestId& request_id) {
    return impl_->take_response(response, request_id);
}

Result<bool> Client::service_is_available() const { return impl_->service_is_available(); }

}  // namespace dmw
