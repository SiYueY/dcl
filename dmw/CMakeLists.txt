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
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -fno-pie)
    add_link_options(-fsanitize=thread -no-pie)
endif()

find_package(fastrtps 2.6.10 CONFIG REQUIRED)

add_library(dmw
    src/context.cpp
    src/client.cpp
    src/event.cpp
    src/event_parent_state.cpp
    src/guard_condition.cpp
    src/message_type.cpp
    src/node.cpp
    src/publisher.cpp
    src/server.cpp
    src/subscriber.cpp
    src/wait_set.cpp
)
add_library(dmw::dmw ALIAS dmw)

# The Fast DDS binding is opt-in: ordinary consumers only need dmw::dmw,
# while generated TopicDataType users link this target before including the
# Fast DDS binding headers.
add_library(fastdds_binding INTERFACE)
add_library(dmw::fastdds_binding ALIAS fastdds_binding)

target_compile_features(dmw PUBLIC cxx_std_17)
target_compile_definitions(dmw PRIVATE DMW_BUILDING_LIBRARY)
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

    add_executable(dmw_service_match_state_test test/service_match_state_test.cpp)
    target_include_directories(dmw_service_match_state_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(dmw_service_match_state_test PRIVATE dmw::dmw)
    add_test(NAME dmw.service_match_state COMMAND dmw_service_match_state_test)
    set_tests_properties(dmw.service_match_state PROPERTIES LABELS "unit")

    add_executable(dmw_name_test test/name_test.cpp)
    target_include_directories(dmw_name_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(dmw_name_test PRIVATE dmw::dmw)
    add_test(NAME dmw.name COMMAND dmw_name_test)
    set_tests_properties(dmw.name PROPERTIES LABELS "unit")
    if(DMW_ENABLE_DDS_INTEGRATION_TESTS)
        set_tests_properties(
            dmw.message_type
            PROPERTIES
                ENVIRONMENT "DMW_ENABLE_DDS_INTEGRATION=1"
                LABELS "unit;integration"
                TIMEOUT 30
        )

        find_package(rclcpp QUIET)
        find_package(std_msgs QUIET)
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
                    LABELS "integration;ros2"
                    TIMEOUT 30
            )
        else()
            message(STATUS "ROS 2 Fast DDS dependencies not found; skipping ROS 2 interop test")
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
