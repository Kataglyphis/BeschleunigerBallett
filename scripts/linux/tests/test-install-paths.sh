#!/usr/bin/env bash
# Src/shared/util/InstallPaths.hpp's layouts, search order and per-user directories, compiled with the host compiler.
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/linux/lib/antfrastructure.sh
source "${here}/../lib/antfrastructure.sh"
_harness="$(antfrastructure_path linux/scripts/lib/test-harness.sh)" || exit 1
# shellcheck source=third_party/ANTfrastructure/linux/scripts/lib/test-harness.sh
source "${_harness}"
work="$(mktemp -d)" || exit 1
trap 'rm -rf "${work}"' EXIT

"${CXX:-c++}" -std=c++20 -Wall -Wextra -I"${here}/../../../Src/shared" -o "${work}/driver" "${here}/install-paths-driver.cpp" || exit 1

# Usage: scenario NAME; the driver's verdict in a fresh scratch tree.
scenario() {
  local tree="${work}/$1"
  mkdir -p "${tree}"
  "${work}/driver" "$1" "${tree}"
}

t_case 'the packages: bin/.. holds Resources'
t_assert_rc 0 scenario bin
t_case 'a flat copy: the program beside Resources'
t_assert_rc 0 scenario flat
t_case 'a Linux bundle: the program or its loader in lib/<name>/'
t_assert_rc 0 scenario bundle
t_case 'bin/.. is tried before bin/ itself'
t_assert_rc 0 scenario precedence
t_case 'the executable tree first, the working directory after it'
t_assert_rc 0 scenario order
t_case 'per-user cache and log directories'
t_assert_rc 0 scenario user-dirs

t_summary
