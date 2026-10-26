# Code Quality Tooling (clang-format, clang-tidy, cmake-format)

The commands, the scoping rules, the two clang-tidy traps and the cadence are
generic and live upstream:
[`ANTfrastructure / code-quality-tooling.md`](https://github.com/Kataglyphis/ANTfrastructure/blob/develop/docs/code-quality-tooling.md).
Read that first; this page carries only what is specific to **this** repo.

Before adding Bash or PowerShell behavior, search this repository and its direct submodules,
including ANTfrastructure. Call or extend the shared owner with project-specific arguments;
the contributor rule is in [AGENTS.md](../../AGENTS.md).

`Compare-RendererPixels.ps1` checks C++ and Rust frames with the same structural verdict:
standard deviation above 2, more than 5% lit pixels, and more than eight luminance buckets.
The local Pester suite runs generated PNGs through both renderer paths and checks their labels,
messages and exit codes; it does not exercise GPU capture.

The ARM64 shader source suite uses ANTfrastructure's `Get-TestSourceCopyLoopText` and
`Invoke-TestSourceCopyLoop` to parse and run one staging loop. Its own fixture files,
expected paths and omitted-shader mutants remain here; it does not compile ARM64 code.

The configs (`.clang-format`, `.clang-tidy`, `gcovr.cfg`) are ANTfrastructure's
too, copied in; `.antfrastructure-shared.manifest` lists the nine assets this
repo takes. Edit them upstream in `shared/config/`, then run
`Sync-SharedConfig.ps1 -RepoRoot . -Write`. `.cmake-format.yaml` also lives at
this repo's root. The drift gate,
`bash third_party/ANTfrastructure/shared/config/sync-shared-config.sh --repo-root . --check`,
is one of the six gates of the hub's lint aggregator, which
`.github/workflows/lint-gates.yml` runs on every push, docs-only ones included
(it has no `paths-ignore`). Its Pester mirror in the Windows lane:
`scripts/windows/tests/SharedConfig.Drift.Tests.ps1`.

## This repo's paths

This dev box has no host LLVM any more (checked 2026-09-25: no
`C:\Program Files\LLVM`; it held LLVM 22.1.8 when these commands were verified
on 2026-07-19): install one before pointing upstream's `$CT` at its
`clang-tidy.exe`. Upstream trap 1, the container build database rewrite, is
for the checkout at `C:\GitHub\BeschleunigerBallett`:

```pwsh
$db = "$env:TEMP\tidydb"; New-Item -ItemType Directory -Force $db | Out-Null
(Get-Content build-clangcl-debug\compile_commands.json -Raw) `
  -replace 'C:/ws', 'C:/GitHub/BeschleunigerBallett' |
  Set-Content "$db\compile_commands.json" -NoNewline
& $CT -p $db --quiet Src/GraphicsEngineVulkan/Main.cpp
```

Upstream trap 2's module skip is in
`third_party/ANTfrastructure/windows/scripts/modules/WindowsClang.Common.psm1`.
Linux equivalent: `scripts/linux/run-static-analysis-format.sh`.

## Running them as part of a build

`Build-Windows.ps1` runs both unless told otherwise:

```pwsh
# format + tidy included (slower)
.\scripts\windows\Build-Windows.ps1 -Configurations 'clangcl-debug'

# the fast loop, skipping them
.\scripts\windows\Build-Windows.ps1 -Configurations 'clangcl-debug' -SkipFormat -SkipTidy
```

**Containerized builds always run clang-format, and clang-tidy again since
2026-10-01**: neither `scripts/windows/Build-Windows-Container.ps1` nor the
Windows x64 lane's `Invoke-WindowsLane.ps1` passes `-SkipFormat`, nor
`-SkipTidy` any more, because the image's clang-tidy (LLVM 23.1.1 when hub
CON10 proved it) reads a clang-cl C++23 BMI.
It covers only the TUs that import no `kataglyphis` module (the hub's
`Invoke-ClangTidyFixStep` skips the rest, upstream trap 2), and `.clang-tidy`
sets no `WarningsAsErrors`: a finding is logged, never fatal; only a TU tidy
cannot parse fails the step.

**The clang-format check is a failing gate** since 2026-10-06, after the sweep
below. Both lanes run `clang-format --dry-run -Werror`, fail on one deviating
file, and refuse a clang-format of another release than the pinned LLVM's
(in `third_party/ANTfrastructure/linux/scripts/01-core/versions.env`): the
fleet formats with the pinned LLVM (owner decision, 2026-10-06). Since
2026-10-19 the hub picks and checks the binary on both lanes.

- **Windows**: `Build-Windows.ps1`'s *clang-format check* step, which the x64
  lane's `Invoke-WindowsLane.ps1` runs since it stopped passing `-SkipFormat`
  (the arm64 cross lane still skips it: formatting does not depend on the
  target). The hub's `Invoke-ClangFormatCheck` requires `LLVM_WINDOWS_VERSION`
  of the PATH clang-format, the image's `C:\llvm-patched\bin\clang-format.exe`.
- **Linux**: the `static-analysis` job's first step,
  `run-static-analysis-format.sh --only-format-check`, over `Src`, `Test` and
  `scripts/riscv64`, the 217 files `Get-ProjectCppFiles` gives Windows. The
  hub's `code_quality_require_pinned_clang_format` takes the first
  `LLVM_RELEASE` clang-format of `/usr/local/llvm-target/bin` and PATH, so a
  distro one (21.1.8 flagged 41 of the swept files) never grades.

Rewrite locally with `Build-Windows.ps1 -ApplyFormat` or the pinned binary's
`-i` (`run-static-analysis-format.sh` uses it too).

**cmake-format is a failing gate too** (since 2026-10-12), in the same
`--only-format-check` step: the hub's pinned cmake-format checks every CMake
file outside `build*/`, `.venv/` and `third_party/` against
`.cmake-format.yaml`. Before, only the full mode ran it, in place, so it
rewrote the CI checkout and never failed; 11 of 18 files had drifted. The sweep
that fixed them is in `.git-blame-ignore-revs`.

## Known state (2026-10-06)

<!-- format-drift-denominator: 217 -->

**0 of 217** own sources under `Src/` and `Test/` differ from `.clang-format`
(0 of 218 with `scripts/riscv64/fp16_helpers.c`), after the sweep in commit
`fb246fbcdae20e0d513c3133455c180f2faecd40`. It ran clang-format **23.1.1** (LLVM
commit `6dfe1677ab8dffbc6ec13d53a1e0215d75147689`, `/usr/local/llvm-target/bin`
in `:latest`, the version `:winamd64` ships as `C:\llvm-patched`) over the 217
files `Get-ProjectCppFiles` lists, read from the git index so a CRLF checkout
cannot count, and reformatted 146 of them. It reflowed 37 comments past the
120-column limit onto two lines, which the one-line comment rule forbids, so
each was rewritten as one shorter line. The commit is in
`.git-blame-ignore-revs`; run `git config blame.ignoreRevsFile
.git-blame-ignore-revs` once per clone so `git blame` skips it.

Before the sweep the check only reported, so drift only grew: 72 of 125 on
2026-07-19, 142 of 216 on 2026-08-05 (Windows), 134 of 217 on 2026-09-29 and
136 of 216 on 2026-10-06 (Linux, distro 21.1.8), 146 of 217 by the pinned
23.1.1.

The hub pin of 2026-10-10 moved the gate to 23.1.3, which reads the same: 0 of
217 in `:winamd64` and `:latest`. Since hub a5443e2c (2026-10-12)
`.clang-format` sets `ReflowComments: Never`, so clang-format leaves comments
as written and a long one stays one line. It also stopped aligning a `//` line
under the trailing comment above it, so the three such pairs in
`VulkanDevice.cpp` and `VulkanInstance.cpp` became one line each. 0 of 217
deviate in `:latest`.
