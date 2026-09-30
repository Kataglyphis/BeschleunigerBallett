#!/usr/bin/env bash
set -euo pipefail

# No venv activation needed: upstream pins --python .venv/bin/python, since uv prefers UV_PYTHON over it.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Not lib/common.sh: this must work from a bare venv bootstrap, before any toolchain is set up.
# shellcheck source=antfrastructure.sh
source "${SCRIPT_DIR}/antfrastructure.sh"

antfrastructure_source linux/scripts/01-core/python_uv.sh

uv_pip_install_requirements "${1:-.venv}" "${2:-requirements.txt}"
