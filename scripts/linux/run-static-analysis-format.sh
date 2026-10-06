#!/usr/bin/env bash
# run-static-analysis-format.sh - project roots over ANTfrastructure's lib/code-quality.sh, whose header lists the Windows divergences.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# shellcheck source=lib/common.sh
source "${SCRIPT_DIR}/lib/common.sh"

# Not a third_party literal: antfrastructure_source honours ANTFRASTRUCTURE_DIR.
antfrastructure_source linux/scripts/lib/code-quality.sh

BUILD_DIR="${BUILD_DIR:-build}"
PRESET="${PRESET:-}"
SCAN_BUILD_OUT="${SCAN_BUILD_OUT:-scan-build-reports}"
CLANG_TIDY_FIX="${CLANG_TIDY_FIX:-false}"
RUN_CLANG_ANALYZE_HTML="${RUN_CLANG_ANALYZE_HTML:-false}"

RUN_FORMAT_AND_TIDY="${RUN_FORMAT_AND_TIDY:-true}"
RUN_SCAN_BUILD="${RUN_SCAN_BUILD:-true}"
RUN_FORMAT_CHECK="${RUN_FORMAT_CHECK:-false}"

# Project defaults for the shared library
CODE_QUALITY_PROJECT_ROOT="${ROOT_DIR}"

# Windows tidies Src only; see divergence 1 in code-quality.sh.
CPP_SOURCE_ROOTS=(Src Test)

# clang++ --analyze only ever looked at Src.
ANALYZE_SOURCE_ROOT="Src"

# Plus the one C file outside them, so this gate grades the same files as the Windows check.
FORMAT_SOURCE_ROOTS=(Src Test scripts/riscv64)
ANALYZE_EXTRA_ARGS=(-DUSE_RUST=1)

CODE_QUALITY_CMAKE_SEARCH_ROOT="."
CODE_QUALITY_CMAKE_EXCLUDE_PATHS=('./build/*' './build-release/*' './third_party/*')
CODE_QUALITY_CMAKE_FORMAT_CONFIG=".cmake-format.yaml"

# The hub's default bootstrap installs cmake-format from its pinned requirements, never this repo's requirements.txt.

# The fleet formats with the pinned LLVM (owner, 2026-10-06); the image's PATH clang-format may still be the distro's.
resolve_pinned_clang_format() {
  local want candidate version
  want="$(antfrastructure_version LLVM_RELEASE)"
  for candidate in /usr/local/llvm-target/bin/clang-format "$(command -v clang-format || true)"; do
    [[ -x "${candidate}" ]] || continue
    version="$("${candidate}" --version)"
    if grep -Eq "clang-format version ${want//./\\.}([^0-9.]|\$)" <<<"${version}"; then
      PINNED_CLANG_FORMAT_DIR="$(dirname "${candidate}")"
      info "clang-format: ${candidate} (${want}, the hub's LLVM_RELEASE)"
      return 0
    fi
  done
  err "No clang-format ${want} (the hub's LLVM_RELEASE) in /usr/local/llvm-target/bin or on PATH; another version formats differently"
}

# A failing gate since the 2026-10-06 sweep; the PATH prefix makes the hub's helper call the pinned binary.
run_clang_format_check() {
  resolve_pinned_clang_format
  local files=()
  mapfile -t files < <(code_quality_find_cpp_files "${FORMAT_SOURCE_ROOTS[@]}")
  [[ ${#files[@]} -gt 0 ]] || err "No C/C++ file under ${FORMAT_SOURCE_ROOTS[*]}: the format gate would grade nothing"
  PATH="${PINNED_CLANG_FORMAT_DIR}:${PATH}" code_quality_check_clang_format "${files[@]}"
  if [[ "${CODE_QUALITY_CLANG_FORMAT_DEVIATIONS}" -gt 0 ]]; then
    err "${CODE_QUALITY_CLANG_FORMAT_DEVIATIONS} file(s) deviate from .clang-format; rewrite them with ${PINNED_CLANG_FORMAT_DIR}/clang-format -i"
  fi
  return 0
}

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
    resolve_pinned_clang_format
    PATH="${PINNED_CLANG_FORMAT_DIR}:${PATH}" code_quality_run_clang_format "${cpp_files[@]}"
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

run_clang_analyze_html() {
  require_tools clang++

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
  --only-format-check          Only check clang-format with the pinned LLVM; fail on any drift (CI)
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
    --only-format-check)
      RUN_FORMAT_AND_TIDY="false"
      RUN_SCAN_BUILD="false"
      RUN_FORMAT_CHECK="true"
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

# Git refuses the host-owned bind mount ("dubious ownership"); no || true, a failed write must stop the gate.
if [[ -d /workspace ]]; then
  git config --global --add safe.directory /workspace
fi

cd "${ROOT_DIR}"

total_steps=0
if [[ "${RUN_FORMAT_CHECK}" == "true" ]]; then
  ((total_steps += 1))
fi
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

if [[ "${RUN_FORMAT_CHECK}" == "true" ]]; then
  info "[${step}/${total_steps}] Checking clang-format with the pinned LLVM..."
  run_clang_format_check
  ((step += 1))
fi

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
