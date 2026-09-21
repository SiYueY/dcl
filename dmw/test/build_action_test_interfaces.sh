#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Generate the workspace-local Fibonacci action interface package used by the
# DMW Action interoperability test when the ROS 2 installation under test does
# not ship example_interfaces (for example ROS 2 Rolling / Jazzy-era stacks).
#
# Usage:
#   dmw/test/build_action_test_interfaces.sh <ros_distro_prefix> <build_dir> <install_dir>
#
# Example (Rolling):
#   dmw/test/build_action_test_interfaces.sh /opt/ros/rolling \
#       /tmp/dmw_action_ifaces/build /tmp/dmw_action_ifaces/install
#
# Afterwards configure DMW with the generated prefix on the search path:
#   AMENT_PREFIX_PATH=<install_dir>:$ROS_PREFIX \
#   CMAKE_PREFIX_PATH=<install_dir>:$ROS_PREFIX \
#     cmake -S dmw -B <build> -DDMW_ENABLE_DDS_INTEGRATION_TESTS=ON

set -euo pipefail

if [[ $# -ne 3 ]]; then
    echo "usage: $0 <ros_distro_prefix> <build_dir> <install_dir>" >&2
    exit 2
fi

ros_prefix="$1"
build_dir="$2"
install_dir="$3"
source_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/ros2_action_test_interfaces"

# rosidl generation is driven by Python; make sure both the distribution's
# generators and the system empy module are importable.
python_site_packages="${ros_prefix}/lib/python3.*/site-packages"
export AMENT_PREFIX_PATH="${install_dir}:${ros_prefix}${AMENT_PREFIX_PATH:+:${AMENT_PREFIX_PATH}}"
export CMAKE_PREFIX_PATH="${install_dir}:${ros_prefix}${CMAKE_PREFIX_PATH:+:${CMAKE_PREFIX_PATH}}"
export LD_LIBRARY_PATH="${install_dir}/lib:${ros_prefix}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export PYTHONPATH
PYTHONPATH="$(ls -d ${python_site_packages} 2>/dev/null | tr '\n' ':')/usr/lib/python3/dist-packages${PYTHONPATH:+:${PYTHONPATH}}"

cmake -S "${source_dir}" -B "${build_dir}" \
    -DCMAKE_INSTALL_PREFIX="${install_dir}" \
    -DPython3_EXECUTABLE=/usr/bin/python3
cmake --build "${build_dir}" -j"$(nproc)" --target install

echo "dmw_action_test_interfaces installed into ${install_dir}"
