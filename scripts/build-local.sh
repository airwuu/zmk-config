#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat >&2 <<'EOF'
Usage: scripts/build-local.sh [all|left|right|reset] [options]

Build ZMK firmware locally in a container, using build.yaml.

Options:
  --runner docker|podman       Container runner to use (default: docker)
  --image IMAGE                Build image (default: zmkfirmware/zmk-build-arm:stable)
  --workspace-dir DIR          Persistent west/build cache (default: .local/zmk-build)
  --artifacts-dir DIR          Output UF2/bin directory (default: artifacts)
  --skip-update                Skip west update; useful after a successful first build
  --clean                      Delete the persistent workspace before building
  -h, --help                   Show this help

Examples:
  scripts/build-local.sh all
  scripts/build-local.sh left --skip-update
  scripts/build-local.sh reset --clean
EOF
}

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
target="all"
runner="${CONTAINER_RUNNER:-docker}"
image="${ZMK_BUILD_IMAGE:-zmkfirmware/zmk-build-arm:stable}"
workspace_dir="${repo_root}/.local/zmk-build"
artifacts_dir="${repo_root}/artifacts"
skip_update=0
clean=0

if [[ "${1:-}" != "" && "${1:-}" != --* && "${1:-}" != "-h" ]]; then
    target="$1"
    shift
fi

while [[ $# -gt 0 ]]; do
    case "$1" in
        --runner)
            runner="${2:?--runner requires docker or podman}"
            shift 2
            ;;
        --image)
            image="${2:?--image requires an image name}"
            shift 2
            ;;
        --workspace-dir)
            workspace_dir="$(realpath -m "$2")"
            shift 2
            ;;
        --artifacts-dir)
            artifacts_dir="$(realpath -m "$2")"
            shift 2
            ;;
        --skip-update)
            skip_update=1
            shift
            ;;
        --clean)
            clean=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            printf 'Unknown option: %s\n\n' "$1" >&2
            usage
            exit 2
            ;;
    esac
done

case "$target" in
    all|left|right|reset|eyelash_corne_left|eyelash_corne_right|eyelash_corne_settings_reset)
        ;;
    *)
        printf 'Unknown target: %s\n\n' "$target" >&2
        usage
        exit 2
        ;;
esac

if ! command -v "$runner" >/dev/null 2>&1; then
    printf 'Container runner not found: %s\n' "$runner" >&2
    exit 1
fi

mkdir -p "$workspace_dir" "$artifacts_dir"

tty_args=(-i)
if [[ -t 0 && -t 1 ]]; then
    tty_args=(-it)
fi

printf 'Using %s image: %s\n' "$runner" "$image"
printf 'Workspace cache: %s\n' "$workspace_dir"
printf 'Artifacts: %s\n' "$artifacts_dir"

"$runner" run --rm "${tty_args[@]}" \
    -v "${repo_root}:/workspaces/zmk-config" \
    -v "${workspace_dir}:/workspaces/zmk-workspace" \
    -v "${artifacts_dir}:/workspaces/zmk-artifacts" \
    -w /workspaces/zmk-workspace \
    "$image" \
    bash -s -- "$target" "$skip_update" "$clean" "$(id -u)" "$(id -g)" <<'CONTAINER_SCRIPT'
set -euo pipefail

target="$1"
skip_update="$2"
clean="$3"
host_uid="$4"
host_gid="$5"
config_repo="/workspaces/zmk-config"
base_dir="/workspaces/zmk-workspace"
config_dir="${base_dir}/config"
artifacts_dir="/workspaces/zmk-artifacts"

restore_ownership() {
    chown -R "${host_uid}:${host_gid}" "$base_dir" "$artifacts_dir" 2>/dev/null || true
}
trap restore_ownership EXIT

target_artifact="$target"
case "$target" in
    all) target_artifact="all" ;;
    left) target_artifact="eyelash_corne_left" ;;
    right) target_artifact="eyelash_corne_right" ;;
    reset) target_artifact="eyelash_corne_settings_reset" ;;
esac

require_tool() {
    if ! command -v "$1" >/dev/null 2>&1; then
        printf 'Missing required tool in container: %s\n' "$1" >&2
        exit 1
    fi
}

require_tool west
git config --global --add safe.directory '*'

if [[ "$clean" == "1" ]]; then
    find "$base_dir" -mindepth 1 -maxdepth 1 -exec rm -rf {} +
fi

rm -rf "$config_dir"
mkdir -p "$config_dir"
cp -a "${config_repo}/config/." "$config_dir/"

if [[ ! -d "${base_dir}/.west" ]]; then
    west init -l "$config_dir"
fi

if [[ "$skip_update" == "1" ]] && ! west list >/dev/null 2>&1; then
    printf 'West cache is not ready for --skip-update; running west update.\n'
    skip_update=0
fi

if [[ "$skip_update" != "1" ]]; then
    west update --fetch-opt=--filter=tree:0
else
    printf 'Skipping west update.\n'
fi

if ! west zephyr-export; then
    printf 'west zephyr-export unavailable; continuing with west build.\n'
fi

emit_build_entries() {
    awk '
        function trim(s) {
            sub(/^[[:space:]]+/, "", s)
            sub(/[[:space:]]+$/, "", s)
            return s
        }
        function value(line) {
            sub(/^[[:space:]-]*[^:]+:[[:space:]]*/, "", line)
            line = trim(line)
            if (line ~ /^".*"$/ || line ~ /^'\''.*'\''$/) {
                line = substr(line, 2, length(line) - 2)
            }
            return line
        }
        function flush() {
            if (board != "") {
                printf "%s\037%s\037%s\037%s\037%s\n", board, shield, snippet, cmake_args, artifact
            }
            board = shield = snippet = cmake_args = artifact = ""
        }
        /^[[:space:]]*-[[:space:]]*board:/ {
            flush()
            board = value($0)
            next
        }
        /^[[:space:]]*shield:/ { shield = value($0); next }
        /^[[:space:]]*snippet:/ { snippet = value($0); next }
        /^[[:space:]]*cmake-args:/ { cmake_args = value($0); next }
        /^[[:space:]]*artifact-name:/ { artifact = value($0); next }
        END { flush() }
    ' "${config_repo}/build.yaml"
}

built=0
while IFS=$'\037' read -r board shield snippet cmake_args artifact; do
    if [[ -z "$artifact" ]]; then
        artifact="${shield:+${shield}-}${board}-zmk"
        artifact="${artifact// /_}"
    fi

    if [[ "$target_artifact" != "all" && "$target_artifact" != "$artifact" ]]; then
        continue
    fi

    build_dir="${base_dir}/build/${artifact}"
    west_args=()
    cmake_extra=(-DZMK_CONFIG="$config_dir")

    if [[ -n "$snippet" ]]; then
        west_args+=(-S "$snippet")
    fi

    if [[ -n "$shield" ]]; then
        cmake_extra+=(-DSHIELD="$shield")
    fi

    if [[ -f "${config_repo}/zephyr/module.yml" ]]; then
        cmake_extra+=(-DZMK_EXTRA_MODULES="$config_repo")
    fi

    if [[ -n "$cmake_args" ]]; then
        read -r -a matrix_cmake_args <<<"$cmake_args"
        cmake_extra+=("${matrix_cmake_args[@]}")
    fi

    printf '\n==> Building %s (%s%s%s)\n' "$artifact" "$board" "${shield:+ / }" "$shield"
    west build -p auto -s zmk/app -d "$build_dir" -b "$board" "${west_args[@]}" -- "${cmake_extra[@]}"

    mkdir -p "$artifacts_dir"
    if [[ -f "${build_dir}/zephyr/zmk.uf2" ]]; then
        cp "${build_dir}/zephyr/zmk.uf2" "${artifacts_dir}/${artifact}.uf2"
        printf 'Wrote %s\n' "${artifacts_dir}/${artifact}.uf2"
    elif [[ -f "${build_dir}/zephyr/zmk.bin" ]]; then
        cp "${build_dir}/zephyr/zmk.bin" "${artifacts_dir}/${artifact}.bin"
        printf 'Wrote %s\n' "${artifacts_dir}/${artifact}.bin"
    else
        printf 'Build succeeded but no zmk.uf2 or zmk.bin was found for %s\n' "$artifact" >&2
        exit 1
    fi

    built=$((built + 1))
done < <(emit_build_entries)

if [[ "$built" -eq 0 ]]; then
    printf 'No build.yaml entry matched target: %s\n' "$target" >&2
    exit 1
fi

printf '\nBuilt %d target(s). Artifacts are in %s\n' "$built" "$artifacts_dir"
CONTAINER_SCRIPT
