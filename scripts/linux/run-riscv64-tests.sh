#!/usr/bin/env bash
# run-riscv64-tests.sh - the riscv64 lane: Debug cross build on the amd64 image, ctest under QEMU; CPU suites per push, one GPU shard per weekly job (owner 2026-10-06).
set -euo pipefail

# Under QEMU the path-traced tests cost 32-200x their x64 time and the ray-traced one 17x, so each is a weekly job of its own.
RISCV64_GPU_SOLO_TESTS=(
  PathTracingAccumulatesAndConverges
  PathTracingAntiAliasesGeometricEdges
  PathTracingRespondsToTheDirectionalLight
  PathTracingHonorsTheQualityControls
  PathTracingHonorsTheUntexturedMetallicFactor
  PathTracingPassesTheWhiteFurnaceTest
  PathTracingPassesTheWhiteMetalFurnaceTest
  PathTracingFloorsAZeroRoughnessTexel
  PathTracedMetalCardReflectsTheSkyAndTheSun
  PathTracedMaskCardShowsItsCutout
  AddedModelAppearsInPathTracing
  ReloadedModelIsVisibleInPathTracing
  RaytracingFrameSkipsTheRasterPass
)
# The clouds cost 22-28x, too much for the raster shard's 90 min a test.
RISCV64_GPU_CLOUD_TESTS='CloudsAcrossManyFramesDoesNotLoseTheDevice|EnablingCloudsChangesTheFrameAndAddsDetail'
RISCV64_GPU_CLOUD_TESTS+='|CloudOffsetMovesTheCloudVolume|CloudsStopAtTheSceneDepth|CloudsStopAtTracedGeometry'
RISCV64_GPU_CLOUD_TESTS+='|CloudsCompositeOverTheSkyAsOverGeometry|CloudsInFrontOfGeometryCoverItInEveryMode'
RISCV64_GPU_CLOUD_TESTS+='|CloudsAreLitByTheSkyTheyDraw|CloudWindMovesTheNoiseAndRepeatsWithItsPeriod'
RISCV64_GPU_CLOUD_TESTS+='|HalfResolutionCloudsKeepOffTheSlabAndCloseToTheFullMarch'
RISCV64_GPU_SWEEP_TEST='GuiInputSweepNeverCrashesOrLosesTheDevice'
# The rest splits by ctest number, every third test a shard: as one, run 45969392334 hit the 6 h job limit at 64 of 95.
RISCV64_GPU_RASTER_SHARDS=3

# The weekly matrix reads its shards here, before any hub library loads, so the list has one home.
if [[ "${1:-}" == "--list-gpu-shards" ]]; then
  shards=()
  for ((part = 1; part <= RISCV64_GPU_RASTER_SHARDS; part++)); do shards+=("raster-${part}"); done
  shards+=(clouds "${RISCV64_GPU_SOLO_TESTS[@]}")
  # A dispatch may name a subset (comma-separated); a name off the list fails before any job starts.
  if [[ -n "${2:-}" ]]; then
    IFS=',' read -r -a wanted <<<"${2// /}"
    for name in "${wanted[@]}"; do
      [[ " ${shards[*]} " == *" ${name} "* ]] || { echo "unknown GPU shard '${name}'" >&2; exit 2; }
    done
    shards=("${wanted[@]}")
  fi
  json="$(printf '"%s",' "${shards[@]}")"
  printf '[%s]\n' "${json%,}"
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

# The GPU step below re-runs the script here, under Xvfb and the fp16 shim, so the hub fails a skipped GPU test.
if [[ "${1:-}" == "--gpu-ctest" ]]; then
  shift
  # riscv64_cross_env's Vulkan paths arrive in the environment; the x86_64 setup-env would undo them.
  source_vulkan_env() { return 0; }
  gpu_rc=0
  ctest_run_main --fail-on-skipped "${CTEST_GPU_SUITES}" "$@" || gpu_rc=$?
  exit "${gpu_rc}"
fi

# Ceilings leave a solo test 300 of the job's 360 min after the 23-33 min cross build; the measured times are in AGENTS.md.
riscv64_gpu_shard_args() {
  local shard="$1" solo_tests part
  solo_tests="$(IFS='|'; printf '%s' "${RISCV64_GPU_SOLO_TESTS[*]}")"
  case "${shard}" in
    all) CTEST_GPU_ARGS=(-R "${CTEST_GPU_SUITES}" -E "^GoldenRender\.${RISCV64_GPU_SWEEP_TEST}\$" --timeout 7200) ;;
    raster-*)
      part="${shard#raster-}"
      [[ "${part}" =~ ^[1-9][0-9]*$ ]] && ((part <= RISCV64_GPU_RASTER_SHARDS)) \
        || { err "unknown RISCV64_GPU_SHARD '${shard}'; see --list-gpu-shards"; exit 2; }
      # -I intersects with -R and -E, so the parts cover the filtered tests once each.
      CTEST_GPU_ARGS=(-R "${CTEST_GPU_SUITES}"
        -E "^GoldenRender\.(${solo_tests}|${RISCV64_GPU_CLOUD_TESTS}|${RISCV64_GPU_SWEEP_TEST})\$"
        -I "${part},,${RISCV64_GPU_RASTER_SHARDS}" --timeout 5400)
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
# The full path-traced histories outrun 300 min on a slow runner; docs/source/gpu-golden-testing.md § The riscv64 light variant.
export KATAGLYPHIS_PT_GOLDEN_VARIANT="${KATAGLYPHIS_PT_GOLDEN_VARIANT:-light}"
info "[gpu] path-traced goldens: KATAGLYPHIS_PT_GOLDEN_VARIANT=${KATAGLYPHIS_PT_GOLDEN_VARIANT}"
# A GPU test that would skip fails instead (Test/commit/VulkanEngine/GpuTestGuard.hpp), so a broken Xvfb cannot pass.
export KATAGLYPHIS_REQUIRE_GPU=1
# --no-tests=error: a shard whose filter matches nothing must fail, not pass empty.
xvfb-run -a -s "-screen 0 1920x1080x24 -noreset" \
  env LD_PRELOAD="${GPU_SHIM_DIR}/libkata_fp16_helpers.so" \
  bash "${BASH_SOURCE[0]}" --gpu-ctest --build-dir "${TEST_DIR}" --build-type Debug -- \
    -j1 "${CTEST_GPU_ARGS[@]}" --no-tests=error
