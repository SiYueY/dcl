#ifndef DMW_COMPATIBILITY_HPP_
#define DMW_COMPATIBILITY_HPP_

namespace dmw {

/// Selects native DDS behavior or the validated ROS 2 Humble wire profile.
enum class CompatibilityProfile { NativeDds, Ros2FastDdsHumble };

}  // namespace dmw

#endif  // DMW_COMPATIBILITY_HPP_
