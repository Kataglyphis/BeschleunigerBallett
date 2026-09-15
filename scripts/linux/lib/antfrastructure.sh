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

# Three places, in order, and the order is the point:
#   1. $ANTFRASTRUCTURE_DIR, if the caller set it. A container bind-mounts the
#      workspace somewhere else than the host, so an explicit answer always wins.
#   2. the submodule, which is what six of the consumers have.
#   3. a plain sibling clone at <repo>/antfrastructure-tools, which is what a
#      consumer with NO submodule has. One repo hand-rolled that probe because
#      this template could not express it; now it can, and there is no reason
#      for a seventh bootstrap variant.
if [ -z "${ANTFRASTRUCTURE_DIR:-}" ]; then
    if [ -d "${KATAGLYPHIS_REPO_ROOT}/third_party/ANTfrastructure" ]; then
        ANTFRASTRUCTURE_DIR="${KATAGLYPHIS_REPO_ROOT}/third_party/ANTfrastructure"
    elif [ -d "${KATAGLYPHIS_REPO_ROOT}/antfrastructure-tools" ]; then
        ANTFRASTRUCTURE_DIR="${KATAGLYPHIS_REPO_ROOT}/antfrastructure-tools"
    else
        # Neither exists: keep the submodule path so the error text below points
        # at the shape this repo declared, rather than at a directory nobody
        # asked for.
        ANTFRASTRUCTURE_DIR="${KATAGLYPHIS_REPO_ROOT}/third_party/ANTfrastructure"
    fi
fi
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
        # The hint has to match THIS repo's shape. `git submodule update` in a
        # repo with no such submodule prints "No submodule mapping found" and
        # sends the reader looking for a broken submodule that never existed.
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
