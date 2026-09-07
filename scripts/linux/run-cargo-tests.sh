#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
source "${SCRIPT_DIR}/lib/common.sh"

# Runs the Rust renderer crate's own test suite (kataglyphis_webgpu_renderer)
# from this repo's CI. Delegates to ContainerHub's cargo_test.sh
# (AGENTS.md § "Reusable Work Belongs in ContainerHub") rather than
# reimplementing it; the package filter below is just an extra arg forwarded
# to `cargo test --all --verbose`, so no upstream change was needed.
#
# GPU-touching tests in the crate already self-skip when no adapter is
# present (headless.rs, ibl.rs, occlusion.rs, gpu_timing.rs all print
# "SKIP: no GPU adapter available in this environment" and return), so a
# GPU-less CI runner still exercises the CPU-only tests and skips the rest.

REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
RUST_PROJECT_DIR="${RUST_PROJECT_DIR:-${REPO_ROOT}/third_party/OxidANT}"
# containerhub_path (from lib/containerhub.sh, sourced by lib/common.sh) instead
# of a "${REPO_ROOT}/third_party/ContainerHub/..." literal: it resolves against
# CONTAINERHUB_DIR, which the literal ignored, and it already fails naming the
# probed path AND the fix - so the hand-rolled -f guard that stood here is gone
# rather than duplicated. This driver is EXEC'd, not sourced, hence _path and
# not _source.
CARGO_TEST_SH="$(containerhub_path linux/scripts/02-toolchain/rust/cargo_test.sh)"

[[ -d "${RUST_PROJECT_DIR}" ]] || err "Rust project dir not found at ${RUST_PROJECT_DIR} (is the OxidANT submodule checked out?)"

export CARGO_TARGET_DIR="${CARGO_TARGET_DIR:-target}"
# The image ships CARGO_HOME=/usr/local/cargo owned by root, and the
# container runs as uid 1001; cargo_test.sh's own guard falls back to a
# writable dir automatically, but pin one explicitly so a registry write
# never depends on the guard's write-probe racing another cargo step's cache.
export CARGO_HOME="${CARGO_HOME:-/tmp/cargo-home}"
mkdir -p "${CARGO_HOME}"

# The image carries TWO Rusts: the PINNED rustup toolchain in
# /usr/local/cargo/bin, and Ubuntu's `cargo` deb at /usr/bin. The image's own
# ENV puts /bin ahead of /usr/local/cargo/bin, and the ordering is only
# corrected by /etc/profile.d/10-rust.sh - which a LOGIN shell sources and this
# script, invoked as `bash run-cargo-tests.sh`, does not. So the deb won here
# while the sibling lanes (which use `bash -lc`) got the pinned toolchain, and
# the suite failed against a Rust two minors older than every other lane:
#   error: rustc 1.93.1 is not supported by the following packages:
#     egui@0.36.1 requires rustc 1.95  (and seven more)
# HOIST it, do not merely add it. The image's ENV already contains
# /usr/local/cargo/bin — just far too late, after /bin — so an
# "add it if missing" guard is a no-op and the deb still wins:
#   [INFO] cargo: /bin/cargo (cargo 1.93.1 ...)
# Drop any existing occurrence, then put it first.
if [[ -x /usr/local/cargo/bin/cargo ]]; then
  _cargo_path=":${PATH}:"
  _cargo_path="${_cargo_path//:\/usr\/local\/cargo\/bin:/:}"
  _cargo_path="${_cargo_path#:}"
  _cargo_path="${_cargo_path%:}"
  export PATH="/usr/local/cargo/bin:${_cargo_path}"
  unset _cargo_path
fi
info "cargo: $(command -v cargo) ($(cargo --version 2>/dev/null || echo 'version unavailable'))"

info "=== Rust renderer test suite (kataglyphis_webgpu_renderer) ==="
( cd "${RUST_PROJECT_DIR}" && bash "${CARGO_TEST_SH}" -p kataglyphis_webgpu_renderer )
