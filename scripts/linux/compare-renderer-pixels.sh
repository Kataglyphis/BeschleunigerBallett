#!/usr/bin/env bash
# compare-renderer-pixels.sh - Compare-RendererPixels.ps1's Linux twin: both renderers draw Dinosaurs, one verdict grades them.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
source "${SCRIPT_DIR}/lib/common.sh"

REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
RUST_PROJECT_DIR="${RUST_PROJECT_DIR:-${REPO_ROOT}/third_party/OxidANT}"
BUILD_DIR="build"
OUT_DIR="logs/renderer-pixels"
# The golden harness's window, so both frames have one size.
WIDTH=1200
HEIGHT=768
skip_cpp=0
skip_rust=0
require_frames=0

usage() {
  cat <<'EOF'
Usage: compare-renderer-pixels.sh [--build-dir DIR] [--out-dir DIR] [--skip-cpp] [--skip-rust] [--require-frames]

Renders Dinosaurs with the C++ golden harness (GoldenRender.RendersNonBlankFrame, through run-ctest.sh
--virtual-display) and with OxidANT's headless_render, then grades both PNGs with compareRendererFrames:
GoldenMetrics.hpp's structural verdict. A skipped renderer's frame already in --out-dir is graded as found.

Exit codes, as Compare-RendererPixels.ps1's: 0 passed, 1 a check or a capture failed, 2 no frame at all (never a pass).
--require-frames: a Vulkan device is there (CI's llvmpipe), so a missing frame fails, as the Windows lane fails
exit 2 on a host with a GPU.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir) BUILD_DIR="${2:?--build-dir needs a value}"; shift 2 ;;
    --out-dir) OUT_DIR="${2:?--out-dir needs a value}"; shift 2 ;;
    --skip-cpp) skip_cpp=1; shift ;;
    --skip-rust) skip_rust=1; shift ;;
    --require-frames) require_frames=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) err "Unknown argument: $1 (see --help)" ;;
  esac
done

# Absolute: the captures run from the repo root and from OxidANT's directory.
[[ "${BUILD_DIR}" == /* ]] || BUILD_DIR="${REPO_ROOT}/${BUILD_DIR}"
[[ "${OUT_DIR}" == /* ]] || OUT_DIR="${REPO_ROOT}/${OUT_DIR}"
mkdir -p "${OUT_DIR}"
cpp_png="${OUT_DIR}/cpp-vulkan.png"
rust_png="${OUT_DIR}/rust-webgpu.png"

grader="${BUILD_DIR}/compareRendererFrames"
[[ -x "${grader}" ]] || err "compareRendererFrames not found at ${grader}; build a Debug tree first."

# run-ctest.sh's GPU run: lavapipe and KATAGLYPHIS_REQUIRE_GPU, and a skip, a SYNC-HAZARD or a VUID fails it.
capture_cpp() {
  info "== C++/Vulkan (golden harness, RendersNonBlankFrame) =="
  # A coverage build's profile goes beside the frames, not into the repo root the suite runs from.
  KATAGLYPHIS_GRADED_FRAME_PNG="${cpp_png}" LLVM_PROFILE_FILE="${OUT_DIR}/cpp-%p.profraw" \
    bash "${SCRIPT_DIR}/run-ctest.sh" --virtual-display --build-dir "${BUILD_DIR}" \
    -- -R '^GoldenRender\.RendersNonBlankFrame$' --no-tests=error
}

capture_rust() {
  local obj="${REPO_ROOT}/Resources/Models/Dinosaurs/dinosaurs.obj"
  local gltf="${OUT_DIR}/dinosaurs.gltf"
  info "== Rust/WebGPU (headless_render, Dinosaurs, ${WIDTH}x${HEIGHT}) =="
  [[ -f "${obj}" ]] || { warn "${obj} is missing (a configure fetches it); no Rust frame."; return 0; }
  # run-cargo-tests.sh's target, which the bind mount keeps; a new container's /tmp home downloads the crates again.
  export CARGO_TARGET_DIR="${CARGO_TARGET_DIR:-target}"
  export CARGO_HOME="${CARGO_HOME:-/tmp/cargo-home}"
  mkdir -p "${CARGO_HOME}"
  # The device the hub's cargo_test.sh gives OxidANT's own tests: lavapipe alone where no GPU node exists.
  vulkan_env_default_cpu_driver
  local -a vector_width=()
  # aarch64: 256 bits draw OxidANT's frame as noise, and Mesa's cache, keyed on no width, hands 128 the golden's 256.
  if [[ "$(uname -m)" == aarch64 ]]; then vector_width=(LP_NATIVE_VECTOR_WIDTH=128 MESA_SHADER_CACHE_DISABLE=true); fi
  # --workspace: cargo test --all's feature set, so its build of the examples is reused, not redone for one package.
  ( cd "${RUST_PROJECT_DIR}" \
      && cargo build --locked --quiet --workspace --example obj2gltf --example headless_render \
      && "${CARGO_TARGET_DIR}/debug/examples/obj2gltf" "${obj}" "${gltf}" \
      && env "${vector_width[@]}" "${CARGO_TARGET_DIR}/debug/examples/headless_render" \
        "${gltf}" "${rust_png}" "${WIDTH}" "${HEIGHT}" )
}

# A renderer that crashes or fails its own golden failed a check; only a capture never attempted checks nothing.
failures=()
# A frame an earlier run left would be graded as this one's.
if (( ! skip_cpp )); then
  rm -f "${cpp_png}"
  SECONDS=0
  rc=0
  capture_cpp || rc=$?
  info "C++ capture: ${SECONDS} s"
  (( rc == 0 )) || failures+=("the C++ capture exited ${rc}")
fi
if (( ! skip_rust )); then
  rm -f "${rust_png}"
  SECONDS=0
  rc=0
  capture_rust || rc=$?
  info "Rust capture: ${SECONDS} s"
  (( rc == 0 )) || failures+=("the Rust capture exited ${rc}")
fi
if (( require_frames )); then
  [[ -f "${cpp_png}" ]] || failures+=("the C++ renderer left no frame, which --require-frames forbids")
  [[ -f "${rust_png}" ]] || failures+=("the Rust renderer left no frame, which --require-frames forbids")
fi

rc=0
"${grader}" "${cpp_png}" "${rust_png}" || rc=$?
(( ${#failures[@]} > 0 )) || exit "${rc}"
for failure in "${failures[@]}"; do
  # As the hub's gates annotate, from inside the CI container too.
  if [[ "${GITHUB_ACTIONS:-}" == true || "${GATES_GITHUB_ANNOTATIONS:-}" == 1 ]]; then
    echo "::error title=compare-renderer-pixels.sh::${failure}"
  fi
  echo "FAIL ${failure}."
done
echo "=== RENDERER COMPARISON FAILED (exit code 1) ==="
exit 1
