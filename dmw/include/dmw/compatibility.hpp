// SPDX-License-Identifier: Apache-2.0

#ifndef DMW__COMPATIBILITY_HPP_
#define DMW__COMPATIBILITY_HPP_

namespace dmw {

/// Selects native DDS behavior or the validated ROS 2 Humble wire profile.
enum class CompatibilityProfile { NativeDds, Ros2FastDdsHumble };

}  // namespace dmw

#endif  // DMW__COMPATIBILITY_HPP_
