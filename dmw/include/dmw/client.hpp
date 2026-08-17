#ifndef DMW_CLIENT_HPP_
#define DMW_CLIENT_HPP_

#include <memory>
#include <string_view>
#include <utility>

#include "dmw/request_id.hpp"
#include "dmw/result.hpp"
#include "dmw/visibility_control.hpp"

namespace dmw {

class Node;

/// Endpoint-local client options; compatibility is inherited from the parent Context.
struct ClientOptions {};

/// Type-erased service client and WaitSet waitable.
class DMW_PUBLIC Client {
public:
    ~Client() noexcept;

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&&) = delete;
    Client& operator=(Client&&) = delete;

    /// Send a non-null request object matching the Client's request MessageType.
    Result<RequestId> write_request(const void* request);

    /// Take a response addressed to this Client; false leaves both outputs unchanged.
    Result<bool> read_response(void* response, RequestId& request_id);

    /// Return a snapshot without combining endpoints from different participants.
    Result<bool> service_is_available() const;
    std::string_view service_name() const noexcept;

private:
    friend class Node;
    friend class WaitSet;

    class Impl;

    explicit Client(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}  // namespace dmw

#endif  // DMW_CLIENT_HPP_
