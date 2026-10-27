#!/usr/bin/env bash
# Packs the third-party and Cargo license install rules into real ZIPs; it does not prove which code the engine links.
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "${here}/../../.." && pwd)"
# shellcheck source=scripts/linux/lib/antfrastructure.sh
source "${here}/../lib/antfrastructure.sh"
_harness="$(antfrastructure_path linux/scripts/lib/test-harness.sh)" || exit 1
# shellcheck disable=SC1090
source "${_harness}"
verifier="$(antfrastructure_path linux/scripts/verify_package_license.py)" || exit 1
python="$(command -v python3 || command -v python)"
work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
licenses=share/licenses/GraphicsEngine
# A Windows CMake reads Git Bash paths only in mixed form.
native() { if command -v cygpath > /dev/null; then cygpath -m "$1"; else printf '%s' "$1"; fi; }

t_case 'the project build installs the third-party and Cargo texts'
t_assert_contains "$(cat "${root}/CMakeLists.txt")" 'include(cmake/ThirdPartyLicenses.cmake)'
t_assert_contains "$(cat "${root}/CMakeLists.txt")" 'include(cmake/CargoLicenses.cmake)'

t_case 'CPackCommon loads at the top level, so its closure scripts are not empty in the install rules'
cpack_first() { awk '/^include\(CPackCommon\)/ { seen = 1 } /^add_subdirectory\(Src\)/ { exit !seen }' "$1"; }
t_assert_ok cpack_first "${root}/CMakeLists.txt"
grep -v '^include(CPackCommon)' "${root}/CMakeLists.txt" > "${work}/CMakeLists.mutant"
t_assert_fails cpack_first "${work}/CMakeLists.mutant"

# GraphicsEngine with GSL and abseil taken from local directories instead of their repositories.
fixture() {
  local dir="$1" module="$2"
  mkdir -p "${dir}/gsl" "${dir}/abseil"
  printf 'GSL license\n' > "${dir}/gsl/LICENSE"
  printf 'abseil license\n' > "${dir}/abseil/LICENSE"
  cat > "${dir}/CMakeLists.txt" <<CMAKE
cmake_minimum_required(VERSION 3.24)
project(GraphicsEngine NONE)
include(FetchContent)
FetchContent_Declare(GSL GIT_REPOSITORY https://invalid.example/gsl.git)
FetchContent_Declare(abseil-cpp GIT_REPOSITORY https://invalid.example/abseil.git)
FetchContent_MakeAvailable(GSL abseil-cpp)
include("$(native "${module}")")
set(CPACK_PACKAGE_FILE_NAME third-party)
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
include(CPack)
CMAKE
}
configure() {
  cmake -S "$1" -B "$1/build" "-DFETCHCONTENT_SOURCE_DIR_GSL=$1/gsl" "-DFETCHCONTENT_SOURCE_DIR_ABSEIL-CPP=$1/abseil" "${@:2}"
}

t_case 'every mapped text, GSL and abseil reach a real package unchanged'
fixture "${work}/tp" "${root}/cmake/ThirdPartyLicenses.cmake"
t_assert_ok configure "${work}/tp"
t_assert_ok cpack --config "${work}/tp/build/CPackConfig.cmake" -G ZIP -B "${work}/tp/dist"
rules=(--require "${licenses}/third_party/gsl/LICENSE=${work}/tp/gsl/LICENSE"
  --require "${licenses}/third_party/abseil/LICENSE=${work}/tp/abseil/LICENSE")
while IFS='=' read -r installed source; do
  rules+=(--require "${licenses}/third_party/${installed}=${root}/${source}")
done < "${root}/licenses/third-party-licenses.txt"
t_assert_ok "${python}" "${verifier}" "${work}/tp/dist/third-party.zip" "${rules[@]}"

t_case 'a missing FetchContent license fails the configure'
fixture "${work}/no-abseil" "${root}/cmake/ThirdPartyLicenses.cmake"
rm "${work}/no-abseil/abseil/LICENSE"
t_assert_fails configure "${work}/no-abseil"

t_case 'a mapping whose name differs from its source, or whose source is missing, fails the configure'
for line in 'glfw/LICENSE=third_party/GLFW/LICENSE.md' 'glfw/LICENSE.md=third_party/GLFW/MISSING/LICENSE.md'; do
  mkdir -p "${work}/map/cmake" "${work}/map/licenses"
  cp "${root}/cmake/ThirdPartyLicenses.cmake" "${work}/map/cmake/"
  printf '%s\n' "${line}" > "${work}/map/licenses/third-party-licenses.txt"
  mkdir -p "${work}/map/third_party/GLFW"
  printf 'zlib\n' >"${work}/map/third_party/GLFW/LICENSE.md"
  fixture "${work}/map/project-${#line}" "${work}/map/cmake/ThirdPartyLicenses.cmake"
  t_assert_fails configure "${work}/map/project-${#line}"
done

t_case 'the C++ bridge crate texts reach a real package'
t_skip_unless 'cargo' command -v cargo
case "$(uname -m)" in aarch64 | arm64) target=aarch64-unknown-linux-gnu ;; *) target=x86_64-unknown-linux-gnu ;; esac
# Corrosion's FindRust runs in third_party/: plain names stay in that scope, the *_CACHED copies are global.
mkdir -p "${work}/cargo/rust"
cat > "${work}/cargo/rust/CMakeLists.txt" <<CMAKE
set(Rust_CARGO cargo)
set(Rust_CARGO_TARGET ${target})
set(Rust_CARGO_CACHED cargo CACHE INTERNAL "")
set(Rust_CARGO_TARGET_CACHED ${target} CACHE INTERNAL "")
CMAKE
cat > "${work}/cargo/CMakeLists.txt" <<CMAKE
cmake_minimum_required(VERSION 3.24)
project(GraphicsEngine NONE)
add_subdirectory(rust)
include("$(native "${root}")/cmake/CargoLicenses.cmake")
set(CPACK_PACKAGE_FILE_NAME cargo)
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
include(CPack)
CMAKE
t_assert_ok cmake -S "${work}/cargo" -B "${work}/cargo/build" "-DPython3_EXECUTABLE=${python}"
t_assert_ok cmake --build "${work}/cargo/build"
t_assert_ok cpack --config "${work}/cargo/build/CPackConfig.cmake" -G ZIP -B "${work}/cargo/dist"
t_assert_ok "${python}" "${verifier}" "${work}/cargo/dist/cargo.zip" \
  --require "${licenses}/cargo/manifest.json=$(native "${work}")/cargo/build/cargo-licenses/manifest.json" \
  --require "${licenses}/cargo/kataglyphis_cxx_bridge@*/workspace/LICENSE=$(native "${root}")/third_party/OxidANT/LICENSE"
t_summary
