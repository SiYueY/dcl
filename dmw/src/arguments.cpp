#include "dmw/arguments.hpp"

#include <utility>

#include "dmw/error.hpp"

namespace dmw {

namespace {

Result<std::pair<std::string, std::string>> parse_assignment(std::string_view value) {
    const auto separator = value.find(":=");
    if (separator == std::string_view::npos || separator == 0 || separator + 2 == value.size()) {
        return Result<std::pair<std::string, std::string>>::failure(
            Error(ErrorCode::InvalidArgument, "ROS argument must use non-empty name:=value syntax"));
    }
    return Result<std::pair<std::string, std::string>>::success(
        {std::string(value.substr(0, separator)), std::string(value.substr(separator + 2))});
}

}  // namespace

Result<Arguments> parse_arguments(const std::vector<std::string>& arguments) {
    Arguments parsed;
    bool in_ros_arguments = false;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto& argument = arguments[index];
        if (argument == "--ros-args") {
            if (in_ros_arguments) {
                return Result<Arguments>::failure(
                    Error(ErrorCode::InvalidArgument, "Nested --ros-args is not valid"));
            }
            in_ros_arguments = true;
            continue;
        }
        if (argument == "--" && in_ros_arguments) {
            in_ros_arguments = false;
            continue;
        }
        if (!in_ros_arguments) {
            parsed.unparsed_arguments_.push_back(argument);
            continue;
        }
        if (argument != "-r" && argument != "--remap" && argument != "-p" &&
            argument != "--param") {
            return Result<Arguments>::failure(
                Error(ErrorCode::InvalidArgument, "Unsupported ROS argument"));
        }
        if (++index == arguments.size()) {
            return Result<Arguments>::failure(
                Error(ErrorCode::InvalidArgument, "ROS argument option requires a value"));
        }
        auto assignment = parse_assignment(arguments[index]);
        if (!assignment) return Result<Arguments>::failure(std::move(assignment.error()));
        if (argument == "-p" || argument == "--param") {
            parsed.parameter_overrides_.push_back(
                {std::move(assignment.value().first), std::move(assignment.value().second)});
        } else if (assignment.value().first == "__node") {
            parsed.node_name_remap_ = std::move(assignment.value().second);
        } else if (assignment.value().first == "__ns") {
            parsed.namespace_remap_ = std::move(assignment.value().second);
        } else {
            parsed.remaps_.push_back(
                {std::move(assignment.value().first), std::move(assignment.value().second)});
        }
    }
    if (in_ros_arguments) {
        return Result<Arguments>::failure(
            Error(ErrorCode::InvalidArgument, "--ros-args must be terminated by --"));
    }
    return Result<Arguments>::success(std::move(parsed));
}

}  // namespace dmw
