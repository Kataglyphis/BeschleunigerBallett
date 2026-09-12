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
APP_RUNNER_DEFAULT_BUILD_DIR="build"
APP_RUNNER_DEFAULT_BUILD_TYPE="RelWithDebInfo"
APP_RUNNER_LABEL="profile"
APP_RUNNER_USAGE_INTRO="Starts the built application from the profile build directory."

app_runner_main "$@"
