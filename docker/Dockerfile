# syntax=docker/dockerfile:1
#
# DCL ROS 2 development and validation image.
#
# Purpose:
#     Provide a reproducible full ROS 2 development environment for DCL.
#     The selected ROS Desktop Full image supplies the standard ROS 2 desktop,
#     visualization, perception, simulation, and command-line environment.
#     This Dockerfile adds the build, debugging, and DCL-specific dependencies
#     required by the repository.
#
# Supported configurations:
#     ROS 2 Humble on Ubuntu 22.04 (Jammy)
#     ROS 2 Jazzy  on Ubuntu 24.04 (Noble)
#
# Build arguments:
#     ROS_BASE_IMAGE   ROS Desktop Full image. The launcher supplies this
#                      together with ROS_DISTRO. The default keeps direct
#                      Humble builds useful.
#     ROS_DISTRO       ROS 2 distribution used to select ROS package names.
#
# Runtime contract:
#     The repository is not copied into the image. docker.sh bind-mounts it at
#     /workspace and runs the container with the host user's UID/GID.
#     docker.sh supplies HOME, ROS_DOMAIN_ID, and host networking at runtime.
#     Optional GUI sessions forward the host X11 display. DDS integration tests
#     additionally force Fast DDS builtin transport to UDPv4.
#
# Installed dependencies:
#     ROS Desktop Full from the base image; C++ build/debug tooling; common ROS
#     workspace tooling; Fast DDS, the Fast DDS RMW implementation and ROSIDL
#     type support used by DCL interoperability tests; and Xvfb for headless GUI
#     smoke/integration tests.
#
# Notes:
#     Package versions intentionally follow the ROS repository configured by
#     the selected tagged base image. This keeps the environment aligned with
#     supported ROS patch releases instead of pinning package revisions that
#     may later disappear from the repository.

ARG ROS_BASE_IMAGE=osrf/ros:humble-desktop-full
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
        gdb \
        git \
        iproute2 \
        iputils-ping \
        ninja-build \
        procps \
        python3-colcon-common-extensions \
        python3-rosdep \
        python3-vcstool \
        xvfb \
        "ros-${ROS_DISTRO}-fastrtps" \
        "ros-${ROS_DISTRO}-rclcpp" \
        "ros-${ROS_DISTRO}-rmw-fastrtps-cpp" \
        "ros-${ROS_DISTRO}-rosidl-typesupport-fastrtps-cpp" \
        "ros-${ROS_DISTRO}-std-msgs" \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace

CMD ["bash"]
