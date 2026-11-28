#!/usr/bin/env bash
# GpuTimingExportTotals.hpp on fake timing sequences: the dump leaves out the load frame, compiled with the host compiler.
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/linux/lib/antfrastructure.sh
source "${here}/../lib/antfrastructure.sh"
_harness="$(antfrastructure_path linux/scripts/lib/test-harness.sh)" || exit 1
# shellcheck source=third_party/ANTfrastructure/linux/scripts/lib/test-harness.sh
source "${_harness}"
# The harness removes it at the suite's exit.
work="$(t_mktemp -d)" || exit 1

"${CXX:-c++}" -std=c++20 -Wall -Wextra -I"${here}/../../../Src/GraphicsEngineVulkan" -o "${work}/driver" \
  "${here}/gpu-timing-export-driver.cpp" || exit 1

t_case 'the slice the load frame recorded stays out, though another image comes back first'
t_assert_rc 0 "${work}/driver" load-frame
t_case 'a recording never submitted leaves the load frame to the next one'
t_assert_rc 0 "${work}/driver" unsubmitted
t_case 'a restart drops what came before it'
t_assert_rc 0 "${work}/driver" restart

t_summary
