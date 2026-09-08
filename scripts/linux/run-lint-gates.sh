#!/usr/bin/env bash
# run-lint-gates.sh - this repository's shell + workflow + secret lint gates.
#
# THIN WRAPPER over ContainerHub linux/scripts/run-lint-gates.sh, which owns all
# three gates, the git-ls-files scope construction, the empty-list vacuity
# guards, the run-all-three-then-fail-once accumulator and the gitleaks
# self-test (clean-tree positive control + planted-PAT canary).
#
# WHY THIS FILE EXISTS AT ALL, given the logic is upstream: before it, the lint
# job was ~120 lines of shell embedded in .github/workflows/Linux.yml `run:`
# blocks, so the gate that blocks this repo's merges could not be reproduced on
# a dev box - the only way to test a change to it was to push. Now CI and a
# human run the SAME entry point:
#
#   bash ./scripts/linux/run-lint-gates.sh
#
# Extra arguments are forwarded, so a narrower sweep is
#   bash ./scripts/linux/run-lint-gates.sh --exclude third_party --exclude build
#
# The consumer root is passed EXPLICITLY and is never inferred upstream: this
# script lives in the consumer, ContainerHub lives inside it at
# third_party/ContainerHub, and a BASH_SOURCE-derived root over there would make
# all three gates grade ContainerHub's own tree and report green over the wrong
# repository.
#
# Three tools, three network bootstraps (shellcheck, actionlint, gitleaks - all
# pinned and SHA-verified upstream), so the first local run is not instant.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/containerhub.sh
source "${SCRIPT_DIR}/lib/containerhub.sh"

containerhub_exec linux/scripts/run-lint-gates.sh "${KATAGLYPHIS_REPO_ROOT}" "$@"
