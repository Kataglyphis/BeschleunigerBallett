#!/usr/bin/env bash
# ci-image-ref.sh [--windows] - wrapper over ANTfrastructure's ci-image-ref.sh, kept for the path workflows type.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Not lib/common.sh: its info()/warn() print to stdout and would corrupt the one line this emits.
# shellcheck source=lib/antfrastructure.sh
source "${SCRIPT_DIR}/lib/antfrastructure.sh"

antfrastructure_exec linux/scripts/ci-image-ref.sh "$@"
