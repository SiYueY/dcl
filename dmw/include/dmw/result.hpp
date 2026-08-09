// SPDX-License-Identifier: Apache-2.0

#ifndef DMW__RESULT_HPP_
#define DMW__RESULT_HPP_

#include <exception>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

#include "dmw/error.hpp"

namespace dmw {

/// Holds either a successful value or an Error.
template <class T>
class Result {
    static_assert(!std::is_void<T>::value, "Use Result<void> for void results");
    static_assert(!std::is_reference<T>::value, "Result<T> does not store references");

public:
    static Result success(T value) {
        return Result(std::in_place_index<0>, std::move_if_noexcept(value));
    }

    static Result failure(Error error) { return Result(std::in_place_index<1>, std::move(error)); }

    bool has_value() const noexcept { return storage_.index() == 0; }

    explicit operator bool() const noexcept { return has_value(); }

    T& value() & noexcept {
        require_value();
        return std::get<0>(storage_);
    }

    const T& value() const& noexcept {
        require_value();
        return std::get<0>(storage_);
    }

    T&& value() && noexcept {
        require_value();
        return std::get<0>(std::move(storage_));
    }

    Error& error() & noexcept {
        require_error();
        return std::get<1>(storage_);
    }

    const Error& error() const& noexcept {
        require_error();
        return std::get<1>(storage_);
    }

    Error&& error() && noexcept {
        require_error();
        return std::get<1>(std::move(storage_));
    }

private:
    template <class U>
    explicit Result(std::in_place_index_t<0>, U&& value)
    : storage_(std::in_place_index<0>, std::forward<U>(value)) {}

    explicit Result(std::in_place_index_t<1>, Error error)
    : storage_(std::in_place_index<1>, std::move(error)) {}

    void require_value() const noexcept {
        if (!has_value()) {
            std::terminate();
        }
    }

    void require_error() const noexcept {
        if (has_value()) {
            std::terminate();
        }
    }

    std::variant<T, Error> storage_;
};

/// Result specialization for operations with no success value.
template <>
class Result<void> {
public:
    static Result success() { return Result(SuccessTag{}); }

    static Result failure(Error error) { return Result(std::move(error)); }

    bool has_value() const noexcept { return !error_.has_value(); }

    explicit operator bool() const noexcept { return has_value(); }

    void value() const noexcept {
        if (!has_value()) {
            std::terminate();
        }
    }

    Error& error() & noexcept {
        require_error();
        return *error_;
    }

    const Error& error() const& noexcept {
        require_error();
        return *error_;
    }

    Error&& error() && noexcept {
        require_error();
        return std::move(*error_);
    }

private:
    struct SuccessTag {};

    explicit Result(SuccessTag) noexcept {}

    explicit Result(Error error) : error_(std::move(error)) {}

    void require_error() const noexcept {
        if (has_value()) {
            std::terminate();
        }
    }

    std::optional<Error> error_;
};

}  // namespace dmw

#endif  // DMW__RESULT_HPP_
