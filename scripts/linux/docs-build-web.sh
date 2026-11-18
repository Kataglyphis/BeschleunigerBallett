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
DOCS_BUILD_UV_INSTALL_REQUIREMENTS_SCRIPT="${SCRIPT_DIR}/lib/uv-sync-project.sh"

# Builds the demo the docs publish (nothing tracks it); wasm-bindgen-cli must match Cargo.lock exactly or it refuses to run.
build_webgpu_wasm_demo() {
    local rpt="third_party/OxidANT"
    local demo="docs/source/_webgpu_demo/webgpu-demo"
    [ -n "$(wasm_bindgen_lock_version "${rpt}/Cargo.lock" 2>/dev/null)" ] ||
        err "No wasm-bindgen version in ${rpt}/Cargo.lock: wasm-bindgen-cli cannot be pinned, so the WebGPU demo cannot be built"
    ensure_wasm32_target &&
    ensure_wasm_bindgen "${rpt}/Cargo.lock" &&
    ( cd "${rpt}" && CARGO_TARGET_DIR="target" \
        cargo build -p kataglyphis_webgpu_renderer --target wasm32-unknown-unknown --release ) &&
    wasm-bindgen "${rpt}/target/wasm32-unknown-unknown/release/kataglyphis_webgpu_renderer.wasm" \
        --out-dir "${rpt}/crates/webgpu_renderer/web/pkg" --target web &&
    mkdir -p "${demo}/pkg" &&
    cp "${rpt}/crates/webgpu_renderer/web/index.html" "${demo}/index.html" &&
    cp "${rpt}/crates/webgpu_renderer/web/pkg/kataglyphis_webgpu_renderer.js" \
        "${demo}/pkg/kataglyphis_webgpu_renderer.js" &&
    cp "${rpt}/crates/webgpu_renderer/web/pkg/kataglyphis_webgpu_renderer_bg.wasm" \
        "${demo}/pkg/kataglyphis_webgpu_renderer_bg.wasm"
}

# The editor's embedded fonts ship in the wasm, so their notices sit beside it.
stage_webgpu_demo_font_notices() {
    local demo="docs/source/_webgpu_demo/webgpu-demo"
    local fonts="third_party/OxidANT/crates/webgpu_renderer/assets/fonts"
    mkdir -p "${demo}/fonts" || return 1
    cp "${fonts}/NOTICE" "${fonts}/Apache-2.0.txt" "${fonts}/OFL-1.1.txt" "${demo}/fonts/"
}

# The wasm is the renderer's whole crate graph for its target; the hub stager fails on a crate OxidANT's policy has not reviewed.
stage_webgpu_demo_crate_licenses() {
    local rpt="third_party/OxidANT"
    local out="docs/source/_webgpu_demo/webgpu-demo/licenses/cargo"
    rm -rf "${out}" || return 1
    # Every platform's crates, as cmake/CargoLicenses.cmake fetches them: the stager reads Cargo's metadata offline.
    cargo fetch --locked --manifest-path "${rpt}/Cargo.toml" || return 1
    python3 "$(antfrastructure_path linux/scripts/06-packaging/stage-cargo-licenses.py)" \
        --workspace "${rpt}" --root-package kataglyphis_webgpu_renderer --target wasm32-unknown-unknown \
        --policy "${rpt}/packaging/cargo-license-policy.json" --output-dir "${out}"
}

# The demo's Colosseum row fetches Colosseum-web.glb beside the page; the release archive carries its notices.
stage_webgpu_demo_colosseum() {
    local demo="docs/source/_webgpu_demo/webgpu-demo"
    local scene="Resources/Models/Colosseum-web"
    rm -rf "${demo}/Colosseum-web" "${demo}/Colosseum-web.glb" || return 1
    python3 -I "$(antfrastructure_path linux/scripts/lib/fetch-data-lock.py)" \
        --lock Resources/assets.lock --dest Resources/Models --extract --only Colosseum-web || return 1
    mkdir -p "${demo}/Colosseum-web" || return 1
    cp "${scene}/Colosseum-web.glb" "${demo}/Colosseum-web.glb" || return 1
    cp -R "${scene}/ATTRIBUTION.txt" "${scene}/LICENSES" "${demo}/Colosseum-web/"
}

# The demo fetches the C++ engine's CC0 sky from a folder beside the page; the archive carries its notices.
stage_webgpu_demo_skybox() {
    local demo="docs/source/_webgpu_demo/webgpu-demo"
    local name="kloofendal_43d_clear_puresky"
    local sky="Resources/Textures/Skybox/${name}"
    rm -rf "${demo:?}/${name}" || return 1
    python3 -I "$(antfrastructure_path linux/scripts/lib/fetch-data-lock.py)" \
        --lock Resources/skyboxes.lock --dest Resources/Textures/Skybox --extract --only "${name}" || return 1
    mkdir -p "${demo}/${name}" || return 1
    cp "${sky}"/{RT,LF,UP,DN,FT,BK}.png "${sky}/ATTRIBUTION.txt" "${demo}/${name}/" || return 1
    cp -R "${sky}/LICENSES" "${demo}/${name}/"
}

info "Building the WebGPU WASM browser demo from the current renderer"
# html_extra_path must exist under -W, demo or not.
mkdir -p docs/source/_webgpu_demo
if build_webgpu_wasm_demo && stage_webgpu_demo_font_notices && stage_webgpu_demo_crate_licenses; then
    info "WebGPU WASM demo built from current source"
    if stage_webgpu_demo_colosseum; then
        info "Colosseum-web staged beside the WebGPU demo"
    elif [ -n "${GITHUB_ACTIONS:-}" ]; then
        echo "::warning::Colosseum-web could not be fetched; the demo publishes without its Colosseum row's scene"
    else
        warn "Colosseum-web could not be fetched; the demo builds without its Colosseum row's scene"
    fi
    if stage_webgpu_demo_skybox; then
        info "The skybox staged beside the WebGPU demo"
    elif [ -n "${GITHUB_ACTIONS:-}" ]; then
        echo "::warning::The skybox could not be fetched; the demo publishes with its procedural sky"
    else
        warn "The skybox could not be fetched; the demo builds with its procedural sky"
    fi
else
    # A wasm without its notices must not publish.
    rm -rf docs/source/_webgpu_demo/webgpu-demo
    if [ -n "${GITHUB_ACTIONS:-}" ]; then
        echo "::warning::WebGPU WASM demo build or its notices failed; the docs publish without it"
    else
        info "WebGPU WASM demo build or its notices skipped/failed; the docs build without it"
    fi
fi

docs_build_main
