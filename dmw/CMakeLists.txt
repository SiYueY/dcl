# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.16)

project(dmw VERSION 0.1.0 LANGUAGES CXX)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)
include(CTest)

option(DMW_ENABLE_DDS_INTEGRATION_TESTS "Run tests that require Fast DDS transports" OFF)
option(DMW_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(DMW_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

if(DMW_ENABLE_SANITIZERS AND DMW_ENABLE_TSAN)
    message(FATAL_ERROR "DMW_ENABLE_SANITIZERS and DMW_ENABLE_TSAN are mutually exclusive")
endif()

if(DMW_ENABLE_SANITIZERS OR DMW_ENABLE_TSAN)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "DMW sanitizers require a GNU or Clang compiler")
    endif()
endif()

if(DMW_ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()
if(DMW_ENABLE_TSAN)
    # GCC TSan on Ubuntu 22.04 requires a fixed executable layout; otherwise
    # it can abort before main() with "unexpected memory mapping".
    # Kernels with 32-bit mmap randomization additionally need ASLR disabled;
    # see cmake/tsan_suppressions.txt for the documented targeted-TSan command
    # (which also suppresses uninstrumented Fast DDS internals).
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -fno-pie)
    add_link_options(-fsanitize=thread -no-pie)
endif()

find_package(fastrtps 2.6.10 CONFIG REQUIRED)

# rmw_dds_common changed the Gid wire layout between ROS 2 generations: Humble
# carries char[24] while Rolling/Jazzy carry char[16].  DMW cannot link a ROS 2
# runtime package, so the graph metadata layout is selected from the Fast DDS
# generation that ships with the target ROS 2 distribution.  A single DMW build
# speaks one layout; mixing ROS 2 generations in one domain is not supported
# (the same limitation applies to ROS 2 itself).
set(
    DMW_RMW_GID_SIZE
    ""
    CACHE STRING
    "rmw_dds_common Gid octets (16 for Rolling/Jazzy, 24 for Humble); empty selects from the Fast DDS version"
)
if(DMW_RMW_GID_SIZE STREQUAL "")
    if(fastrtps_VERSION VERSION_LESS "2.13")
        set(DMW_RMW_GID_SIZE 24)
    else()
        set(DMW_RMW_GID_SIZE 16)
    endif()
endif()
message(STATUS "DMW graph metadata rmw Gid size: ${DMW_RMW_GID_SIZE}")

add_library(dmw
    src/context.cpp
    src/arguments.cpp
    src/clock.cpp
    src/impl/action_impl.cpp
    src/impl/action_goal_registry.cpp
    src/impl/context_impl.cpp
    src/client.cpp
    src/impl/client_impl.cpp
    src/event.cpp
    src/graph.cpp
    src/impl/graph_candidates.cpp
    src/impl/graph_metadata.cpp
    src/impl/event_impl.cpp
    src/impl/event_parent_state.cpp
    src/guard_condition.cpp
    src/impl/guard_condition_impl.cpp
    src/message_type.cpp
    src/impl/message_type.cpp
    src/node.cpp
    src/impl/node_impl.cpp
    src/impl/parameter_store.cpp
    src/publisher.cpp
    src/impl/publisher_impl.cpp
    src/server.cpp
    src/impl/server_impl.cpp
    src/subscriber.cpp
    src/impl/subscriber_impl.cpp
    src/timer.cpp
    src/wait_set.cpp
    src/impl/wait_set_impl.cpp
)
add_library(dmw::dmw ALIAS dmw)

# The Fast DDS binding is opt-in: ordinary consumers only need dmw::dmw,
# while generated TopicDataType users link this target before including the
# Fast DDS binding headers.
add_library(fastdds_binding INTERFACE)
add_library(dmw::fastdds_binding ALIAS fastdds_binding)

target_compile_features(dmw PUBLIC cxx_std_17)
target_compile_definitions(dmw PRIVATE DMW_BUILDING_LIBRARY)
target_compile_definitions(dmw PRIVATE DMW_RMW_GID_SIZE=${DMW_RMW_GID_SIZE})
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(dmw PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
target_include_directories(dmw
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(dmw PUBLIC fastrtps)
if(DMW_ENABLE_SANITIZERS)
    # Static DMW archives contain instrumented objects. Export the runtime
    # requirement so an installed consumer links them correctly as well.
    target_link_options(dmw INTERFACE -fsanitize=address,undefined)
endif()
if(DMW_ENABLE_TSAN)
    target_link_options(dmw INTERFACE -fsanitize=thread -no-pie)
endif()
target_compile_features(fastdds_binding INTERFACE cxx_std_17)
target_include_directories(fastdds_binding
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(fastdds_binding INTERFACE dmw fastrtps)

install(
    TARGETS dmw fastdds_binding
    EXPORT dmwTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(
    DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(
    EXPORT dmwTargets
    FILE dmwTargets.cmake
    NAMESPACE dmw::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dmw
)

configure_package_config_file(
    cmake/dmwConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/dmwConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dmw
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/dmwConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/dmwConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/dmwConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dmw
)

if(BUILD_TESTING)
    add_executable(dmw_message_type_test test/message_type_test.cpp)
    target_link_libraries(dmw_message_type_test PRIVATE dmw::fastdds_binding)
    add_test(NAME dmw.message_type COMMAND dmw_message_type_test)
    set_tests_properties(dmw.message_type PROPERTIES LABELS "unit")

    add_executable(dmw_qos_test test/qos_test.cpp)
    target_include_directories(dmw_qos_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(dmw_qos_test PRIVATE dmw::dmw)
    add_test(NAME dmw.qos COMMAND dmw_qos_test)
    set_tests_properties(dmw.qos PROPERTIES LABELS "unit")

    add_executable(dmw_arguments_test test/arguments_test.cpp)
    target_link_libraries(dmw_arguments_test PRIVATE dmw::dmw)
    add_test(NAME dmw.arguments COMMAND dmw_arguments_test)
    set_tests_properties(dmw.arguments PROPERTIES LABELS "unit")

    add_executable(dmw_clock_test test/clock_test.cpp)
    target_link_libraries(dmw_clock_test PRIVATE dmw::dmw)
    add_test(NAME dmw.clock COMMAND dmw_clock_test)
    set_tests_properties(dmw.clock PROPERTIES LABELS "unit")

    add_executable(dmw_public_api_link_test test/public_api_link_test.cpp)
    target_link_libraries(dmw_public_api_link_test PRIVATE dmw::dmw)
    add_test(NAME dmw.public_api_link COMMAND dmw_public_api_link_test)
    set_tests_properties(dmw.public_api_link PROPERTIES LABELS "build")

    add_executable(dmw_context_state_test test/context_state_test.cpp)
    target_include_directories(dmw_context_state_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(dmw_context_state_test PRIVATE dmw::dmw)
    add_test(NAME dmw.context_state COMMAND dmw_context_state_test)
    set_tests_properties(dmw.context_state PROPERTIES LABELS "unit")

    add_executable(dmw_identity_test test/identity_test.cpp)
    target_include_directories(dmw_identity_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(dmw_identity_test PRIVATE dmw::dmw)
    add_test(NAME dmw.identity COMMAND dmw_identity_test)
    set_tests_properties(dmw.identity PROPERTIES LABELS "unit")

    add_executable(dmw_temporary_sample_test test/temporary_sample_test.cpp)
    target_include_directories(dmw_temporary_sample_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(dmw_temporary_sample_test PRIVATE dmw::fastdds_binding)
    add_test(NAME dmw.temporary_sample COMMAND dmw_temporary_sample_test)
    set_tests_properties(dmw.temporary_sample PROPERTIES LABELS "unit")

    add_executable(dmw_event_parent_state_test test/event_parent_state_test.cpp)
    target_include_directories(dmw_event_parent_state_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(dmw_event_parent_state_test PRIVATE dmw::dmw)
    add_test(NAME dmw.event_parent_state COMMAND dmw_event_parent_state_test)
    set_tests_properties(dmw.event_parent_state PROPERTIES LABELS "unit")

    add_executable(dmw_request_response_test test/request_response_test.cpp)
    target_include_directories(dmw_request_response_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(dmw_request_response_test PRIVATE dmw::dmw)
    add_test(NAME dmw.request_response COMMAND dmw_request_response_test)
    set_tests_properties(dmw.request_response PROPERTIES LABELS "unit")

    add_executable(dmw_name_test test/name_test.cpp)
    target_include_directories(dmw_name_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(dmw_name_test PRIVATE dmw::dmw)
    add_test(NAME dmw.name COMMAND dmw_name_test)
    set_tests_properties(dmw.name PROPERTIES LABELS "unit")

    add_executable(dmw_wait_set_test test/wait_set_test.cpp)
    target_link_libraries(dmw_wait_set_test PRIVATE dmw::dmw)
    add_test(NAME dmw.wait_set COMMAND dmw_wait_set_test)
    set_tests_properties(dmw.wait_set PROPERTIES LABELS "unit")

    add_executable(dmw_timer_test test/timer_test.cpp)
    target_link_libraries(dmw_timer_test PRIVATE dmw::dmw)
    add_test(NAME dmw.timer COMMAND dmw_timer_test)
    set_tests_properties(dmw.timer PROPERTIES LABELS "unit")

    add_executable(dmw_graph_test test/graph_test.cpp)
    target_link_libraries(dmw_graph_test PRIVATE dmw::fastdds_binding)
    add_executable(dmw_parameter_test test/parameter_test.cpp)
    target_include_directories(dmw_parameter_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(dmw_parameter_test PRIVATE dmw::dmw)
    add_test(NAME dmw.parameter COMMAND dmw_parameter_test)
    set_tests_properties(dmw.parameter PROPERTIES LABELS "unit")

    add_executable(dmw_action_goal_test test/action_goal_test.cpp)
    target_include_directories(dmw_action_goal_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(dmw_action_goal_test PRIVATE dmw::dmw)
    add_test(NAME dmw.action_goal COMMAND dmw_action_goal_test)
    set_tests_properties(dmw.action_goal PROPERTIES LABELS "unit")

    add_executable(dmw_action_test test/action_test.cpp)
    target_link_libraries(dmw_action_test PRIVATE dmw::fastdds_binding)
    add_test(NAME dmw.action COMMAND dmw_action_test)
    set_tests_properties(
        dmw.action
        PROPERTIES
            ENVIRONMENT "DMW_ENABLE_DDS_INTEGRATION=1"
            LABELS "unit;integration"
            TIMEOUT 40
    )

    add_executable(dmw_v1_stress_test test/v1_stress_test.cpp)
    target_link_libraries(dmw_v1_stress_test PRIVATE dmw::fastdds_binding)
    add_test(NAME dmw.v1_stress COMMAND dmw_v1_stress_test)
    set_tests_properties(
        dmw.v1_stress
        PROPERTIES
            ENVIRONMENT "DMW_ENABLE_DDS_INTEGRATION=1"
            LABELS "integration;stress"
            TIMEOUT 120
    )

    add_executable(dmw_client_library_prototype_test test/client_library_prototype_test.cpp)
    target_link_libraries(dmw_client_library_prototype_test PRIVATE dmw::fastdds_binding)
    add_test(NAME dmw.client_library_prototype COMMAND dmw_client_library_prototype_test)
    set_tests_properties(
        dmw.client_library_prototype
        PROPERTIES
            ENVIRONMENT "DMW_ENABLE_DDS_INTEGRATION=1"
            LABELS "integration;prototype"
            TIMEOUT 60
    )

    add_executable(dmw_remapping_test test/remapping_test.cpp)
    target_link_libraries(dmw_remapping_test PRIVATE dmw::fastdds_binding)
    add_test(NAME dmw.remapping COMMAND dmw_remapping_test)
    set_tests_properties(
        dmw.remapping
        PROPERTIES
            ENVIRONMENT "DMW_ENABLE_DDS_INTEGRATION=1"
            LABELS "integration;naming"
            TIMEOUT 60
    )

    add_executable(dmw_qos_operation_test test/qos_operation_test.cpp)
    target_link_libraries(dmw_qos_operation_test PRIVATE dmw::fastdds_binding)
    add_test(NAME dmw.qos_operation COMMAND dmw_qos_operation_test)
    set_tests_properties(
        dmw.qos_operation
        PROPERTIES
            ENVIRONMENT "DMW_ENABLE_DDS_INTEGRATION=1"
            LABELS "integration;qos"
            TIMEOUT 60
    )

    add_executable(dmw_graph_revision_test test/graph_revision_test.cpp)
    target_include_directories(dmw_graph_revision_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(dmw_graph_revision_test PRIVATE dmw::dmw)
    add_test(NAME dmw.graph_revision COMMAND dmw_graph_revision_test)
    set_tests_properties(dmw.graph_revision PROPERTIES LABELS "unit")

    add_executable(dmw_server_fsm_test test/server_fsm_test.cpp)
    target_link_libraries(dmw_server_fsm_test PRIVATE dmw::fastdds_binding)
    add_test(NAME dmw.server_fsm COMMAND dmw_server_fsm_test)
    set_tests_properties(
        dmw.server_fsm
        PROPERTIES
            ENVIRONMENT "DMW_ENABLE_DDS_INTEGRATION=1"
            LABELS "integration;service"
            TIMEOUT 60
    )

    add_test(NAME dmw.graph COMMAND dmw_graph_test)
    set_tests_properties(
        dmw.graph
        PROPERTIES
            ENVIRONMENT "DMW_ENABLE_DDS_INTEGRATION=1"
            LABELS "unit;integration"
            TIMEOUT 30
    )

    if(DMW_ENABLE_DDS_INTEGRATION_TESTS)
        add_executable(dmw_interop_peer test/interop_peer.cpp)
        target_link_libraries(dmw_interop_peer PRIVATE dmw::fastdds_binding)

        add_executable(dmw_benchmark test/benchmark.cpp)
        target_include_directories(dmw_benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_link_libraries(dmw_benchmark PRIVATE dmw::fastdds_binding)

        set_tests_properties(
            dmw.message_type
            dmw.wait_set
            PROPERTIES
                ENVIRONMENT "DMW_ENABLE_DDS_INTEGRATION=1"
                LABELS "unit;integration"
                TIMEOUT 30
        )

        add_executable(dmw_lifecycle_stress_test test/lifecycle_stress_test.cpp)
        target_link_libraries(dmw_lifecycle_stress_test PRIVATE dmw::fastdds_binding)
        add_test(NAME dmw.lifecycle_stress COMMAND dmw_lifecycle_stress_test)
        set_tests_properties(
            dmw.lifecycle_stress
            PROPERTIES
                ENVIRONMENT "DMW_ENABLE_DDS_INTEGRATION=1"
                LABELS "integration;stress"
                TIMEOUT 30
        )

        find_package(rclcpp QUIET)
        find_package(std_msgs QUIET)
        find_package(example_interfaces QUIET)
        # The example_interfaces-based interop tests must be built against the
        # same ROS 2 installation as rclcpp: mixing distributions links
        # incompatible ABIs and produces tests that cannot pass.
        set(dmw_example_interfaces_usable FALSE)
        if(rclcpp_FOUND AND example_interfaces_FOUND)
            string(REGEX REPLACE "/share/rclcpp/cmake$" "" dmw_rclcpp_prefix "${rclcpp_DIR}")
            string(
                REGEX REPLACE "/share/example_interfaces/cmake$" ""
                dmw_example_interfaces_prefix "${example_interfaces_DIR}"
            )
            if(dmw_rclcpp_prefix STREQUAL dmw_example_interfaces_prefix)
                set(dmw_example_interfaces_usable TRUE)
            else()
                message(
                    STATUS
                    "example_interfaces (${dmw_example_interfaces_prefix}) does not match rclcpp "
                    "(${dmw_rclcpp_prefix}); skipping interop tests that need it"
                )
            endif()
        endif()
        if(rclcpp_FOUND AND std_msgs_FOUND)
            add_executable(dmw_ros2_topic_interop_test test/ros2_topic_interop_test.cpp)
            target_link_libraries(
                dmw_ros2_topic_interop_test
                PRIVATE
                    dmw::fastdds_binding
                    rclcpp::rclcpp
                    std_msgs::std_msgs__rosidl_typesupport_cpp
                    std_msgs::std_msgs__rosidl_typesupport_fastrtps_cpp
            )
            add_test(NAME dmw.ros2_topic_interop COMMAND dmw_ros2_topic_interop_test)
            set_tests_properties(
                dmw.ros2_topic_interop
                PROPERTIES
                    ENVIRONMENT "FASTDDS_BUILTIN_TRANSPORTS=UDPv4"
                    LABELS "integration;ros2"
                    TIMEOUT 30
            )
        else()
            message(STATUS "ROS 2 Fast DDS dependencies not found; skipping ROS 2 interop test")
        endif()
        if(rclcpp_FOUND AND dmw_example_interfaces_usable)
            add_executable(dmw_ros2_service_interop_test test/ros2_service_interop_test.cpp)
            target_link_libraries(
                dmw_ros2_service_interop_test
                PRIVATE
                    dmw::fastdds_binding
                    rclcpp::rclcpp
                    example_interfaces::example_interfaces__rosidl_typesupport_cpp
                    example_interfaces::example_interfaces__rosidl_typesupport_fastrtps_cpp
            )
            add_test(NAME dmw.ros2_service_interop COMMAND dmw_ros2_service_interop_test)
            set_tests_properties(
                dmw.ros2_service_interop
                PROPERTIES
                    ENVIRONMENT "FASTDDS_BUILTIN_TRANSPORTS=UDPv4"
                    LABELS "integration;ros2"
                    TIMEOUT 30
            )
        else()
            message(STATUS "ROS 2 Service dependencies not found; skipping Service interop test")
        endif()
        find_package(rclcpp_action QUIET)
        find_package(action_msgs QUIET)
        find_package(std_srvs QUIET)
        if(rclcpp_FOUND AND std_srvs_FOUND)
            # std_srvs is present in every supported ROS 2 distribution, so this
            # service interop test also runs on the Rolling/Jazzy-era stack.
            add_executable(
                dmw_ros2_service_setbool_interop_test
                test/ros2_service_setbool_interop_test.cpp
            )
            target_link_libraries(
                dmw_ros2_service_setbool_interop_test
                PRIVATE
                    dmw::fastdds_binding
                    rclcpp::rclcpp
                    std_srvs::std_srvs__rosidl_typesupport_cpp
                    std_srvs::std_srvs__rosidl_typesupport_fastrtps_cpp
            )
            add_test(
                NAME dmw.ros2_service_setbool_interop
                COMMAND dmw_ros2_service_setbool_interop_test
            )
            set_tests_properties(
                dmw.ros2_service_setbool_interop
                PROPERTIES
                    ENVIRONMENT "FASTDDS_BUILTIN_TRANSPORTS=UDPv4"
                    LABELS "integration;ros2"
                    TIMEOUT 60
            )
        else()
            message(STATUS "std_srvs not found; skipping SetBool service interop test")
        endif()
        if(rclcpp_FOUND AND std_msgs_FOUND)
            add_executable(dmw_ros2_graph_interop_test test/ros2_graph_interop_test.cpp)
            target_link_libraries(
                dmw_ros2_graph_interop_test
                PRIVATE
                    dmw::fastdds_binding
                    rclcpp::rclcpp
                    std_msgs::std_msgs__rosidl_typesupport_cpp
                    std_msgs::std_msgs__rosidl_typesupport_fastrtps_cpp
            )
            add_test(NAME dmw.ros2_graph_interop COMMAND dmw_ros2_graph_interop_test)
            set_tests_properties(
                dmw.ros2_graph_interop
                PROPERTIES
                    ENVIRONMENT "FASTDDS_BUILTIN_TRANSPORTS=UDPv4"
                    LABELS "integration;ros2"
                    TIMEOUT 60
            )
        else()
            message(STATUS "ROS 2 graph dependencies not found; skipping Graph interop test")
        endif()
        if(rclcpp_action_FOUND AND action_msgs_FOUND AND dmw_example_interfaces_usable)
            add_executable(dmw_ros2_action_interop_test test/ros2_action_interop_test.cpp)
            target_link_libraries(
                dmw_ros2_action_interop_test
                PRIVATE
                    dmw::fastdds_binding
                    rclcpp::rclcpp
                    rclcpp_action::rclcpp_action
                    example_interfaces::example_interfaces__rosidl_typesupport_cpp
                    example_interfaces::example_interfaces__rosidl_typesupport_fastrtps_cpp
                    action_msgs::action_msgs__rosidl_typesupport_cpp
                    action_msgs::action_msgs__rosidl_typesupport_fastrtps_cpp
            )
            add_test(NAME dmw.ros2_action_interop COMMAND dmw_ros2_action_interop_test)
            set_tests_properties(
                dmw.ros2_action_interop
                PROPERTIES
                    ENVIRONMENT "FASTDDS_BUILTIN_TRANSPORTS=UDPv4"
                    LABELS "integration;ros2"
                    TIMEOUT 60
            )
        else()
            message(STATUS "ROS 2 Action dependencies not found; skipping Action interop test")
        endif()

        # When the installation under test has no action interface package, the
        # same test can run against the workspace-local Fibonacci definition
        # (see test/ros2_action_test_interfaces, which mirrors
        # example_interfaces/action/Fibonacci).
        find_package(dmw_action_test_interfaces QUIET)
        if(
            NOT dmw_example_interfaces_usable
            AND rclcpp_action_FOUND
            AND action_msgs_FOUND
            AND dmw_action_test_interfaces_FOUND
        )
            add_executable(dmw_ros2_action_local_interop_test test/ros2_action_interop_test.cpp)
            target_compile_definitions(
                dmw_ros2_action_local_interop_test
                PRIVATE
                    DMW_ROS_ACTION_INTERFACE_HEADER="dmw_action_test_interfaces/action/fibonacci.hpp"
                    DMW_ROS_ACTION_INTERFACE_NAMESPACE=dmw_action_test_interfaces::action
                    DMW_ROS_ACTION_TYPE_NAME_PREFIX="dmw_action_test_interfaces::action"
            )
            target_link_libraries(
                dmw_ros2_action_local_interop_test
                PRIVATE
                    dmw::fastdds_binding
                    rclcpp::rclcpp
                    rclcpp_action::rclcpp_action
                    dmw_action_test_interfaces::dmw_action_test_interfaces__rosidl_typesupport_cpp
                    dmw_action_test_interfaces::dmw_action_test_interfaces__rosidl_typesupport_fastrtps_cpp
                    action_msgs::action_msgs__rosidl_typesupport_cpp
                    action_msgs::action_msgs__rosidl_typesupport_fastrtps_cpp
            )
            add_test(
                NAME dmw.ros2_action_local_interop
                COMMAND dmw_ros2_action_local_interop_test
            )
            set_tests_properties(
                dmw.ros2_action_local_interop
                PROPERTIES
                    ENVIRONMENT "FASTDDS_BUILTIN_TRANSPORTS=UDPv4"
                    LABELS "integration;ros2"
                    TIMEOUT 60
            )
        endif()
    endif()

    file(GLOB_RECURSE dmw_public_headers CONFIGURE_DEPENDS
        RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/include/dmw/*.hpp
    )

    set(dmw_header_check_targets)
    foreach(dmw_header IN LISTS dmw_public_headers)
        string(MD5 dmw_header_id ${dmw_header})
        set(dmw_header_source
            ${CMAKE_CURRENT_BINARY_DIR}/header_checks/${dmw_header_id}.cpp)
        file(GENERATE
            OUTPUT ${dmw_header_source}
            CONTENT "#include <${dmw_header}>\nint main() { return 0; }\n"
        )
        set(dmw_header_target dmw_header_check_${dmw_header_id})
        add_library(${dmw_header_target} OBJECT EXCLUDE_FROM_ALL ${dmw_header_source})
        if(dmw_header STREQUAL "dmw/fastdds/message_type.hpp")
            target_link_libraries(${dmw_header_target} PRIVATE dmw::fastdds_binding)
        else()
            target_link_libraries(${dmw_header_target} PRIVATE dmw::dmw)
        endif()
        list(APPEND dmw_header_check_targets ${dmw_header_target})
    endforeach()

    add_custom_target(dmw_header_checks ALL DEPENDS ${dmw_header_check_targets})
    add_test(
        NAME dmw.header_checks
        COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target dmw_header_checks
    )
    set_tests_properties(dmw.header_checks PROPERTIES LABELS "build")

    add_test(
        NAME dmw.install_consumer
        COMMAND ${CMAKE_COMMAND}
            -DDMW_BUILD_DIR=${CMAKE_BINARY_DIR}
            -DDMW_CONSUMER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/test/package_consumer
            -DDMW_CONSUMER_BINARY_DIR=${CMAKE_BINARY_DIR}/package_consumer
            -DFASTDDS_DIR=${fastrtps_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/test/verify_install_consumer.cmake
    )
    set_tests_properties(dmw.install_consumer PROPERTIES LABELS "package")
endif()
