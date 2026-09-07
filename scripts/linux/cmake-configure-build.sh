#!/usr/bin/env bash
# cmake-configure-build.sh - project wrapper around ContainerHub's generic
# CMake build driver. Everything reusable (arg parsing, cargo/ccache/sccache
# writability fallbacks, Vulkan env, parallelism, configure+build) lives in
# ContainerHub's linux/scripts/lib/cmake-build.sh; only this project's defaults
# and its Slang pre-build step live here.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
source "${SCRIPT_DIR}/lib/common.sh"

# lib/common.sh sources lib/containerhub.sh, so containerhub_source is already
# defined. It resolves against CONTAINERHUB_DIR - which the hand-rolled
# "${SCRIPT_DIR}/../../third_party/ContainerHub/..." literal this replaces could
# not honour - and fails naming the probed path AND the fix.
containerhub_source linux/scripts/lib/cmake-build.sh

CMAKE_BUILD_DEFAULT_PRESET="linux-debug-clang"
CMAKE_BUILD_DEFAULT_BUILD_DIR="build"
CMAKE_BUILD_DEFAULT_VULKAN_SETUP_SCRIPT="/opt/vulkan/1.4.341.1/setup-env.sh"
CMAKE_BUILD_DEFAULT_MB_PER_JOB="4000"  # 4GB RAM per parallel job
CMAKE_BUILD_PREBUILD_LABEL="Slang shader precompilation"
CMAKE_BUILD_USAGE_INTRO="Configures and builds BeschleunigerBallett inside the Linux container image."

# Compile Slang shaders to SPIR-V (C++) and WGSL (Rust). Runs for ALL builds
# (not just Release) because the C++ code loads Slang-emitted SPIR-V at
# runtime via File I/O, not embedded — the .spv files must exist on disk.
# See docs/shader-sharing.md.
#
# A failure here is fatal (cmake-build.sh default). This call used to end in
# `|| warn "Slang shader precompilation failed"`, which hid a real failure and
# left CI with a green build and no SPIR-V at all. Use
# --allow-prebuild-failure if a build genuinely has to continue without shaders.
cmake_build_prebuild_hook() {
  bash "${SCRIPT_DIR}/compile-slang-shaders.sh"
}

cmake_build_main "$@"
