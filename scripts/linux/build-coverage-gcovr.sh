#!/usr/bin/env bash
# build-coverage-gcovr.sh - project wrapper around ContainerHub's generic
# coverage driver (linux/scripts/lib/coverage.sh). Only this project's report
# root and exclusion filters live here.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
source "${SCRIPT_DIR}/lib/common.sh"

# lib/common.sh sources lib/containerhub.sh, so containerhub_source is already
# defined. It resolves against CONTAINERHUB_DIR - which the hand-rolled
# "${SCRIPT_DIR}/../../third_party/ContainerHub/..." literal this replaces could
# not honour - and fails naming the probed path AND the fix.
containerhub_source linux/scripts/lib/coverage.sh

# gcovr walks the compile directory for .gcda/.gcno, which for this project is
# the repo root the container builds from.
GCOVR_ROOT="${GCOVR_ROOT:-.}"

# No exclusion filters applied today; add regexes here (e.g. 'third_party/.*')
# rather than in the shared library.
COVERAGE_GCOVR_EXCLUDES=()

coverage_run_gcovr "${GCOVR_ROOT}"
