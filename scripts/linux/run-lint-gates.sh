#!/usr/bin/env bash
# run-lint-gates.sh [--exclude DIR ...] - wrapper over ANTfrastructure's run-lint-gates.sh, so CI and a dev box run one entry point.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/antfrastructure.sh
source "${SCRIPT_DIR}/lib/antfrastructure.sh"

# The root is explicit: one derived upstream would grade ANTfrastructure's own tree and report green.
antfrastructure_exec linux/scripts/run-lint-gates.sh "${KATAGLYPHIS_REPO_ROOT}" "$@"
