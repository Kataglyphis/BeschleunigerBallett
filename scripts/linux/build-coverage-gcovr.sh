#!/usr/bin/env bash
# build-coverage-gcovr.sh - this project's report root and filters over ANTfrastructure's lib/coverage.sh.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
source "${SCRIPT_DIR}/lib/common.sh"

# Not a third_party literal: antfrastructure_source honours ANTFRASTRUCTURE_DIR.
antfrastructure_source linux/scripts/lib/coverage.sh

# gcovr finds .gcda/.gcno under the compile directory, here the repo root the container builds from.
GCOVR_ROOT="${GCOVR_ROOT:-.}"

# Project exclusions go here, not in the shared library.
COVERAGE_GCOVR_EXCLUDES=()

coverage_run_gcovr "${GCOVR_ROOT}"
