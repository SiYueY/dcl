#!/usr/bin/env bash
#
# DCL shared Docker launcher for ROS 2 development and compatibility testing.
#
# Purpose:
#     Build and run the DCL Docker environment selected by an entry script such
#     as humble.sh or jazzy.sh. The container uses host networking so ROS 2
#     processes can communicate naturally with the host and peer containers.
#     Optional GUI mode forwards the host X11 display for tools such as RViz.
#     DDS integration tests additionally force Fast DDS builtin transport to
#     UDPv4 for deterministic wire-level interoperability testing.
#
# Usage:
#     docker.sh --ros-distro <distro> --base-image <image> [options] [command]
#
# Commands:
#     shell               Start an interactive shell. This is the default.
#     build               Configure and build DMW.
#     test                Configure, build, and run DMW tests.
#     integration-test    Configure, build, and run DDS/ROS 2 integration tests.
#     benchmark           Configure, build, and run the DMW foundation benchmark.
#     rebuild             Rebuild the Docker image.
#
# Parameters:
#     --ros-distro <distro>   ROS 2 distribution name, e.g. humble or jazzy.
#     --base-image <image>    Docker base image for the selected distribution.
#     --gui                   Enable X11 forwarding for GUI applications.
#
# Environment:
#     ROS_DOMAIN_ID           Optional ROS 2 domain ID override. Defaults to 23.
#     DISPLAY                 Required when --gui is enabled.
#
# Dependencies:
#     bash, docker, id, mktemp, rm
#
# Exit codes:
#     0       Success.
#     1       Script-detected runtime or environment failure.
#     2       Invalid command-line usage or configuration.
#     130     Interrupted by SIGINT.
#     143     Terminated by SIGTERM.
#     Other   Status propagated from Docker, CMake, or CTest.

set -Eeuo pipefail

readonly DEFAULT_COMMAND="shell"
readonly DEFAULT_ROS_DOMAIN_ID="23"
readonly FASTDDS_TRANSPORTS="UDPv4"
readonly CONTAINER_WORKSPACE="/workspace/dcl"
readonly CONTAINER_HOME="/tmp/dcl-home"

SCRIPT_DIR=""
REPO_ROOT=""
DOCKERFILE=""
ROS_DISTRO=""
ROS_BASE_IMAGE=""
COMMAND=""
ROS_DOMAIN_ID_VALUE=""
GUI_ENABLED="false"
IMAGE_NAME=""
BUILD_DIR=""
INTEGRATION_BUILD_DIR=""
TEMP_HOME_DIR=""

# Purpose:
#     Print usage information. If the ROS distribution is already known, show
#     the public wrapper syntax rather than the internal docker.sh syntax.
# Arguments:
#     None.
# Returns:
#     0.
# Exit codes:
#     None.
# Side effects:
#     Writes help text to stdout.
print_usage() {
    if [[ -n "${ROS_DISTRO}" ]]; then
        printf '%s\n' \
            "Usage: ${ROS_DISTRO}.sh [options] [command]" \
            '' \
            'Commands:' \
            '    shell               Start an interactive shell. This is the default.' \
            '    build               Configure and build DMW.' \
            '    test                Configure, build, and run DMW tests.' \
            '    integration-test    Configure, build, and run DDS/ROS 2 integration tests.' \
            '    benchmark           Configure, build, and run the DMW foundation benchmark.' \
            '    rebuild             Rebuild the Docker image.' \
            '' \
            'Options:' \
            '    --gui        Enable X11 forwarding for GUI applications.' \
            '    -h, --help   Show this help message.' \
            '' \
            'Environment:' \
            '    ROS_DOMAIN_ID   ROS 2 domain ID. Defaults to 23.' \
            '    DISPLAY         Required when --gui is enabled.'
        return 0
    fi

    printf '%s\n' \
        'Usage: docker.sh --ros-distro <distro> --base-image <image> [options] [command]' \
        '' \
        'Commands:' \
        '    shell               Start an interactive shell. default.' \
        '    build               Configure and build DMW.' \
        '    test                Configure, build, and run DMW tests.' \
        '    integration-test    Configure, build, and run DDS/ROS 2 integration tests.' \
        '    benchmark           Configure, build, and run the DMW foundation benchmark.' \
        '    rebuild             Rebuild the Docker image.' \
        '' \
        'Options:' \
        '    --ros-distro <distro>   ROS 2 distribution name.' \
        '    --base-image <image>    Docker base image.' \
        '    --gui                   Enable X11 forwarding for GUI applications.' \
        '    -h, --help              Show this help message.' \
        '' \
        'Environment:' \
        '    ROS_DOMAIN_ID           ROS 2 domain ID. Defaults to 23.' \
        '    DISPLAY                 Required when --gui is enabled.'
}

# Purpose:
#     Print an informational message with a consistent prefix.
# Arguments:
#     $@ - Message text.
# Returns:
#     0.
# Exit codes:
#     None.
# Side effects:
#     Writes to stdout.
log_info() {
    printf '[DCL Docker] %s\n' "$*"
}

# Purpose:
#     Print an error message with a consistent prefix.
# Arguments:
#     $@ - Error message text.
# Returns:
#     0.
# Exit codes:
#     None.
# Side effects:
#     Writes to stderr.
log_error() {
    printf '[DCL Docker] Error: %s\n' "$*" >&2
}

# Purpose:
#     Abort after a runtime or environment failure detected by this script.
# Arguments:
#     $@ - Error message text.
# Returns:
#     Does not return.
# Exit codes:
#     1.
# Side effects:
#     Writes an error message to stderr.
die_runtime() {
    log_error "$@"
    exit 1
}

# Purpose:
#     Abort after invalid command-line usage or configuration.
# Arguments:
#     $@ - Error message text.
# Returns:
#     Does not return.
# Exit codes:
#     2.
# Side effects:
#     Writes the error and usage information to stderr.
die_usage() {
    log_error "$@"
    printf '\n' >&2
    print_usage >&2
    exit 2
}

# Purpose:
#     Resolve the directory containing this script without depending on the
#     caller's current working directory.
# Arguments:
#     None.
# Returns:
#     0 on success; non-zero if the directory cannot be entered.
# Exit codes:
#     None directly.
# Side effects:
#     Writes the absolute directory path to stdout. The caller's cwd is not
#     modified because directory changes occur in a subshell.
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
#     Initialize absolute paths derived from the location of docker.sh.
# Arguments:
#     None.
# Returns:
#     0.
# Exit codes:
#     1 if the script directory or repository root cannot be resolved.
# Side effects:
#     Assigns SCRIPT_DIR, REPO_ROOT, and DOCKERFILE and makes them readonly.
initialize_paths() {
    local resolved_script_dir
    local resolved_repo_root

    if ! resolved_script_dir="$(resolve_script_dir)"; then
        die_runtime "Unable to resolve the Docker script directory."
    fi

    if ! resolved_repo_root="$(cd -- "${resolved_script_dir}/.." && pwd -P)"; then
        die_runtime "Unable to resolve the DCL repository root."
    fi

    SCRIPT_DIR="${resolved_script_dir}"
    REPO_ROOT="${resolved_repo_root}"
    DOCKERFILE="${SCRIPT_DIR}/Dockerfile"

    readonly SCRIPT_DIR
    readonly REPO_ROOT
    readonly DOCKERFILE
}

# Purpose:
#     Verify that an executable required by this script is available in PATH.
# Arguments:
#     $1 - Executable name.
# Returns:
#     0 when the executable exists.
# Exit codes:
#     1 when the executable cannot be found.
# Side effects:
#     Writes an error message to stderr on failure.
require_command() {
    local command_name

    command_name="$1"

    if ! command -v "${command_name}" >/dev/null 2>&1; then
        die_runtime "Required command '${command_name}' was not found in PATH."
    fi
}

# Purpose:
#     Verify that a command-line option has a following value.
# Arguments:
#     $1 - Option name.
#     $2 - Candidate value.
# Returns:
#     0 when the value is present.
# Exit codes:
#     2 when the value is missing.
# Side effects:
#     Writes an error and usage information to stderr on failure.
require_option_value() {
    local option_name
    local option_value

    option_name="$1"
    option_value="$2"

    if [[ -z "${option_value}" || "${option_value}" == --* ]]; then
        die_usage "Missing value for '${option_name}'."
    fi
}

# Purpose:
#     Parse internal environment options and the requested public command.
# Arguments:
#     $@ - Command-line arguments.
# Returns:
#     0 when parsing succeeds.
# Exit codes:
#     0 for --help; 2 for invalid usage.
# Side effects:
#     Assigns ROS_DISTRO, ROS_BASE_IMAGE, GUI_ENABLED, and COMMAND.
parse_arguments() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --ros-distro)
                require_option_value "$1" "${2:-}"
                ROS_DISTRO="$2"
                shift 2
                ;;

            --base-image)
                require_option_value "$1" "${2:-}"
                ROS_BASE_IMAGE="$2"
                shift 2
                ;;

            --gui)
                GUI_ENABLED="true"
                shift
                ;;

            shell | build | test | integration-test | benchmark | rebuild)
                if [[ -n "${COMMAND}" ]]; then
                    die_usage "Multiple commands were specified: '${COMMAND}' and '$1'."
                fi

                COMMAND="$1"
                shift
                ;;

            -h | --help)
                print_usage
                exit 0
                ;;

            *)
                die_usage "Unknown argument: '$1'."
                ;;
        esac
    done

    if [[ -z "${ROS_DISTRO}" ]]; then
        die_usage "The '--ros-distro' option is required."
    fi

    if [[ -z "${ROS_BASE_IMAGE}" ]]; then
        die_usage "The '--base-image' option is required."
    fi

    if [[ -z "${COMMAND}" ]]; then
        COMMAND="${DEFAULT_COMMAND}"
    fi
}

# Purpose:
#     Normalize and validate ROS_DOMAIN_ID as a decimal integer.
# Arguments:
#     $1 - Candidate domain ID string.
# Returns:
#     0 when valid.
# Exit codes:
#     2 when the value is not an integer in the supported 0..232 range.
# Side effects:
#     Assigns ROS_DOMAIN_ID_VALUE.
validate_domain_id() {
    local candidate
    local normalized

    candidate="$1"

    if [[ ! "${candidate}" =~ ^[0-9]+$ ]]; then
        die_usage "ROS_DOMAIN_ID must be an integer between 0 and 232."
    fi

    normalized="${candidate}"

    # Bash interprets leading-zero numbers as octal in arithmetic contexts.
    # Remove redundant zeros first so a value such as "023" means decimal 23.
    while [[ "${normalized}" == 0* && "${#normalized}" -gt 1 ]]; do
        normalized="${normalized#0}"
    done

    # The supported value is at most three digits. Checking the length before
    # arithmetic also avoids evaluating an arbitrarily large integer string.
    if [[ "${#normalized}" -gt 3 ]]; then
        die_usage "ROS_DOMAIN_ID must be between 0 and 232."
    fi

    if ((normalized > 232)); then
        die_usage "ROS_DOMAIN_ID must be between 0 and 232."
    fi

    ROS_DOMAIN_ID_VALUE="${normalized}"
}

# Purpose:
#     Validate parsed configuration and derive immutable runtime values.
# Arguments:
#     None.
# Returns:
#     0 when configuration is valid.
# Exit codes:
#     2 for invalid ROS distribution or ROS_DOMAIN_ID.
# Side effects:
#     Assigns immutable domain, image, GUI, and build-directory values.
validate_configuration() {
    local requested_domain_id

    if [[ ! "${ROS_DISTRO}" =~ ^[a-z0-9][a-z0-9_-]*$ ]]; then
        die_usage "Invalid ROS 2 distribution name: '${ROS_DISTRO}'."
    fi

    requested_domain_id="${ROS_DOMAIN_ID:-${DEFAULT_ROS_DOMAIN_ID}}"
    validate_domain_id "${requested_domain_id}"

    IMAGE_NAME="dcl:${ROS_DISTRO}"
    BUILD_DIR="build/docker/${ROS_DISTRO}/dmw"
    INTEGRATION_BUILD_DIR="build/docker/${ROS_DISTRO}/dmw-integration"

    readonly ROS_DISTRO
    readonly ROS_BASE_IMAGE
    readonly COMMAND
    readonly ROS_DOMAIN_ID_VALUE
    readonly GUI_ENABLED
    readonly IMAGE_NAME
    readonly BUILD_DIR
    readonly INTEGRATION_BUILD_DIR
}

# Purpose:
#     Verify host dependencies, Docker daemon access, and repository layout.
# Arguments:
#     None.
# Returns:
#     0 when the environment is usable.
# Exit codes:
#     1 for a missing dependency, unavailable daemon, or invalid layout.
# Side effects:
#     Queries the local Docker daemon.
validate_environment() {
    require_command "docker"
    require_command "id"
    require_command "mktemp"
    require_command "rm"

    if ! docker info >/dev/null 2>&1; then
        die_runtime "Cannot connect to the Docker daemon. Check that Docker is running and that the current user can access it."
    fi

    if [[ ! -f "${DOCKERFILE}" ]]; then
        die_runtime "Dockerfile not found: ${DOCKERFILE}"
    fi

    if [[ ! -d "${REPO_ROOT}/dmw" ]]; then
        die_runtime "Expected DMW directory not found under repository root: ${REPO_ROOT}"
    fi
}

# Purpose:
#     Validate host-side requirements for an X11-enabled container.
# Arguments:
#     None.
# Returns:
#     0 when GUI mode is disabled or the X11 host environment is usable.
# Exit codes:
#     1 when GUI mode is enabled without DISPLAY or the X11 socket directory.
# Side effects:
#     None.
validate_gui_environment() {
    if [[ "${GUI_ENABLED}" != "true" ]]; then
        return 0
    fi

    if [[ -z "${DISPLAY:-}" ]]; then
        die_runtime "DISPLAY is not set; GUI mode requires an X11 display."
    fi

    if [[ ! -d /tmp/.X11-unix ]]; then
        die_runtime "X11 socket directory '/tmp/.X11-unix' does not exist."
    fi
}

# Purpose:
#     Create a private temporary HOME directory for the container user.
# Arguments:
#     None.
# Returns:
#     0 when the directory exists.
# Exit codes:
#     1 if mktemp cannot create the directory.
# Side effects:
#     Creates a host temporary directory and assigns TEMP_HOME_DIR.
create_temp_home() {
    local temp_dir

    if [[ -n "${TEMP_HOME_DIR}" ]]; then
        return 0
    fi

    if ! temp_dir="$(mktemp -d -t 'dcl-docker-home.XXXXXXXX')"; then
        die_runtime "Unable to create a temporary HOME directory for the container."
    fi

    TEMP_HOME_DIR="${temp_dir}"
}

# ShellCheck cannot infer calls made indirectly by trap and may report the
# handler as unreachable/unused. The function is intentionally trap-only.
# shellcheck disable=SC2317,SC2329
# Purpose:
#     Remove temporary resources while preserving the original exit status.
# Arguments:
#     $1 - Exit status captured by the EXIT trap before cleanup begins.
# Returns:
#     Does not return.
# Exit codes:
#     Re-exits with the original status supplied in $1.
# Side effects:
#     Removes TEMP_HOME_DIR. Cleanup failures are reported but do not replace
#     the original exit status.
cleanup() {
    local original_status

    original_status="$1"

    # Disable the EXIT trap before calling exit to prevent recursive cleanup.
    trap - EXIT

    if [[ -n "${TEMP_HOME_DIR}" && -d "${TEMP_HOME_DIR}" ]]; then
        if ! rm -rf -- "${TEMP_HOME_DIR}"; then
            printf '[DCL Docker] Warning: Failed to remove temporary directory: %s\n' \
                "${TEMP_HOME_DIR}" >&2
        fi
    fi

    exit "${original_status}"
}

# ShellCheck cannot infer calls made indirectly by trap and may report the
# handler as unreachable/unused. The function is intentionally trap-only.
# shellcheck disable=SC2317,SC2329
# Purpose:
#     Convert SIGINT into the conventional shell exit status.
# Arguments:
#     None.
# Returns:
#     Does not return.
# Exit codes:
#     130.
# Side effects:
#     Writes a diagnostic to stderr; the EXIT trap performs cleanup.
handle_sigint() {
    printf '\n[DCL Docker] Interrupted.\n' >&2
    exit 130
}

# ShellCheck cannot infer calls made indirectly by trap and may report the
# handler as unreachable/unused. The function is intentionally trap-only.
# shellcheck disable=SC2317,SC2329
# Purpose:
#     Convert SIGTERM into the conventional shell exit status.
# Arguments:
#     None.
# Returns:
#     Does not return.
# Exit codes:
#     143.
# Side effects:
#     Writes a diagnostic to stderr; the EXIT trap performs cleanup.
handle_sigterm() {
    printf '[DCL Docker] Terminated.\n' >&2
    exit 143
}

# Purpose:
#     Install cleanup and signal handlers before temporary resources are used.
# Arguments:
#     None.
# Returns:
#     0.
# Exit codes:
#     None directly.
# Side effects:
#     Installs EXIT, SIGINT, and SIGTERM traps for this shell.
install_traps() {
    # Capture $? in the trap command itself. Running another command first
    # would overwrite the status that cleanup is required to preserve.
    trap 'cleanup "$?"' EXIT
    trap handle_sigint INT
    trap handle_sigterm TERM
}

# Purpose:
#     Build the Docker image for the selected ROS 2 distribution.
# Arguments:
#     None.
# Returns:
#     0 when docker build succeeds.
# Exit codes:
#     Propagates docker build failures.
# Side effects:
#     Creates or updates IMAGE_NAME in the local Docker image store.
build_image() {
    log_info "Building Docker image '${IMAGE_NAME}'."
    log_info "Base image: ${ROS_BASE_IMAGE}"

    docker build \
        --file "${DOCKERFILE}" \
        --build-arg "ROS_DISTRO=${ROS_DISTRO}" \
        --build-arg "ROS_BASE_IMAGE=${ROS_BASE_IMAGE}" \
        --tag "${IMAGE_NAME}" \
        "${SCRIPT_DIR}"
}

# Purpose:
#     Ensure the selected Docker image exists before starting a container.
# Arguments:
#     None.
# Returns:
#     0 when the image already exists or is built successfully.
# Exit codes:
#     Propagates docker build failures.
# Side effects:
#     May build the Docker image when it is absent.
ensure_image() {
    if docker image inspect "${IMAGE_NAME}" >/dev/null 2>&1; then
        return 0
    fi

    log_info "Docker image '${IMAGE_NAME}' does not exist locally."
    build_image
}

# Purpose:
#     Run a command inside the standard DCL container environment.
# Arguments:
#     $@ - Command and arguments to execute in the container.
# Returns:
#     The status returned by docker run.
# Exit codes:
#     Propagates docker run or container-command failures.
# Side effects:
#     Creates TEMP_HOME_DIR when needed, starts a disposable container, mounts
#     the repository at /workspace/dcl, materializes the host UID/GID in the
#     container account database, uses host networking, and optionally forwards
#     the host X11 display when --gui is enabled.
run_container() {
    local host_uid
    local host_gid
    local -a docker_args

    validate_gui_environment
    create_temp_home

    host_uid="$(id -u)"
    host_gid="$(id -g)"

    docker_args=(
        run
        --rm
        --init
        --network "host"
        --env "DCL_HOST_UID=${host_uid}"
        --env "DCL_HOST_GID=${host_gid}"
        --env "HOME=${CONTAINER_HOME}"
        --env "ROS_DOMAIN_ID=${ROS_DOMAIN_ID_VALUE}"
        --volume "${REPO_ROOT}:${CONTAINER_WORKSPACE}"
        --volume "${TEMP_HOME_DIR}:${CONTAINER_HOME}"
        --workdir "${CONTAINER_WORKSPACE}"
    )

    if [[ "${GUI_ENABLED}" == "true" ]]; then
        docker_args+=(
            --env "DISPLAY=${DISPLAY}"
            --volume "/tmp/.X11-unix:/tmp/.X11-unix"
        )

        if [[ -d /dev/dri ]]; then
            docker_args+=(
                --device "/dev/dri:/dev/dri"
            )
        fi
    fi

    # Add terminal options only when matching host file descriptors are TTYs.
    # This keeps interactive shells pleasant without breaking non-TTY CI jobs.
    if [[ -t 0 ]]; then
        docker_args+=(--interactive)
    fi

    if [[ -t 1 ]]; then
        docker_args+=(--tty)
    fi

    docker "${docker_args[@]}" "${IMAGE_NAME}" "$@"
}

# Purpose:
#     Run a command inside the DCL integration-test environment.
# Arguments:
#     $@ - Command and arguments to execute in the container.
# Returns:
#     The status returned by run_container.
# Exit codes:
#     Propagates docker run or container-command failures.
# Side effects:
#     Uses the standard container environment and forces Fast DDS builtin
#     transports to UDPv4 for deterministic DDS interoperability testing.
run_integration_container() {
    run_container \
        env "FASTDDS_BUILTIN_TRANSPORTS=${FASTDDS_TRANSPORTS}" \
        "$@"
}

# Purpose:
#     Start the default interactive development shell.
# Arguments:
#     None.
# Returns:
#     The status returned by the shell/container.
# Exit codes:
#     Propagates docker run failures.
# Side effects:
#     Starts a disposable container attached to available terminal streams.
run_shell() {
    log_info "Starting ROS 2 ${ROS_DISTRO} development shell."
    log_info "ROS_DOMAIN_ID=${ROS_DOMAIN_ID_VALUE}; GUI=${GUI_ENABLED}."

    run_container bash
}

# Purpose:
#     Configure the standard DMW CMake build tree for the selected ROS 2
#     environment.
# Arguments:
#     None.
# Returns:
#     0 when CMake configuration succeeds.
# Exit codes:
#     Propagates Docker or CMake failures.
# Side effects:
#     Creates or updates BUILD_DIR in the host repository.
configure_dmw() {
    log_info "Configuring DMW for ROS 2 ${ROS_DISTRO}."
    log_info "Build directory: ${BUILD_DIR}"

    run_container \
        cmake \
        -S "dmw" \
        -B "${BUILD_DIR}" \
        -G "Ninja" \
        -DBUILD_TESTING=ON \
        -DDMW_ENABLE_DDS_INTEGRATION_TESTS=OFF
}

# Purpose:
#     Compile the standard DMW build tree.
# Arguments:
#     None.
# Returns:
#     0 when compilation succeeds.
# Exit codes:
#     Propagates Docker or CMake build failures.
# Side effects:
#     Writes build artifacts under BUILD_DIR in the host repository.
compile_dmw() {
    log_info "Compiling DMW for ROS 2 ${ROS_DISTRO}."

    run_container \
        cmake \
        --build "${BUILD_DIR}"
}

# Purpose:
#     Run the standard DMW CTest suite from the configured build tree.
# Arguments:
#     None.
# Returns:
#     0 when all tests pass.
# Exit codes:
#     Propagates Docker or CTest failures.
# Side effects:
#     Executes tests and may update test result files under BUILD_DIR.
run_dmw_tests() {
    log_info "Running DMW tests for ROS 2 ${ROS_DISTRO}."

    run_container \
        ctest \
        --test-dir "${BUILD_DIR}" \
        --output-on-failure
}

# Purpose:
#     Configure the DMW integration-test build tree with DDS transport tests
#     enabled.
# Arguments:
#     None.
# Returns:
#     0 when CMake configuration succeeds.
# Exit codes:
#     Propagates Docker or CMake failures.
# Side effects:
#     Creates or updates INTEGRATION_BUILD_DIR in the host repository.
configure_dmw_integration() {
    log_info "Configuring DMW integration tests for ROS 2 ${ROS_DISTRO}."
    log_info "Integration build directory: ${INTEGRATION_BUILD_DIR}"

    run_integration_container \
        cmake \
        -S "dmw" \
        -B "${INTEGRATION_BUILD_DIR}" \
        -G "Ninja" \
        -DBUILD_TESTING=ON \
        -DDMW_ENABLE_DDS_INTEGRATION_TESTS=ON
}

# Purpose:
#     Compile the DMW integration-test build tree.
# Arguments:
#     None.
# Returns:
#     0 when compilation succeeds.
# Exit codes:
#     Propagates Docker or CMake build failures.
# Side effects:
#     Writes build artifacts under INTEGRATION_BUILD_DIR in the host repository.
compile_dmw_integration() {
    log_info "Compiling DMW integration tests for ROS 2 ${ROS_DISTRO}."

    run_integration_container \
        cmake \
        --build "${INTEGRATION_BUILD_DIR}"
}

# Purpose:
#     Run DMW tests carrying the integration label.
# Arguments:
#     None.
# Returns:
#     0 when all integration-labelled tests pass.
# Exit codes:
#     Propagates Docker or CTest failures.
# Side effects:
#     Executes integration tests and may update test result files under
#     INTEGRATION_BUILD_DIR.
run_dmw_integration_tests() {
    log_info "Running DMW DDS/ROS 2 integration tests for ROS 2 ${ROS_DISTRO}."

    run_integration_container \
        ctest \
        --test-dir "${INTEGRATION_BUILD_DIR}" \
        --output-on-failure \
        --label-regex "integration"
}

# Purpose:
#     Perform a complete DMW configure-and-build operation.
# Arguments:
#     None.
# Returns:
#     0 when configuration and compilation succeed.
# Exit codes:
#     Propagates configuration or compilation failures.
# Side effects:
#     Creates or updates DMW build artifacts under BUILD_DIR.
build_dmw() {
    configure_dmw
    compile_dmw
}

# Purpose:
#     Perform a self-contained DMW configure, build, and test operation.
# Arguments:
#     None.
# Returns:
#     0 when configuration, compilation, and tests all succeed.
# Exit codes:
#     Propagates configuration, compilation, or test failures.
# Side effects:
#     Creates or updates build and test artifacts under BUILD_DIR.
test_dmw() {
    configure_dmw
    compile_dmw
    run_dmw_tests
}

# Purpose:
#     Perform a self-contained DMW DDS/ROS 2 integration-test operation.
# Arguments:
#     None.
# Returns:
#     0 when configuration, compilation, and integration tests all succeed.
# Exit codes:
#     Propagates configuration, compilation, or test failures.
# Side effects:
#     Creates or updates integration build/test artifacts under
#     INTEGRATION_BUILD_DIR.
integration_test_dmw() {
    configure_dmw_integration
    compile_dmw_integration
    run_dmw_integration_tests
}

# Purpose:
#     Build and run the non-asserting DMW foundation performance benchmark.
# Arguments:
#     None.
# Returns:
#     0 when configuration, compilation, and benchmark execution succeed.
# Exit codes:
#     Propagates Docker, configuration, compilation, or benchmark failures.
# Side effects:
#     Creates or updates integration build artifacts and writes benchmark
#     measurements to stdout.
benchmark_dmw() {
    configure_dmw_integration
    compile_dmw_integration
    log_info "Running DMW foundation benchmark for ROS 2 ${ROS_DISTRO}."
    run_integration_container "${INTEGRATION_BUILD_DIR}/dmw_benchmark"
}

# Purpose:
#     Dispatch the validated public command.
# Arguments:
#     None.
# Returns:
#     The status returned by the selected operation.
# Exit codes:
#     Propagates operation failures.
# Side effects:
#     May build an image, start a container, build DMW, or run tests.
execute_command() {
    case "${COMMAND}" in
        shell)
            ensure_image
            run_shell
            ;;

        build)
            ensure_image
            build_dmw
            ;;

        test)
            ensure_image
            test_dmw
            ;;

        integration-test)
            ensure_image
            integration_test_dmw
            ;;

        benchmark)
            ensure_image
            benchmark_dmw
            ;;

        rebuild)
            build_image
            ;;
    esac
}

# Purpose:
#     Program entry point.
# Arguments:
#     $@ - Command-line arguments supplied by the wrapper or direct caller.
# Returns:
#     0 on success.
# Exit codes:
#     Uses the exit-code contract documented in the file header.
# Side effects:
#     Initializes paths, installs traps, validates input/environment, and
#     executes the selected Docker operation.
main() {
    initialize_paths
    install_traps
    parse_arguments "$@"
    validate_configuration
    validate_environment
    execute_command
}

main "$@"
