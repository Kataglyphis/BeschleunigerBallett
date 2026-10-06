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
image's clang-tidy (LLVM 23.1.1) reads a clang-cl C++23 BMI (hub CON10). It still
covers only the TUs that import no `kataglyphis` module: the hub's
`Invoke-ClangTidyFixStep` skips the rest (upstream trap 2, below), and
`.clang-tidy` sets no `WarningsAsErrors`, so a finding is logged, never fatal;
only a TU tidy cannot parse fails the step. The clang-format check always runs
too (the container script has no `-SkipFormat` to forward; host
`Build-Windows.ps1` accepts it), and it has the failure mode upstream warns of:
it reports its deviating count but never fails the build on it.

## Known state (2026-10-06)

<!-- format-drift-denominator: 216 -->

**136 of 216** own sources under `Src/` and `Test/` differ from
`.clang-format`. Measured 2026-10-06 in the Linux image with its
`clang-format` 21.1.8 and `--dry-run -Werror` over the eight extensions
`Get-ProjectCppFiles` walks, reading each file from the git index so a CRLF
checkout cannot count. The denominator fell from 217 when the Kompute
playground's generated `shader/my_shader.hpp` left the tree; 2026-09-29 had
134 of 217. Earlier figures came from the Windows container's pair
(`Get-ProjectCppFiles` + `Invoke-ClangFormatCheck`): 72 of 125 on 2026-07-19,
140 of 215 on 2026-08-04 and 142 of 216 on 2026-08-05, so part of the drop to
134 may be the other `clang-format` build rather than fixed files.
`Invoke-ClangFormatCheck` (see the caveat above) reports this count on every
container build and **never fails the build** on it — that is why it grew
from 72 to 142 while every build stayed green.
Reformatting them is a **decision, not a chore**: it touches most of the
engine in one commit and will collide with in-flight work. Tracked in
`BACKLOG.md` — do it deliberately, ideally right after a merge point, and
add the commit to `.git-blame-ignore-revs` so history stays readable.
