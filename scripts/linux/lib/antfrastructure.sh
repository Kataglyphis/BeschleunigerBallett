#!/usr/bin/env bash
# Copied verbatim from ANTfrastructure
# `shared/linux/templates/antfrastructure.sh` — do not hand-edit; sync from
# upstream instead. This is the one build-tooling file that cannot be sourced
# out of the submodule, because it is what *finds* the submodule.
#
# Entry points: antfrastructure_path / antfrastructure_source / antfrastructure_exec.
# See ANTfrastructure shared/linux/templates/README.md.
# Load guard: sourcing twice is free and common (a driver and its wrapper both
# want the helpers).
[ -n "${_KATAGLYPHIS_ANTFRASTRUCTURE_SH_LOADED:-}" ] && return 0
_KATAGLYPHIS_ANTFRASTRUCTURE_SH_LOADED=1

# ADJUST this if the file moves. scripts/linux/lib -> repo root is three levels.
: "${KATAGLYPHIS_REPO_ROOT_RELATIVE:=../../..}"

_antfrastructure_lib_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Both are overridable from the environment. That matters in the container,
# where the workspace is bind-mounted at a different path than on the host.
KATAGLYPHIS_REPO_ROOT="${KATAGLYPHIS_REPO_ROOT:-$(cd "${_antfrastructure_lib_dir}/${KATAGLYPHIS_REPO_ROOT_RELATIVE}" && pwd)}"
ANTFRASTRUCTURE_DIR="${ANTFRASTRUCTURE_DIR:-${KATAGLYPHIS_REPO_ROOT}/third_party/ANTfrastructure}"
export KATAGLYPHIS_REPO_ROOT ANTFRASTRUCTURE_DIR

# Absolute path of a file inside the submodule, or a hard failure naming it.
#
# The error text names the probed path AND the fix on purpose: the failure is
# almost always "submodule not checked out" or "the file moved upstream", and
# both are invisible from bash's own message.
antfrastructure_path() {
    local relative_path="${1:?relative path required}"
    local resolved="${ANTFRASTRUCTURE_DIR}/${relative_path}"
    if [ ! -e "$resolved" ]; then
        echo "Error: ANTfrastructure file not found: ${resolved}" >&2
        echo "       If the whole directory is missing, the submodule is not checked out:" >&2
        echo "       git submodule update --init --recursive third_party/ANTfrastructure" >&2
        echo "       If only this file is missing, it moved upstream — check ${ANTFRASTRUCTURE_DIR}/docs/INDEX.md" >&2
        return 1
    fi
    printf '%s' "$resolved"
}

# Source a ANTfrastructure shell library, e.g.
#   antfrastructure_source linux/scripts/01-core/logging.sh
# Upstream libraries are load-guarded, so sourcing one twice is free.
antfrastructure_source() {
    local resolved
    resolved="$(antfrastructure_path "${1:?relative path required}")" || return 1
    # shellcheck disable=SC1090
    source "$resolved"
}

# Replace this process with a ANTfrastructure driver, forwarding the caller's
# arguments, e.g.
#   antfrastructure_exec linux/scripts/02-toolchain/python/ci_tests.sh "$@"
#
# This is the wrapper pattern. Every consumer that delegates to an upstream
# driver had hand-rolled the same guard-then-exec block; getting it wrong is
# silent, because `exec` on a missing file under `set -e` reports only bash's
# own error.
#
# WORKSPACE_ROOT is pinned here because upstream's detect_workspace derives it
# from the *sourcing script's* location — which, for a delegated driver, resolves
# inside third_party/ANTfrastructure instead of the consuming repo, so
# every tool would run against the submodule tree. detect_workspace honours a
# pre-set value and still overrides to /workspace in the container, so CI is
# unaffected. This is the single most common thing to break when a wrapper is
# "simplified".
antfrastructure_exec() {
    local relative_path="${1:?relative path required}"
    shift
    local resolved
    resolved="$(antfrastructure_path "$relative_path")" || exit 1
    export WORKSPACE_ROOT="${WORKSPACE_ROOT:-$KATAGLYPHIS_REPO_ROOT}"
    exec bash "$resolved" "$@"
}
