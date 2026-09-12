#!/usr/bin/env bash
# build-coverage-llvm.sh - project wrapper around ANTfrastructure's generic
# coverage driver (linux/scripts/lib/coverage.sh). Only this project's test
# suite name, profile paths and exclusion filters live here.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
source "${SCRIPT_DIR}/lib/common.sh"

# lib/common.sh sources lib/antfrastructure.sh, so antfrastructure_source is already
# defined. It resolves against ANTFRASTRUCTURE_DIR - which the hand-rolled
# "${SCRIPT_DIR}/../../third_party/ANTfrastructure/..." literal this replaces could
# not honour - and fails naming the probed path AND the fix.
antfrastructure_source linux/scripts/lib/coverage.sh

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      BUILD_DIR_ARG="${2:-}"
      shift 2
      ;;
    --coverage-json)
      COVERAGE_JSON_ARG="${2:-}"
      shift 2
      ;;
    -*)
      err "Unknown argument: $1"
      ;;
    *)
      break
      ;;
  esac
done

BUILD_DIR="${BUILD_DIR_ARG:-${BUILD_DIR:-build}}"
COVERAGE_JSON="${COVERAGE_JSON_ARG:-${COVERAGE_JSON:-${BUILD_DIR}/coverage.json}}"

require_tools llvm-profdata llvm-cov

# compileTestSuite is device-free (it links VulkanEngineCore but touches no
# Vulkan), which is what lets the coverage run work in headless CI.
TEST_SUITE="${BUILD_DIR}/compileTestSuite"
PROFRAW="${BUILD_DIR}/Test/compile/default.profraw"
PROFDATA="${BUILD_DIR}/compileTestSuite.profdata"

# No exclusion filters applied today; add llvm-cov -ignore-filename-regex
# patterns here rather than in the shared library.
COVERAGE_LLVM_IGNORE_REGEX=()

coverage_llvm_generate_profile "${TEST_SUITE}" "${PROFRAW}"
coverage_llvm_report "${TEST_SUITE}" "${PROFRAW}" "${PROFDATA}" "${COVERAGE_JSON}"
