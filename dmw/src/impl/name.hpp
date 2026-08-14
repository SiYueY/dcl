#ifndef DMW_IMPL__NAME_HPP_
#define DMW_IMPL__NAME_HPP_

#include <string>
#include <string_view>

#include "dmw/error.hpp"
#include "dmw/result.hpp"

namespace dmw {
namespace impl {

inline bool has_invalid_name_syntax(std::string_view value) noexcept {
    return value.find("//") != std::string_view::npos ||
           value.find('~') != std::string_view::npos || value.find('{') != std::string_view::npos ||
           value.find('}') != std::string_view::npos;
}

inline Result<std::string> normalize_namespace(std::string_view value) {
    if (value.empty()) {
        return Result<std::string>::success("/");
    }
    if (has_invalid_name_syntax(value)) {
        return Result<std::string>::failure(
            Error(ErrorCode::InvalidName, "Namespace contains unsupported syntax"));
    }

    std::string normalized;
    if (value.front() == '/') {
        normalized.assign(value);
    } else {
        normalized = "/" + std::string(value);
    }
    if (normalized.size() > 1 && normalized.back() == '/') {
        return Result<std::string>::failure(
            Error(ErrorCode::InvalidName, "Namespace must not have a trailing slash"));
    }
    return Result<std::string>::success(std::move(normalized));
}

inline Result<std::string> resolve_name(std::string_view node_namespace, std::string_view value) {
    if (value.empty() || has_invalid_name_syntax(value) || value.back() == '/') {
        return Result<std::string>::failure(
            Error(ErrorCode::InvalidName, "Name contains unsupported syntax"));
    }
    if (value.front() == '/') {
        return Result<std::string>::success(std::string(value));
    }
    if (node_namespace == "/") {
        return Result<std::string>::success("/" + std::string(value));
    }
    return Result<std::string>::success(std::string(node_namespace) + "/" + std::string(value));
}

}  // namespace impl
}  // namespace dmw

#endif  // DMW_IMPL__NAME_HPP_
