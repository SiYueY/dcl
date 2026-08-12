# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.16)

project(dmw_package_consumer LANGUAGES CXX)

find_package(dmw CONFIG REQUIRED)

add_executable(dmw_package_consumer main.cpp)
target_link_libraries(dmw_package_consumer PRIVATE dmw::dmw)

add_executable(dmw_binding_package_consumer binding_main.cpp)
target_link_libraries(dmw_binding_package_consumer PRIVATE dmw::fastdds_binding)
