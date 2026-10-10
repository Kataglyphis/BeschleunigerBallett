# Code Quality Tooling (clang-format, clang-tidy, cmake-format)

The commands, the scoping rules, the two clang-tidy traps and the cadence are
generic and live upstream:
[`ANTfrastructure / code-quality-tooling.md`](../third_party/ANTfrastructure/docs/code-quality-tooling.md).
Read that first — this page only carries what is specific to **this** repo.

The configs themselves (`.clang-format`, `.clang-tidy`, `gcovr.cfg`) are owned
by ANTfrastructure as well and copied in here; `.antfrastructure-shared.manifest`
lists the seven assets this repo takes. The every-push gate is
`bash third_party/ANTfrastructure/shared/config/sync-shared-config.sh --repo-root . --check`
— the hub's lint aggregator runs it as one of its six gates, and the aggregator
runs from `.github/workflows/lint-gates.yml`, which has no `paths-ignore`, so
"every push" is literal and includes docs-only ones;
`scripts/windows/tests/SharedConfig.Drift.Tests.ps1` is its Pester mirror in the
Windows lane. Edit them upstream in `shared/config/`, then run
`Sync-SharedConfig.ps1 -RepoRoot . -Write`. `.cmake-format.yaml` also lives at
this repo's root.

## This repo's paths

This dev box has no host LLVM any more (checked 2026-09-25: there is no
`C:\Program Files\LLVM`; it held LLVM 22.1.8 when these commands were verified
on 2026-07-19), so install one before pointing upstream's `$CT` at its
`clang-tidy.exe`. The container build database rewrite (upstream trap 1)
resolves here, for the checkout at `C:\GitHub\BeschleunigerBallett`, to:

```pwsh
$db = "$env:TEMP\tidydb"; New-Item -ItemType Directory -Force $db | Out-Null
(Get-Content build-clangcl-debug\compile_commands.json -Raw) `
  -replace 'C:/ws', 'C:/GitHub/BeschleunigerBallett' |
  Set-Content "$db\compile_commands.json" -NoNewline
& $CT -p $db --quiet Src/GraphicsEngineVulkan/Main.cpp
```

The module-skip from upstream trap 2 is implemented in
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

**Caveat worth knowing:** containerized builds run clang-tidy again since
2026-10-01: neither `scripts/windows/Build-Windows-Container.ps1` nor the Windows
x64 lane's `Invoke-WindowsLane.ps1` passes `-SkipTidy` any more, because the
image's clang-tidy (LLVM 23.1.1 when hub CON10 proved it) reads a clang-cl C++23 BMI (hub CON10). It still
covers only the TUs that import no `kataglyphis` module: the hub's
`Invoke-ClangTidyFixStep` skips the rest (upstream trap 2, below), and
`.clang-tidy` sets no `WarningsAsErrors`, so a finding is logged, never fatal;
only a TU tidy cannot parse fails the step.

**The clang-format check is a failing gate** (since 2026-10-06, after the sweep
below). On Windows it is `Build-Windows.ps1`'s *clang-format check* step, which
the x64 lane's `Invoke-WindowsLane.ps1` runs since it stopped passing
`-SkipFormat` (the arm64 cross lane still skips it: formatting does not depend
on the target). On Linux it is the `static-analysis` job's first step,
`run-static-analysis-format.sh --only-format-check`, over `Src`, `Test` and
`scripts/riscv64`, the 217 files `Get-ProjectCppFiles` gives Windows. Both run
`clang-format --dry-run -Werror` and fail on one deviating file, and both
refuse a clang-format whose version is not the hub's `LLVM_RELEASE` (in
`third_party/ANTfrastructure/linux/scripts/01-core/versions.env`): the fleet formats with the pinned LLVM
(owner decision, 2026-10-06). Windows' PATH one already is
(`C:\llvm-patched\bin\clang-format.exe`). Linux prefers
`/usr/local/llvm-target/bin/clang-format` because the image's `/usr/bin` one is
still the distro's 21.1.8 until the hub switches it, and 21.1.8 flags 41 of the
swept files. A local rewrite: `Build-Windows.ps1 -ApplyFormat`, or the pinned
binary's `-i` (`run-static-analysis-format.sh` uses it too).

## Known state (2026-10-06)

<!-- format-drift-denominator: 216 -->

**0 of 216** own sources under `Src/` and `Test/` differ from `.clang-format`
(0 of 217 with `scripts/riscv64/fp16_helpers.c`), after the sweep in commit
`d59d210f6f93254e131996dca666e582f3d3b40e`. The sweep ran clang-format **23.1.1** (LLVM commit
`6dfe1677ab8dffbc6ec13d53a1e0215d75147689`, `/usr/local/llvm-target/bin` in
`:latest`, the version `:winamd64` ships as `C:\llvm-patched`) over the 217
files `Get-ProjectCppFiles` lists, reading each from the git index so a CRLF
checkout cannot count. It reformatted 146 of them. 37 comments that ran past
the 120-column limit were reflowed onto two lines, which the one-line comment
rule forbids, so the sweep rewrote each as one shorter line. The commit is in
`.git-blame-ignore-revs`; run `git config blame.ignoreRevsFile
.git-blame-ignore-revs` once per clone so `git blame` skips it.

Before the sweep the drift only grew, because the check reported and never
failed: 72 of 125 on 2026-07-19, 142 of 216 on 2026-08-05 (Windows), 134 of
217 on 2026-09-29 and 136 of 216 on 2026-10-06 (Linux, distro 21.1.8), and 146
of 217 by the pinned 23.1.1.

The hub pin of 2026-10-10 moved the gate to 23.1.3, which reads the same: 0 of 217 in
`:winamd64` and in `:latest`.
