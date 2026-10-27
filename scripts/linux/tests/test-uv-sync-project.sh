#!/usr/bin/env bash
# Exercises the real shared sync helper with fake uv, not package resolution or Sphinx.
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/linux/lib/antfrastructure.sh
source "${here}/../lib/antfrastructure.sh"
_harness="$(antfrastructure_path linux/scripts/lib/test-harness.sh)" || exit 1
# shellcheck source=third_party/ANTfrastructure/linux/scripts/lib/test-harness.sh
source "${_harness}"
work="$(mktemp -d)" || exit 1
trap 'rm -rf "${work}"' EXIT
venv="${work}/venv with spaces"
mkdir -p "${work}/bin" "${work}/mutant" "${venv}/bin" "${venv}/lib" || exit 1

cat > "${venv}/bin/activate" <<'STUB'
export VIRTUAL_ENV="${TEST_VENV}"
STUB
cat > "${venv}/bin/python" <<'STUB'
#!/usr/bin/env bash
exit 0
STUB
cat > "${work}/bin/uv" <<'STUB'
#!/usr/bin/env bash
printf '%s\n' "${UV_PROJECT_ENVIRONMENT-<unset>}" "${UV_PYTHON-<unset>}" \
  "${VIRTUAL_ENV-<unset>}" "$@" > "${TEST_UV_LOG}"
exit "${TEST_UV_EXIT}"
STUB
chmod +x "${venv}/bin/python" "${work}/bin/uv" || exit 1

run_wrapper() {
  (cd "${work}" && env -u ORT_CHAIN_WHEEL_DIR -u UV_SYNC_EXTRAS \
    PATH="${work}/bin:${PATH}" UV_PYTHON=/poison/python VIRTUAL_ENV=/poison/venv \
    UV_PROJECT_ENVIRONMENT=/poison/project TEST_VENV="${venv}" \
    TEST_UV_LOG="${work}/calls" TEST_UV_EXIT="${2:-0}" bash "$1" "${venv}")
}

check_contract() {
  local -a actual expected
  run_wrapper "$1" || return 2
  mapfile -t actual < "${work}/calls" || return 2
  expected=("${venv}" '<unset>' '<unset>' sync --dev --all-extras --locked --python "${venv}/bin/python")
  [[ "${actual[*]}" == "${expected[*]}" && ${#actual[@]} -eq ${#expected[@]} ]]
}

wrapper="${here}/../lib/uv-sync-project.sh"
t_case 'a separate shell pins the requested venv and clears conflicting environment settings'
t_assert_ok check_contract "${wrapper}"
t_case 'a failed shared sync fails the wrapper'
t_assert_rc 42 run_wrapper "${wrapper}" 42

cp "${here}/../lib/antfrastructure.sh" "${here}/../lib/renovate-git-mode.sh" "${work}/mutant/" || exit 1
sed 's/uv_sync_project --locked/uv_sync_project/' "${wrapper}" > "${work}/mutant/uv-sync-project.sh" || exit 1
t_case 'the contract rejects a missing explicit lock requirement'
t_assert_rc 1 check_contract "${work}/mutant/uv-sync-project.sh"
sed 's/^uv_venv_activate /: /' "${wrapper}" > "${work}/mutant/uv-sync-project.sh" || exit 1
t_case 'the contract rejects a sync that never selects the requested venv'
t_assert_rc 1 check_contract "${work}/mutant/uv-sync-project.sh"
t_summary
