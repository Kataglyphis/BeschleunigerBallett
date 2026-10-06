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
  # Sync validation rides along with the GPU suites: 921 s against 900 s on llvmpipe (docs/gpu-golden-testing.md).
  export VK_KHRONOS_VALIDATION_VALIDATE_SYNC=1
  xvfb-run -a -s "-screen 0 1920x1080x24" bash "${BASH_SOURCE[0]}" "$@"
  exit 0
fi

ctest_run_parse_args "$@"
ctest_run_prepare_env
started="$(mktemp)"
ctest_rc=0
ctest_run_execute || ctest_rc=$?

# Most tests ignore validation messages, so a hazard reaches the verdict only through ctest's own log.
if [[ -n "${VK_KHRONOS_VALIDATION_VALIDATE_SYNC:-}" ]]; then
  mapfile -t logs < <(find Testing/Temporary -name 'LastTest*.log' -newer "${started}" 2>/dev/null)
  hazards="$(cat "${logs[@]}" /dev/null | grep -c 'SYNC-HAZARD' || true)"
  if [[ "${hazards}" -gt 0 ]]; then
    warn "Synchronization validation logged ${hazards} SYNC-HAZARD line(s); the tests that did:"
    awk '/^[0-9]+\/[0-9]+ Testing: /{ name = $3 } /SYNC-HAZARD/ && name != "" { print "  " name; name = "" }' "${logs[@]}" >&2
    ctest_rc=1
  fi
fi
rm -f "${started}"
exit "${ctest_rc}"
