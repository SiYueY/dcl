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
