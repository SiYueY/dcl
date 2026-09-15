#!/usr/bin/env bash
#
# DCL Humble/Jazzy Fast DDS wire interoperability test.
#
# Purpose:
#     Build the DMW integration peers for both supported ROS 2 distributions
#     and verify bidirectional Topic and Service communication between their
#     respective Fast DDS versions over UDPv4.
#
# Usage:
#     cross-integration-test.sh
#
# Environment:
#     ROS_DOMAIN_ID   Optional DDS domain ID. Defaults to 23.
#
# Exit codes:
#     0       Every build and cross-version probe passed.
#     1       A required command, image, build, or probe failed.
#     2       Invalid configuration.
#     130     Interrupted by SIGINT.
#     143     Terminated by SIGTERM.

set -Eeuo pipefail

readonly DEFAULT_ROS_DOMAIN_ID="23"
readonly FASTDDS_TRANSPORTS="UDPv4"
readonly CONTAINER_WORKSPACE="/workspace/dcl"
readonly CONTAINER_HOME="/tmp/dcl-home"

SCRIPT_DIR=""
REPO_ROOT=""
ROS_DOMAIN_ID_VALUE=""
TEMP_ROOT=""
PEER_PID=""

log_info() {
    printf '[DCL Docker] %s\n' "$*"
}

die() {
    printf '[DCL Docker] Error: %s\n' "$*" >&2
    exit "$2"
}

cleanup() {
    local status="$1"
    trap - EXIT
    if [[ -n "${TEMP_ROOT}" && -d "${TEMP_ROOT}" ]]; then
        rm -rf -- "${TEMP_ROOT}" || \
            printf '[DCL Docker] Warning: Failed to remove temporary directory: %s\n' "${TEMP_ROOT}" >&2
    fi
    exit "${status}"
}

resolve_script_dir() {
    local source_path="${BASH_SOURCE[0]}"
    local source_dir
    if [[ "${source_path}" == */* ]]; then source_dir="${source_path%/*}"; else source_dir="."; fi
    (cd -- "${source_dir}" && pwd -P)
}

validate_domain_id() {
    local candidate="$1"
    while [[ "${candidate}" == 0* && "${#candidate}" -gt 1 ]]; do candidate="${candidate#0}"; done
    if [[ ! "${candidate}" =~ ^[0-9]+$ || "${#candidate}" -gt 3 ]] || ((candidate > 232)); then
        die "ROS_DOMAIN_ID must be an integer between 0 and 232." 2
    fi
    ROS_DOMAIN_ID_VALUE="${candidate}"
}

prepare_distribution() {
    local distro="$1"
    "${SCRIPT_DIR}/${distro}.sh" integration-test
}

run_peer() {
    local distro="$1"
    local role="$2"
    local payload="$3"
    local log_file="$4"
    local home_dir="${TEMP_ROOT}/${distro}-${role}"
    local peer="${CONTAINER_WORKSPACE}/build/docker/${distro}/dmw-integration/dmw_interop_peer"

    mkdir -- "${home_dir}"
    docker run --rm --init --network host \
        --env "DCL_HOST_UID=$(id -u)" \
        --env "DCL_HOST_GID=$(id -g)" \
        --env "HOME=${CONTAINER_HOME}" \
        --env "ROS_DOMAIN_ID=${ROS_DOMAIN_ID_VALUE}" \
        --env "FASTDDS_BUILTIN_TRANSPORTS=${FASTDDS_TRANSPORTS}" \
        --volume "${REPO_ROOT}:${CONTAINER_WORKSPACE}" \
        --volume "${home_dir}:${CONTAINER_HOME}" \
        --workdir "${CONTAINER_WORKSPACE}" \
        "dcl:${distro}" "${peer}" "${role}" "${payload}" >"${log_file}" 2>&1 &
    PEER_PID="$!"
}

run_case() {
    local label="$1"
    local receiver_distro="$2"
    local receiver_role="$3"
    local sender_distro="$4"
    local sender_role="$5"
    local payload="${label}-${ROS_DOMAIN_ID_VALUE}"
    local receiver_log="${TEMP_ROOT}/${label}-receiver.log"
    local sender_log="${TEMP_ROOT}/${label}-sender.log"
    local receiver_pid
    local sender_pid

    log_info "Cross-version ${label}: ${sender_distro}/${sender_role} -> ${receiver_distro}/${receiver_role}."
    run_peer "${receiver_distro}" "${receiver_role}" "${payload}" "${receiver_log}"
    receiver_pid="${PEER_PID}"
    # Start the endpoint that must receive/discover first. The peer itself
    # still verifies actual Fast DDS matching; this only fixes process order.
    sleep 0.2
    run_peer "${sender_distro}" "${sender_role}" "${payload}" "${sender_log}"
    sender_pid="${PEER_PID}"

    local failed="false"
    if ! wait "${receiver_pid}"; then failed="true"; fi
    if ! wait "${sender_pid}"; then failed="true"; fi
    if [[ "${failed}" == "true" ]]; then
        printf '[DCL Docker] %s receiver log:\n' "${label}" >&2
        sed -n '1,160p' "${receiver_log}" >&2 || true
        printf '[DCL Docker] %s sender log:\n' "${label}" >&2
        sed -n '1,160p' "${sender_log}" >&2 || true
        die "Cross-version ${label} probe failed." 1
    fi
}

main() {
    if [[ $# -ne 0 ]]; then die "This command does not accept arguments." 2; fi
    SCRIPT_DIR="$(resolve_script_dir)" || die "Unable to resolve script directory." 1
    REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd -P)" || die "Unable to resolve repository root." 1
    command -v docker >/dev/null 2>&1 || die "Required command 'docker' was not found in PATH." 1
    command -v id >/dev/null 2>&1 || die "Required command 'id' was not found in PATH." 1
    command -v mktemp >/dev/null 2>&1 || die "Required command 'mktemp' was not found in PATH." 1
    validate_domain_id "${ROS_DOMAIN_ID:-${DEFAULT_ROS_DOMAIN_ID}}"
    TEMP_ROOT="$(mktemp -d -t 'dcl-cross-integration.XXXXXXXX')" || die "Unable to create temporary directory." 1
    trap 'cleanup "$?"' EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM

    prepare_distribution humble
    prepare_distribution jazzy
    run_case topic_humble_to_jazzy jazzy sub humble pub
    run_case topic_jazzy_to_humble humble sub jazzy pub
    run_case service_humble_to_jazzy jazzy server humble client
    run_case service_jazzy_to_humble humble server jazzy client
    log_info "Humble/Jazzy Topic and Service wire interoperability passed."
}

main "$@"
