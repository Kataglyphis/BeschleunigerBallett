#!/usr/bin/env bash
# test-compare-renderer-pixels.sh: compare-renderer-pixels.sh's verdict over stubbed captures; a broken capture never passes.
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/linux/lib/antfrastructure.sh
source "${here}/../lib/antfrastructure.sh"
_harness="$(antfrastructure_path linux/scripts/lib/test-harness.sh)" || exit 1
# shellcheck source=third_party/ANTfrastructure/linux/scripts/lib/test-harness.sh
source "${_harness}"
logging="$(antfrastructure_path linux/scripts/01-core/logging.sh)" || exit 1
work="$(mktemp -d)" || exit 1
trap 'rm -rf "${work}"' EXIT

# make_repo <name>: the real script over stubs; a marker file in the repo (cpp-fails, cpp-no-frame, rust-crash, arch) steers them.
make_repo() {
  local repo="${work}/$1" examples="${work}/$1/third_party/OxidANT/target/debug/examples"
  mkdir -p "${repo}/scripts/linux/lib" "${repo}/bin" "${repo}/build" "${examples}" "${repo}/Resources/Models/Dinosaurs"
  cp "${here}/../compare-renderer-pixels.sh" "${repo}/scripts/linux/"
  printf 'source %q\nvulkan_env_default_cpu_driver() { echo cpu-driver-default >> %q; }\n' \
    "${logging}" "${repo}/calls" > "${repo}/scripts/linux/lib/common.sh"
  touch "${repo}/Resources/Models/Dinosaurs/dinosaurs.obj"
  # The golden writes its frame before it grades it, so a failing golden still leaves one.
  cat > "${repo}/scripts/linux/run-ctest.sh" <<EOF
echo "run-ctest.sh \$*" >> ${repo}/calls
[ -e ${repo}/cpp-no-frame ] || echo png > "\${KATAGLYPHIS_GRADED_FRAME_PNG}"
[ ! -e ${repo}/cpp-fails ] || exit 1
EOF
  cat > "${repo}/build/compareRendererFrames" <<'EOF'
#!/usr/bin/env bash
if [ ! -f "$1" ] && [ ! -f "$2" ]; then echo "=== PIXEL COMPARISON: NOTHING CHECKED (exit code 2) ==="; exit 2; fi
echo "=== PIXEL COMPARISON PASSED ==="
EOF
  printf '#!/usr/bin/env bash\necho "cargo $*" >> %q\n' "${repo}/calls" > "${repo}/bin/cargo"
  printf '#!/usr/bin/env bash\ncat %q 2>/dev/null || echo x86_64\n' "${repo}/arch" > "${repo}/bin/uname"
  printf '#!/usr/bin/env bash\necho gltf > "$2"\n' > "${examples}/obj2gltf"
  cat > "${examples}/headless_render" <<EOF
#!/usr/bin/env bash
echo "headless_render width=\${LP_NATIVE_VECTOR_WIDTH:-} cache-off=\${MESA_SHADER_CACHE_DISABLE:-}" >> ${repo}/calls
[ ! -e ${repo}/rust-crash ] || exit 139
echo png > "\$2"
EOF
  chmod +x "${repo}/build/compareRendererFrames" "${repo}/bin/cargo" "${repo}/bin/uname" \
    "${examples}/obj2gltf" "${examples}/headless_render"
}

# run_script <repo> [args...]: the script's exit code, under run_env's extra variables; its output goes to <repo>/out.
run_env=()
run_script() {
  local repo="$1"
  shift
  (cd "${repo}" && env -u GITHUB_ACTIONS -u GATES_GITHUB_ANNOTATIONS -u CARGO_TARGET_DIR -u LP_NATIVE_VECTOR_WIDTH \
    -u MESA_SHADER_CACHE_DISABLE CARGO_HOME="${repo}/cargo-home" PATH="${repo}/bin:${PATH}" "${run_env[@]}" \
    bash scripts/linux/compare-renderer-pixels.sh "$@" > "${repo}/out" 2>&1)
}

make_repo pass
repo="${work}/pass"
t_case "two frames that pass are the grader's pass, and the golden runs through run-ctest.sh's GPU branch"
t_assert_rc 0 run_script "${repo}" --require-frames
t_assert_grep "${repo}/calls" '^run-ctest\.sh --virtual-display --build-dir .*/pass/build -- -R \^GoldenRender\\\.RendersNonBlankFrame\$ --no-tests=error$'
t_assert_grep "${repo}/calls" '^cargo build --locked --quiet --workspace --example obj2gltf --example headless_render$'
t_assert_grep "${repo}/calls" '^cpu-driver-default$'
t_assert_grep "${repo}/calls" '^headless_render width= cache-off=$'
t_assert_grep "${repo}/out" 'PIXEL COMPARISON PASSED'

for broken in cpp-fails rust-crash; do
  make_repo "${broken}"
  repo="${work}/${broken}"
  touch "${repo}/${broken}"
  t_case "a ${broken} capture fails the run, though the frame left behind passes"
  t_assert_rc 1 run_script "${repo}"
  t_assert_grep "${repo}/out" 'PIXEL COMPARISON PASSED'
  t_assert_grep "${repo}/out" "^FAIL the $([ "${broken}" = cpp-fails ] && echo 'C\+\+ capture exited 1' || echo 'Rust capture exited 139')\.$"
  t_assert_grep "${repo}/out" '^=== RENDERER COMPARISON FAILED \(exit code 1\) ===$'
done

repo="${work}/rust-crash"
t_case "under the hub's annotation switch a failed capture is an ::error:: line"
run_env=(GATES_GITHUB_ANNOTATIONS=1)
t_assert_rc 1 run_script "${repo}"
run_env=()
t_assert_grep "${repo}/out" '^::error title=compare-renderer-pixels\.sh::the Rust capture exited 139$'

make_repo stale
repo="${work}/stale"
touch "${repo}/cpp-no-frame"
mkdir -p "${repo}/logs/renderer-pixels"
echo png > "${repo}/logs/renderer-pixels/cpp-vulkan.png"
t_case "--require-frames fails a capture that left no frame, and an earlier run's frame does not stand in"
t_assert_rc 1 run_script "${repo}" --require-frames
t_assert_grep "${repo}/out" '^FAIL the C\+\+ renderer left no frame, which --require-frames forbids\.$'
t_assert_fails test -e "${repo}/logs/renderer-pixels/cpp-vulkan.png"

make_repo nothing
repo="${work}/nothing"
t_case "no frame at all checks nothing: exit 2, never a pass"
t_assert_rc 2 run_script "${repo}" --skip-cpp --skip-rust
t_assert_no_grep "${repo}/out" 'PASSED'
t_case "--require-frames makes that absence a failure"
t_assert_rc 1 run_script "${repo}" --skip-cpp --skip-rust --require-frames
t_assert_grep "${repo}/out" '^FAIL the Rust renderer left no frame, which --require-frames forbids\.$'

make_repo arm64
repo="${work}/arm64"
echo aarch64 > "${repo}/arch"
t_case "on aarch64 the Rust half draws at 128-bit vectors without Mesa's shader cache"
t_assert_rc 0 run_script "${repo}"
t_assert_grep "${repo}/calls" '^headless_render width=128 cache-off=true$'
t_summary
