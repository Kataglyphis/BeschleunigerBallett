#!/usr/bin/env bash
# test-collect-release-assets.sh: collect-release-assets.sh against artifact trees shaped like the lanes' uploads.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
collect="${here}/../collect-release-assets.sh"
work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
fails=0
check() {
  if "${@:2}"; then printf 'ok    %s\n' "$1"; else printf 'FAIL  %s\n' "$1"; fails=$((fails + 1)); fi
}

a="${work}/artifacts"
p=GraphicsEngine-1.5.0
for pair in x64:x86_64 arm64:aarch64; do
  arch="${pair%%:*}" m="${pair##*:}"
  l="${a}/linux-packages-${arch}"
  mkdir -p "${l}"
  for ext in deb tar.gz AppImage; do printf 'pkg' > "${l}/${p}-Linux-${m}-Release-Clang-23.1.3.${ext}"; done
  printf 'flatpak' > "${l}/${p}-linux.flatpak"
  w="${a}/BeschleunigerBallett-windows-${arch}"
  mkdir -p "${w}/packages" "${w}/msix" "${w}/bundle/bin"
  for ext in msi zip; do printf 'pkg' > "${w}/packages/${p}-Windows-${m}-Release-Clang-23.1.3.${ext}"; done
  printf 'msix' > "${w}/msix/GraphicsEngine_${arch}.msix"
  printf 'exe' > "${w}/bundle/bin/GraphicsEngine.exe"
done
printf 'nsis' > "${a}/BeschleunigerBallett-windows-x64/packages/${p}-Windows-x86_64-Release-Clang-23.1.3.exe"
mkdir -p "${a}/BeschleunigerBallett-windows-arm64-tests/packages"
printf 'msi' > "${a}/BeschleunigerBallett-windows-arm64-tests/packages/tests.msi"

out="${work}/release"
check "it collects" bash "${collect}" "${a}" "${out}"
check "the Linux packages of both arches" test "$(find "${out}" -maxdepth 1 -name '*-Linux-*' | wc -l)" = 6
check "each flatpak named for its arch" test -e "${out}/${p}-linux-x86_64.flatpak" -a -e "${out}/${p}-linux-aarch64.flatpak"
check "the MSI, zip and MSIX of both Windows arches" test "$(find "${out}" -maxdepth 1 \( -name '*.msi' -o -name '*.msix' -o -name '*.zip' \) | wc -l)" = 6
check "the x64 installer, but no exe from a bundle" bash -c "test -e '${out}/${p}-Windows-x86_64-Release-Clang-23.1.3.exe' && ! test -e '${out}/GraphicsEngine.exe'"
check "nothing from a test artifact" test ! -e "${out}/tests.msi"
check "relative paths, as release.yml passes them" bash -c "cd '${work}' && bash '${collect}' artifacts release-relative >/dev/null && test -e release-relative/GraphicsEngine_arm64.msix"

mkdir -p "${a}/BeschleunigerBallett-windows-x64-copy/packages"
cp "${a}/BeschleunigerBallett-windows-x64/packages/${p}-Windows-x86_64-Release-Clang-23.1.3.msi" "${a}/BeschleunigerBallett-windows-x64-copy/packages/"
check "two artifacts with one file name fail rather than overwrite" bash -c "! bash '${collect}' '${a}' '${work}/release2' >/dev/null 2>&1"
mkdir -p "${work}/none/x"
check "no packages at all fail" bash -c "! bash '${collect}' '${work}/none' '${work}/release3' >/dev/null 2>&1"

if [ "${fails}" -gt 0 ]; then
  printf '%d of the collect checks failed\n' "${fails}"
  exit 1
fi
printf 'collect-release-assets: all checks passed\n'
