#!/usr/bin/env bash
# Slang to SPIR-V and WGSL: this project's paths over ANTfrastructure's lib/slang-compile.sh (twin of Build-SlangShaders.ps1).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
source "${SCRIPT_DIR}/lib/common.sh"

REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SLANG_ROOT="${REPO_ROOT}/Resources/ShadersSlang"
BUILD_ROOT="${SLANG_ROOT}/build"

# Not a third_party literal: antfrastructure_source honours ANTFRASTRUCTURE_DIR.
antfrastructure_source linux/scripts/lib/slang-compile.sh

# Paths only - the driver holds the behaviour.
SLANG_COMPILE_MANIFEST="${SLANG_ROOT}/shader-manifest.json"
SLANG_COMPILE_SOURCE_ROOT="${SLANG_ROOT}"
SLANG_COMPILE_SPIRV_OUTPUT_ROOT="${BUILD_ROOT}/spirv"
SLANG_COMPILE_WGSL_OUTPUT_ROOT="${BUILD_ROOT}/wgsl"
SLANG_COMPILE_COMBINED_OUTPUT_DIR="${BUILD_ROOT}"
# wgslMap "dst" paths are relative to the repository root (the Rust crates).
SLANG_COMPILE_DEST_ROOT="${REPO_ROOT}"

slang_compile_main
