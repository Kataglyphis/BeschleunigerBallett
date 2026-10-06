#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
source "${SCRIPT_DIR}/lib/common.sh"

REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
RUST_PROJECT_DIR="${RUST_PROJECT_DIR:-${REPO_ROOT}/third_party/OxidANT}"
# _path, not _source: the driver runs as a child, not sourced.
CARGO_FMT_CLIPPY_SH="$(antfrastructure_path linux/scripts/02-toolchain/rust/cargo_fmt_clippy.sh)"

[[ -d "${RUST_PROJECT_DIR}" ]] || err "Rust project dir not found at ${RUST_PROJECT_DIR} (is the OxidANT submodule checked out?)"

export CARGO_TARGET_DIR="${CARGO_TARGET_DIR:-target}"
# The image's CARGO_HOME is root-owned; pin a writable one so registry writes never race the guard's write-probe.
export CARGO_HOME="${CARGO_HOME:-/tmp/cargo-home}"
mkdir -p "${CARGO_HOME}"

# OxidANT's own gate (its ci-container-steps.sh fmt-clippy), so a pin it passed passes here; --all-features needs GTK4.
export CARGO_CLIPPY_ARGS="${CARGO_CLIPPY_ARGS---workspace --locked --features kataglyphis_media?/gstreamer,kataglyphis_inference?/onnxruntime}"

info "cargo: $(command -v cargo) ($(cargo --version 2>/dev/null || echo 'version unavailable'))"

info "=== Rust crate lints (rustfmt + clippy -D warnings, whole OxidANT workspace) ==="
( cd "${RUST_PROJECT_DIR}" && bash "${CARGO_FMT_CLIPPY_SH}" )
