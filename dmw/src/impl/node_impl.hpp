// SPDX-License-Identifier: Apache-2.0

#ifndef DMW__IMPL__NODE_IMPL_HPP_
#define DMW__IMPL__NODE_IMPL_HPP_

#include <memory>
#include <string>

#include "dmw/node.hpp"
#include "impl/fastdds/context_state.hpp"

namespace dmw {

class Node::Impl {
public:
    Impl(
        std::shared_ptr<impl::fastdds::ContextState> context_state, std::string name,
        std::string ns) noexcept
    : context_state_(std::move(context_state)), name_(std::move(name)), ns_(std::move(ns)) {}

    std::shared_ptr<impl::fastdds::ContextState> context_state_;
    std::string name_;
    std::string ns_;
};

}  // namespace dmw

#endif  // DMW__IMPL__NODE_IMPL_HPP_
