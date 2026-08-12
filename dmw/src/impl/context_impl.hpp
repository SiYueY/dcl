// SPDX-License-Identifier: Apache-2.0

#ifndef DMW__IMPL__CONTEXT_IMPL_HPP_
#define DMW__IMPL__CONTEXT_IMPL_HPP_

#include <memory>

#include "dmw/context.hpp"
#include "impl/fastdds/context_state.hpp"

namespace dmw {

class Context::Impl {
public:
    explicit Impl(std::shared_ptr<impl::fastdds::ContextState> state) noexcept
    : state_(std::move(state)) {}

    std::shared_ptr<impl::fastdds::ContextState> state_;
};

}  // namespace dmw

#endif  // DMW__IMPL__CONTEXT_IMPL_HPP_
