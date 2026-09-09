#!/usr/bin/env bash
# renovate-local.sh - what is behind in this repo's 15 submodules, and moving
# the gitlinks that are safe to move. Dependency upgrades go through this, not
# by hand.
#
# THIN WRAPPER over ContainerHub linux/scripts/renovate-local.sh, which owns the
# whole tool: the on-demand, checksum-verified bootstrap of RENOVATE_NODE_VERSION
# and RENOVATE_VERSION, both pinned in the hub's linux/scripts/01-core/versions.env
# (RENOVATE_NODE_VERSION is deliberately NOT the canonical NODE_VERSION - Renovate
# declares engines.node "^24.11.0", so one name cannot serve both); running
# Renovate as a LOCAL CLI over this repo's own .github/renovate.json; the
# machine-readable report it parses; and the git half that applies what Renovate
# can only detect (`--platform=local` forces dryRun - Renovate decides, git
# applies).
#
#   bash ./scripts/linux/renovate-local.sh                    # report (default)
#   bash ./scripts/linux/renovate-local.sh --apply --dry-run  # show the plan
#   bash ./scripts/linux/renovate-local.sh --apply            # move the gitlinks
#   bash ./scripts/linux/renovate-local.sh --managers npm,dockerfile
#   bash ./scripts/linux/renovate-local.sh --print-bin        # resolved CLI path
#
# WHY THIS FILE EXISTS, given the logic is upstream: the same reason
# run-lint-gates.sh does - the invocation has to be one thing a human types
# here, and this repo has 15 submodules, more than any other consumer. The repo
# root is passed EXPLICITLY, exactly as run-lint-gates.sh passes it: upstream
# defaults its target to $PWD, so a run from a subdirectory would grade that
# subtree instead. Do not pass a path of your own - a second one is an error
# upstream; point the hub script at another repo directly.
#
# WHAT --apply REFUSES HERE, which is load-bearing in THIS repo: 14 of the 15
# submodules declare a `branch =`; `third_party/FUZZTEST` deliberately does not,
# because it is pinned to the tip of a FROZEN release_<date> line. An unset
# branch does NOT disarm `git submodule update --remote` - it makes it fall back
# to the REMOTE'S DEFAULT branch, i.e. 82 commits of main for that pin (see the
# corrected paragraph above the FUZZTEST block in .gitmodules). So --apply
# passes explicit paths, only for submodules that declare a branch, and prints
# the rest as REFUSED, to be moved by hand and deliberately - and, for FUZZTEST,
# with the Abseil coupling in AGENTS.md, "Critical Invariant: Submodule Pins".
#
# WHAT --apply DOES NOT COVER: it moves gitlinks, nothing else. Point it at
# another manager with --managers and you get a report; it applies nothing for
# them - this repo's requirements.txt included.
#
# WHERE TO RUN IT: WSL, both halves - there is no node on the Windows host. That
# stands a Linux git next to a Windows checkout, which reads every text file as
# modified; --apply would abort PART WAY THROUGH and leave the superproject half
# updated. The upstream script settles that, not you: it switches to git.exe when
# WSL can reach it, and refuses up front when it cannot. Nothing is staged or
# committed either way.
#
# Full rationale, the local workflow and the GitHub-token variant:
# third_party/ContainerHub/docs/dependency-updates.md
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# containerhub.sh only - NOT lib/common.sh: nothing here needs its Vulkan/Rust
# environment work, and its info()/warn() write to stdout, where the report
# table this script exists to print is the only thing that belongs.
# shellcheck source=lib/containerhub.sh
source "${SCRIPT_DIR}/lib/containerhub.sh"

containerhub_exec linux/scripts/renovate-local.sh "${KATAGLYPHIS_REPO_ROOT}" "$@"
