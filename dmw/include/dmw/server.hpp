#ifndef DMW_SERVER_HPP_
#define DMW_SERVER_HPP_

#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>

#include "dmw/request_id.hpp"
#include "dmw/result.hpp"
#include "dmw/take_status.hpp"
#include "dmw/visibility_control.hpp"

namespace dmw {

class Node;

struct ServerOptions {
    /// Maximum number of pending or responding requests tracked by this Server.
    std::size_t max_pending_requests{1024};
};

/// Type-erased service server and WaitSet waitable.
class DMW_PUBLIC Server {
public:
    ~Server() noexcept;

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;

    /// Take a request and register its RequestId as pending.
    Result<TakeStatus> take_request(void* request, RequestId& request_id);

    /// Respond only to a RequestId currently pending on this Server.
    Result<void> send_response(const RequestId& request_id, const void* response);

    std::string_view service_name() const noexcept;

private:
    friend class Node;
    friend class WaitSet;

    class Impl;

    explicit Server(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}  // namespace dmw

#endif  // DMW_SERVER_HPP_
