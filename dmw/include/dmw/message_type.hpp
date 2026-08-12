// SPDX-License-Identifier: Apache-2.0

#ifndef DMW__MESSAGE_TYPE_HPP_
#define DMW__MESSAGE_TYPE_HPP_

#include <memory>
#include <string_view>
#include <utility>

#include "dmw/visibility_control.hpp"

namespace dmw {

namespace fastdds {
class MessageTypeAccess;
}  // namespace fastdds

/// Cheap-copy handle to an immutable middleware type binding descriptor.
class DMW_PUBLIC MessageType {
public:
    MessageType(const MessageType&) = default;
    MessageType& operator=(const MessageType&) = default;
    MessageType(MessageType&&) noexcept = default;
    MessageType& operator=(MessageType&&) noexcept = default;

    /// Return the DDS wire type name.
    std::string_view type_name() const noexcept;

private:
    friend class fastdds::MessageTypeAccess;

    class Impl;

    explicit MessageType(std::shared_ptr<const Impl> impl) noexcept : impl_(std::move(impl)) {}

    std::shared_ptr<const Impl> impl_;
};

}  // namespace dmw

#endif  // DMW__MESSAGE_TYPE_HPP_
