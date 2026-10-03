#!/usr/bin/env bash
# run-riscv64-tests.sh - the riscv64 lane: Debug cross build on the amd64 image, ctest under QEMU; the CPU suite first, the GPU suites serially behind the fp16 shim (enabled by the owner 2026-10-03).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
source "${SCRIPT_DIR}/lib/common.sh"

antfrastructure_source linux/scripts/lib/riscv64-cross.sh
antfrastructure_source linux/scripts/lib/cmake-build.sh
antfrastructure_source linux/scripts/lib/ctest-run.sh

BUILD_DIR="build-riscv64"
# Integration and GoldenRender need a GPU and run in the shim arm below; the CPU suite keeps its exclusion.
CTEST_EXCLUDE_RISCV64='^(Integration|GoldenRender)\.'
CTEST_GPU_FILTER='^(Integration|GoldenRender)\.'
CTEST_GPU_EXCLUDE='^GoldenRender\.GuiInputSweepNeverCrashesOrLosesTheDevice$'
CTEST_GPU_TIMEOUT=7200

riscv64_cross_env
# The x86_64 setup-env this hook sources would undo riscv64_cross_env's Vulkan paths.
source_vulkan_env() { return 0; }

CMAKE_BUILD_PREBUILD_LABEL="Slang shader precompilation"
cmake_build_prebuild_hook() {
  bash "${SCRIPT_DIR}/compile-slang-shaders.sh"
}

cmake_build_main --preset linux-riscv64-cross --build-dir "${BUILD_DIR}" --mb-per-job 4000
ctest_run_main --build-dir "${BUILD_DIR}" --build-type Debug --ctest-exclude "${CTEST_EXCLUDE_RISCV64}" "$@"

# llvmpipe's ORC JIT calls the fp16 conversion helpers riscv64 glibc does not ship; the
# LD_PRELOAD shim defines them. Measured 2026-10-03 with it: 35/40 GPU tests pass, and the
# five 90-min-cap misses were -j8 contention, not correctness - at -j1 a heavy PathTracing
# test took 23 min, so the serial run fits the job with this 2 h per-test ceiling.
if [ "${RISCV64_GPU_TESTS:-1}" = "0" ]; then
  info "[gpu] RISCV64_GPU_TESTS=0 - skipping ${CTEST_GPU_FILTER} suites"
  exit 0
fi
# Every path in the arm is absolute: after ctest_run_main the shell sits inside
# ${BUILD_DIR}, so a relative GPU path doubled (/workspace/build-riscv64/build-riscv64)
# and the first attempt ran its GPU ctest with "No tests were found" as a green exit.
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TEST_DIR="${REPO_ROOT}/${BUILD_DIR}"
GPU_SHIM_DIR="${TEST_DIR}/fp16-shim"
test -d "${TEST_DIR}" || { err "[gpu] no ${TEST_DIR} - refusing a silent green"; exit 1; }
mkdir -p "${GPU_SHIM_DIR}"
"${RISCV64_CROSS_BIN}/riscv64-linux-gnu-clang" -shared -fPIC -O2 "${SCRIPT_DIR}/../riscv64/fp16_helpers.c" -o "${GPU_SHIM_DIR}/libkata_fp16_helpers.so"
test -f "${GPU_SHIM_DIR}/libkata_fp16_helpers.so" || { err "[gpu] the shim did not build - refusing an unshimmed run"; exit 1; }
info "[gpu] building the fp16 shim for the Integration/GoldenRender suites"
xvfb-run -a -s "-screen 0 1920x1080x24" \
  env LD_PRELOAD="${GPU_SHIM_DIR}/libkata_fp16_helpers.so" \
  ctest --test-dir "${TEST_DIR}" -C Debug -j1 \
    -R "${CTEST_GPU_FILTER}" -E "${CTEST_GPU_EXCLUDE}" \
    --output-on-failure --timeout "${CTEST_GPU_TIMEOUT}"
