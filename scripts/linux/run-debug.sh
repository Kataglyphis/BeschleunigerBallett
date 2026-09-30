#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
source "${SCRIPT_DIR}/lib/common.sh"

# Not a third_party literal: antfrastructure_source honours ANTFRASTRUCTURE_DIR.
antfrastructure_source linux/scripts/lib/app-runner.sh

APP_RUNNER_DEFAULT_EXE_NAME="GraphicsEngine"
APP_RUNNER_DEFAULT_BUILD_DIR="build"
APP_RUNNER_DEFAULT_BUILD_TYPE="Debug"
APP_RUNNER_USAGE_INTRO="Starts the built application from the debug build directory."
APP_RUNNER_ENABLE_SHADER_CLEAN=true
APP_RUNNER_SHADER_CLEAN_DIR="Resources/ShadersSlang/build"
APP_RUNNER_SHADER_COMPILE_SCRIPT="${SCRIPT_DIR}/compile-slang-shaders.sh"

# The PATH probes matter: an image with glslc/vulkaninfo baked in but no SDK directory must not trigger an install.
vulkan_sdk_available() {
  if declare -F vulkan_env_find_setup_script >/dev/null 2>&1 &&
     vulkan_env_find_setup_script >/dev/null; then
    return 0
  fi
  has_tool glslc || has_tool vulkaninfo
}

# Try to install Vulkan SDK using ANTfrastructure's setup-dependencies.sh.
install_vulkan_via_antfrastructure() {
  local sd
  # A miss only warns: this is an optional dev-box convenience that CI never reaches.
  if ! sd="$(antfrastructure_path linux/scripts/02-toolchain/setup-dependencies.sh)"; then
    warn "Cannot auto-install Vulkan: ANTfrastructure's setup-dependencies.sh is not available."
    return 1
  fi

  local ver="${VULKAN_VERSION:-}"
  if [[ -z "${ver}" ]]; then
    ver="$(antfrastructure_version VULKAN_VERSION)" || return 1
  fi
  info "Attempting to install Vulkan SDK ${ver} using ${sd} (may require sudo and take several minutes)"
  # Run the helper; it contains its own privilege escalation where needed
  if bash "${sd}" --vulkan-version "${ver}" vulkan; then
    info "ANTfrastructure helper finished (attempted Vulkan install)."
    return 0
  else
    warn "ANTfrastructure helper returned non-zero while attempting Vulkan install."
    return 2
  fi
}

# app_runner_main already sourced any existing SDK; this hook owns only the dev-box auto-install.
app_runner_post_vulkan_hook() {
  if vulkan_sdk_available; then
    return 0
  fi

  info "Vulkan SDK/tools not detected on PATH. Attempting automatic install..."
  if install_vulkan_via_antfrastructure; then
    # Second pass: the first one ran before the SDK existed.
    source_vulkan_env
  else
    warn "Automatic Vulkan installation failed or was not available. Continue at your own risk."
  fi
}

app_runner_env_hook() {
  export LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH

  # Enable Vulkan loader debug output by default (can be overridden externally)
  export VK_LOADER_DEBUG="${VK_LOADER_DEBUG:-all}"
  info "VK_LOADER_DEBUG=${VK_LOADER_DEBUG}"
}

app_runner_main "$@"
