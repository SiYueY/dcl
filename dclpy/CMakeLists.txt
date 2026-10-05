cmake_minimum_required(VERSION 3.20)
project(dclpy VERSION 0.1.0 LANGUAGES CXX)

find_package(Python 3.10 COMPONENTS Interpreter Development.Module REQUIRED)
if(Python_VERSION VERSION_GREATER_EQUAL 3.14)
    message(FATAL_ERROR "DCLPY V1 supports CPython 3.10 through 3.13")
endif()
find_package(pybind11 3.0 CONFIG REQUIRED)
find_package(dmw 0.1 CONFIG REQUIRED)
find_package(fastrtps CONFIG REQUIRED)
find_package(fastcdr CONFIG REQUIRED)
set(DCLPY_ROS_PROFILE "humble" CACHE STRING "Frozen ROS interface compatibility profile")
option(DCLPY_ENABLE_DDS_TESTS "Run DCLPY native tests requiring local DDS networking" OFF)
option(DCLPY_BUILD_TESTS "Build DCLPY native contract tests" OFF)
option(DCLPY_BUILD_MFR3DUO_INTERFACES
       "Generate and install mfr3duo_msgs_dclpy and its ROSIDL provider dependencies" OFF)
set(DCLPY_INTERFACE_GENERATOR_PYTHON "/usr/bin/python3.10" CACHE FILEPATH
    "Python interpreter with rosidl_parser for interface provider generation")
set(DCLPY_MFR3DUO_INTERFACE_PREFIX "" CACHE PATH
    "Install prefix containing share/mfr3duo_msgs; required for DCLPY_BUILD_MFR3DUO_INTERFACES")
set(DCLPY_ROS_INTERFACE_PREFIX "/opt/ros/humble" CACHE PATH
    "ROS 2 Humble prefix containing standard ROSIDL packages for provider generation")

pybind11_add_module(_dclpy NO_EXTRAS src/_dclpy/module.cpp src/_dclpy/error.cpp src/_dclpy/parameter.cpp src/_dclpy/type_support.cpp
    src/_dclpy/context.cpp src/_dclpy/qos.cpp src/_dclpy/entity.cpp src/_dclpy/build_info.cpp
    src/_dclpy/publisher.cpp src/_dclpy/subscription.cpp src/_dclpy/wait_set.cpp src/_dclpy/wait_set_bind.cpp src/_dclpy/native_io.cpp src/_dclpy/native_io_bind.cpp src/_dclpy/service_bind.cpp src/_dclpy/timer.cpp src/_dclpy/graph.cpp src/_dclpy/action.cpp)
target_link_libraries(_dclpy PRIVATE dmw::dmw dmw::fastdds_binding)
target_include_directories(_dclpy PRIVATE include src/_dclpy)
target_compile_features(_dclpy PRIVATE cxx_std_17)
target_compile_options(_dclpy PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_compile_definitions(_dclpy PRIVATE
    DCLPY_VERSION="${PROJECT_VERSION}"
    DCLPY_ROS_PROFILE="${DCLPY_ROS_PROFILE}"
    DCLPY_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
    DCLPY_FASTDDS_VERSION="${fastrtps_VERSION}"
    DCLPY_FASTCDR_VERSION="${fastcdr_VERSION}")
set_target_properties(_dclpy PROPERTIES
    INSTALL_RPATH "$<TARGET_FILE_DIR:dmw::dmw>"
    INSTALL_RPATH_USE_LINK_PATH TRUE)
install(TARGETS _dclpy LIBRARY DESTINATION dclpy)
install(DIRECTORY include/ DESTINATION include)

if(DCLPY_BUILD_MFR3DUO_INTERFACES)
    if(NOT EXISTS "${DCLPY_MFR3DUO_INTERFACE_PREFIX}/share/mfr3duo_msgs")
        message(FATAL_ERROR
            "DCLPY_MFR3DUO_INTERFACE_PREFIX must contain share/mfr3duo_msgs when "
            "DCLPY_BUILD_MFR3DUO_INTERFACES is enabled")
    endif()
    if(NOT EXISTS "${DCLPY_INTERFACE_GENERATOR_PYTHON}")
        message(FATAL_ERROR "DCLPY_INTERFACE_GENERATOR_PYTHON does not exist")
    endif()
    if(NOT EXISTS "${DCLPY_ROS_INTERFACE_PREFIX}/share/nav_msgs")
        message(FATAL_ERROR
            "DCLPY_ROS_INTERFACE_PREFIX must contain the Humble nav_msgs ROSIDL package")
    endif()

    file(GLOB_RECURSE DCLPY_MFR3DUO_INTERFACE_INPUTS CONFIGURE_DEPENDS
         "${DCLPY_MFR3DUO_INTERFACE_PREFIX}/share/mfr3duo_msgs/*/*.idl"
         "${DCLPY_ROS_INTERFACE_PREFIX}/share/action_msgs/*/*.idl"
         "${DCLPY_ROS_INTERFACE_PREFIX}/share/nav_msgs/*/*.idl"
         "${DCLPY_ROS_INTERFACE_PREFIX}/share/geometry_msgs/*/*.idl"
         "${DCLPY_ROS_INTERFACE_PREFIX}/share/std_msgs/*/*.idl"
         "${DCLPY_ROS_INTERFACE_PREFIX}/share/builtin_interfaces/*/*.idl"
         "${DCLPY_ROS_INTERFACE_PREFIX}/share/unique_identifier_msgs/*/*.idl")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${CMAKE_CURRENT_SOURCE_DIR}/tools/interface_generator/generate.py"
                 ${DCLPY_MFR3DUO_INTERFACE_INPUTS})

    # Providers are generated into the build tree from installed ROSIDL, then
    # added as normal CMake targets so `cmake --install` delivers importable
    # mfr3duo_msgs_dclpy/action.{Move,Grasp} modules with their dependencies.
    set(DCLPY_GENERATED_INTERFACES_DIR "${CMAKE_CURRENT_BINARY_DIR}/interfaces")
    execute_process(
        COMMAND "${DCLPY_INTERFACE_GENERATOR_PYTHON}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/interface_generator/generate.py"
                --package mfr3duo_msgs
                --package nav_msgs
                --prefix "${DCLPY_MFR3DUO_INTERFACE_PREFIX}"
                --prefix "${DCLPY_ROS_INTERFACE_PREFIX}"
                --output "${DCLPY_GENERATED_INTERFACES_DIR}"
        RESULT_VARIABLE DCLPY_INTERFACE_GENERATION_RESULT
        OUTPUT_VARIABLE DCLPY_INTERFACE_GENERATION_OUTPUT
        ERROR_VARIABLE DCLPY_INTERFACE_GENERATION_ERROR)
    if(NOT DCLPY_INTERFACE_GENERATION_RESULT EQUAL 0)
        message(FATAL_ERROR "DCLPY interface generation failed:\n${DCLPY_INTERFACE_GENERATION_OUTPUT}${DCLPY_INTERFACE_GENERATION_ERROR}")
    endif()
    add_subdirectory("${DCLPY_GENERATED_INTERFACES_DIR}"
                     "${CMAKE_CURRENT_BINARY_DIR}/generated_interfaces")
endif()

if(DCLPY_BUILD_TESTS)
    enable_testing()
    find_package(Threads REQUIRED)
    add_executable(dclpy_entity_test tests/unit/entity_test.cpp)
    target_include_directories(dclpy_entity_test PRIVATE src/_dclpy)
    target_link_libraries(dclpy_entity_test PRIVATE dmw::dmw Threads::Threads)
    target_compile_features(dclpy_entity_test PRIVATE cxx_std_17)
    target_compile_options(dclpy_entity_test PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME dclpy.entity COMMAND dclpy_entity_test)
    set_tests_properties(dclpy.entity PROPERTIES TIMEOUT 20)
    if(DCLPY_ENABLE_DDS_TESTS)
        add_executable(dclpy_native_io_test tests/unit/native_io_test.cpp
            src/_dclpy/native_io.cpp src/_dclpy/wait_set.cpp)
        target_include_directories(dclpy_native_io_test PRIVATE src/_dclpy include ${Python_INCLUDE_DIRS})
        target_link_libraries(dclpy_native_io_test PRIVATE dmw::dmw Threads::Threads)
        set_target_properties(dclpy_native_io_test PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN TRUE)
        target_compile_features(dclpy_native_io_test PRIVATE cxx_std_17)
        target_compile_options(dclpy_native_io_test PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
        add_test(NAME dclpy.native_io COMMAND dclpy_native_io_test)
        set_tests_properties(dclpy.native_io PROPERTIES TIMEOUT 20)
    endif()
endif()
