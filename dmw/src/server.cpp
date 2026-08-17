#include "dmw/server.hpp"

#include <utility>

#include "impl/server_impl.hpp"

namespace dmw {

Server::Server(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Server::~Server() noexcept = default;

std::string_view Server::service_name() const noexcept { return impl_->service_name(); }

Result<bool> Server::read_request(void* request, RequestId& request_id) {
    return impl_->read_request(request, request_id);
}

Result<void> Server::write_response(const RequestId& request_id, const void* response) {
    return impl_->write_response(request_id, response);
}

}  // namespace dmw
