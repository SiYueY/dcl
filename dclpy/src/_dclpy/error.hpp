#ifndef DCLPY_DETAIL_ERROR_HPP_
#define DCLPY_DETAIL_ERROR_HPP_

#include <stdexcept>
#include <string>
#include <utility>

#include "dmw/result.hpp"

namespace dclpy::detail {

class MiddlewareFailure : public std::runtime_error {
public:
    explicit MiddlewareFailure(const dmw::Error& error)
    : std::runtime_error(std::string(error.message())), code(error.code()) {}
    dmw::ErrorCode code;
};

class EntityClosed : public std::runtime_error {
public:
    EntityClosed() : std::runtime_error("Entity is closing or closed") {}
};

class ContextShutdown : public std::runtime_error {
public:
    ContextShutdown() : std::runtime_error("Context no longer accepts user work") {}
};

template <typename T>
T unwrap(dmw::Result<T> result) {
    if (!result) throw MiddlewareFailure(result.error());
    return std::move(result).value();
}

inline void unwrap(dmw::Result<void> result) {
    if (!result) throw MiddlewareFailure(result.error());
}

}  // namespace dclpy::detail

#endif  // DCLPY_DETAIL_ERROR_HPP_
