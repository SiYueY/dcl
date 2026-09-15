#!/usr/bin/env bash
#
# DCL Docker runtime identity entry point.
#
# Purpose:
#     Materialize the host UID/GID in the disposable container's account
#     database, then execute the requested command with that numeric identity.
#     This preserves host ownership for bind-mounted build artifacts while
#     ensuring standard name lookups such as whoami and groups succeed.
#
# Environment:
#     DCL_HOST_UID   Required non-negative numeric host user ID.
#     DCL_HOST_GID   Required non-negative numeric host primary group ID.
#
# Exit codes:
#     1   Invalid runtime configuration or account-creation failure.
#     Other statuses are propagated from the requested command.

set -Eeuo pipefail

readonly DEFAULT_ACCOUNT_NAME="dcl"

# Purpose:
#     Print a runtime error with a consistent prefix.
# Arguments:
#     $@ - Error text.
# Returns:
#     Does not return.
# Exit codes:
#     1.
# Side effects:
#     Writes an error message to stderr.
die_runtime() {
    printf '[DCL Docker] Error: %s\n' "$*" >&2
    exit 1
}

# Purpose:
#     Read and validate a non-negative decimal ID from the environment.
# Arguments:
#     $1 - Environment variable name.
# Returns:
#     Writes the validated ID to stdout.
# Exit codes:
#     1 when the variable is missing or is not a decimal integer.
# Side effects:
#     None.
read_id() {
    local variable_name
    local value

    variable_name="$1"
    value="${!variable_name:-}"

    if [[ ! "${value}" =~ ^[0-9]+$ ]]; then
        die_runtime "${variable_name} must be a non-negative decimal integer."
    fi

    printf '%s\n' "${value}"
}

# Purpose:
#     Add a group for the host GID when the image has no such group.
# Arguments:
#     $1 - Validated host group ID.
# Returns:
#     0 when the group already exists or was created.
# Exit codes:
#     Propagates groupadd failures.
# Side effects:
#     May append one group record to the container-local account database.
ensure_group() {
    local host_gid
    local group_name

    host_gid="$1"

    if getent group "${host_gid}" >/dev/null; then
        return 0
    fi

    group_name="${DEFAULT_ACCOUNT_NAME}"
    if getent group "${group_name}" >/dev/null; then
        group_name="${DEFAULT_ACCOUNT_NAME}-${host_gid}"
    fi

    groupadd --gid "${host_gid}" "${group_name}"
}

# Purpose:
#     Add a user for the host UID when the image has no such user.
# Arguments:
#     $1 - Validated host user ID.
#     $2 - Validated host primary group ID.
# Returns:
#     0 when the user already exists or was created.
# Exit codes:
#     Propagates useradd failures.
# Side effects:
#     May append one user record to the container-local account database.
ensure_user() {
    local host_uid
    local host_gid
    local user_name

    host_uid="$1"
    host_gid="$2"

    if getent passwd "${host_uid}" >/dev/null; then
        return 0
    fi

    user_name="${DEFAULT_ACCOUNT_NAME}"
    if getent passwd "${user_name}" >/dev/null; then
        user_name="${DEFAULT_ACCOUNT_NAME}-${host_uid}"
    fi

    useradd \
        --uid "${host_uid}" \
        --gid "${host_gid}" \
        --no-create-home \
        --shell /bin/bash \
        "${user_name}"
}

# Purpose:
#     Materialize the supplied identity and replace this root process with the
#     base ROS entry point, which initializes the ROS environment before it
#     runs the requested command as the host UID/GID.
# Arguments:
#     $@ - Command and arguments to execute.
# Returns:
#     Does not return after a successful exec.
# Exit codes:
#     1 when invoked without a command or without root privileges; otherwise
#     propagates account setup or requested-command failures.
# Side effects:
#     May create container-local passwd/group records, initializes the ROS
#     environment through the base image entry point, then drops privileges.
main() {
    local host_uid
    local host_gid

    if [[ $# -eq 0 ]]; then
        die_runtime "No command was supplied to the container entry point."
    fi

    if [[ "$(id -u)" != "0" ]]; then
        die_runtime "The container entry point must start as root."
    fi

    host_uid="$(read_id DCL_HOST_UID)"
    host_gid="$(read_id DCL_HOST_GID)"

    ensure_group "${host_gid}"
    ensure_user "${host_uid}" "${host_gid}"

    # Preserve the base osrf/ros image contract: it sources the selected ROS
    # setup script before exec'ing setpriv. setpriv then drops privileges for
    # the requested command without retaining root's supplementary groups.
    exec /ros_entrypoint.sh \
        setpriv \
        --reuid "${host_uid}" \
        --regid "${host_gid}" \
        --clear-groups \
        "$@"
}

main "$@"
