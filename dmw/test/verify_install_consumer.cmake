# SPDX-License-Identifier: Apache-2.0

set(dmw_install_prefix "${DMW_BUILD_DIR}/install")

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${DMW_BUILD_DIR}" --prefix "${dmw_install_prefix}"
    RESULT_VARIABLE dmw_install_result
)
if(NOT dmw_install_result EQUAL 0)
    message(FATAL_ERROR "Failed to install DMW into the CTest staging prefix")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -S "${DMW_CONSUMER_SOURCE_DIR}"
        -B "${DMW_CONSUMER_BINARY_DIR}"
        "-Ddmw_DIR=${dmw_install_prefix}/lib/cmake/dmw"
        "-Dfastrtps_DIR=${FASTDDS_DIR}"
    RESULT_VARIABLE dmw_consumer_configure_result
)
if(NOT dmw_consumer_configure_result EQUAL 0)
    message(FATAL_ERROR "Failed to configure the installed DMW package consumer")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${DMW_CONSUMER_BINARY_DIR}"
    RESULT_VARIABLE dmw_consumer_build_result
)
if(NOT dmw_consumer_build_result EQUAL 0)
    message(FATAL_ERROR "Failed to build the installed DMW package consumer")
endif()

# Building is not enough: the installed package must actually run for a
# runtime-only consumer and for a consumer that uses the Fast DDS binding.
execute_process(
    COMMAND "${DMW_CONSUMER_BINARY_DIR}/dmw_package_consumer"
    RESULT_VARIABLE dmw_runtime_consumer_result
)
if(NOT dmw_runtime_consumer_result EQUAL 0)
    message(FATAL_ERROR "Installed DMW runtime-only consumer failed to run")
endif()

execute_process(
    COMMAND "${DMW_CONSUMER_BINARY_DIR}/dmw_binding_package_consumer"
    RESULT_VARIABLE dmw_binding_consumer_result
)
if(NOT dmw_binding_consumer_result EQUAL 0)
    message(FATAL_ERROR "Installed DMW binding consumer failed to run")
endif()
