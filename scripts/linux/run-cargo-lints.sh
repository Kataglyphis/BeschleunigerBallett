#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
source "${SCRIPT_DIR}/lib/common.sh"

# Runs the Rust renderer crate's own fmt/clippy checks (kataglyphis_webgpu_renderer)
# from this repo's CI. Delegates to ContainerHub's cargo_fmt_clippy.sh
# (AGENTS.md § "Reusable Work Belongs in ContainerHub") rather than
# reimplementing it, the same way run-cargo-tests.sh delegates to
# cargo_test.sh. Run workspace-wide with no `-p` filter: `cargo fmt --all -p
# <crate>` is a conflicting-arguments error, and workspace-wide is exactly
# what the submodule's own CI runs (rust_ubuntu26_04.yml).

REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
RUST_PROJECT_DIR="${RUST_PROJECT_DIR:-${REPO_ROOT}/third_party/OxidANT}"
# containerhub_path (from lib/containerhub.sh, sourced by lib/common.sh) instead
# of a "${REPO_ROOT}/third_party/ContainerHub/..." literal: it resolves against
# CONTAINERHUB_DIR, which the literal ignored, and it already fails naming the
# probed path AND the fix - so the hand-rolled -f guard that stood here is gone
# rather than duplicated. This driver is EXEC'd, not sourced, hence _path and
# not _source.
CARGO_FMT_CLIPPY_SH="$(containerhub_path linux/scripts/02-toolchain/rust/cargo_fmt_clippy.sh)"

[[ -d "${RUST_PROJECT_DIR}" ]] || err "Rust project dir not found at ${RUST_PROJECT_DIR} (is the OxidANT submodule checked out?)"

export CARGO_TARGET_DIR="${CARGO_TARGET_DIR:-target}"
# The image ships CARGO_HOME=/usr/local/cargo owned by root, and the
# container runs as uid 1001; pin a writable dir explicitly (same fallback
# run-cargo-tests.sh uses) so a registry write never depends on a write-probe
# racing another cargo step's cache.
export CARGO_HOME="${CARGO_HOME:-/tmp/cargo-home}"
mkdir -p "${CARGO_HOME}"

info "=== Rust renderer fmt/clippy checks (kataglyphis_webgpu_renderer) ==="
( cd "${RUST_PROJECT_DIR}" && bash "${CARGO_FMT_CLIPPY_SH}" )
