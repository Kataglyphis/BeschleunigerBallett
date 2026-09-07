#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
source "${SCRIPT_DIR}/lib/common.sh"
# lib/common.sh sources lib/containerhub.sh, so containerhub_source is already
# defined. It resolves against CONTAINERHUB_DIR - which the hand-rolled
# "${SCRIPT_DIR}/../../third_party/ContainerHub/..." literals these calls
# replace could not honour - and fails naming the probed path AND the fix.
#
# ensure_wasm32_target lives in ContainerHub: making the wasm32 target usable
# without assuming rustup is not this project's problem, it is a property of
# the images.
containerhub_source linux/scripts/lib/rust-toolchain.sh


# Builds kataglyphis_webgpu_renderer for wasm32-unknown-unknown, optimises with
# wasm-opt -Oz, and fails the step if the result exceeds the size budget - so a
# bloat regression is caught here, not discovered after the demo is already
# deployed by "Sync files to domain". Mirrors scripts/Test-WasmSizeBudget.ps1
# (the local/Windows equivalent), adapted to the container's bash + no
# guaranteed wasm-opt on PATH.
#
# Budget default measured 2026-07-31 straight off `cargo build --release` +
# wasm-opt -Oz on this host: pre-opt 9,873,819 bytes, post-opt 8,730,038 bytes.
# The ~3.7 MB figure previously carried in BACKLOG.md/docs was stale - nothing
# had ever measured or enforced it. 12 MiB gives ~1.8 MiB of headroom above the
# measured figure without hiding a real regression.

# binaryen bootstrap (pinned + SHA-verified against versions.env) and the
# wasm-opt feature flags come from ContainerHub's generic driver; only the
# budget and the crate below are this project's data. The driver's PowerShell
# twin backs scripts/Test-WasmSizeBudget.ps1.
containerhub_source linux/scripts/lib/wasm-opt.sh

REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
RUST_PROJECT_DIR="${RUST_PROJECT_DIR:-${REPO_ROOT}/third_party/OxidANT}"
WASM_FILE="${RUST_PROJECT_DIR}/target/wasm32-unknown-unknown/release/kataglyphis_webgpu_renderer.wasm"
BUDGET_BYTES="${WASM_SIZE_BUDGET_BYTES:-12582912}" # 12 MiB

info "=== Wasm Size Budget Test ==="
info "Budget: ${BUDGET_BYTES} bytes ($(( BUDGET_BYTES / 1024 / 1024 )) MiB)"

info "Ensuring wasm32-unknown-unknown target is installed"
# Not `rustup target add` directly: the CI image has no rustup, so that exited
# 127 and `set -e` failed this step before it measured anything (2026-08-06).
#
# A toolchain that cannot target wasm at all is an ENVIRONMENT gap, not a size
# regression, and this gate exists to catch size regressions. Failing the lane
# for it would say "the demo got too big" when the truth is "nothing was
# weighed" - the same trap the pixel-comparison step fell into. So skip, but
# say so as a GitHub ::warning:: that names the cause: a skipped budget must
# never read as a budget that passed. The deployed demo falls back to the
# committed snapshot in docs/source/_webgpu_demo, exactly as it does when the
# docs step's best-effort rebuild is skipped.
if ! ensure_wasm32_target; then
  echo "::warning::Wasm size budget SKIPPED - this toolchain cannot build wasm32-unknown-unknown (no rustup, no wasm32 std in the image). Nothing was weighed; the committed demo snapshot is unchanged. Fix ContainerHub's install-rust.sh to restore the target."
  info "=== Wasm Size Budget Test: SKIPPED (no wasm32 toolchain) ==="
  exit 0
fi

info "Building kataglyphis_webgpu_renderer for wasm32-unknown-unknown (release)"
( cd "${RUST_PROJECT_DIR}" && CARGO_TARGET_DIR="target" \
    cargo build -p kataglyphis_webgpu_renderer --target wasm32-unknown-unknown --release )

[[ -f "${WASM_FILE}" ]] || err "Wasm file not found at ${WASM_FILE}"

PRE_SIZE=$(stat -c%s "${WASM_FILE}")
info "Pre-opt size: ${PRE_SIZE} bytes"

info "Running wasm-opt -Oz"
OPT_FILE="${WASM_FILE%.wasm}.opt.wasm"
# Bootstraps the pinned binaryen release (SHA-verified against versions.env)
# when wasm-opt is not already on PATH, then optimises with the wasm feature
# flags wgpu/naga codegen needs - see wasm-opt.sh for both.
wasm_opt_optimize "${WASM_FILE}" "${OPT_FILE}" -Oz
mv -f "${OPT_FILE}" "${WASM_FILE}"

FINAL_SIZE=$(stat -c%s "${WASM_FILE}")
info "Post-opt size: ${FINAL_SIZE} bytes (saved $(( PRE_SIZE - FINAL_SIZE )) bytes)"

if (( FINAL_SIZE > BUDGET_BYTES )); then
  err "FAIL: wasm size ${FINAL_SIZE} bytes exceeds budget of ${BUDGET_BYTES} bytes ($(( FINAL_SIZE - BUDGET_BYTES )) bytes over). Consider feature flags, LTO, dead-code elimination, or raising WASM_SIZE_BUDGET_BYTES deliberately."
fi

info "PASS: wasm size ${FINAL_SIZE} bytes is within budget ($(( BUDGET_BYTES - FINAL_SIZE )) bytes to spare)."
