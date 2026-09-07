#!/usr/bin/env bash
# Compile Slang shaders to SPIR-V (Vulkan/C++) and WGSL (Rust/WebGPU).
# Linux equivalent of scripts/windows/Build-SlangShaders.ps1.
#
# Everything generic - resolving slangc, expanding the -I include paths, reading
# the manifest, compiling each (file, entry, target) with staleness checking,
# the combined WGSL emit with its post-emit patch table, the
# minSlangcVersionForWgsl floor and the WGSL varying-location validator - lives
# upstream in ContainerHub's linux/scripts/lib/slang-compile.sh (twin of
# windows/scripts/modules/WindowsSlang.Common.psm1). This script keeps only this
# project's paths.
#
# The project data (entry points, targets, the combined-WGSL map and the
# depth-texture patch table) is single-sourced from
# Resources/ShadersSlang/shader-manifest.json, shared with the Windows script.
# Schema notes live in that file's "_comment" fields. Reading it requires
# python3 (NOT jq - see the driver for that story).
#
# Usage: bash compile-slang-shaders.sh
#
# slangc is resolved from VULKAN_SDK/bin/slangc, then PATH; a missing slangc is
# a hard failure (exit 2), never a silent skip.
#
# Staleness: an output is reused only when it is newer than its source AND
# every .slang file under the Slang tree AND the manifest file itself
# (conservative — an import or manifest edit rebuilds every dependent).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
source "${SCRIPT_DIR}/lib/common.sh"

REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SLANG_ROOT="${REPO_ROOT}/Resources/ShadersSlang"
BUILD_ROOT="${SLANG_ROOT}/build"

# lib/common.sh sources lib/containerhub.sh, so containerhub_source is already
# defined. It resolves against CONTAINERHUB_DIR - which the hand-rolled
# "${REPO_ROOT}/third_party/ContainerHub/..." literal this replaces could not
# honour - and fails naming the probed path AND the fix.
containerhub_source linux/scripts/lib/slang-compile.sh

# Paths only - the driver holds the behaviour.
SLANG_COMPILE_MANIFEST="${SLANG_ROOT}/shader-manifest.json"
SLANG_COMPILE_SOURCE_ROOT="${SLANG_ROOT}"
SLANG_COMPILE_SPIRV_OUTPUT_ROOT="${BUILD_ROOT}/spirv"
SLANG_COMPILE_WGSL_OUTPUT_ROOT="${BUILD_ROOT}/wgsl"
SLANG_COMPILE_COMBINED_OUTPUT_DIR="${BUILD_ROOT}"
# wgslMap "dst" paths are relative to the repository root (the Rust crates).
SLANG_COMPILE_DEST_ROOT="${REPO_ROOT}"

slang_compile_main
