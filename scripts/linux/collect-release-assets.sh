#!/usr/bin/env bash
# collect-release-assets.sh <artifacts dir> <out dir>: the lanes' packages, flat and uniquely named, for a GitHub release.
set -euo pipefail

artifacts="${1:?usage: collect-release-assets.sh <artifacts dir> <out dir>}"
out="${2:?usage: collect-release-assets.sh <artifacts dir> <out dir>}"
mkdir -p "$out"
# Absolute, so every step below means the same folder whatever its working directory.
out="$(cd "$out" && pwd)"

# put <file> <name>: one release file; a second one of the same name would silently replace the first.
put() {
  if [ -e "${out}/$2" ]; then
    printf 'two artifacts carry %s\n' "$2" >&2
    exit 1
  fi
  cp "$1" "${out}/$2"
}

for dir in "$artifacts"/*/; do
  name="$(basename "$dir")"
  # Test bundles and results are no deliverable.
  case "$name" in *-tests|*-test-results) continue ;; esac
  # bundle/ ships as the zip CPack makes from it; its loose exe and dlls are no package.
  while IFS= read -r -d '' file; do
    put "$file" "$(basename "$file")"
  done < <(find "$dir" -type d -name bundle -prune -o -type f \( -name '*.deb' -o -name '*.AppImage' -o -name '*.tar.gz' \
    -o -name '*.tgz' -o -name '*.msi' -o -name '*.msix' -o -name '*.zip' -o -name '*.exe' \) -print0)
  # CPack names the flatpak without its arch, so both arches' would collide.
  for flatpak in "$dir"*.flatpak; do
    [ -e "$flatpak" ] || continue
    case "$name" in *-arm64) arch=aarch64 ;; *-x64) arch=x86_64 ;; *) printf 'no arch in %s\n' "$name" >&2; exit 1 ;; esac
    put "$flatpak" "$(basename "${flatpak%.flatpak}")-${arch}.flatpak"
  done
done

if [ -z "$(ls -A "$out")" ]; then
  printf 'no packages under %s\n' "$artifacts" >&2
  exit 1
fi
ls -1 "$out"
