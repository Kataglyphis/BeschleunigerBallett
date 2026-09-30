#!/usr/bin/env bash
# common.sh - shared helpers for these scripts: ANTfrastructure's when reachable, local fallbacks otherwise.

SCRIPT_LIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Never a ../../.. literal: the bootstrap honours the ANTFRASTRUCTURE_DIR override the container needs.
# shellcheck source=/dev/null
source "${SCRIPT_LIB_DIR}/antfrastructure.sh"

ANTFRASTRUCTURE_CORE="${ANTFRASTRUCTURE_DIR}/linux/scripts/01-core"

# Read from versions.env so no literal here goes stale on a pin bump.
antfrastructure_version() {
  local key="${1:?versions.env key required}" file value
  file="$(antfrastructure_path linux/scripts/01-core/versions.env)" || return 1
  value="$(grep -m1 "^${key}=" "${file}" | cut -d= -f2)"
  if [[ -z "${value}" ]]; then
    echo "[ERROR] ${key} not found in ${file}" >&2
    return 1
  fi
  printf '%s' "${value}"
}

# Wider than antfrastructure_source on purpose: the image bakes these files at /opt/scripts/core, with no submodule.
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

# Presence only, so "present and broken" still fails loudly when sourced.
have_module() {
  _module_candidates "${1:?module name required}"
  local c
  for c in "${_MODULE_CANDIDATES[@]}"; do
    [[ -f "${c}" ]] && return 0
  done
  return 1
}

# Every script calls info/warn/err, so the logging fallback is a real implementation.
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

# Absent is a degraded mode (callers guard with declare -F); present but failing to load is fatal.
for _optional_module in platform.sh verify.sh parallelism.sh vulkan-env.sh; do
  if have_module "${_optional_module}"; then
    source_module "${_optional_module}"
  fi
done
unset _optional_module

# strict=0: a dev box without an SDK warns and proceeds, unlike the image-side source_vulkan_sdk_env.
source_vulkan_env() {
  if declare -F vulkan_env_source >/dev/null 2>&1; then
    vulkan_env_source "" "keep-libs" 0
    return 0
  fi

  warn "vulkan-env.sh module not found – continuing without explicit sourcing"
  return 0
}

# tool-checks.sh guards each definition with declare -F, so source it before any local definition.
source_module tool-checks.sh

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

# Applied on source so every script here gets the hub's Rust toolchain and CARGO_HOME guards; never copy them.
antfrastructure_source linux/scripts/02-toolchain/rust/_rust_toolchain_guard.sh
antfrastructure_source linux/scripts/02-toolchain/rust/_cargo_home_guard.sh
