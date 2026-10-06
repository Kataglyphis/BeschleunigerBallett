#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
source "${SCRIPT_DIR}/lib/common.sh"

# The crate's GPU tests self-skip without an adapter, so a GPU-less runner still runs the CPU-only ones.

REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
RUST_PROJECT_DIR="${RUST_PROJECT_DIR:-${REPO_ROOT}/third_party/OxidANT}"
# _path, not _source: the driver runs as a child, not sourced.
CARGO_TEST_SH="$(antfrastructure_path linux/scripts/02-toolchain/rust/cargo_test.sh)"

[[ -d "${RUST_PROJECT_DIR}" ]] || err "Rust project dir not found at ${RUST_PROJECT_DIR} (is the OxidANT submodule checked out?)"

export CARGO_TARGET_DIR="${CARGO_TARGET_DIR:-target}"
# The image's CARGO_HOME is root-owned; pin a writable one so registry writes never race the guard's write-probe.
export CARGO_HOME="${CARGO_HOME:-/tmp/cargo-home}"
mkdir -p "${CARGO_HOME}"

# Not this repo's C++ goldens: OxidANT needs no 256-bit BVH sort (hub CON44), on which LLVM's AArch64 JIT aborts.
if [[ "$(uname -m)" == aarch64 ]]; then export LP_NATIVE_VECTOR_WIDTH=128; fi

info "cargo: $(command -v cargo) ($(cargo --version 2>/dev/null || echo 'version unavailable'))"

info "=== Rust renderer test suite (kataglyphis_webgpu_renderer) ==="
( cd "${RUST_PROJECT_DIR}" && bash "${CARGO_TEST_SH}" -p kataglyphis_webgpu_renderer )
