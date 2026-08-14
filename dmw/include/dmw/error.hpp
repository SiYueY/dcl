#ifndef DMW_ERROR_HPP_
#define DMW_ERROR_HPP_

#include <string>
#include <string_view>
#include <utility>

namespace dmw {

/// Error codes returned by expected DMW failures.
enum class ErrorCode {
    InvalidArgument,
    InvalidState,
    InvalidName,
    TypeMismatch,
    AlreadyExists,
    NotFound,
    AlreadyRegistered,
    NotRegistered,
    Busy,
    Timeout,
    Unsupported,
    IncompatibleQos,
    ParentDestroyed,
    ResourceExhausted,
    DdsError,
    ContextShutdown
};

/// Describes an expected DMW failure.
class Error {
public:
    Error(ErrorCode code, std::string message) : code_(code), message_(std::move(message)) {}

    ErrorCode code() const noexcept { return code_; }

    std::string_view message() const noexcept { return message_; }

private:
    ErrorCode code_;
    std::string message_;
};

}  // namespace dmw

#endif  // DMW_ERROR_HPP_
