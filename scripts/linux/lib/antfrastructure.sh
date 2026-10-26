#!/usr/bin/env bash
# Copy of ANTfrastructure's shared/linux/templates/antfrastructure.sh; the sync gate holds the body, so never hand-edit.
[ -n "${_KATAGLYPHIS_ANTFRASTRUCTURE_SH_LOADED:-}" ] && return 0
_KATAGLYPHIS_ANTFRASTRUCTURE_SH_LOADED=1

# ADJUST this if the file moves. scripts/linux/lib -> repo root is three levels.
: "${KATAGLYPHIS_REPO_ROOT_RELATIVE:=../../..}"

_antfrastructure_lib_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ ! -f "${_antfrastructure_lib_dir}/renovate-git-mode.sh" ]; then
    echo "Error: Renovate Git mode companion missing beside antfrastructure.sh" >&2
    return 1
fi
# shellcheck source=renovate-git-mode.sh
. "${_antfrastructure_lib_dir}/renovate-git-mode.sh"

# Overridable because a container bind-mounts the workspace at a different path.
KATAGLYPHIS_REPO_ROOT="${KATAGLYPHIS_REPO_ROOT:-$(cd "${_antfrastructure_lib_dir}/${KATAGLYPHIS_REPO_ROOT_RELATIVE}" && pwd)}"

# Order matters: an explicit ANTFRASTRUCTURE_DIR, then the submodule, then a sibling antfrastructure-tools clone.
if [ -z "${ANTFRASTRUCTURE_DIR:-}" ]; then
    if [ -d "${KATAGLYPHIS_REPO_ROOT}/third_party/ANTfrastructure" ]; then
        ANTFRASTRUCTURE_DIR="${KATAGLYPHIS_REPO_ROOT}/third_party/ANTfrastructure"
    elif [ -d "${KATAGLYPHIS_REPO_ROOT}/antfrastructure-tools" ]; then
        ANTFRASTRUCTURE_DIR="${KATAGLYPHIS_REPO_ROOT}/antfrastructure-tools"
    else
        # Neither exists: keep the submodule path so the error below names the declared shape.
        ANTFRASTRUCTURE_DIR="${KATAGLYPHIS_REPO_ROOT}/third_party/ANTfrastructure"
    fi
fi
export KATAGLYPHIS_REPO_ROOT ANTFRASTRUCTURE_DIR

# Absolute path of a file inside the submodule, or a failure naming the path and the fix bash's own error hides.
antfrastructure_path() {
    local relative_path="${1:?relative path required}"
    local resolved="${ANTFRASTRUCTURE_DIR}/${relative_path}"
    if [ ! -e "$resolved" ]; then
        echo "Error: ANTfrastructure file not found: ${resolved}" >&2
        # The hint must match this repo's shape: without the submodule, `git submodule update` misleads.
        if grep -q 'third_party/ANTfrastructure' "${KATAGLYPHIS_REPO_ROOT}/.gitmodules" 2>/dev/null; then
            echo "       If the whole directory is missing, the submodule is not checked out:" >&2
            echo "       git submodule update --init --recursive third_party/ANTfrastructure" >&2
        else
            echo "       This repo declares no ANTfrastructure submodule. Clone it beside the" >&2
            echo "       checkout, or point ANTFRASTRUCTURE_DIR at one you already have:" >&2
            echo "       git clone --depth 1 https://github.com/Kataglyphis/ANTfrastructure antfrastructure-tools" >&2
            echo "       export ANTFRASTRUCTURE_DIR=<checkout>" >&2
        fi
        echo "       If only this file is missing, the pinned ANTfrastructure predates it or it" >&2
        echo "       moved: bump the gitlink (git submodule update --remote --merge" >&2
        echo "       third_party/ANTfrastructure), then check ${ANTFRASTRUCTURE_DIR}/docs/INDEX.md" >&2
        return 1
    fi
    printf '%s' "$resolved"
}

# antfrastructure_source <relative>: source a load-guarded upstream library, e.g. linux/scripts/01-core/logging.sh
antfrastructure_source() {
    local resolved
    resolved="$(antfrastructure_path "${1:?relative path required}")" || return 1
    # shellcheck disable=SC1090
    source "$resolved"
}

# antfrastructure_exec <relative> "$@": exec a driver with WORKSPACE_ROOT pinned; see shared/linux/templates/README.md § The three entry points
antfrastructure_exec() {
    local relative_path="${1:?relative path required}"
    shift
    local resolved
    resolved="$(antfrastructure_path "$relative_path")" || exit 1
    export WORKSPACE_ROOT="${WORKSPACE_ROOT:-$KATAGLYPHIS_REPO_ROOT}"
    exec bash "$resolved" "$@"
}

antfrastructure_renovate_will_apply() {
    local arg apply=0
    for arg in "$@"; do
        case "$arg" in
            --apply) apply=1 ;;
            --dry-run) return 1 ;;
        esac
    done
    [ "$apply" -eq 1 ]
}

antfrastructure_renovate_check_gitlink() {
    local git_bin="$1" git_root="$2" record mode="" oid="" stage="" file=""
    record="$("$git_bin" -C "$git_root" ls-files --stage -- third_party/ANTfrastructure)" || return 1
    IFS=$' \t' read -r mode oid stage file <<< "$record" || true
    if [ "$mode" != 160000 ] || [[ ! "$oid" =~ ^[0-9a-f]{40}$ ]] ||
       [ "$stage" != 0 ] || [ "$file" != third_party/ANTfrastructure ] ||
       [[ "$record" == *$'\n'* ]]; then
        echo "Renovate bootstrap: third_party/ANTfrastructure is not one tracked direct gitlink." >&2
        return 1
    fi
    return 0
}

antfrastructure_renovate_check_mapping() {
    local git_bin="$1" git_root="$2" mappings line count=0
    if ! "$git_bin" -C "$git_root" ls-files --error-unmatch .gitmodules >/dev/null 2>&1 ||
       ! "$git_bin" -C "$git_root" diff --quiet -- .gitmodules ||
       ! "$git_bin" -C "$git_root" diff --cached --quiet -- .gitmodules; then
        echo "Renovate bootstrap: .gitmodules must be tracked and unchanged." >&2
        return 1
    fi
    mappings="$("$git_bin" -C "$git_root" config -f .gitmodules --get-regexp '^submodule\..*\.path$')" || return 1
    while IFS= read -r line; do
        [ "${line#* }" != third_party/ANTfrastructure ] || count=$((count + 1))
    done <<< "$mappings"
    if [ "$count" -ne 1 ]; then
        echo "Renovate bootstrap: .gitmodules must map the direct hub path exactly once." >&2
        return 1
    fi
    return 0
}

antfrastructure_renovate_check_path() {
    local root="$1" path="$2" entries
    if [ -L "$root/third_party" ] || [ -L "$path" ] ||
       { [ -e "$root/third_party" ] && [ ! -d "$root/third_party" ]; } ||
       { [ -e "$path" ] && [ ! -d "$path" ]; }; then
        echo "Renovate bootstrap: the hub path is redirected or is not a directory." >&2
        return 1
    fi
    if [ -d "$path" ]; then
        entries="$(ls -A -- "$path")" || return 1
        if [ -n "$entries" ]; then
            echo "Renovate bootstrap: the uninitialized hub path is not empty." >&2
            return 1
        fi
    fi
    return 0
}

antfrastructure_renovate_check_hub() {
    local root="$1" path="$2" git_bin="$3" git_root="$4"
    antfrastructure_renovate_check_gitlink "$git_bin" "$git_root" || return 1
    antfrastructure_renovate_check_mapping "$git_bin" "$git_root" || return 1
    antfrastructure_renovate_check_path "$root" "$path"
}

antfrastructure_renovate_restore_paths() {
    local root="$1" path="$2" path_absent="$3" parent_absent="$4" failed=0
    if [ "$path_absent" -eq 1 ]; then
        if [ -d "$path" ]; then rmdir -- "$path" || failed=1; fi
    elif [ ! -d "$path" ]; then
        mkdir -- "$path" || failed=1
    fi
    if [ "$parent_absent" -eq 1 ] && [ -d "$root/third_party" ]; then
        rmdir -- "$root/third_party" || failed=1
    fi
    return "$failed"
}

antfrastructure_renovate_finish() {
    local status="$1" root="$2" config="$3" backup_dir="$4" path_absent="$5" parent_absent="$6"
    local git_bin="$7" git_root="$8" failed=0
    trap - EXIT INT TERM
    if [ "$status" -ne 0 ] && [ "$status" -ne 2 ]; then
        "$git_bin" -C "$git_root" submodule deinit --force -- third_party/ANTfrastructure >/dev/null || failed=1
        cp -p -- "$backup_dir/config" "$config" &&
            cmp -s -- "$backup_dir/config" "$config" || failed=1
        antfrastructure_renovate_restore_paths "$root" "$root/third_party/ANTfrastructure" \
            "$path_absent" "$parent_absent" || failed=1
    fi
    if [ "$failed" -ne 0 ]; then
        echo "Renovate bootstrap: hub rollback failed; config backup at $backup_dir/config." >&2
        exit 1
    fi
    if ! rm -f -- "$backup_dir/config" || ! rmdir -- "$backup_dir"; then
        echo "Renovate bootstrap: backup cleanup failed; inspect $backup_dir." >&2
        exit 1
    fi
    exit "$status"
}

antfrastructure_renovate_exec() (
    local relative_path="${1:?relative path required}"
    shift
    local root="${1:?consumer root required}"
    shift
    local path="$root/third_party/ANTfrastructure" resolved config backup_dir
    local git_bin=git git_root="$root" win_root
    local path_absent=0 parent_absent=0
    if antfrastructure_path "$relative_path" >/dev/null 2>&1 ||
       [ -e "$path/.git" ] ||
       ! antfrastructure_renovate_will_apply "$@" ||
       [ "$ANTFRASTRUCTURE_DIR" != "$path" ]; then
        antfrastructure_exec "$relative_path" "$root" "$@"
    fi
    if win_root="$(antfrastructure_renovate_windows_git_root "$root")"; then
        git_bin=git.exe
        git_root="$win_root"
    fi
    antfrastructure_renovate_check_hub "$root" "$path" "$git_bin" "$git_root" || exit 1
    [ -e "$path" ] || path_absent=1
    [ -e "$root/third_party" ] || parent_absent=1
    # Convert Windows Git's config path before the shell backs up its bytes.
    if [ "$git_bin" = git.exe ]; then
        config="$("$git_bin" -C "$git_root" rev-parse --path-format=absolute --git-path config)" || exit 1
        config="$(wslpath -u "$config")" || exit 1
    else
        config="$(git -C "$root" rev-parse --path-format=absolute --git-path config)" || exit 1
    fi
    if [ ! -f "$config" ] || [ -L "$config" ]; then
        echo "Renovate bootstrap: parent Git config is missing or redirected." >&2
        exit 1
    fi
    backup_dir="$(mktemp -d)" || exit 1
    if ! cp -p -- "$config" "$backup_dir/config"; then
        rmdir -- "$backup_dir" || true
        exit 1
    fi
    trap 'antfrastructure_renovate_finish "$?" "$root" "$config" "$backup_dir" "$path_absent" "$parent_absent" "$git_bin" "$git_root"' EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
    "$git_bin" -C "$git_root" submodule update --init -- third_party/ANTfrastructure || exit $?
    resolved="$(antfrastructure_path "$relative_path")" || exit 1
    export WORKSPACE_ROOT="${WORKSPACE_ROOT:-$root}"
    if bash "$resolved" "$root" "$@"; then exit 0; else exit $?; fi
)
