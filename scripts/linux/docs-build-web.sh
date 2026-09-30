#!/usr/bin/env bash
# docs-build-web.sh - this project's paths and WebGPU demo over ANTfrastructure's lib/docs-build.sh.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
source "${SCRIPT_DIR}/lib/common.sh"
# Not third_party literals: antfrastructure_source honours ANTFRASTRUCTURE_DIR.
antfrastructure_source linux/scripts/lib/rust-toolchain.sh

antfrastructure_source linux/scripts/lib/docs-build.sh

# Directory the C++ build wrote its Doxygen/Graphviz SVGs to.
DOCS_OUT="${1:-${DOCS_OUT:-build/build/html}}"

# Repo-root relative: the library resolves them against DOCS_BUILD_PROJECT_ROOT (cwd by default).
DOCS_BUILD_SVG_SOURCE_DIR="${DOCS_OUT}"
DOCS_BUILD_GENERATOR_SCRIPT="graphviz_generator.py"
DOCS_BUILD_UV_VENV_CREATE_SCRIPT="${SCRIPT_DIR}/lib/uv-venv-create.sh"
DOCS_BUILD_UV_INSTALL_REQUIREMENTS_SCRIPT="${SCRIPT_DIR}/lib/uv-install-requirements.sh"

# Best-effort refresh of the committed demo; wasm-bindgen-cli must match Cargo.lock exactly or it refuses to run.
build_webgpu_wasm_demo() {
    local rpt="third_party/OxidANT"
    local demo="docs/source/_webgpu_demo/webgpu-demo"
    local wb_ver
    wb_ver="$(grep -A1 '^name = "wasm-bindgen"$' "${rpt}/Cargo.lock" | grep '^version' | head -1 | sed -E 's/version = "(.*)"/\1/')"
    [ -n "${wb_ver}" ] || return 1
    ensure_wasm32_target &&
    { { command -v wasm-bindgen >/dev/null 2>&1 \
        && [ "$(wasm-bindgen --version 2>/dev/null | awk '{print $2}')" = "${wb_ver}" ]; } \
        || cargo install --locked wasm-bindgen-cli --version "${wb_ver}"; } &&
    ( cd "${rpt}" && CARGO_TARGET_DIR="target" \
        cargo build -p kataglyphis_webgpu_renderer --target wasm32-unknown-unknown --release ) &&
    wasm-bindgen "${rpt}/target/wasm32-unknown-unknown/release/kataglyphis_webgpu_renderer.wasm" \
        --out-dir "${rpt}/crates/webgpu_renderer/web/pkg" --target web &&
    cp "${rpt}/crates/webgpu_renderer/web/index.html" "${demo}/index.html" &&
    cp "${rpt}/crates/webgpu_renderer/web/pkg/kataglyphis_webgpu_renderer.js" \
        "${demo}/pkg/kataglyphis_webgpu_renderer.js" &&
    cp "${rpt}/crates/webgpu_renderer/web/pkg/kataglyphis_webgpu_renderer_bg.wasm" \
        "${demo}/pkg/kataglyphis_webgpu_renderer_bg.wasm"
}

info "Rebuilding the WebGPU WASM browser demo from the current renderer"
if build_webgpu_wasm_demo; then
    info "WebGPU WASM demo rebuilt from current source"
else
    info "WebGPU WASM demo rebuild skipped/failed; keeping the committed snapshot"
fi

docs_build_main
