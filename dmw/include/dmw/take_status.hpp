// SPDX-License-Identifier: Apache-2.0

#ifndef DMW__TAKE_STATUS_HPP_
#define DMW__TAKE_STATUS_HPP_

namespace dmw {

/// Distinguishes a successful take from an empty reader history.
enum class TakeStatus { Taken, NoData };

}  // namespace dmw

#endif  // DMW__TAKE_STATUS_HPP_
