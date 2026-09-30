#!/usr/bin/env bash
# Copy of ANTfrastructure's shared/linux/templates/antfrastructure.sh; the sync gate holds the body, so never hand-edit.
[ -n "${_KATAGLYPHIS_ANTFRASTRUCTURE_SH_LOADED:-}" ] && return 0
_KATAGLYPHIS_ANTFRASTRUCTURE_SH_LOADED=1

# ADJUST this if the file moves. scripts/linux/lib -> repo root is three levels.
: "${KATAGLYPHIS_REPO_ROOT_RELATIVE:=../../..}"

_antfrastructure_lib_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Overridable because a container bind-mounts the workspace at a different path.
KATAGLYPHIS_REPO_ROOT="${KATAGLYPHIS_REPO_ROOT:-$(cd "${_antfrastructure_lib_dir}/${KATAGLYPHIS_REPO_ROOT_RELATIVE}" && pwd)}"

# Order matters: an explicit ANTFRASTRUCTURE_DIR, then the submodule, then a sibling antfrastructure-tools clone.
if [ -z "${ANTFRASTRUCTURE_DIR:-}" ]; then
    if [ -d "${KATAGLYPHIS_REPO_ROOT}/third_party/ANTfrastructure" ]; then
        ANTFRASTRUCTURE_DIR="${KATAGLYPHIS_REPO_ROOT}/third_party/ANTfrastructure"
    elif [ -d "${KATAGLYPHIS_REPO_ROOT}/antfrastructure-tools" ]; then
        ANTFRASTRUCTURE_DIR="${KATAGLYPHIS_REPO_ROOT}/antfrastructure-tools"
    else
        # Neither exists: keep the submodule path so the error below names the declared shape.
        ANTFRASTRUCTURE_DIR="${KATAGLYPHIS_REPO_ROOT}/third_party/ANTfrastructure"
    fi
fi
export KATAGLYPHIS_REPO_ROOT ANTFRASTRUCTURE_DIR

# Absolute path of a file inside the submodule, or a failure naming the path and the fix bash's own error hides.
antfrastructure_path() {
    local relative_path="${1:?relative path required}"
    local resolved="${ANTFRASTRUCTURE_DIR}/${relative_path}"
    if [ ! -e "$resolved" ]; then
        echo "Error: ANTfrastructure file not found: ${resolved}" >&2
        # The hint must match this repo's shape: without the submodule, `git submodule update` misleads.
        if grep -q 'third_party/ANTfrastructure' "${KATAGLYPHIS_REPO_ROOT}/.gitmodules" 2>/dev/null; then
            echo "       If the whole directory is missing, the submodule is not checked out:" >&2
            echo "       git submodule update --init --recursive third_party/ANTfrastructure" >&2
        else
            echo "       This repo declares no ANTfrastructure submodule. Clone it beside the" >&2
            echo "       checkout, or point ANTFRASTRUCTURE_DIR at one you already have:" >&2
            echo "       git clone --depth 1 https://github.com/Kataglyphis/ANTfrastructure antfrastructure-tools" >&2
            echo "       export ANTFRASTRUCTURE_DIR=<checkout>" >&2
        fi
        echo "       If only this file is missing, the pinned ANTfrastructure predates it or it" >&2
        echo "       moved: bump the gitlink (git submodule update --remote --merge" >&2
        echo "       third_party/ANTfrastructure), then check ${ANTFRASTRUCTURE_DIR}/docs/INDEX.md" >&2
        return 1
    fi
    printf '%s' "$resolved"
}

# antfrastructure_source <relative>: source a load-guarded upstream library, e.g. linux/scripts/01-core/logging.sh
antfrastructure_source() {
    local resolved
    resolved="$(antfrastructure_path "${1:?relative path required}")" || return 1
    # shellcheck disable=SC1090
    source "$resolved"
}

# antfrastructure_exec <relative> "$@": exec a driver with WORKSPACE_ROOT pinned; see shared/linux/templates/README.md § The three entry points
antfrastructure_exec() {
    local relative_path="${1:?relative path required}"
    shift
    local resolved
    resolved="$(antfrastructure_path "$relative_path")" || exit 1
    export WORKSPACE_ROOT="${WORKSPACE_ROOT:-$KATAGLYPHIS_REPO_ROOT}"
    exec bash "$resolved" "$@"
}
