#!/usr/bin/env bash
# bump-version.sh <new_version> - VERSION.txt is the only copy; CMake, Sphinx and the MSIX all read it.
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
