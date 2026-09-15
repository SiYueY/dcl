#!/usr/bin/env bash
#
# DCL ROS 2 Humble Docker entry point.
#
# Purpose:
#     Select the ROS 2 Humble Desktop Full environment and delegate Docker
#     operations to docker.sh.
#
# Usage:
#     humble.sh [options] [command]
#     humble.sh --help
#
# Parameters:
#     Optional command and options forwarded unchanged to docker.sh.
#
# Dependencies:
#     bash and an executable docker.sh in the same directory.
#
# Exit codes:
#     0       Success.
#     1       Wrapper runtime or environment failure.
#     2       Invalid usage reported by docker.sh.
#     130     Interrupted by SIGINT.
#     143     Terminated by SIGTERM.
#     Other   Status propagated by docker.sh.

set -Eeuo pipefail

readonly ROS_DISTRO="humble"
readonly ROS_BASE_IMAGE="osrf/ros:humble-desktop-full"

# Purpose:
#     Resolve the directory containing this entry script independently of the
#     caller's current working directory.
# Arguments:
#     None.
# Returns:
#     0 on success; non-zero if the directory cannot be entered.
# Exit codes:
#     None directly.
# Side effects:
#     Writes the absolute script directory to stdout; does not change the
#     caller's current directory.
resolve_script_dir() {
    local source_path
    local source_dir

    source_path="${BASH_SOURCE[0]}"

    if [[ "${source_path}" == */* ]]; then
        source_dir="${source_path%/*}"
    else
        source_dir="."
    fi

    (
        cd -- "${source_dir}" || exit 1
        pwd -P
    )
}

# Purpose:
#     Validate the shared launcher and replace this process with docker.sh
#     configured for ROS 2 Humble.
# Arguments:
#     $@ - Optional public command/options or help option.
# Returns:
#     Does not return after a successful exec.
# Exit codes:
#     1 if docker.sh is missing or not executable; otherwise propagated by
#     docker.sh.
# Side effects:
#     Replaces the wrapper process with docker.sh.
main() {
    local script_dir
    local docker_script

    if ! script_dir="$(resolve_script_dir)"; then
        printf '[DCL Docker] Error: Unable to resolve the Humble script directory.\n' >&2
        exit 1
    fi

    docker_script="${script_dir}/docker.sh"

    if [[ ! -f "${docker_script}" ]]; then
        printf '[DCL Docker] Error: Shared Docker script not found: %s\n' \
            "${docker_script}" >&2
        exit 1
    fi

    if [[ ! -x "${docker_script}" ]]; then
        printf '[DCL Docker] Error: Shared Docker script is not executable: %s\n' \
            "${docker_script}" >&2
        exit 1
    fi

    exec "${docker_script}" \
        --ros-distro "${ROS_DISTRO}" \
        --base-image "${ROS_BASE_IMAGE}" \
        "$@"
}

main "$@"
