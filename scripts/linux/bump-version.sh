#!/usr/bin/env bash
# bump-version.sh - write a new project version into the repo-root VERSION.txt.
#
# Usage:   bash ./scripts/linux/bump-version.sh <new_version>
# Example: bash ./scripts/linux/bump-version.sh 1.6.0
#
# VERSION.txt is the single source of the project version. Its readers are
# CMakeLists.txt (project(... VERSION ...) and the PROJECT_VERSION compile
# definition), docs/source/conf.py (the Sphinx release string) and
# scripts/windows/Build-Windows.ps1 (the MSIX package version). None of them has
# a copy of the number, so this script is the whole bump.
#
# It lives under scripts/linux/ with the other shell entry points rather than at
# the repo root: the lint gates take their scope from `git ls-files`, and a
# script that sits where nobody expects one is a script nobody reads.
set -euo pipefail

NEW_VERSION=${1:-}

if [ -z "$NEW_VERSION" ]; then
    echo "Error: No version provided."
    echo "Usage: bash ./scripts/linux/bump-version.sh <new_version>"
    exit 1
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

echo "$NEW_VERSION" > "${REPO_ROOT}/VERSION.txt"
echo "Version bumped to $NEW_VERSION in VERSION.txt"
