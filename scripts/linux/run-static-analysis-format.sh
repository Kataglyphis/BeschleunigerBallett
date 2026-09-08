#!/usr/bin/env bash
# run-static-analysis-format.sh - project wrapper around ContainerHub's generic
# code-quality driver. Everything reusable (cmake-format bootstrap, the
# file-enumeration walks, clang-format, clang-tidy, and the container
# compile-database path remapping) lives in ContainerHub's
# linux/scripts/lib/code-quality.sh; only this project's source roots, tool
# arguments and paths live here.
#
# NOTE: the Windows formatting/tidy path deliberately behaves differently on six
# axes (source roots, module-TU skip, --header-filter, --checks, per-file vs
# batched invocation, missing-compile-DB handling). They are enumerated at the
# top of code-quality.sh. Do not "align" either side without reading that block.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# shellcheck source=lib/common.sh
source "${SCRIPT_DIR}/lib/common.sh"

# lib/common.sh sources lib/containerhub.sh, so containerhub_source is already
# defined. It resolves against CONTAINERHUB_DIR - which the hand-rolled
# "${SCRIPT_DIR}/../../third_party/ContainerHub/..." literal this replaces could
# not honour - and fails naming the probed path AND the fix.
containerhub_source linux/scripts/lib/code-quality.sh

BUILD_DIR="${BUILD_DIR:-build}"
PRESET="${PRESET:-}"
SCAN_BUILD_OUT="${SCAN_BUILD_OUT:-scan-build-reports}"
CLANG_TIDY_FIX="${CLANG_TIDY_FIX:-false}"
RUN_CLANG_ANALYZE_HTML="${RUN_CLANG_ANALYZE_HTML:-false}"

RUN_FORMAT_AND_TIDY="${RUN_FORMAT_AND_TIDY:-true}"
RUN_SCAN_BUILD="${RUN_SCAN_BUILD:-true}"

# --- Project defaults consumed by the shared library -------------------------
CODE_QUALITY_PROJECT_ROOT="${ROOT_DIR}"

# Source roots walked for clang-format AND clang-tidy. Windows tidies Src only;
# see divergence 1 in code-quality.sh.
CPP_SOURCE_ROOTS=(Src Test)

# clang++ --analyze only ever looked at Src.
ANALYZE_SOURCE_ROOT="Src"
ANALYZE_EXTRA_ARGS=(-DUSE_RUST=1)

CODE_QUALITY_CMAKE_SEARCH_ROOT="."
CODE_QUALITY_CMAKE_EXCLUDE_PATHS=('./build/*' './build-release/*' './third_party/*')
CODE_QUALITY_CMAKE_FORMAT_CONFIG=".cmake-format.yaml"

# The container image builds against /opt/gcc-<version>; when that exact
# toolchain is missing on the host, its flags are stripped from the remapped
# compile DB so a local clang-tidy run still works.
CODE_QUALITY_GCC_TOOLCHAIN_PROBE_DIR="/opt/gcc-15.2.0"
CODE_QUALITY_GCC_TOOLCHAIN_PREFIX="/opt/gcc-"

# cmake-format is installed into the repo-local .venv when it is not on PATH.
#
# The install knob is a FUNCTION, not lib/uv-install-requirements.sh: that
# wrapper defaults to this repo's root requirements.txt, which is the Sphinx
# DOCS stack (sphinx, breathe, exhale, junit2html, ...) - eleven unpinned
# packages dragged in merely to run a formatter, and a docs-stack resolution
# failure would then break the formatting gate. The gate installs ContainerHub's
# pinned cmake-format bootstrap set instead (cmake-format==0.6.13 plus the
# pyyaml it needs to read .cmake-format.yaml at all) - the very file the hub's
# own preflight cmake-format gate installs, so both graders run one version.
#
# A function is what upstream itself passes here (check_cmake_format in
# ContainerHub's linux/scripts/preflight.sh): code-quality.sh runs the knob as a
# command, so a shell function needs no exec bit and no extra wrapper file. It
# runs in the subshell code-quality.sh wraps it in, so sourcing python_uv.sh
# there cannot leak its helpers over this script's own.
CODE_QUALITY_UV_VENV_CREATE_SCRIPT="${SCRIPT_DIR}/lib/uv-venv-create.sh"
cmake_format_install_requirements() {
  local requirements
  requirements="$(containerhub_path linux/scripts/cmake-format.requirements.txt)" || return 1
  containerhub_source linux/scripts/01-core/python_uv.sh
  # Same venv code-quality.sh then activates and probes for cmake-format;
  # honour the knob rather than restating its default.
  uv_pip_install_requirements "${CODE_QUALITY_VENV_DIR:-${ROOT_DIR}/.venv}" "${requirements}"
}
CODE_QUALITY_UV_INSTALL_REQUIREMENTS_SCRIPT=cmake_format_install_requirements

run_format_and_tidy() {
  code_quality_ensure_cmake_format

  require_tools cmake-format clang-format clang-tidy

  if [[ ! -f "${BUILD_DIR}/compile_commands.json" ]]; then
    err "Missing ${BUILD_DIR}/compile_commands.json. Run CMake configure first, e.g. scripts/linux/cmake-configure-build.sh --build-dir ${BUILD_DIR} --preset <preset>"
  fi

  info "Formatting CMake files..."
  local cmake_files=()
  mapfile -t cmake_files < <(code_quality_find_cmake_files)

  if [[ ${#cmake_files[@]} -gt 0 ]]; then
    code_quality_run_cmake_format "${cmake_files[@]}"
  fi

  local cpp_search_dirs=() root
  for root in "${CPP_SOURCE_ROOTS[@]}"; do
    if [[ -d "${root}" ]]; then
      cpp_search_dirs+=("${root}")
    fi
  done

  local cpp_files=()
  local clang_tidy_files=()
  if [[ ${#cpp_search_dirs[@]} -gt 0 ]]; then
    info "Finding C/C++ source files..."
    mapfile -t cpp_files < <(code_quality_find_cpp_files "${cpp_search_dirs[@]}")
    mapfile -t clang_tidy_files < <(code_quality_find_clang_tidy_files "${cpp_search_dirs[@]}")
  fi

  if [[ ${#cpp_files[@]} -gt 0 ]]; then
    code_quality_run_clang_format "${cpp_files[@]}"
  fi

  code_quality_prepare_compile_db "${BUILD_DIR}"

  warn "Disabling for internal bug of clang-tidy..."
  CODE_QUALITY_CLANG_TIDY_ARGS=(-checks=-modernize-use-scoped-lock)
  CODE_QUALITY_CLANG_TIDY_FIX="${CLANG_TIDY_FIX}"

  if [[ ${#clang_tidy_files[@]} -gt 0 ]]; then
    code_quality_run_clang_tidy "${CODE_QUALITY_COMPILE_DB_DIR}" "${clang_tidy_files[@]}"
  fi

  code_quality_cleanup_compile_db
}

run_scan_build() {
  require_tools scan-build

  mkdir -p "${SCAN_BUILD_OUT}"

  local scan_cmd=(scan-build -o "${SCAN_BUILD_OUT}" cmake --build "${BUILD_DIR}")
  if [[ -n "${PRESET}" ]]; then
    scan_cmd+=(--preset "${PRESET}")
  fi

  info "Running scan-build..."
  "${scan_cmd[@]}"
}

# Resolves the GCC prefix clang++ must be pointed at, and applies it.
#
# This lived in Linux.yml as a `bash -lc` prologue in front of the
# --only-clang-analyze-html step, which meant CI analysed against the image's
# libstdc++ and a human running the same flag locally analysed against whatever
# the system GCC happened to be - with nothing saying the two differed. It
# belongs here, where both callers get it.
#
# clang++ --analyze runs DIRECTLY: no CMake, no preset, so it cannot inherit the
# --gcc-toolchain flags CMakeLists.txt computes for every preset-driven step.
#
# The flag goes on the COMMAND LINE, via ANALYZE_EXTRA_ARGS. The workflow only
# ever exported CXXFLAGS, and clang++ does not read CXXFLAGS - that is a
# make/CMake convention, not a compiler one - so the toolchain never actually
# reached the analyzer. The exports are kept as well, unchanged, because that is
# the environment the CI step has always run with and the cmake-driven modes of
# this same script do honour them.
#
# The prefix is DISCOVERED, never hardcoded: it used to be the literal
# /opt/gcc-16.1.0, the image bumped to 16.2.0, and every Linux job died on
# "cannot find crtbeginS.o" - a message naming neither the flag nor the version.
# gcc-toolchain-root.sh shares the resolution with CMakeLists.txt.
#
# A failure to resolve is FATAL, exactly as it was in the workflow's `set -e`
# prologue: analysing against the system GCC's headers by accident is the silent
# wrong answer this whole path exists to prevent. Run this in the family image
# (ContainerHub docs/rancher-desktop-linux-containers.md), which is where CI
# runs it and where GCC_PREFIX is exported for it.
apply_gcc_toolchain_for_analysis() {
  local gcc_root
  gcc_root="$(bash "${SCRIPT_DIR}/gcc-toolchain-root.sh")"
  info "Using GCC toolchain: ${gcc_root}"
  export CXXFLAGS="--gcc-toolchain=${gcc_root}"
  export LDFLAGS="-L${gcc_root}/lib64 -Wl,-rpath,${gcc_root}/lib64 --gcc-toolchain=${gcc_root}"
  ANALYZE_EXTRA_ARGS+=("--gcc-toolchain=${gcc_root}")
}

run_clang_analyze_html() {
  require_tools clang++

  apply_gcc_toolchain_for_analysis

  if [[ ! -d "${ANALYZE_SOURCE_ROOT}" ]]; then
    warn "Skipping clang++ --analyze: ${ANALYZE_SOURCE_ROOT} directory not found."
    return
  fi

  mapfile -t src_cpp_files < <(find "${ANALYZE_SOURCE_ROOT}" -type f \( -name '*.cpp' -o -name '*.cc' \))
  if [[ ${#src_cpp_files[@]} -eq 0 ]]; then
    warn "Skipping clang++ --analyze: no ${ANALYZE_SOURCE_ROOT}/*.cpp or ${ANALYZE_SOURCE_ROOT}/*.cc files found."
    return
  fi

  info "Running clang++ --analyze (HTML output)..."
  clang++ --analyze "${ANALYZE_EXTRA_ARGS[@]}" -Xanalyzer -analyzer-output=html "${src_cpp_files[@]}"
}

usage() {
  cat <<'EOF'
Usage: run-static-analysis-format.sh [options]

Single entrypoint for formatting and static analysis.

Runs by default:
  1) cmake-format + clang-format + clang-tidy
  2) scan-build

Options:
  --build-dir <path>           Build directory (default: build)
  --preset <name>              CMake preset name for scan-build
  --scan-build-out <path>      Output directory for scan-build reports (default: scan-build-reports)
  --fix                        Apply clang-tidy fixes (-fix)
  --with-clang-analyze-html    Also run clang++ --analyze (HTML output)

  --only-format                Run only format + clang-tidy step
  --only-scan-build            Run only scan-build step
  --only-clang-analyze-html    Run only clang++ --analyze HTML step

  -h, --help                   Show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      BUILD_DIR="${2:-}"
      shift 2
      ;;
    --preset)
      PRESET="${2:-}"
      shift 2
      ;;
    --scan-build-out)
      SCAN_BUILD_OUT="${2:-}"
      shift 2
      ;;
    --fix)
      CLANG_TIDY_FIX="true"
      shift
      ;;
    --with-clang-analyze-html)
      RUN_CLANG_ANALYZE_HTML="true"
      shift
      ;;
    --only-format)
      RUN_FORMAT_AND_TIDY="true"
      RUN_SCAN_BUILD="false"
      shift
      ;;
    --only-scan-build)
      RUN_FORMAT_AND_TIDY="false"
      RUN_SCAN_BUILD="true"
      shift
      ;;
    --only-clang-analyze-html)
      RUN_FORMAT_AND_TIDY="false"
      RUN_SCAN_BUILD="false"
      RUN_CLANG_ANALYZE_HTML="true"
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      err "Unknown argument: $1"
      ;;
  esac
done

# The compile-DB hint quotes the final build dir, so refresh it after parsing.
CODE_QUALITY_COMPILE_DB_HINT="e.g. scripts/linux/cmake-configure-build.sh --build-dir ${BUILD_DIR} --preset <preset>"

git config --global --add safe.directory /workspace || true

cd "${ROOT_DIR}"

total_steps=0
if [[ "${RUN_FORMAT_AND_TIDY}" == "true" ]]; then
  ((total_steps += 1))
fi
if [[ "${RUN_SCAN_BUILD}" == "true" ]]; then
  ((total_steps += 1))
fi
if [[ "${RUN_CLANG_ANALYZE_HTML}" == "true" ]]; then
  ((total_steps += 1))
fi

if [[ ${total_steps} -eq 0 ]]; then
  err "Nothing to run. Use --help for available options."
fi

step=1

if [[ "${RUN_FORMAT_AND_TIDY}" == "true" ]]; then
  info "[${step}/${total_steps}] Running format + clang-tidy..."
  run_format_and_tidy
  ((step += 1))
fi

if [[ "${RUN_SCAN_BUILD}" == "true" ]]; then
  info "[${step}/${total_steps}] Running scan-build..."
  run_scan_build
  ((step += 1))
fi

if [[ "${RUN_CLANG_ANALYZE_HTML}" == "true" ]]; then
  info "[${step}/${total_steps}] Running clang++ --analyze HTML report generation..."
  run_clang_analyze_html
fi

info "Static analysis and formatting pipeline completed successfully."
