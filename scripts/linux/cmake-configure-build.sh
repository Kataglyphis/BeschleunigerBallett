#!/usr/bin/env bash
# cmake-configure-build.sh - this project's defaults and Slang pre-build over ANTfrastructure's lib/cmake-build.sh.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
source "${SCRIPT_DIR}/lib/common.sh"

# Not a third_party literal: antfrastructure_source honours ANTFRASTRUCTURE_DIR.
antfrastructure_source linux/scripts/lib/cmake-build.sh

CMAKE_BUILD_DEFAULT_PRESET="linux-debug-clang"
CMAKE_BUILD_DEFAULT_BUILD_DIR="build"
CMAKE_BUILD_DEFAULT_VULKAN_SETUP_SCRIPT="/opt/vulkan/$(antfrastructure_version VULKAN_VERSION)/setup-env.sh"
CMAKE_BUILD_DEFAULT_MB_PER_JOB="4000"  # 4GB RAM per parallel job
CMAKE_BUILD_PREBUILD_LABEL="Slang shader precompilation"
CMAKE_BUILD_USAGE_INTRO="Configures and builds BeschleunigerBallett inside the Linux container image."

# Every config needs the .spv on disk (loaded at runtime), so a failure is fatal; see docs/shader-sharing.md.
cmake_build_prebuild_hook() {
  bash "${SCRIPT_DIR}/compile-slang-shaders.sh"
}

cmake_build_main "$@"
