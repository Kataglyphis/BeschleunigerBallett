#!/usr/bin/env bash
set -euo pipefail

# Thin wrapper: the uv venv logic lives upstream in ANTfrastructure's shared
# python_uv.sh so every project uses one implementation. Creates ./.venv in
# the caller's working directory, like the plain `uv venv` this replaced.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# This script deliberately does NOT source lib/common.sh (it must stay usable
# from a bare venv bootstrap, before any toolchain is set up), so it sources the
# bootstrap directly. antfrastructure.sh is load-guarded, so this is free when
# common.sh already pulled it in.
#
# It replaces a "${REPO_ROOT}/third_party/ANTfrastructure/..." literal plus its
# hand-rolled -f guard: the literal ignored the ANTFRASTRUCTURE_DIR override, and
# antfrastructure_source already fails naming the probed path AND the fix.
# shellcheck source=antfrastructure.sh
source "${SCRIPT_DIR}/antfrastructure.sh"

antfrastructure_source linux/scripts/01-core/python_uv.sh

# Empty python version on purpose: let uv resolve the interpreter (honouring
# UV_PYTHON exported by the CI containers) exactly like the previous plain
# `uv venv` call did, instead of pinning python_uv.sh's default version.
uv_venv_create "${1:-.venv}" ""
