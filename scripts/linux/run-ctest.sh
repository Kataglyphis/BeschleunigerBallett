#!/usr/bin/env bash
# run-ctest.sh - project wrapper around ANTfrastructure's generic ctest runner.
# Everything reusable (arg parsing, git safe.directory, Vulkan env, the ctest
# verbosity/-T test flag set and the --ctest-exclude plumbing) lives in
# ANTfrastructure's linux/scripts/lib/ctest-run.sh; only this project's defaults
# live here.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
source "${SCRIPT_DIR}/lib/common.sh"

# lib/common.sh sources lib/antfrastructure.sh, so antfrastructure_source is already
# defined. It resolves against ANTFRASTRUCTURE_DIR - which the hand-rolled
# "${SCRIPT_DIR}/../../third_party/ANTfrastructure/..." literal this replaces could
# not honour - and fails naming the probed path AND the fix.
antfrastructure_source linux/scripts/lib/ctest-run.sh

CTEST_RUN_DEFAULT_BUILD_DIR="build"
CTEST_RUN_DEFAULT_BUILD_TYPE="Debug"
CTEST_RUN_USAGE_INTRO="Runs the BeschleunigerBallett test suite inside the Linux container image."

# No default --ctest-exclude: which suites are GPU/device-dependent differs per
# lane (Linux.yml excludes Integration/GoldenRender and the shader-freshness
# check on the headless runners), so the exclusion stays an explicit CI
# argument rather than a silent default here.

ctest_run_main "$@"
