#!/usr/bin/env bash
set -euo pipefail

# The shared sync pins the writable project environment even when the image sets UV_PYTHON.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Not lib/common.sh: this must work from a bare venv bootstrap, before any toolchain is set up.
# shellcheck source=antfrastructure.sh
source "${SCRIPT_DIR}/antfrastructure.sh"

antfrastructure_source linux/scripts/01-core/python_uv.sh

uv_venv_activate "${1:-.venv}"
uv_sync_project --locked
