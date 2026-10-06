#!/usr/bin/env bash
# run-riscv64-tests.sh - the riscv64 lane: Debug cross build on the amd64 image, ctest under QEMU; CPU suites per push, one GPU shard per weekly job (owner 2026-10-06).
set -euo pipefail

# Path-traced, ray-traced and cloud tests run ~58x their x64 time under QEMU, so each solo test is a weekly job of its own.
RISCV64_GPU_SOLO_TESTS=(
  PathTracingAccumulatesAndConverges
  PathTracingAntiAliasesGeometricEdges
  PathTracingRespondsToTheDirectionalLight
  PathTracingHonorsTheQualityControls
  PathTracingPassesTheWhiteFurnaceTest
  PathTracedMaskCardShowsItsCutout
  AddedModelAppearsInPathTracing
  ReloadedModelIsVisibleInPathTracing
  RaytracingFrameSkipsTheRasterPass
)
RISCV64_GPU_CLOUD_TESTS='CloudsAcrossManyFramesDoesNotLoseTheDevice|EnablingCloudsChangesTheFrameAndAddsDetail'
RISCV64_GPU_SWEEP_TEST='GuiInputSweepNeverCrashesOrLosesTheDevice'

# The weekly matrix reads its shards here, before any hub library loads, so the list has one home.
if [[ "${1:-}" == "--list-gpu-shards" ]]; then
  printf '["raster","clouds"'
  printf ',"%s"' "${RISCV64_GPU_SOLO_TESTS[@]}"
  printf ']\n'
  exit 0
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
source "${SCRIPT_DIR}/lib/common.sh"

antfrastructure_source linux/scripts/lib/riscv64-cross.sh
antfrastructure_source linux/scripts/lib/cmake-build.sh
antfrastructure_source linux/scripts/lib/ctest-run.sh

BUILD_DIR="build-riscv64"
# Integration and GoldenRender need a GPU: the CPU run excludes them, a GPU shard runs them behind the fp16 shim.
CTEST_GPU_SUITES='^(Integration|GoldenRender)\.'
RISCV64_GPU_SHARD="${RISCV64_GPU_SHARD:-}"

# Ceilings leave a solo test ~5 h of the 6 h job after the ~20 min cross build; the measured times are in AGENTS.md.
riscv64_gpu_shard_args() {
  local shard="$1" solo_tests
  solo_tests="$(IFS='|'; printf '%s' "${RISCV64_GPU_SOLO_TESTS[*]}")"
  case "${shard}" in
    all) CTEST_GPU_ARGS=(-R "${CTEST_GPU_SUITES}" -E "^GoldenRender\.${RISCV64_GPU_SWEEP_TEST}\$" --timeout 7200) ;;
    raster)
      CTEST_GPU_ARGS=(-R "${CTEST_GPU_SUITES}"
        -E "^GoldenRender\.(${solo_tests}|${RISCV64_GPU_CLOUD_TESTS}|${RISCV64_GPU_SWEEP_TEST})\$" --timeout 5400)
      ;;
    clouds) CTEST_GPU_ARGS=(-R "^GoldenRender\.(${RISCV64_GPU_CLOUD_TESTS})\$" --timeout 9000) ;;
    *)
      [[ "|${solo_tests}|" == *"|${shard}|"* ]] || { err "unknown RISCV64_GPU_SHARD '${shard}'; see --list-gpu-shards"; exit 2; }
      CTEST_GPU_ARGS=(-R "^GoldenRender\.${shard}\$" --timeout 18000)
      ;;
  esac
  return 0
}

# Resolved before the build, so a mistyped shard fails in seconds rather than after it.
if [[ -n "${RISCV64_GPU_SHARD}" ]]; then
  riscv64_gpu_shard_args "${RISCV64_GPU_SHARD}"
fi

riscv64_cross_env
# The x86_64 setup-env this hook sources would undo riscv64_cross_env's Vulkan paths.
source_vulkan_env() { return 0; }

CMAKE_BUILD_PREBUILD_LABEL="Slang shader precompilation"
cmake_build_prebuild_hook() {
  bash "${SCRIPT_DIR}/compile-slang-shaders.sh"
}

cmake_build_main --preset linux-riscv64-cross --build-dir "${BUILD_DIR}" --mb-per-job 4000

# A GPU shard skips the CPU suites, which every push already ran; "all" is the old serial lane, for a local host.
if [[ -z "${RISCV64_GPU_SHARD}" || "${RISCV64_GPU_SHARD}" == all ]]; then
  ctest_run_main --build-dir "${BUILD_DIR}" --build-type Debug --ctest-exclude "${CTEST_GPU_SUITES}" "$@"
fi
if [[ -z "${RISCV64_GPU_SHARD}" ]]; then
  info "[gpu] no RISCV64_GPU_SHARD - the GPU suites run in the weekly shards"
  exit 0
fi

# Absolute paths: ctest_run_main leaves the shell inside ${BUILD_DIR}, and a relative one doubled it once.
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TEST_DIR="${REPO_ROOT}/${BUILD_DIR}"
GPU_SHIM_DIR="${TEST_DIR}/fp16-shim"
test -d "${TEST_DIR}" || { err "[gpu] no ${TEST_DIR} - refusing a silent green"; exit 1; }
mkdir -p "${GPU_SHIM_DIR}"
# riscv64 glibc lacks the fp16 conversion helpers llvmpipe's ORC JIT calls; the LD_PRELOAD shim defines them.
info "[gpu] building the fp16 shim for shard ${RISCV64_GPU_SHARD}"
"${RISCV64_CROSS_BIN}/riscv64-linux-gnu-clang" -shared -fPIC -O2 "${SCRIPT_DIR}/../riscv64/fp16_helpers.c" -o "${GPU_SHIM_DIR}/libkata_fp16_helpers.so"
test -f "${GPU_SHIM_DIR}/libkata_fp16_helpers.so" || { err "[gpu] the shim did not build - refusing an unshimmed run"; exit 1; }
# --no-tests=error: a shard whose filter matches nothing must fail, not pass empty.
xvfb-run -a -s "-screen 0 1920x1080x24" \
  env LD_PRELOAD="${GPU_SHIM_DIR}/libkata_fp16_helpers.so" \
  ctest --test-dir "${TEST_DIR}" -C Debug -j1 "${CTEST_GPU_ARGS[@]}" \
    --no-tests=error --output-on-failure
