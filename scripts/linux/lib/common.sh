#!/usr/bin/env bash
# common.sh - shared utilities for BeschleunigerBallett Linux scripts
# Sources utilities from ANTfrastructure when available, provides fallbacks

SCRIPT_LIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Where ANTfrastructure is, and how to resolve a file inside it, comes from the
# canonical bootstrap — a verbatim copy of upstream's
# shared/linux/templates/antfrastructure.sh. It defines ANTFRASTRUCTURE_DIR (honouring
# an environment override, which matters in the container) plus
# antfrastructure_path / antfrastructure_source / antfrastructure_exec.
#
# Before this, the submodule path was spelled out here as a ../../.. literal.
# Six repos each had their own version of that line; see ANTfrastructure
# shared/linux/templates/README.md for what they drifted into.
# shellcheck source=/dev/null
source "${SCRIPT_LIB_DIR}/antfrastructure.sh"

ANTFRASTRUCTURE_CORE="${ANTFRASTRUCTURE_DIR}/linux/scripts/01-core"

# source_module keeps THIS repo's search order, which is deliberately wider than
# antfrastructure_source's single path:
#   1. lib/<name>          — a local override wins
#   2. ANTfrastructure        — the submodule checkout
#   3. lib/../<name>       — legacy layout
#   4. /opt/scripts/core   — where the image bakes these same files, and where
#                            there is no submodule to resolve against at all
# That last one is why this cannot simply become antfrastructure_source.
# Fills _MODULE_CANDIDATES with the probe list for <name>. ONE definition, used
# by both source_module and have_module: a second copy of this list is how a
# presence test and the load that follows it drift apart.
_module_candidates() {
  local name="$1"
  _MODULE_CANDIDATES=(
    "${SCRIPT_LIB_DIR}/${name}"
    "${ANTFRASTRUCTURE_CORE}/${name}"
    "${SCRIPT_LIB_DIR}/../${name}"
    "/opt/scripts/core/${name}"
  )
}

source_module() {
  local name="$1"
  if [[ -z "${name}" ]]; then
    echo "[ERROR] source_module requires a filename" >&2
    return 1
  fi

  _module_candidates "${name}"

  local c
  for c in "${_MODULE_CANDIDATES[@]}"; do
    if [[ -f "${c}" ]]; then
      # shellcheck disable=SC1090
      source "${c}"
      return 0
    fi
  done

  echo "[ERROR] required module '${name}' not found (searched: ${_MODULE_CANDIDATES[*]})" >&2
  return 1
}

# Is <name> resolvable at all? Answers the question the OPTIONAL imports below
# actually ask, without conflating "not present" with "present and broken".
have_module() {
  _module_candidates "${1:?module name required}"
  local c
  for c in "${_MODULE_CANDIDATES[@]}"; do
    [[ -f "${c}" ]] && return 0
  done
  return 1
}

# Logging: ANTfrastructure's when reachable, a local definition when it is not.
# This one is not optional - every script here calls info/warn/err - so the
# fallback is a real implementation rather than a shrug.
if have_module logging.sh; then
  source_module logging.sh
else
  info() { printf '[1;34m[INFO][0m %s
' "$*"; }
  warn() { printf '[1;33m[WARN][0m %s
' "$*" >&2; }
  err() { printf '[1;31m[ERROR][0m %s
' "$*" >&2; exit 1; }
  die() { err "$@"; }
  log() { info "$@"; }
fi

# OPTIONAL ANTfrastructure modules. Every caller of these guards with `declare -F`
# (source_vulkan_env below, get_build_jobs further down), so a missing one is a
# documented degraded mode rather than a fault - the image bakes some of them at
# /opt/scripts/core and a bare checkout has none.
#
# What is NOT optional, and what the `source_module X 2>/dev/null || true` this
# replaces could not tell apart, is a module that EXISTS and fails to load: a
# syntax error or a failed nested source became a silent no-op, and the first
# symptom was an undefined function somewhere far away. Presence is now the
# question asked, and a load failure is fatal.
#
# vulkan-env.sh in particular carries the union of the search strategies this
# file and scripts/linux/run-debug.sh used to implement inline (explicit
# $VULKAN_SETUP_SCRIPT, $VULKAN_VERSION under /opt/vulkan and ~/vulkan, the
# arch-subdirectory glob, $VULKAN_SDK/setup-env.sh, the plain
# /opt/vulkan/*/setup-env.sh sweep and the glslc-on-PATH short circuit) plus the
# prefix handling from 02-toolchain/vulkan.sh, and it is dependency-free: it
# does NOT drag in downloads.sh or a file-scope `set -euo pipefail`.
for _optional_module in platform.sh verify.sh parallelism.sh vulkan-env.sh; do
  if have_module "${_optional_module}"; then
    source_module "${_optional_module}"
  fi
done
unset _optional_module

# Standard Vulkan environment sourcing (shared across all scripts).
# Non-strict on purpose (strict=0): a dev box without an installed SDK must warn
# and still proceed, unlike the image-side source_vulkan_sdk_env which returns 1.
source_vulkan_env() {
  if declare -F vulkan_env_source >/dev/null 2>&1; then
    vulkan_env_source "" "keep-libs" 0
    return 0
  fi

  warn "vulkan-env.sh module not found – continuing without explicit sourcing"
  return 0
}

# has_tool / require_tools now have ONE owner: ANTfrastructure
# linux/scripts/01-core/tool-checks.sh. Before it, this eight-line primitive
# existed three times with no canonical copy - here, and as two inline
# `if ! declare -F has_tool` fallbacks inside ANTfrastructure's own
# lib/code-quality.sh and lib/coverage.sh, whose comments said the pair
# "normally come from the project's common.sh". That is an upside-down
# ownership arrow: a hub driver should not depend on a CONSUMER to supply its
# helper. Sourcing the module here is the consumer half of removing it.
#
# The module guards each definition with `declare -F`, so it must be sourced
# BEFORE any local definition - a local one first would silently win and the
# adoption would be cosmetic.
#
# THE `else` BRANCH IS A PIN FALLBACK, NOT A SECOND IMPLEMENTATION TO MAINTAIN.
# tool-checks.sh landed in ANTfrastructure after the commit third_party/ANTfrastructure
# currently pins, and this file is sourced by every Linux script in the repo, so
# a hard dependency would break the whole tree until the gitlink moves. It is
# reached only when the module is genuinely ABSENT: have_module asks about
# presence, so a module that exists and fails to load is a hard error rather
# than a silent downgrade to the local copy.
#
# TO RETIRE: in the same commit that bumps third_party/ANTfrastructure to a hub
# commit shipping linux/scripts/01-core/tool-checks.sh, replace this whole block
# with the single line `source_module tool-checks.sh` and delete this comment.
if have_module tool-checks.sh; then
  source_module tool-checks.sh
else
  # Ensure required tools are installed. Names EVERY missing tool, not just the
  # first: reporting one at a time turns "install these four" into four failed
  # runs.
  require_tools() {
    local missing=()
    for tool in "$@"; do
      if ! command -v "$tool" >/dev/null 2>&1; then
        missing+=("$tool")
      fi
    done

    if [[ ${#missing[@]} -gt 0 ]]; then
      err "Required tools not found: ${missing[*]}"
    fi
  }

  # Check if tool exists (without error)
  has_tool() {
    command -v "$1" >/dev/null 2>&1
  }
fi

# Compute optimal parallel jobs (with memory cap)
# Falls back to nproc if parallelism.sh not available
get_build_jobs() {
  local mb_per_job="${1:-4000}"  # Default: 4GB per job
  
  if declare -f compute_jobs_with_mem_cap &>/dev/null; then
    compute_jobs_with_mem_cap "" "${mb_per_job}"
  else
    local cores
    cores=$(nproc --all 2>/dev/null || echo 1)
    echo "${cores}"
  fi
}

# Get script root directory
get_script_root() {
  cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd
}

# Get project root directory
get_project_root() {
  # common.sh lives in scripts/linux/lib; go three levels up to reach repo root
  cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd
}

# source_hub_module() stood here. It was a third, redundant way to reach into
# the submodule - a <category>/<name> split over a hard-coded
# "${SCRIPT_LIB_DIR}/../../../third_party/ANTfrastructure/..." literal that ignored
# the ANTFRASTRUCTURE_DIR override the bootstrap above exists to provide, and that
# returned a bare 1 so every caller had to invent its own error text.
#
# Its two callers (docs-build-web.sh and wasm-size-budget.sh, both for
# lib/rust-toolchain.sh) now call antfrastructure_source directly with the full
# hub-relative path. Nothing else referenced it.
#
# source_module() above is NOT redundant with antfrastructure_source and stays: its
# search order is deliberately wider, ending at /opt/scripts/core, where the
# image bakes these files and where there is no submodule to resolve against.

# ---------------------------------------------------------------------------
# Rust toolchain selection and CARGO_HOME, applied on source so every script in
# this directory gets it (all 14 source this file).
#
# BOTH are ANTfrastructure's, and this file used to carry its own copies. They were
# not merely duplicates, they were the weaker versions:
#
#   * the CARGO_HOME probe defaulted to /usr/local/cargo rather than
#     $HOME/.cargo, so on a developer host with CARGO_HOME unset it probed a
#     directory nobody can write, concluded "not writable", and silently
#     redirected the real ~/.cargo to /tmp/cargo-home;
#   * it tested with [[ -w ]], which the hub guard's own header names as the
#     REJECTED test: [ -w ] reports the root-owned dir as writable to uid 1001
#     while an actual touch is denied. The hub writes a probe file instead.
#
# The guards also ran anyway, transitively, through _cargo_wrapper.sh - so the
# same algorithm was executing twice per invocation, in two versions.
antfrastructure_source linux/scripts/02-toolchain/rust/_rust_toolchain_guard.sh
antfrastructure_source linux/scripts/02-toolchain/rust/_cargo_home_guard.sh
