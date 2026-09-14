#!/usr/bin/env bash
set -euo pipefail

# ─────────────────────────────────────────────────────────────────────────
# Agentic loop for BeschleunigerBallett (Linux / Rancher Desktop).
#
# Thin wrapper: sources the reusable library from the ANTfrastructure
# submodule (linux/scripts/lib/agentic-loop.sh), parses flags into the env
# vars the library reads, and calls run_agentic_loop. Task prompts default to
# ANTfrastructure's shared/agentic-loop/prompts/*.md - do not hard-code prompt
# text here.
#
# Engines (config .engine, or --engine / AGENTIC_ENGINE):
#   claude   — Claude Code CLI; models come from the config
#   opencode — OpenCode CLI; models come from the config
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
# Through the bootstrap (adopting-in-a-new-project.md section 8) rather than a
# third_party literal, so the ANTFRASTRUCTURE_DIR override is honoured.
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
      head -31 "$0" | tail -28
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
run_agentic_loop "$CONFIG_PATH" "$REPO_ROOT" "linux" || EXIT_CODE=$?
