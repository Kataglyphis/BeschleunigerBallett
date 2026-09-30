#!/usr/bin/env bash
set -euo pipefail

# Creates .venv in the caller's working directory through ANTfrastructure's python_uv.sh.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Not lib/common.sh: this must work from a bare venv bootstrap, before any toolchain is set up.
# shellcheck source=antfrastructure.sh
source "${SCRIPT_DIR}/antfrastructure.sh"

antfrastructure_source linux/scripts/01-core/python_uv.sh

# Empty version: uv resolves the interpreter and honours the containers' UV_PYTHON.
uv_venv_create "${1:-.venv}" ""
