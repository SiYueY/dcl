# syntax=docker/dockerfile:1
#
# DCL ROS 2 development image.
#
# Purpose:
#     Provide a small, reproducible build environment for DCL/DMW development
#     and ROS 2 Fast DDS interoperability testing.
#
# Supported configurations:
#     ROS 2 Humble on Ubuntu 22.04 (Jammy)
#     ROS 2 Jazzy  on Ubuntu 24.04 (Noble)
#
# Build arguments:
#     ROS_BASE_IMAGE   ROS base image. The launcher supplies this together with
#                      ROS_DISTRO. The default keeps direct Humble builds useful.
#     ROS_DISTRO       ROS 2 distribution used to select ROS package names.
#
# Runtime contract:
#     The repository is not copied into the image. docker.sh bind-mounts it at
#     /workspace and runs the container with the host user's UID/GID.
#     docker.sh also supplies HOME, ROS_DOMAIN_ID, host networking, and
#     FASTDDS_BUILTIN_TRANSPORTS=UDPv4 at runtime.
#
# Installed dependencies:
#     C++ build toolchain, CMake, Ninja, Fast DDS, rclcpp, std_msgs, the Fast DDS
#     RMW implementation, and Fast DDS ROSIDL type support.
#
# Notes:
#     Package versions intentionally follow the ROS repository configured by
#     the selected tagged base image. This keeps the environment aligned with
#     supported ROS patch releases instead of pinning package revisions that
#     may later disappear from the repository.

ARG ROS_BASE_IMAGE=ros:humble-ros-base-jammy
FROM ${ROS_BASE_IMAGE}

ARG ROS_DISTRO=humble

# DCL supports Fast DDS only. Make the ROS side use the matching RMW by default
# so interoperability tests do not depend on the base image's RMW selection.
ENV RMW_IMPLEMENTATION=rmw_fastrtps_cpp

# Version pinning is intentionally omitted here; see the file-level note above.
# hadolint ignore=DL3008
RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install --yes --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        "ros-${ROS_DISTRO}-fastrtps" \
        "ros-${ROS_DISTRO}-rclcpp" \
        "ros-${ROS_DISTRO}-rmw-fastrtps-cpp" \
        "ros-${ROS_DISTRO}-rosidl-typesupport-fastrtps-cpp" \
        "ros-${ROS_DISTRO}-std-msgs" \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace

CMD ["bash"]
