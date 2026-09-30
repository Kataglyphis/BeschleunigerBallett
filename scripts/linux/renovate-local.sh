#!/usr/bin/env bash
# renovate-local.sh [--apply [--dry-run]] [--managers ...] - see third_party/ANTfrastructure/docs/dependency-updates.md
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Not lib/common.sh: its info()/warn() write to stdout, where only the report belongs.
# shellcheck source=lib/antfrastructure.sh
source "${SCRIPT_DIR}/lib/antfrastructure.sh"

# The repo root is explicit: upstream defaults to $PWD and would grade a subdirectory instead.
antfrastructure_exec linux/scripts/renovate-local.sh "${KATAGLYPHIS_REPO_ROOT}" "$@"
