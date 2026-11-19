#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
source "${SCRIPT_DIR}/lib/common.sh"

# The hub's cargo_test.sh runs `cargo test --all`, which overrides a -p: the whole OxidANT workspace, as its own lane.

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

# As in OxidANT's lane: under Xvfb, and a GPU test that finds no adapter fails instead of skipping itself.
export KATAGLYPHIS_REQUIRE_GPU=1

info "=== OxidANT workspace test suite (cargo test --all) ==="
# A child, not exec: xvfb-run as PID 1 omits its readiness signal.
( cd "${RUST_PROJECT_DIR}" && xvfb-run -a bash "${CARGO_TEST_SH}" )
