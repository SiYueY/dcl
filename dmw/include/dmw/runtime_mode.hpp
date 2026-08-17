#ifndef DMW_RUNTIME_MODE_HPP_
#define DMW_RUNTIME_MODE_HPP_

namespace dmw {

/// Selects native DDS behavior or the validated ROS 2 Humble/Fast DDS wire mode.
enum class RuntimeMode {
    DDS,  // DDS
    ROS2  // ROS 2
};

}  // namespace dmw

#endif  // DMW_RUNTIME_MODE_HPP_
