#!/usr/bin/env bash
# A Windows-authored CRLF tree needs its native Git even when a WSL probe sees only LF source files.
[ -n "${_ANTFRASTRUCTURE_RENOVATE_GIT_MODE_LOADED:-}" ] && return 0
_ANTFRASTRUCTURE_RENOVATE_GIT_MODE_LOADED=1

antfrastructure_renovate_windows_git_root() {
    local win_root autocrlf
    command -v git.exe >/dev/null 2>&1 || return 1
    command -v wslpath >/dev/null 2>&1 || return 1
    win_root="$(wslpath -w "$1" 2>/dev/null)" || return 1
    case "$win_root" in [A-Za-z]:\\*) ;; *) return 1 ;; esac
    git.exe -C "$win_root" rev-parse --git-dir >/dev/null 2>&1 || return 1
    if [ "${2:-}" = --eol-proven ]; then printf '%s' "$win_root"; return 0; fi
    autocrlf="$(git.exe -C "$win_root" config --get core.autocrlf 2>/dev/null || true)"
    [ "$autocrlf" = true ] || return 1
    printf '%s' "$win_root"
}
