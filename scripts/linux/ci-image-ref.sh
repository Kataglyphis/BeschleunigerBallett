#!/usr/bin/env bash
# ci-image-ref.sh - print the family CI container image reference.
#
# THIN WRAPPER. The logic moved to ContainerHub
# linux/scripts/ci-image-ref.sh, which is now the fleet's one owner of "compose
# ${IMAGE_REGISTRY_PREFIX}:${CI_IMAGE_*_TAG} out of versions.env and print it".
# Its PowerShell twin is WindowsContainerImage.Common.psm1's
# Get-CiImageReference, and ContainerHub's own verify_ci_image_refs.py gates
# that all three compose the same string.
#
# This file stays because the invocation stays: `scripts/linux/ci-image-ref.sh`
# is what .github/workflows/Linux.yml types and what a human types on a dev box.
# The wrapper keeps that spelling working while the implementation lives in one
# place. It adds NOTHING to the upstream contract - same arguments, same
# stdout-only output, same non-zero exit on a missing key.
#
# Usage:
#   scripts/linux/ci-image-ref.sh              # Linux image (default)
#   scripts/linux/ci-image-ref.sh --windows    # Windows image
#
# Local repro of a CI step against exactly the image CI used:
#   docker run --rm -v "$PWD:/workspace" -w /workspace \
#     "$(scripts/linux/ci-image-ref.sh)" <cmd>
# See ContainerHub docs/rancher-desktop-linux-containers.md.
#
# ---------------------------------------------------------------------------
# HISTORY KEPT HERE ON PURPOSE: why the tag is :latest-cross, and why ONE tag.
# The VALUE lives upstream in versions.env; this is the measured record of how
# it got there, which upstream's header does not carry.
#
# Not :latest - that is the old single-arch build and had not been rebuilt since
# 2026-04-16, three months of toolchain drift behind the cross lane. Worse,
# :latest's manifest list still ADVERTISES amd64/arm64/riscv64 while every child
# manifest it points at 404s (MANIFEST_UNKNOWN); it is not a fallback for
# anything.
#
# And not a per-arch expression: on 2026-08-04 the :latest-cross manifest list
# contained exactly ONE entry, linux/amd64, so `docker pull` on ubuntu-26.04-arm
# died with "no matching manifest for linux/arm64/v8 in the manifest list
# entries" and took the whole ARM lane with it; naming :latest-cross-amd64 /
# :latest-cross-arm64 was the way around that. Re-verified against GHCR
# 2026-08-12: the index lists linux/amd64, linux/arm64 AND linux/riscv64 and all
# three child manifests resolve, so Docker picks the right one per runner and
# the arch no longer belongs in the tag.
# ---------------------------------------------------------------------------
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# containerhub.sh only - NOT lib/common.sh: common.sh's info()/warn() print to
# stdout, which would corrupt the one line this script exists to emit.
# shellcheck source=lib/containerhub.sh
source "${SCRIPT_DIR}/lib/containerhub.sh"

containerhub_exec linux/scripts/ci-image-ref.sh "$@"
