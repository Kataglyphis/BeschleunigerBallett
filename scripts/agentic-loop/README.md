# Agentic loop — the BeschleunigerBallett half

A planner/executor loop that drains `BACKLOG.md` autonomously: an expensive
planner writes tasks, a cheap executor implements them, and builds, tests and
quality gates run on a cadence.

**The loop's logic is not in this directory.** It lives in the ContainerHub
submodule — `windows/scripts/modules/WindowsAgenticLoop.Common.psm1` and
`linux/scripts/lib/agentic-loop.sh`. What is here is the thin consumer half:
one config, two runners, two prompt overlays. Everything generic (queue
discipline, the blocked-task protocol, model tiering, retries, build-failure
fixing) is documented upstream and deliberately not repeated here — this file
used to restate it, and the restatement is what went stale.

## Upstream docs

| Topic | Doc |
| --- | --- |
| Module API, `Invoke-AgenticLoop` parameters, the config-key table (it does not yet list the two `*PromptOverlayFile` keys — see [The prompt overlays](#the-prompt-overlays)), prerequisites, env overrides, usage examples | [`windows-agentic-loop.md`](../../third_party/ContainerHub/docs/windows-agentic-loop.md) |
| Build-matrix entry fields, sanitizer env vars, cycling order, full-matrix sweeps | [`agentic-loop-build-matrix.md`](../../third_party/ContainerHub/docs/agentic-loop-build-matrix.md) |
| What a consumer owns, what stays upstream, and the `- [ ]` / `- [b]` / `- [x]` backlog protocol | [`templates/README.md`](../../third_party/ContainerHub/shared/agentic-loop/templates/README.md) |

## What this directory owns

| File | Purpose |
| --- | --- |
| [`AgenticLoop.config.json`](AgenticLoop.config.json) | Engine choice, model IDs, cadences, timeouts, build matrix, build/test/quality commands |
| [`Invoke-AgenticLoop.ps1`](Invoke-AgenticLoop.ps1) | Windows runner — resolves the module, loads the config, calls `Invoke-AgenticLoop` |
| [`Run-AgenticLoop.sh`](Run-AgenticLoop.sh) | Linux runner — sources the library, maps flags onto the env vars it reads, calls `run_agentic_loop` |
| [`AgenticPromptOverlay.psm1`](AgenticPromptOverlay.psm1) | Overlay preflight for the Windows runner: fails the loop when a declared overlay never reaches the agent (the Bash runner carries the same assertion inline) |
| [`prompts/planner-overlay.md`](prompts/planner-overlay.md) | Project delta appended to ContainerHub's shared planner system prompt |
| [`prompts/executor-overlay.md`](prompts/executor-overlay.md) | Project delta appended to ContainerHub's shared executor system prompt |

Both runners are the upstream templates with their header comment, the loop name
and one preflight added. Keep them that way: no prompt text and no build-config
list belongs in a runner — hard-coding either is what let the Windows and Linux
copies drift apart once already. The preflight is the one deliberate deviation,
and it holds no prompt text: it asserts that the overlays the config declares
actually reach the agent, because the shared library treats every way of losing
them as a warning or as nothing at all. It belongs upstream; it lives here until
it is there. See [The prompt overlays](#the-prompt-overlays).

The two overlays are the **only** prompt text this repo owns. Both engines are
fed the same composition of ContainerHub's shared role prompt plus the overlay:
`claude` gets it as a temp file behind `--append-system-prompt-file`, composed
by the module at start-up, and `opencode` gets it as `.opencode/agents/<role>.md`,
because opencode takes no prompt file on its command line.

`.opencode/agents/` is **tracked**, and stays tracked. It was deleted and
gitignored on 2026-09-07 as a build artefact "regenerated on every loop start" —
but nothing generates it. There is no `Write-AgenticOpenCodeAgentFile`, or any
other writer of `.opencode/`, anywhere in the pinned ContainerHub, and the Bash
half's `invoke_opencode` goes straight to `opencode run --agent <role>`, which
resolves the file out of the checkout. The delete left a fresh clone on
`engine: opencode` running both roles with no role prompt at all.

The drift that motivated the delete was real — by then the files had become a
stale pre-extraction copy of the shared prompt: 225 lines that had lost the
executor incident narrative ("On 2026-07-31 three consecutive executor sessions
launched a container build in the background… zero tasks completed"), the
`timeout: 600000` guidance, the `until <done-check>; do sleep 10; done` polling
pattern, and the `- [b]` rationale with its commit step. The cure is a gate, not
a deletion: the tracked copies now hold the composed shared-prompt + overlay
text, and
[`AgenticLoop.PromptOverlays.Tests.ps1`](../windows/tests/AgenticLoop.PromptOverlays.Tests.ps1)
fails if either stops containing the shared role prompt or this repo's overlay
verbatim. **Edit the overlay, not these files.** They may go back to being
generated once a generator exists AND is proven to run before the first
`opencode run`.

[`.opencode/commands/`](../../.opencode/commands) stays hand-written and tracked:
those are TUI commands, not role prompts. They give the OpenCode TUI `/plan`,
`/execute`, `/build <preset>`, `/test` and `/quality` for driving single loop
phases interactively. OpenCode itself is installed via `scoop install opencode`
(or `npm install -g opencode-ai`) and authenticated once with
`opencode auth login` — the upstream prerequisites section covers the other
tools but does not name that auth step.

## Running it

```pwsh
pwsh -File .\scripts\agentic-loop\Invoke-AgenticLoop.ps1
```

```bash
./scripts/agentic-loop/Run-AgenticLoop.sh
```

`-Engine`, `-DryRun`, `-MaxIterations`, `-SkipBuild`, `-SkipTests`,
`-SkipQuality`, `-PlannerOnly`, `-ExecutorOnly` (and their `--kebab-case`
equivalents on the Bash side) map one-to-one onto the module parameters
documented in
[`windows-agentic-loop.md`](../../third_party/ContainerHub/docs/windows-agentic-loop.md).

## What this repo configures

The config also restates several upstream defaults on purpose, to pin them
against upstream drift — the bullets below say which values actually deviate.
The key-by-key reference is upstream in
[`windows-agentic-loop.md`](../../third_party/ContainerHub/docs/windows-agentic-loop.md).

- `engine: "claude"` (upstream default: `opencode`), with
  `plannerModel: claude-opus-5`, `plannerFallbackModel: claude-fable-5` for
  when Opus is overloaded, and `executorModel: claude-sonnet-5`. The
  `opencode` engine stays configured as an alternative (GLM 5.2 planner,
  DeepSeek v4 Flash executor).
- `fullMatrixEveryNIterations: 5` (default 0 = no sweeps) — the full-matrix
  sweep cadence. The other cadences (`buildEveryNTasks: 3`,
  `qualityEveryNTasks: 5`, `refactorEveryNIterations: 3`) pin the defaults.
- `plannerTimeoutSeconds: 1800`, `executorTimeoutSeconds: 3600` (default 0 =
  no timeout) — per-role wall-clock timeouts; the executor value also covers
  the build fixer. The config's generic `timeoutSeconds: 1200` is dead while
  both per-role values are set: `Get-AgentTimeoutForRole` only falls through
  to it for a role whose own timeout is 0.
- `agentRetryDelaySeconds: 30` (default 20). `agentRetries: 2`,
  `fixBuildFailures: true` and `maxConsecutiveBuildFailures: 3` pin the
  defaults.
- `plannerAllowedTools: "Read Glob Grep Edit(BACKLOG.md) Bash(git:*) PowerShell(git:*)"`
  — the planner reads the whole tree but writes only the backlog. The executor
  runs with `bypassPermissions` (the upstream default, restated because it is
  a trust statement: this is a trusted repo).
- `build.windowsTestCommand` / `build.linuxTestCommand` (no upstream default)
  — the fallback test command for any matrix entry without its own
  `testCommand`. Both point at the debug build's ctest; see the matrix bullet
  below for the consequence.
- `logging.logDir: logs/agentic-loop` (pins the default) — one timestamped log
  per run, carrying all agent, build, test and quality output.
- The `buildMatrix` maps this repo's presets to their build dirs and `ctest`
  commands: `clangcl-debug` (ASAN), `clangcl-profile`, `clangcl-release` on
  Windows; `linux-debug-asan-clang`, `linux-debug-tsan-clang`,
  `linux-profile-clang`, `linux-release-clang` on Linux. The release entries
  carry `testCommand: null`, which does **not** skip tests — the module falls
  back to `build.windowsTestCommand` / `build.linuxTestCommand`, and both are
  set here, so the release lane currently re-runs the debug build's ctest.
  `null` only means "no tests" in a config that leaves both fallback keys
  unset; if the release lanes are meant to be build-only, those two keys must
  be nulled as well.

## Build and quality commands

Windows builds never run CMake on the host. They go through the Stevedore
container script
[`scripts/windows/Build-Windows-Container.ps1`](../windows/Build-Windows-Container.ps1),
whose configuration name maps to a preset via
[`Build-Windows.config.psd1`](../windows/Build-Windows.config.psd1). Stevedore
setup and service recovery:
[`windows-stevedore-and-docker.md`](../../third_party/ContainerHub/docs/windows-stevedore-and-docker.md).

Linux builds go through
[`scripts/linux/cmake-configure-build.sh`](../linux/cmake-configure-build.sh),
natively or in a Rancher Desktop container.

The quality gate on Windows is
`Build-Windows.ps1 -Configurations clangcl-debug -SkipBuild -SkipTests -SkipPerfTests -SkipMsix`
(clang-tidy + cmake-format over the clangcl-debug build — keep the
`-Configurations` flag when running it by hand, because `Build-Windows.ps1`
defaults to all five presets without it); on Linux it is
[`scripts/linux/run-static-analysis-format.sh`](../linux/run-static-analysis-format.sh).

## The prompt overlays

`--append-system-prompt-file` takes exactly one file, so the module concatenates
ContainerHub's shared role prompt with this repo's overlay into a temp file at
startup rather than making the consumer keep a whole copy of the shared prompt.
The overlays are declared in the config's top-level `promptOverlays` block
(engine-agnostic, because both engines are fed the same composition) **and
mirrored under `engines.claude`** — two keys the upstream config-key table does
not document yet (it only lists the older full-override `plannerPromptFile` /
`executorPromptFile` shape). The mirror is not redundancy to tidy away: the
pinned `Resolve-AgenticEngine` reads `*PromptOverlayFile` from `engines.<engine>`
and nowhere else, so a top-level-only declaration made both overlays unread, with
no warning and no log line. Keep the two copies identical until the pinned hub
reads `promptOverlays`. The overlays therefore carry only what is true of *this*
renderer:

- [`prompts/planner-overlay.md`](prompts/planner-overlay.md) — which `BACKLOG.md`
  heading a task belongs under, which preset a task should name (`clangcl-debug`
  for iteration, `clangcl-profile` for benchmarks, `clangcl-release` for
  packaging), and the conventions a proposed task must never violate: no
  exceptions, `PipelineBuilder` for pipelines, VMA for buffer/image memory.
- [`prompts/executor-overlay.md`](prompts/executor-overlay.md) — the exact build
  commands, why a container build needs a container-native `--build-dir`, why
  Windows tests run the delivered executable from the repo root instead of
  `ctest --test-dir` (the container build's CTest metadata carries container
  paths), and the stale-SPIR-V trap after touching a shader.

They reach the `claude` agent on Windows only — an upstream defect, not a design
choice. The Bash half
(`third_party/ContainerHub/linux/scripts/lib/agentic-engines.sh`) still reads
only `plannerPromptFile` / `executorPromptFile`, so a Linux `claude` run gets no
project system prompt at all.

That used to happen **silently**, which is the worse half of the defect: a loop
producing work without this repo's build commands or conventions looks exactly
like a healthy one. It no longer does. Both runners preflight the overlays and
refuse to start when a declared overlay does not reach the agent:

| Runner | Preflight |
| --- | --- |
| [`Invoke-AgenticLoop.ps1`](Invoke-AgenticLoop.ps1) | `Assert-AgenticPromptOverlay` from [`AgenticPromptOverlay.psm1`](AgenticPromptOverlay.psm1) — resolves the config through the module, then requires the composed prompt to contain the overlay |
| [`Run-AgenticLoop.sh`](Run-AgenticLoop.sh) | `assert_prompt_overlays` — same assertion against `CLAUDE_*_PROMPT_FILE` (claude) or `.opencode/agents/<role>.md` (opencode) |

So today `Run-AgenticLoop.sh --engine claude` **exits 1 before the first agent
call**, naming `agentic-engines.sh` and what it has to learn. `--engine opencode`
passes, because the composed role prompts are tracked. Both preflights also fail
on a missing overlay file (which the module otherwise downgrades to a `WARN`) and
on the two overlay declarations drifting apart.
