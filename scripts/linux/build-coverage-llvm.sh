#!/usr/bin/env bash
# build-coverage-llvm.sh - this project's suite, profile paths and filters over ANTfrastructure's lib/coverage.sh.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
source "${SCRIPT_DIR}/lib/common.sh"

# Not a third_party literal: antfrastructure_source honours ANTFRASTRUCTURE_DIR.
antfrastructure_source linux/scripts/lib/coverage.sh
antfrastructure_source linux/scripts/lib/compiler-llvm-tools.sh

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

# Use the compiler's own pair: PATH's older llvm-profdata rejects the image clang's raw profile.
use_compiler_llvm_tools "${BUILD_DIR}" llvm-profdata llvm-cov
require_tools llvm-profdata llvm-cov

# compileTestSuite touches no Vulkan device, so coverage runs in headless CI.
TEST_SUITE="${BUILD_DIR}/compileTestSuite"
PROFRAW="${BUILD_DIR}/Test/compile/default.profraw"
PROFDATA="${BUILD_DIR}/compileTestSuite.profdata"

# Project -ignore-filename-regex patterns go here, not in the shared library.
COVERAGE_LLVM_IGNORE_REGEX=()

coverage_llvm_generate_profile "${TEST_SUITE}" "${PROFRAW}"
coverage_llvm_report "${TEST_SUITE}" "${PROFRAW}" "${PROFDATA}" "${COVERAGE_JSON}"
