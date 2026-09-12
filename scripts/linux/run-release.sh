#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
source "${SCRIPT_DIR}/lib/common.sh"

# lib/common.sh sources lib/antfrastructure.sh, so antfrastructure_source is already
# defined. It resolves against ANTFRASTRUCTURE_DIR - which the hand-rolled
# "${SCRIPT_DIR}/../../third_party/ANTfrastructure/..." literal this replaces could
# not honour - and fails naming the probed path AND the fix.
antfrastructure_source linux/scripts/lib/app-runner.sh

APP_RUNNER_DEFAULT_EXE_NAME="GraphicsEngine"
APP_RUNNER_DEFAULT_BUILD_DIR="build-release"
APP_RUNNER_DEFAULT_BUILD_TYPE="Release"
APP_RUNNER_LABEL="release"
APP_RUNNER_USAGE_INTRO="Starts the built application from the release build directory."
APP_RUNNER_ENABLE_SHADER_CLEAN=true
APP_RUNNER_SHADER_CLEAN_DIR="Resources/ShadersSlang/build"
APP_RUNNER_SHADER_COMPILE_SCRIPT="${SCRIPT_DIR}/compile-slang-shaders.sh"

# For release builds we prefer not to have validation layers intercepting Vulkan if present
app_runner_env_hook() {
  export VK_LAYER_PATH=""
  export VK_INSTANCE_LAYERS=""
}

app_runner_main "$@"
