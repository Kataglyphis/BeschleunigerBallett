#!/usr/bin/env bash
set -euo pipefail

# Thin wrapper: the requirements install lives upstream in ContainerHub's
# shared python_uv.sh. uv_pip_install_requirements carries the load-bearing
# `--python .venv/bin/python` pin (uv honours UV_PYTHON over the activated
# venv - see the explanatory comment upstream), so no activation is needed.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# This script deliberately does NOT source lib/common.sh (it must stay usable
# from a bare venv bootstrap, before any toolchain is set up), so it sources the
# bootstrap directly. containerhub.sh is load-guarded, so this is free when
# common.sh already pulled it in.
#
# It replaces a "${REPO_ROOT}/third_party/ContainerHub/..." literal plus its
# hand-rolled -f guard: the literal ignored the CONTAINERHUB_DIR override, and
# containerhub_source already fails naming the probed path AND the fix.
# shellcheck source=containerhub.sh
source "${SCRIPT_DIR}/containerhub.sh"

containerhub_source linux/scripts/01-core/python_uv.sh

uv_pip_install_requirements "${1:-.venv}" "${2:-requirements.txt}"
