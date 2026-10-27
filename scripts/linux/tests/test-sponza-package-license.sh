#!/usr/bin/env bash
# Tests the shared CMake installation in a real ZIP; it does not inspect all native dependencies.
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "${here}/../../.." && pwd)"
# shellcheck source=scripts/linux/lib/antfrastructure.sh
source "${here}/../lib/antfrastructure.sh"
_harness="$(antfrastructure_path linux/scripts/lib/test-harness.sh)" || exit 1
# shellcheck disable=SC1090
source "${_harness}"
verifier="$(antfrastructure_path linux/scripts/verify_package_license.py)" || exit 1
work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

t_case 'the official source text matches the recorded SHA-256'
t_assert_ok bash -c 'printf "%s  %s\n" "$1" "$2" | sha256sum -c --status' _ \
  e6bc9e9c474700b708f568bac9e5a8a9bcb2b1dad53442f5ba449fcb848b8e76 "${root}/licenses/CC-BY-3.0.txt"
t_case 'the project CMake build includes the scene install rules'
t_assert_contains "$(cat "${root}/CMakeLists.txt")" 'include(cmake/SponzaLicenses.cmake)'

t_case 'the CMake install rule carries both complete scene texts into a real package'
cat > "${work}/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.20)
project(sponza_license NONE)
include("${SPONZA_MODULE}")
set(CPACK_PACKAGE_FILE_NAME sponza-license)
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
include(CPack)
CMAKE
t_assert_ok cmake -S "${work}" -B "${work}/build" "-DSPONZA_MODULE=${root}/cmake/SponzaLicenses.cmake"
t_assert_ok cpack --config "${work}/build/CPackConfig.cmake" -G ZIP -B "${work}/dist"
t_assert_ok python3 "${verifier}" "${work}/dist/sponza-license.zip" \
  --require "Resources/Models/crytek-sponza/CC-BY-3.0.txt=${root}/licenses/CC-BY-3.0.txt" \
  --require "Resources/Models/crytek-sponza/crytek-sponza.NOTICE=${root}/licenses/crytek-sponza.NOTICE"
t_summary
