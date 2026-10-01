#!/usr/bin/env bash
# run-riscv64-tests.sh - the riscv64 lane: Debug cross build on the amd64 image, ctest under QEMU without the GPU suites.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
source "${SCRIPT_DIR}/lib/common.sh"

antfrastructure_source linux/scripts/lib/riscv64-cross.sh
antfrastructure_source linux/scripts/lib/cmake-build.sh
antfrastructure_source linux/scripts/lib/ctest-run.sh

BUILD_DIR="build-riscv64"
# Integration and GoldenRender need a GPU; see AGENTS.md § The riscv64 lane
CTEST_EXCLUDE_RISCV64='^(Integration|GoldenRender)\.'

riscv64_cross_env
# The x86_64 setup-env this hook sources would undo riscv64_cross_env's Vulkan paths.
source_vulkan_env() { return 0; }

CMAKE_BUILD_PREBUILD_LABEL="Slang shader precompilation"
cmake_build_prebuild_hook() {
  bash "${SCRIPT_DIR}/compile-slang-shaders.sh"
}

cmake_build_main --preset linux-riscv64-cross --build-dir "${BUILD_DIR}" --mb-per-job 4000
ctest_run_main --build-dir "${BUILD_DIR}" --build-type Debug --ctest-exclude "${CTEST_EXCLUDE_RISCV64}" "$@"
