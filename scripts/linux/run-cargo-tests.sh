#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
source "${SCRIPT_DIR}/lib/common.sh"

# Runs the Rust renderer crate's own test suite (kataglyphis_webgpu_renderer)
# from this repo's CI. Delegates to ANTfrastructure's cargo_test.sh
# (AGENTS.md § "Reusable Work Belongs in ANTfrastructure") rather than
# reimplementing it; the package filter below is just an extra arg forwarded
# to `cargo test --all --verbose`, so no upstream change was needed.
#
# GPU-touching tests in the crate already self-skip when no adapter is
# present (headless.rs, ibl.rs, occlusion.rs, gpu_timing.rs all print
# "SKIP: no GPU adapter available in this environment" and return), so a
# GPU-less CI runner still exercises the CPU-only tests and skips the rest.

REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
RUST_PROJECT_DIR="${RUST_PROJECT_DIR:-${REPO_ROOT}/third_party/OxidANT}"
# antfrastructure_path (from lib/antfrastructure.sh, sourced by lib/common.sh) instead
# of a "${REPO_ROOT}/third_party/ANTfrastructure/..." literal: it resolves against
# ANTFRASTRUCTURE_DIR, which the literal ignored, and it already fails naming the
# probed path AND the fix - so the hand-rolled -f guard that stood here is gone
# rather than duplicated. This driver is EXEC'd, not sourced, hence _path and
# not _source.
CARGO_TEST_SH="$(antfrastructure_path linux/scripts/02-toolchain/rust/cargo_test.sh)"

[[ -d "${RUST_PROJECT_DIR}" ]] || err "Rust project dir not found at ${RUST_PROJECT_DIR} (is the OxidANT submodule checked out?)"

export CARGO_TARGET_DIR="${CARGO_TARGET_DIR:-target}"
# The image ships CARGO_HOME=/usr/local/cargo owned by root, and the
# container runs as uid 1001; cargo_test.sh's own guard falls back to a
# writable dir automatically, but pin one explicitly so a registry write
# never depends on the guard's write-probe racing another cargo step's cache.
# NOTE: _cargo_home_guard.sh has already run via lib/common.sh, so this `:-`
# OVERRIDES the guard's answer rather than filling a blank. Deliberate; see above.
export CARGO_HOME="${CARGO_HOME:-/tmp/cargo-home}"
mkdir -p "${CARGO_HOME}"

# The PATH hoist that used to stand here is gone: lib/common.sh (sourced at the
# top of this file) now sources ANTfrastructure's
# 02-toolchain/rust/_rust_toolchain_guard.sh, which does exactly this and is the
# owner of it. This was the third copy of the same seven lines in this repo.
info "cargo: $(command -v cargo) ($(cargo --version 2>/dev/null || echo 'version unavailable'))"

info "=== Rust renderer test suite (kataglyphis_webgpu_renderer) ==="
( cd "${RUST_PROJECT_DIR}" && bash "${CARGO_TEST_SH}" -p kataglyphis_webgpu_renderer )
