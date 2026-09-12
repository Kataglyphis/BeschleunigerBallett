# Code Quality Tooling (clang-format, clang-tidy, cmake-format)

The commands, the scoping rules, the two clang-tidy traps and the cadence are
generic and live upstream:
[`ANTfrastructure / code-quality-tooling.md`](../third_party/ANTfrastructure/docs/code-quality-tooling.md).
Read that first — this page only carries what is specific to **this** repo.

The configs themselves (`.clang-format`, `.clang-tidy`, `gcovr.cfg`) are owned
by ANTfrastructure as well and copied in here; `scripts/windows/tests/SharedConfig.Drift.Tests.ps1`
fails if a local copy drifts. Edit them upstream in `shared/config/`, then run
`Sync-SharedConfig.ps1 -RepoRoot . -Write`. `.cmake-format.yaml` also lives at
this repo's root.

## This repo's paths

Host LLVM is 22.1.8 at `C:\Program Files\LLVM\bin\`. The container build
database rewrite (upstream trap 1) resolves here to:

```pwsh
$db = "$env:TEMP\tidydb"; New-Item -ItemType Directory -Force $db | Out-Null
(Get-Content build-clangcl-debug\compile_commands.json -Raw) `
  -replace 'C:/ws', 'D:/GitHub/BeschleunigerBallett' |
  Set-Content "$db\compile_commands.json" -NoNewline
& $CT -p $db --quiet Src/GraphicsEngineVulkan/Main.cpp
```

The module-skip from upstream trap 2 is implemented in
`scripts/windows/modules/WindowsClang.Common.psm1`.

Linux equivalent: `scripts/linux/run-static-analysis-format.sh`.

## Running them as part of a build

`Build-Windows.ps1` runs both unless told otherwise:

```pwsh
# format + tidy included (slower)
.\scripts\windows\Build-Windows.ps1 -Configurations 'clangcl-debug'

# the fast loop, skipping them
.\scripts\windows\Build-Windows.ps1 -Configurations 'clangcl-debug' -SkipFormat -SkipTidy
```

**Caveat worth knowing:** `scripts/windows/Build-Windows-Container.ps1`
hard-codes `-SkipTidy` when it invokes `Build-Windows.ps1`, so containerized
builds never run clang-tidy — but they always run the clang-format check (only
`-SkipTidy` is hard-coded; the container script has no `-SkipFormat` to
forward). Host `Build-Windows.ps1` accepts `-SkipFormat`. That is why tidy drift
accumulates even when every build is green — and format drift accumulates too,
for the reason upstream calls "the failure mode to watch for": the clang-format
check that does run reports its deviating count but never fails the build on it.

## Known state (2026-08-05)

<!-- format-drift-denominator: 216 -->

**142 of 216** own sources under `Src/` and `Test/` differ from
`.clang-format` (up from 72 of 125 on 2026-07-19, and 140 of 215 on
2026-08-04 — the two new deviating files arrived between those dates;
`TextureDecode.ixx`, the 216th source, was formatted before it landed).
Re-measured 2026-08-05 with `Get-ProjectCppFiles` + `clang-format --dry-run
-Werror`, the same pair `Invoke-ClangFormatCheck` uses.
`Invoke-ClangFormatCheck` (see the caveat above) reports this count on every
container build and **never fails the build** on it — that is why it grew
from 72 to 142 while every build stayed green.
Reformatting them is a **decision, not a chore**: it touches most of the
engine in one commit and will collide with in-flight work. Tracked in
`BACKLOG.md` — do it deliberately, ideally right after a merge point, and
add the commit to `.git-blame-ignore-revs` so history stays readable.
