# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.16)

project(dmw_action_test_interfaces)

find_package(ament_cmake REQUIRED)
find_package(rosidl_default_generators REQUIRED)
find_package(builtin_interfaces REQUIRED)
find_package(unique_identifier_msgs REQUIRED)
find_package(action_msgs REQUIRED)

rosidl_generate_interfaces(
    ${PROJECT_NAME}
    "action/Fibonacci.action"
    DEPENDENCIES
        builtin_interfaces
        unique_identifier_msgs
        action_msgs
)

ament_export_dependencies(rosidl_default_runtime)
ament_package()
