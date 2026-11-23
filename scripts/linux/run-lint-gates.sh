#!/usr/bin/env bash
# run-lint-gates.sh [--exclude DIR ...] - wrapper over ANTfrastructure's run-lint-gates.sh, so CI and a dev box run one entry point.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/antfrastructure.sh
source "${SCRIPT_DIR}/lib/antfrastructure.sh"

# The workflow conventions lint-gates.yml enforces; a caller's own value wins.
export WORKFLOW_CONVENTIONS_GATE="${WORKFLOW_CONVENTIONS_GATE-sha-pin,permissions,job-timeout,artifact-error}"
# Explicit root (one derived upstream grades the hub's tree), and --ratchets as lint-gates.yml passes it.
antfrastructure_exec linux/scripts/run-lint-gates.sh "${KATAGLYPHIS_REPO_ROOT}" --ratchets "$@"
