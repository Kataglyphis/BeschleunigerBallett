#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
source "${SCRIPT_DIR}/lib/common.sh"
# Not third_party literals: antfrastructure_source honours ANTFRASTRUCTURE_DIR.
antfrastructure_source linux/scripts/lib/rust-toolchain.sh


# Catches wasm bloat before the demo deploys; twin of scripts/windows/Test-WasmSizeBudget.ps1.
antfrastructure_source linux/scripts/lib/wasm-opt.sh

REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
RUST_PROJECT_DIR="${RUST_PROJECT_DIR:-${REPO_ROOT}/third_party/OxidANT}"
WASM_FILE="${RUST_PROJECT_DIR}/target/wasm32-unknown-unknown/release/kataglyphis_webgpu_renderer.wasm"
BUDGET_BYTES="${WASM_SIZE_BUDGET_BYTES:-12582912}" # 12 MiB

info "=== Wasm Size Budget Test ==="
info "Budget: ${BUDGET_BYTES} bytes ($(( BUDGET_BYTES / 1024 / 1024 )) MiB)"

info "Ensuring wasm32-unknown-unknown target is installed"
# No wasm target is an environment gap, not a size regression: skip with a warning that never reads as a pass.
if ! ensure_wasm32_target; then
  echo "::warning::Wasm size budget SKIPPED - this toolchain cannot build wasm32-unknown-unknown (no rustup, no wasm32 std in the image). Nothing was weighed; the committed demo snapshot is unchanged. Fix ANTfrastructure's install-rust.sh to restore the target."
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
# Bootstraps pinned binaryen if needed and adds the feature flags wgpu/naga codegen needs.
wasm_opt_optimize "${WASM_FILE}" "${OPT_FILE}" -Oz
mv -f "${OPT_FILE}" "${WASM_FILE}"

FINAL_SIZE=$(stat -c%s "${WASM_FILE}")
info "Post-opt size: ${FINAL_SIZE} bytes (saved $(( PRE_SIZE - FINAL_SIZE )) bytes)"

if (( FINAL_SIZE > BUDGET_BYTES )); then
  err "FAIL: wasm size ${FINAL_SIZE} bytes exceeds budget of ${BUDGET_BYTES} bytes ($(( FINAL_SIZE - BUDGET_BYTES )) bytes over). Consider feature flags, LTO, dead-code elimination, or raising WASM_SIZE_BUDGET_BYTES deliberately."
fi

info "PASS: wasm size ${FINAL_SIZE} bytes is within budget ($(( BUDGET_BYTES - FINAL_SIZE )) bytes to spare)."
