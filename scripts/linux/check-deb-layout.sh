#!/usr/bin/env bash
# check-deb-layout.sh <package.deb>: the desktop entry, AppStream file and icon sit where desktops look, and no build-time file ships.
set -euo pipefail

deb="${1:?usage: check-deb-layout.sh <package.deb>}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=scripts/linux/lib/antfrastructure.sh
source "${repo_root}/scripts/linux/lib/antfrastructure.sh"
license_verifier="$(antfrastructure_path linux/scripts/verify_package_license.py)"
# Field 6 is the path; a symlink's " -> target" follows it.
listing="$(dpkg-deb -c "${deb}" | awk '{print $6}')"
fails=0

for required in \
  ./usr/bin/GraphicsEngine \
  ./usr/share/applications/GraphicsEngine.desktop \
  ./usr/share/metainfo/de.jonasheinle.GraphicsEngine.appdata.xml \
  ./usr/share/licenses/GraphicsEngine/LICENSE \
  ./usr/share/icons/hicolor/256x256/apps/Engine_logo.png; do
  if ! grep -qxF "${required}" <<< "${listing}"; then
    printf 'FAIL  missing %s\n' "${required}"
    fails=$((fails + 1))
  fi
done

if ! python3 "${license_verifier}" "${deb}" \
  --require "usr/share/licenses/GraphicsEngine/LICENSE=${repo_root}/LICENSE" \
  --require "usr/Resources/Models/crytek-sponza/CC-BY-3.0.txt=${repo_root}/licenses/CC-BY-3.0.txt" \
  --require "usr/Resources/Models/crytek-sponza/crytek-sponza.NOTICE=${repo_root}/licenses/crytek-sponza.NOTICE"; then
  printf 'FAIL  an installed project or scene license text is missing or altered\n'
  fails=$((fails + 1))
fi

# A relative usr/share under the /usr prefix, and a vendored library's install rules.
stray="$(grep -E '^\./usr/(usr/|include/|lib/cmake/|lib/pkgconfig/|lib/[^/]+\.a$)' <<< "${listing}" || true)"
if [[ -n "${stray}" ]]; then
  printf 'FAIL  files no package should carry:\n%s\n' "${stray}"
  fails=$((fails + 1))
fi

if [[ "${fails}" -gt 0 ]]; then
  printf '%s: %d layout check(s) failed\n' "${deb##*/}" "${fails}"
  exit 1
fi
printf '%s: desktop integration under /usr/share, no build-time files\n' "${deb##*/}"
