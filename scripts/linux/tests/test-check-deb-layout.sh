#!/usr/bin/env bash
# test-check-deb-layout.sh: check-deb-layout.sh against synthetic .debs, one per way the layout broke or could break.
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/linux/lib/antfrastructure.sh
source "${here}/../lib/antfrastructure.sh"
_harness="$(antfrastructure_path linux/scripts/lib/test-harness.sh)" || exit 1
# shellcheck source=third_party/ANTfrastructure/linux/scripts/lib/test-harness.sh
source "${_harness}"
gate="${here}/../check-deb-layout.sh"
work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

repo="$(cd "${here}/../../.." && pwd)"
licenses=usr/share/licenses/GraphicsEngine
bridge="${licenses}/cargo/kataglyphis_cxx_bridge@3.0.0/workspace/LICENSE"
third_party=("${licenses}/third_party/gsl/LICENSE" "${licenses}/third_party/abseil/LICENSE" "${licenses}/cargo/manifest.json" "${bridge}")
while IFS='=' read -r installed _; do third_party+=("${licenses}/third_party/${installed}"); done < "${repo}/licenses/third-party-licenses.txt"

# Builds <name>.deb from the good layout plus extra paths (+path) minus removed ones (-path).
make_deb() {
  local name="$1" root="${work}/$1" entry
  shift
  mkdir -p "${root}/DEBIAN"
  printf 'Package: graphicsengine\nVersion: 1.0\nArchitecture: all\nMaintainer: t <t@t>\nDescription: t\n' > "${root}/DEBIAN/control"
  for entry in usr/bin/GraphicsEngine usr/share/applications/GraphicsEngine.desktop \
    usr/share/metainfo/de.jonasheinle.GraphicsEngine.appdata.xml usr/share/icons/hicolor/256x256/apps/Engine_logo.png \
    usr/share/licenses/GraphicsEngine/LICENSE usr/Resources/Models/crytek-sponza/CC-BY-3.0.txt \
    usr/Resources/Models/crytek-sponza/crytek-sponza.NOTICE \
    usr/lib/graphicsengine/GraphicsEngine "${third_party[@]}" "$@"; do
    case "${entry}" in
      -*) rm -f "${root}/${entry#-}" ;;
      *) mkdir -p "$(dirname "${root}/${entry#+}")" && printf 'x' > "${root}/${entry#+}" ;;
    esac
  done
  if [[ -f "${root}/usr/share/licenses/GraphicsEngine/LICENSE" ]]; then
    cp "${here}/../../../LICENSE" "${root}/usr/share/licenses/GraphicsEngine/LICENSE"
  fi
  for text in CC-BY-3.0.txt crytek-sponza.NOTICE; do
    if [[ -f "${root}/usr/Resources/Models/crytek-sponza/${text}" ]]; then
      cp "${here}/../../../licenses/${text}" "${root}/usr/Resources/Models/crytek-sponza/${text}"
    fi
  done
  while IFS='=' read -r installed source; do
    if [[ -f "${root}/${licenses}/third_party/${installed}" ]]; then
      cp "${repo}/${source}" "${root}/${licenses}/third_party/${installed}"
    fi
  done < "${repo}/licenses/third-party-licenses.txt"
  if [[ -f "${root}/${bridge}" ]]; then cp "${repo}/third_party/OxidANT/LICENSE" "${root}/${bridge}"; fi
  dpkg-deb --root-owner-group -b "${root}" "${work}/${name}.deb" > /dev/null || exit 1
}

make_deb good
make_deb doubled +usr/usr/share/applications/GraphicsEngine.desktop -usr/share/applications/GraphicsEngine.desktop
make_deb glfw-headers +usr/include/GLFW/glfw3.h
make_deb glfw-static +usr/lib/libglfw3.a
make_deb glfw-config +usr/lib/cmake/glfw3/glfw3Config.cmake
make_deb no-icon -usr/share/icons/hicolor/256x256/apps/Engine_logo.png
make_deb no-license -usr/share/licenses/GraphicsEngine/LICENSE
make_deb wrong-license
printf 'incorrect license\n' > "${work}/wrong-license/usr/share/licenses/GraphicsEngine/LICENSE"
dpkg-deb --root-owner-group -b "${work}/wrong-license" "${work}/wrong-license.deb" > /dev/null || exit 1
make_deb no-scene-license -usr/Resources/Models/crytek-sponza/CC-BY-3.0.txt
make_deb no-scene-notice -usr/Resources/Models/crytek-sponza/crytek-sponza.NOTICE
make_deb no-glfw-license "-${licenses}/third_party/glfw/LICENSE.md"
make_deb no-font-license "-${licenses}/third_party/imgui-fonts/OFL-1.1.txt"
make_deb no-abseil-license "-${licenses}/third_party/abseil/LICENSE"
make_deb no-cargo-manifest "-${licenses}/cargo/manifest.json"
make_deb no-bridge-license "-${bridge}"
make_deb wrong-imgui-license
printf 'incorrect license\n' > "${work}/wrong-imgui-license/${licenses}/third_party/imgui/LICENSE.txt"
dpkg-deb --root-owner-group -b "${work}/wrong-imgui-license" "${work}/wrong-imgui-license.deb" > /dev/null || exit 1
make_deb wrong-scene-license
printf 'incorrect license\n' > "${work}/wrong-scene-license/usr/Resources/Models/crytek-sponza/CC-BY-3.0.txt"
dpkg-deb --root-owner-group -b "${work}/wrong-scene-license" "${work}/wrong-scene-license.deb" > /dev/null || exit 1

t_case "the good layout passes"
t_assert_rc 0 bash "${gate}" "${work}/good.deb"
t_case "a desktop entry under /usr/usr fails"
t_assert_rc 1 bash "${gate}" "${work}/doubled.deb"
t_case "GLFW's headers fail"
t_assert_rc 1 bash "${gate}" "${work}/glfw-headers.deb"
t_case "GLFW's static library fails"
t_assert_rc 1 bash "${gate}" "${work}/glfw-static.deb"
t_case "GLFW's CMake package fails"
t_assert_rc 1 bash "${gate}" "${work}/glfw-config.deb"
t_case "a missing icon fails"
t_assert_rc 1 bash "${gate}" "${work}/no-icon.deb"
t_case "a missing project license fails"
t_assert_rc 1 bash "${gate}" "${work}/no-license.deb"
t_case "a modified project license fails"
t_assert_rc 1 bash "${gate}" "${work}/wrong-license.deb"
t_case "a missing scene license fails"
t_assert_rc 1 bash "${gate}" "${work}/no-scene-license.deb"
t_case "a missing scene notice fails"
t_assert_rc 1 bash "${gate}" "${work}/no-scene-notice.deb"
t_case "a modified scene license fails"
t_assert_rc 1 bash "${gate}" "${work}/wrong-scene-license.deb"
for case in no-glfw-license no-font-license no-abseil-license no-cargo-manifest no-bridge-license wrong-imgui-license; do
  t_case "a package without its third-party text fails (${case})"
  t_assert_rc 1 bash "${gate}" "${work}/${case}.deb"
done
t_summary
