#!/usr/bin/env bash
set -euo pipefail

# ─────────────────────────────────────────────────────────────────────────
# Agentic loop for BeschleunigerBallett (Linux / Rancher Desktop)
#
# Thin wrapper around the reusable library in ANTfrastructure's
# linux/scripts/lib/agentic-loop.sh.
#
# Engines (config .engine, or --engine / AGENTIC_ENGINE):
#   claude   — planner: Opus 5 (fallback Fable 5), executor: Sonnet
#   opencode — planner: GLM 5.2, executor: DeepSeek v4 Flash
#
# Usage:
#   ./scripts/agentic-loop/Run-AgenticLoop.sh [options]
#
# Options:
#   --config PATH        Config JSON path (default: AgenticLoop.config.json)
#   --engine NAME        Engine override: claude | opencode
#   --dry-run            Print actions without invoking agents or builds
#   --max-iterations N   Override max iterations (0 = unlimited)
#   --skip-build         Skip the build phase
#   --skip-tests         Skip the test phase
#   --skip-quality       Skip clang-tidy / cmake-format
#   --planner-only       Run the planner once and exit
#   --executor-only      Drain the current queue and exit
#   --help               Show this help
# ─────────────────────────────────────────────────────────────────────────

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# ── Source reusable library from ANTfrastructure ───────────────────────────
# The bootstrap lives in scripts/linux/lib/antfrastructure.sh (a verbatim copy of
# ANTfrastructure's shared/linux/templates/antfrastructure.sh) and is the one file
# that cannot come out of the submodule, because it is what FINDS the submodule.
# It resolves KATAGLYPHIS_REPO_ROOT / ANTFRASTRUCTURE_DIR from its OWN location, so
# sourcing it from this directory is correct, and it is load-guarded.
#
# This replaces a "${REPO_ROOT}/third_party/ANTfrastructure/..." literal and its
# hand-rolled if/else: the literal ignored the ANTFRASTRUCTURE_DIR override, and
# antfrastructure_source fails naming the probed path AND the fix.
# shellcheck source=../linux/lib/antfrastructure.sh
source "${REPO_ROOT}/scripts/linux/lib/antfrastructure.sh"

antfrastructure_source linux/scripts/lib/agentic-loop.sh

# ── Arg parsing (exported env flags are consumed by the library) ────────
CONFIG_PATH="${SCRIPT_DIR}/AgenticLoop.config.json"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --config)         CONFIG_PATH="$2"; shift 2 ;;
    --engine)         export AGENTIC_ENGINE="$2"; shift 2 ;;
    --dry-run)        export DRY_RUN=true; shift ;;
    --max-iterations) export MAX_ITERATIONS_OVERRIDE="$2"; shift 2 ;;
    --skip-build)     export SKIP_BUILD=true; shift ;;
    --skip-tests)     export SKIP_TESTS=true; shift ;;
    --skip-quality)   export SKIP_QUALITY=true; shift ;;
    --planner-only)   export PLANNER_ONLY=true; shift ;;
    --executor-only)  export EXECUTOR_ONLY=true; shift ;;
    --help|-h)
      head -30 "$0" | tail -28
      exit 0 ;;
    *) echo "Unknown option: $1"; exit 1 ;;
  esac
done

if ! command -v jq &>/dev/null; then
  echo "ERROR: jq is required. Install it: sudo apt install jq (or equivalent)."
  exit 1
fi
if [[ ! -f "$CONFIG_PATH" ]]; then
  echo "ERROR: Config not found: $CONFIG_PATH"
  exit 1
fi

init_agentic_loop "BeschleunigerBallett" "$REPO_ROOT"

EXIT_CODE=0

cleanup() {
  local ec=$?
  [[ $ec -ne 0 ]] && EXIT_CODE=$ec
  complete_agentic_loop "$EXIT_CODE"
  exit $EXIT_CODE
}
trap cleanup EXIT

error_handler() {
  log "Unhandled error at line $1: '$2'" "FATAL"
  EXIT_CODE=1
}
trap 'error_handler $LINENO "$BASH_COMMAND"' ERR

cd "$REPO_ROOT"

# ── Preflight: a declared overlay that no reader picks up is an ERROR ────
# The overlays are the only prompt text this repo owns, and every way of losing
# one is silent:
#   * the config declares them in a block the reader does not consult (moving
#     them out of engines.<engine> did exactly that on the PowerShell half);
#   * the overlay FILE is missing, which the composer downgrades to a WARN;
#   * the reader has no overlay shape at all, which is the state of
#     load_engine_config in agentic-engines.sh: it reads only the older
#     full-override plannerPromptFile / executorPromptFile keys, so a Linux run
#     hands claude no project system prompt and says nothing.
# So the delivery is asserted rather than assumed: resolve the config the way
# the loop will, then require the prompt the agent is actually handed to CONTAIN
# the overlay. Refusing to start is the point - a loop that silently drops this
# repo's build commands and conventions produces work that has to be redone.
assert_prompt_overlays() {
  local config="$1" repo_root="$2"
  local role key top eng effective overlay delivered mechanism remedy line missing overlay_lines

  # Cheap (one jq pass over a 5 KB file) and idempotent; run_agentic_loop calls
  # it again for itself. Needed here because CLAUDE_*_PROMPT_FILE is what the
  # claude adapter will actually pass to --append-system-prompt-file.
  load_engine_config "$config" "$repo_root" || return 1

  for role in planner executor; do
    key="${role}PromptOverlayFile"
    top=$(jq -r --arg k "$key" '.promptOverlays[$k] // ""' "$config")
    eng=$(jq -r --arg k "$key" --arg e "${AGENTIC_ENGINE}" '.engines[$e][$k] // ""' "$config")
    effective="${top:-$eng}"
    # Nothing declared for this role: the shared prompt alone is the intended
    # configuration. What this guards is a DECLARED overlay going unread.
    [[ -z "$effective" ]] && continue

    # The two locations are a compatibility mirror of ONE setting, kept because
    # the PowerShell and Bash readers disagree on where the key lives. Two
    # copies of a path drift, and the drift shows up as the loop quietly using
    # whichever copy its own reader consults.
    if [[ -n "$top" && -n "$eng" && "$top" != "$eng" ]]; then
      log "Prompt overlay declared twice with different values: promptOverlays.${key}='${top}' but engines.${AGENTIC_ENGINE}.${key}='${eng}'. Make them identical." "FATAL"
      return 1
    fi

    overlay="$effective"
    [[ "$overlay" != /* ]] && overlay="${repo_root}/${overlay}"
    if [[ ! -s "$overlay" ]]; then
      log "Prompt overlay declared but missing or empty: ${key}='${effective}' -> ${overlay}. The composer treats this as a WARN and runs on the shared prompt alone; restore the file or drop the declaration." "FATAL"
      return 1
    fi
    # Blank-but-not-zero-length is its own trap: the containment check below
    # would find nothing missing and report success over an overlay that says
    # nothing. An empty overlay is indistinguishable from a lost one.
    overlay_lines=0
    while IFS= read -r line; do
      [[ -z "${line//[[:space:]]/}" ]] && continue
      overlay_lines=$((overlay_lines + 1))
    done < "$overlay"
    if (( overlay_lines == 0 )); then
      log "Prompt overlay declared but blank: ${overlay} has no non-whitespace line. Write it or drop the declaration." "FATAL"
      return 1
    fi

    case "${AGENTIC_ENGINE}" in
      opencode)
        # opencode takes no prompt file on its command line: `opencode run
        # --agent <role>` resolves this path out of the checkout, so it IS the
        # delivery path.
        delivered="${repo_root}/.opencode/agents/${role}.md"
        mechanism="opencode resolves .opencode/agents/${role}.md from the checkout"
        remedy="restore .opencode/agents/${role}.md - it is tracked, so 'git checkout -- .opencode/agents' brings it back and a fresh clone already has it"
        ;;
      claude)
        if [[ "$role" == "planner" ]]; then
          delivered="${CLAUDE_PLANNER_PROMPT_FILE:-}"
        else
          delivered="${CLAUDE_EXECUTOR_PROMPT_FILE:-}"
        fi
        mechanism="claude is given --append-system-prompt-file with what load_engine_config resolved, and load_engine_config reads only .engines.claude.${role}PromptFile - it does not implement the ${key} shape at all (ANTfrastructure linux/scripts/lib/agentic-engines.sh)"
        remedy="teach load_engine_config the ${key} shape upstream (compose shared system-prompt + overlay the way the PowerShell half's New-AgenticComposedPrompt does), or drop the declaration and accept the shared prompt alone"
        ;;
      *)
        log "Unknown engine '${AGENTIC_ENGINE}': cannot tell how the ${role} overlay would be delivered." "FATAL"
        return 1
        ;;
    esac

    if [[ -z "$delivered" || ! -f "$delivered" ]]; then
      log "Prompt overlay declared but NOT delivered to the ${role}: ${key}='${effective}' never reaches the agent - ${mechanism}. Fix: ${remedy}. Running without it is not something this loop does silently." "FATAL"
      return 1
    fi

    # Containment, line by line: `grep -vxF -f delivered overlay` prints the
    # overlay lines the delivered prompt does not carry verbatim. It exits 1
    # when it prints nothing, which is the PASSING case, so the count is read
    # off the output rather than off the exit status.
    missing=0
    while IFS= read -r line; do
      [[ -z "${line//[[:space:]]/}" ]] && continue
      missing=$((missing + 1))
    done < <(grep -vxF -f "$delivered" "$overlay")
    if (( missing > 0 )); then
      log "Prompt overlay declared but ABSENT from what the ${role} agent is handed: ${missing} line(s) of ${overlay} are not in ${delivered} (${mechanism}). The agent would run without this repo's project rules." "FATAL"
      return 1
    fi
    log "Prompt overlay for ${role}: delivered via ${delivered}"
  done
}

assert_prompt_overlays "$CONFIG_PATH" "$REPO_ROOT" || exit 1

run_agentic_loop "$CONFIG_PATH" "$REPO_ROOT" "linux" || EXIT_CODE=$?
