#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
source "${SCRIPT_DIR}/lib/common.sh"

# lib/common.sh sources lib/containerhub.sh, so containerhub_source is already
# defined. It resolves against CONTAINERHUB_DIR - which the hand-rolled
# "${SCRIPT_DIR}/../../third_party/ContainerHub/..." literal this replaces could
# not honour - and fails naming the probed path AND the fix.
containerhub_source linux/scripts/lib/app-runner.sh

APP_RUNNER_DEFAULT_EXE_NAME="GraphicsEngine"
APP_RUNNER_DEFAULT_BUILD_DIR="build"
APP_RUNNER_DEFAULT_BUILD_TYPE="Debug"
APP_RUNNER_USAGE_INTRO="Starts the built application from the debug build directory."
APP_RUNNER_ENABLE_SHADER_CLEAN=true
APP_RUNNER_SHADER_CLEAN_DIR="Resources/ShadersSlang/build"
APP_RUNNER_SHADER_COMPILE_SCRIPT="${SCRIPT_DIR}/compile-slang-shaders.sh"

# Is a usable Vulkan SDK reachable at all? Used ONLY to decide whether the
# auto-install below is worth attempting.
#
# The five-probe sweep this used to hand-roll (glslc on PATH, vulkaninfo on
# PATH, $VULKAN_SDK/setup-env.sh, /opt/vulkan/*/setup-env.sh,
# ~/vulkan/*/setup-env.sh) is ContainerHub's
# linux/scripts/01-core/vulkan-env.sh - vulkan_env_find_setup_script, whose
# header enumerates that exact union and adds $VULKAN_SETUP_SCRIPT, the
# $VULKAN_VERSION-pinned path and the arch-subdirectory layout
# (/opt/vulkan/<ver>/x86_64/setup-env.sh) that the local copy missed. lib/
# common.sh already sources that module for source_vulkan_env; this is the same
# module answering the same question.
#
# The two PATH probes stay HERE because vulkan_env_find_setup_script answers
# "is there a setup-env.sh to source", not "are the tools usable": a container
# image with glslc/vulkaninfo baked in and no SDK directory is a valid
# environment for this launcher and must not trigger an install. vulkaninfo in
# particular has no upstream equivalent at all - vulkan_env_source's own
# fallback checks glslc only.
vulkan_sdk_available() {
  if declare -F vulkan_env_find_setup_script >/dev/null 2>&1 &&
     vulkan_env_find_setup_script >/dev/null; then
    return 0
  fi
  has_tool glslc || has_tool vulkaninfo
}

# Try to install Vulkan SDK using ContainerHub's setup-dependencies.sh.
install_vulkan_via_containerhub() {
  local sd
  # containerhub_path resolves against CONTAINERHUB_DIR (env-overridable) and,
  # on a miss, prints the probed path AND the submodule fix itself - strictly
  # more than the warn it replaces said.
  #
  # The second candidate the old code fell back to - linux/scripts/
  # setup-dependencies.sh, without the 02-toolchain/ segment - does not exist in
  # ContainerHub, so that branch could only ever turn "the submodule is not
  # checked out" into a message naming a path that was never right anyway. It is
  # gone rather than translated.
  #
  # A miss stays a warn-and-return here on purpose: this is an OPTIONAL
  # convenience for a dev box with no Vulkan SDK, and the caller
  # (app_runner_post_vulkan_hook) already decides what a failed auto-install
  # means. Nothing in CI reaches it.
  if ! sd="$(containerhub_path linux/scripts/02-toolchain/setup-dependencies.sh)"; then
    warn "Cannot auto-install Vulkan: ContainerHub's setup-dependencies.sh is not available."
    return 1
  fi

  local ver="${VULKAN_VERSION:-1.4.341.1}"
  info "Attempting to install Vulkan SDK ${ver} using ${sd} (may require sudo and take several minutes)"
  # Run the helper; it contains its own privilege escalation where needed
  if bash "${sd}" --vulkan-version "${ver}" vulkan; then
    info "ContainerHub helper finished (attempted Vulkan install)."
    return 0
  else
    warn "ContainerHub helper returned non-zero while attempting Vulkan install."
    return 2
  fi
}

# If no Vulkan SDK was found, attempt an auto-install and then source it.
#
# app_runner_main has ALREADY called source_vulkan_env by the time this hook
# runs (ContainerHub linux/scripts/lib/app-runner.sh:187-188 -> lib/common.sh's
# source_vulkan_env -> vulkan_env_source "" keep-libs 0), so a pre-existing SDK
# is already sourced and there is nothing left for this hook to do. What this
# hook uniquely owns - and the only reason it still exists - is the
# auto-INSTALL: nothing upstream installs an SDK on a dev box that has none.
#
# The three-way sourcing cascade that used to follow the install here
# (/opt/vulkan/$VER, then ~/vulkan/$VER, then the first /opt/vulkan/*/
# setup-env.sh) was a third copy of the same search; the second
# source_vulkan_env below is that search, run again now that the install has
# put something on disk. It also picks up the two layouts the local cascade
# could not: an SDK under ~/vulkan/*/ without an exact $VULKAN_VERSION match,
# and the arch-subdirectory layout.
app_runner_post_vulkan_hook() {
  if vulkan_sdk_available; then
    return 0
  fi

  info "Vulkan SDK/tools not detected on PATH. Attempting automatic install..."
  if install_vulkan_via_containerhub; then
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
