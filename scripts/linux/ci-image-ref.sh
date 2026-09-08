#!/usr/bin/env bash
# ci-image-ref.sh - print the family CI container image reference.
#
# ONE place in the fleet owns the two image tags: ContainerHub's
# linux/scripts/01-core/versions.env, which composes
#   ${IMAGE_REGISTRY_PREFIX}:${CI_IMAGE_LINUX_TAG}    for Linux
#   ${IMAGE_REGISTRY_PREFIX}:${CI_IMAGE_WINDOWS_TAG}  for Windows
# The four container composite actions carry that same value as the DEFAULT of
# their `image` input, so a workflow step that just wants the family image omits
# the input entirely and this script is not involved.
#
# This exists for the one caller that cannot omit an input because it is not
# calling an action at all: the ASan job's fuzz-seed loop, which runs its own
# `docker run` because it needs a loop that keeps going after a failing target
# (the action takes a single command line). Before this, that step named the tag
# through a workflow-level env literal - a second copy of the tag, in this repo,
# that a fleet-wide bump would have left behind.
#
# It is equally the local entry point: `docker run --rm -v "$PWD:/workspace" \
# -w /workspace "$(scripts/linux/ci-image-ref.sh)" <cmd>` reproduces a CI step
# on a dev box against exactly the image CI used. See ContainerHub
# docs/rancher-desktop-linux-containers.md.
#
# WHY :latest-cross AND NOT :latest (the reason the tag is what it is; the value
# itself now lives upstream). The plain tag is the old single-arch build and had
# not been rebuilt since 2026-04-16 - three months of toolchain drift behind the
# cross lane, which is rebuilt routinely. Worse, :latest's manifest list still
# ADVERTISES amd64/arm64/riscv64 while every child manifest it points at 404s
# (MANIFEST_UNKNOWN) - it is not a fallback for anything.
#
# And why ONE tag rather than a per-arch expression: on 2026-08-04 the
# :latest-cross manifest list contained exactly ONE entry, linux/amd64, so
# `docker pull` on ubuntu-26.04-arm died with "no matching manifest for
# linux/arm64/v8 in the manifest list entries" and took the whole ARM lane with
# it; naming :latest-cross-amd64 / :latest-cross-arm64 was the way around that.
# Re-verified against GHCR 2026-08-12: the index lists linux/amd64, linux/arm64
# AND linux/riscv64 and all three child manifests resolve, so Docker picks the
# right one per runner and the arch no longer belongs in the tag.
#
# Usage:
#   scripts/linux/ci-image-ref.sh              # Linux image (default)
#   scripts/linux/ci-image-ref.sh --windows    # Windows image
#
# Prints the reference on stdout and NOTHING else, so it is safe in a command
# substitution. Every diagnostic goes to stderr; a missing key is a hard failure
# rather than an empty string, because an empty image reference reaches
# `docker run` as "run the argument after it as an image" and fails somewhere
# far away from the cause.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# containerhub.sh only - NOT lib/common.sh: common.sh's info()/warn() print to
# stdout, which would corrupt the one line this script exists to emit.
# shellcheck source=lib/containerhub.sh
source "${SCRIPT_DIR}/lib/containerhub.sh"

TAG_KEY="CI_IMAGE_LINUX_TAG"
case "${1:-}" in
  --linux|"") ;;
  --windows) TAG_KEY="CI_IMAGE_WINDOWS_TAG" ;;
  -h|--help)
    sed -n '2,45p' "${BASH_SOURCE[0]}" >&2
    exit 0
    ;;
  *)
    echo "ci-image-ref.sh: unknown argument '$1' (expected --linux or --windows)" >&2
    exit 1
    ;;
esac

VERSIONS_ENV="$(containerhub_path linux/scripts/01-core/versions.env)" || exit 1

# PARSED, never sourced: versions.env is inert KEY=value data and sourcing it
# would execute whatever else it grows. The same reason ContainerHub's own
# verify_ci_image_refs.py parses it rather than sourcing it.
read_key() {
  local key="$1" value
  value="$(sed -n "s/^${key}=//p" "${VERSIONS_ENV}" | tail -n 1)"
  value="${value%\"}"; value="${value#\"}"
  value="${value%\'}"; value="${value#\'}"
  if [ -z "${value}" ]; then
    echo "ci-image-ref.sh: ${key} is not set in ${VERSIONS_ENV}" >&2
    echo "                 That file is the fleet's single source of truth for the CI image tags;" >&2
    echo "                 a missing key means the ContainerHub pin predates the convention." >&2
    return 1
  fi
  printf '%s' "${value}"
}

prefix="$(read_key IMAGE_REGISTRY_PREFIX)" || exit 1
tag="$(read_key "${TAG_KEY}")" || exit 1

printf '%s:%s\n' "${prefix}" "${tag}"
