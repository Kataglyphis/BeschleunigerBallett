#!/usr/bin/env bash
# gcc-toolchain-root.sh - print the GCC prefix Clang should be pointed at.
#
# The shell counterpart of the auto-detection in CMakeLists.txt, for the steps
# that invoke clang++ DIRECTLY (no CMake, no preset) and so cannot inherit the
# --gcc-toolchain flags CMake computes.
#
# Why this exists at all: the prefix used to be written out literally as
# /opt/gcc-16.1.0 - in the presets and in Linux.yml. When the build image
# bumped GCC 16.1.0 -> 16.2.0 that path stopped existing, and every Linux job
# died in compiler detection with
#
#   /usr/bin/x86_64-linux-gnu-ld.bfd: cannot find crtbeginS.o
#
# which names neither the flag nor the version. A version-keyed path must be
# discovered, never hardcoded: ANTfrastructure installs GCC at
# /opt/gcc-${GCC_VERSION} (linux/scripts/01-core/cross-gcc.sh) and bumps
# GCC_VERSION whenever it likes.
#
# Selection order:
#   1. MYPROJECT_GCC_TOOLCHAIN_PATH  - explicit override.
#   2. GCC_PREFIX                    - the build image EXPORTS this
#                                      (=/opt/gcc-16.2.0 today, set from
#                                      GCC_VERSION); it is the authoritative
#                                      answer whenever we are in the container.
#   3. the NEWEST /opt/gcc-* that actually contains crtbeginS.o.
# The last condition is the point of step 3: a prefix that cannot supply the
# startup objects is not a usable toolchain, so it must not be selected merely
# for existing.
#
# Prints the prefix on stdout. Exits 1 with a diagnostic on stderr if none is
# usable, so a caller running under `set -e` stops here rather than silently
# analysing against the system GCC's headers.
set -euo pipefail

if [ -n "${MYPROJECT_GCC_TOOLCHAIN_PATH:-}" ]; then
  if [ ! -d "${MYPROJECT_GCC_TOOLCHAIN_PATH}" ]; then
    echo "gcc-toolchain-root: MYPROJECT_GCC_TOOLCHAIN_PATH='${MYPROJECT_GCC_TOOLCHAIN_PATH}' does not exist" >&2
    exit 1
  fi
  printf '%s\n' "${MYPROJECT_GCC_TOOLCHAIN_PATH}"
  exit 0
fi

# Set by the CI image itself (Dockerfile ENV GCC_PREFIX=/opt/gcc-${GCC_VERSION}).
# Trust it when it resolves; fall through to the scan when it does not, so a
# stale exported value cannot be worse than no value at all.
if [ -n "${GCC_PREFIX:-}" ] && [ -d "${GCC_PREFIX}" ]; then
  printf '%s\n' "${GCC_PREFIX}"
  exit 0
fi

# `sort -V` so gcc-16.2.0 beats gcc-9.5.0; a plain lexicographic sort does not.
best=""
while IFS= read -r candidate; do
  [ -d "${candidate}" ] || continue
  if compgen -G "${candidate}/lib/gcc/*/*/crtbeginS.o" >/dev/null 2>&1; then
    best="${candidate}"
    break
  fi
done < <(find /opt -maxdepth 1 -type d -name 'gcc-*' 2>/dev/null | sort -Vr)

if [ -z "${best}" ]; then
  echo "gcc-toolchain-root: no usable GCC prefix under /opt (need lib/gcc/*/*/crtbeginS.o)." >&2
  echo "gcc-toolchain-root: found: $(find /opt -maxdepth 1 -type d -name 'gcc-*' 2>/dev/null | tr '\n' ' ')" >&2
  exit 1
fi

printf '%s\n' "${best}"
