#!/usr/bin/env bash
# test-docs-build-web.sh: docs-build-web.sh's wasm-bindgen pin from LF, CRLF, pin-less and missing Cargo.lock files.
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/linux/lib/antfrastructure.sh
source "${here}/../lib/antfrastructure.sh"
_harness="$(antfrastructure_path linux/scripts/lib/test-harness.sh)" || exit 1
# shellcheck source=third_party/ANTfrastructure/linux/scripts/lib/test-harness.sh
source "${_harness}"
logging="$(antfrastructure_path linux/scripts/01-core/logging.sh)" || exit 1
toolchain="$(antfrastructure_path linux/scripts/lib/rust-toolchain.sh)" || exit 1
work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

# The backend first: a match on the name's prefix would take its version instead.
lock='[[package]]\nname = "wasm-bindgen-backend"\nversion = "0.2.99"\n\n[[package]]\nname = "wasm-bindgen"\nversion = "0.2.100"\n'

# make_repo <name> <lock|missing>: the real script, a common.sh with the hub's logging and stubbed hub calls, cargo and wasm-bindgen stubs.
make_repo() {
  local repo="${work}/$1" web="${work}/$1/third_party/OxidANT/crates/webgpu_renderer/web"
  mkdir -p "${repo}/scripts/linux/lib" "${repo}/bin" "${web}"
  cp "${here}/../docs-build-web.sh" "${repo}/scripts/linux/"
  printf 'source %q\nsource %q\nantfrastructure_source() { :; }\nantfrastructure_path() { case "$1" in *stage-cargo-licenses.py) echo "${PWD}/stager.py" ;; *) echo "${PWD}/fetcher.py" ;; esac; }\nensure_wasm32_target() { :; }\ndocs_build_main() { echo "docs_build_main reached"; }\n' \
    "${logging}" "${toolchain}" > "${repo}/scripts/linux/lib/common.sh"
  local fonts="${repo}/third_party/OxidANT/crates/webgpu_renderer/assets/fonts"
  mkdir -p "${fonts}" && touch "${fonts}/NOTICE" "${fonts}/Apache-2.0.txt" "${fonts}/OFL-1.1.txt"
  # The fetcher stub unpacks a Colosseum-web unless a fetch-fails marker says the release is missing.
  cat > "${repo}/fetcher.py" <<'EOF'
import pathlib, sys
if pathlib.Path("fetch-fails").exists():
    sys.exit(1)
scene = pathlib.Path("Resources/Models/Colosseum-web")
(scene / "LICENSES").mkdir(parents=True, exist_ok=True)
for name in ("Colosseum-web.glb", "ATTRIBUTION.txt", "LICENSES/CC-BY-4.0.txt"):
    (scene / name).write_text(name)
EOF
  # The stager stub writes a manifest into --output-dir unless a stage-fails marker says the policy has a gap.
  cat > "${repo}/stager.py" <<'EOF'
import pathlib, sys
if pathlib.Path("stage-fails").exists():
    sys.exit("unreviewed crate")
out = pathlib.Path(sys.argv[sys.argv.index("--output-dir") + 1])
out.mkdir(parents=True, exist_ok=True)
(out / "manifest.json").write_text(" ".join(sys.argv[1:]))
EOF
  # The cargo stub records the version it installs, which the wasm-bindgen stub then reports, as a real install would.
  printf '#!/usr/bin/env bash\necho "cargo $*" >> %q\n[ "$1" != install ] || echo "$5" > %q\n' "${repo}/calls" "${repo}/wb-version" > "${repo}/bin/cargo"
  cat > "${repo}/bin/wasm-bindgen" <<'EOF'
#!/usr/bin/env bash
[ "$1" != --version ] || { echo "wasm-bindgen $(cat "$(dirname "$0")/../wb-version" 2>/dev/null || echo 0.0.0)"; exit 0; }
mkdir -p "$3" && touch "$3/kataglyphis_webgpu_renderer.js" "$3/kataglyphis_webgpu_renderer_bg.wasm"
EOF
  chmod +x "${repo}/bin/cargo" "${repo}/bin/wasm-bindgen"
  printf '<!doctype html>\n' > "${web}/index.html"
  [ "$2" = missing ] || printf '%b' "$2" > "${repo}/third_party/OxidANT/Cargo.lock"
}

# run_script <repo>: the script's exit code; its output goes to <repo>/out.
run_script() { (cd "$1" && env -u GITHUB_ACTIONS PATH="$1/bin:${PATH}" bash scripts/linux/docs-build-web.sh > "$1/out" 2>&1); }

make_repo lf "${lock}"
make_repo crlf "${lock//'\n'/'\r\n'}"
make_repo pinless '[[package]]\nname = "serde"\nversion = "1.0.0"\n'
make_repo missing missing
make_repo unpublished "${lock}"
touch "${work}/unpublished/fetch-fails"
mkdir -p "${work}/unpublished/docs/source/_webgpu_demo/webgpu-demo"
touch "${work}/unpublished/docs/source/_webgpu_demo/webgpu-demo/Colosseum-web.glb"
make_repo ungraded "${lock}"
touch "${work}/ungraded/stage-fails"

for shape in lf crlf; do
  repo="${work}/${shape}"
  t_case "a ${shape} Cargo.lock pins wasm-bindgen-cli to wasm-bindgen's version and builds the demo"
  t_assert_rc 0 run_script "${repo}"
  t_assert_grep "${repo}/calls" '^cargo install --locked wasm-bindgen-cli --version 0\.2\.100$'
  t_assert_grep "${repo}/out" 'WebGPU WASM demo built from current source'
  t_assert_ok test -f "${repo}/docs/source/_webgpu_demo/webgpu-demo/pkg/kataglyphis_webgpu_renderer_bg.wasm"
  t_case "a ${shape} build puts the Colosseum and the scene's and fonts' notices beside the demo"
  t_assert_grep "${repo}/out" 'Colosseum-web staged beside the WebGPU demo'
  t_assert_ok test -f "${repo}/docs/source/_webgpu_demo/webgpu-demo/Colosseum-web.glb"
  t_assert_ok test -f "${repo}/docs/source/_webgpu_demo/webgpu-demo/Colosseum-web/ATTRIBUTION.txt"
  t_assert_ok test -f "${repo}/docs/source/_webgpu_demo/webgpu-demo/Colosseum-web/LICENSES/CC-BY-4.0.txt"
  t_assert_ok test -f "${repo}/docs/source/_webgpu_demo/webgpu-demo/fonts/OFL-1.1.txt"
  t_case "a ${shape} build stages the renderer's wasm crate texts with OxidANT's policy"
  t_assert_grep "${repo}/calls" '^cargo fetch --locked --manifest-path third_party/OxidANT/Cargo\.toml$'
  t_assert_grep "${repo}/docs/source/_webgpu_demo/webgpu-demo/licenses/cargo/manifest.json" \
    '--root-package kataglyphis_webgpu_renderer --target wasm32-unknown-unknown --policy third_party/OxidANT/packaging/cargo-license-policy\.json'
done

repo="${work}/ungraded"
t_case "a crate the policy has not reviewed drops the demo and still builds the docs"
t_assert_rc 0 run_script "${repo}"
t_assert_grep "${repo}/out" 'unreviewed crate'
t_assert_grep "${repo}/out" 'the docs build without it'
t_assert_grep "${repo}/out" 'docs_build_main reached'
t_assert_fails test -e "${repo}/docs/source/_webgpu_demo/webgpu-demo"

repo="${work}/unpublished"
t_case "an unfetchable Colosseum warns, drops a stale copy and still builds the docs"
t_assert_rc 0 run_script "${repo}"
t_assert_grep "${repo}/out" 'Colosseum-web could not be fetched'
t_assert_grep "${repo}/out" 'docs_build_main reached'
t_assert_fails test -e "${repo}/docs/source/_webgpu_demo/webgpu-demo/Colosseum-web.glb"
t_assert_ok test -f "${repo}/docs/source/_webgpu_demo/webgpu-demo/fonts/NOTICE"

for shape in pinless missing; do
  repo="${work}/${shape}"
  t_case "a ${shape} Cargo.lock stops the script with a message, before the docs build"
  t_assert_rc 1 run_script "${repo}"
  t_assert_grep "${repo}/out" 'No wasm-bindgen version in third_party/OxidANT/Cargo.lock'
  t_assert_no_grep "${repo}/out" 'docs_build_main reached'
done
t_summary
