#!/usr/bin/env bash
# run-ctest.sh [--virtual-display] - this project's defaults over ANTfrastructure's lib/ctest-run.sh.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
source "${SCRIPT_DIR}/lib/common.sh"

# Not a third_party literal: antfrastructure_source honours ANTFRASTRUCTURE_DIR.
antfrastructure_source linux/scripts/lib/ctest-run.sh

CTEST_RUN_DEFAULT_BUILD_DIR="build"
CTEST_RUN_DEFAULT_BUILD_TYPE="Debug"
CTEST_RUN_USAGE_INTRO="Runs the BeschleunigerBallett test suite inside the Linux container image."

# No default --ctest-exclude: the suites differ per lane, so reusable-linux.yml passes it.

# Xvfb lets the GPU suites render on llvmpipe; xvfb-run hangs as PID 1, hence a child, not exec.
if [[ "${1:-}" == "--virtual-display" ]]; then
  shift
  # Mesa 26.0 lavapipe's BVH radix sort assumes 8-lane subgroups; 128-bit hosts (arm64) get 4 and it scribbles memory.
  export LP_NATIVE_VECTOR_WIDTH="${LP_NATIVE_VECTOR_WIDTH:-256}"
  xvfb-run -a -s "-screen 0 1920x1080x24" bash "${BASH_SOURCE[0]}" "$@"
  exit 0
fi

ctest_run_main "$@"
