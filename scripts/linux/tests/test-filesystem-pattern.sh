#!/usr/bin/env bash
# quality-report.toml's throwing_filesystem_calls row on a planted tree; run-lint-gates.sh grades the real Src/.
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/linux/lib/antfrastructure.sh
source "${here}/../lib/antfrastructure.sh"
_harness="$(antfrastructure_path linux/scripts/lib/test-harness.sh)" || exit 1
# shellcheck source=third_party/ANTfrastructure/linux/scripts/lib/test-harness.sh
source "${_harness}"
gate="$(antfrastructure_path linux/scripts/verify_pattern_limits.py)" || exit 1
work="$(mktemp -d)" || exit 1
trap 'rm -rf "${work}"' EXIT
cp "${here}/../../../quality-report.toml" "${work}/" || exit 1
mkdir -p "${work}/Src/engine" || exit 1
git -C "${work}" init -q || exit 1

# Usage: grade LINE; the row's verdict on a tree whose one source under a nested Src/ directory holds LINE.
grade() {
  printf 'void load() {\n  %s\n}\n' "$1" > "${work}/Src/engine/load.cpp"
  git -C "${work}" add -A
  python3 "${gate}" --root "${work}" > "${work}/out" 2>&1
}

t_case 'a throwing overload under a nested Src/ directory fails the lint gates'
for call in 'std::filesystem::create_directories(dir);' 'std::filesystem::recursive_directory_iterator it(root);' \
  'auto cwd = std::filesystem::current_path();' 'auto when = std::filesystem::last_write_time(path);'; do
  t_assert_rc 1 grade "${call}"
done

t_case 'the error_code overloads and a default-constructed end iterator pass'
for call in 'std::filesystem::create_directories(dir, ec);' 'std::filesystem::directory_iterator it(root, ec);' \
  'auto cwd = std::filesystem::current_path(ec);' 'const std::filesystem::directory_iterator end;'; do
  t_assert_rc 0 grade "${call}"
done

t_summary
