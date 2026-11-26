# Code Quality Tooling (clang-format, clang-tidy, cmake-format)

The commands, the scoping rules, the two clang-tidy traps and the cadence are
generic and live upstream:
[`ANTfrastructure / code-quality-tooling.md`](https://github.com/Kataglyphis/ANTfrastructure/blob/develop/docs/code-quality-tooling.md).
Read that first; this page carries only what is specific to **this** repo.

Before adding Bash or PowerShell behavior, search this repository and its direct submodules,
including ANTfrastructure. Call or extend the shared owner with project-specific arguments;
the contributor rule is in [AGENTS.md](../../AGENTS.md).

The `powershell-lint` CI job calls ANTfrastructure's reusable PowerShell quality workflow.
It uses the hub's pinned Gallery installer and reports complexity; `analyzer-strict: false`
preserves this repo's current advisory analyzer policy while parse failures stay fatal.

`Compare-RendererPixels.ps1` checks C++ and Rust frames with the same structural verdict:
standard deviation above 2, more than 5% lit pixels, and more than eight luminance buckets.
The local Pester suite runs generated PNGs through both renderer paths and checks their labels,
messages and exit codes; it does not exercise GPU capture.

The `Compare-PerfBaseline` Pester suite uses ANTfrastructure's `New-TestDir -Prefix` for its
temporary baseline fixtures and `Remove-TestDir` in `AfterAll` for suite-scoped cleanup.
The comparison assertions exercise the real script with temporary JSON, not the checked-in
baseline.

The ARM64 shader source suite uses ANTfrastructure's `Get-TestSourceCopyLoopText` and
`Invoke-TestSourceCopyLoop` to parse and run one staging loop. Its own fixture files,
expected paths and omitted-shader mutants remain here; it does not compile ARM64 code.

The configs (`.clang-format`, `.clang-tidy`, `gcovr.cfg`) are ANTfrastructure's
too, copied in; `.antfrastructure-shared.manifest` lists the nine assets this
repo takes. Edit them upstream in `shared/config/`, then run
`Sync-SharedConfig.ps1 -RepoRoot . -Write`. `.cmake-format.yaml` also lives at
this repo's root. The drift gate,
`bash third_party/ANTfrastructure/shared/config/sync-shared-config.sh --repo-root . --check`,
is one of the ten always-on gates of the hub's lint aggregator, which
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

`Build-Windows.ps1` runs both unless told otherwise. These commands run inside the image; on
the host, build through `Build-Windows-Container.ps1`, which always runs both:

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
  `scripts/riscv64`, the 219 files `Get-ProjectCppFiles` gives Windows. The
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

**IWYU reports, and fails only on a unit it cannot read** (hub CON295 (a)):
`run-static-analysis-format.sh --only-iwyu --build-dir <tree>` runs the hub's
`code_quality_run_iwyu` over the units of `Src` and `Test` that neither declare
nor import a module (the image's include-what-you-use is built on clang 19 and
reads no clang 23 module file), and writes `logs/code-quality/iwyu.txt`. The
`static-analysis` job runs it after the debug build.

## Warnings: errors in every own target, a census behind them

Hub CON295 (c), owner decision D11 (fix first, then errors).

**A warning in the product, a suite or a fuzz target is an error.** `VulkanEngineCore` and `GraphicsEngine`
(`Src/GraphicsEngineVulkan/CMakeLists.txt`), the three suites (`Test/cmake/CommonTestBuild.cmake`) and the fuzz
targets (`Test/fuzz/CMakeLists.txt`) set CMake's `COMPILE_WARNING_AS_ERROR`, which CMake spells `-Werror`, or `/WX`
under clang-cl. The suites take the project's warnings from `VulkanEngineCore`; the fuzz targets link
`myproject_warnings` themselves, not `myproject_options`, whose sanitizers the preset decides for them.
`cmake --compile-no-warning-as-error` lifts the errors for one configure, say to try a compiler the image does not
ship yet. The Kompute playground links no warning set and keeps the compiler's defaults.

**Foreign headers are SYSTEM, so their warnings stay theirs.** GLM, ImGui, STB, VMA, GSL, spdlog, nlohmann_json and
the Vulkan SDK; GLFW (`add_subdirectory(GLFW SYSTEM)`), cxx's generated bridge headers (`oxidant_bridge`'s `SYSTEM`
property) and Abseil, which `Main.cpp` and FuzzTest include; and for the tests GoogleTest
(`FetchContent_Declare(... SYSTEM)`), Google Benchmark and FuzzTest with what it fetches (`add_subdirectory(...
SYSTEM)`). Abseil is SYSTEM outside clang-cl only: clang-cl misparses its `function_ref.h` as a system header
(AccelerANTgine, 4eac5a56), and its `/W4` raises none of the warnings GCC (`-Wpedantic` on `__int128`) and clang
(`-Wnullability-extension`) print there. The engine imports no foreign C++ module, so no BMI CMake builds with the
product's flags carries a foreign warning. The hub's crash handler, which the suites compile on Windows, takes `/w`
from `kataglyphis_add_crash_dump`.

**What `-w` hid.** Until the suites dropped `-w` (`/w`), they printed no warning. Without it, arm64 Debug showed 77
own warnings over 11 flags under clang (22 `-Wmissing-designated-field-initializers`, 12 `-Wmacro-redefined`, 12
`-Wmissing-field-initializers`, 12 `-Wdouble-promotion`) and 66 under GCC (21 `-Wmissing-field-initializers`, 12
unflagged macro redefinitions, 7 `-Wuseless-cast`, 7 `-Wdouble-promotion`), counted as the census counts. The macros
were six suites re-defining `GLM_FORCE_*`, which `VulkanEngineCore` already passes; the missing fields were the
Vulkan structs of `sharedShaderVectorsSuite`'s compute harness, which now name every member. With the project's
warnings the fuzz targets added `example_fuzz_test.cpp`'s `int` indices. The CRT deprecates `getenv` under
clang-cl, so the suites' 24 reads go through `common/EnvironmentVariable.hpp`, as the engine's do; with that, the
Windows x64 lane's three clang-cl builds read none (run 45775919309, before the errors).

**Every Linux build and both Windows lanes hold their own warnings in a census.** Each build step of
`reusable-linux.yml` tees its log into `logs/build-<build>.log`, and the job's last steps run the hub's
[`verify_warning_census.py`](https://github.com/Kataglyphis/ANTfrastructure/blob/develop/docs/code-quality-tooling.md#the-warning-census)
over it as lane `<arch>-<build>` against `scripts/linux/warning-census.txt`. The eight builds are `clang-debug`
(`clang-tests`), `clang-asan` and `clang-tsan` (`sanitizers`), `clang-analysis` (`static-analysis`),
`clang-profile` and `gcc-profile` (`profiling`), `gcc-debug` (`gcc`) and `clang-release` (`release`), on `x64` and
`arm64`. `Build-Windows.ps1 -WarningCensus`, which both Windows lanes pass, writes each clang-cl build's output to
`logs/windows/<build dir>.log` (the hub's `Invoke-CmakeConfigureAndBuild -BuildOutputLog`) and grades it as lane
`windows-<x64|arm64>-clangcl-<debug|profile|release>` against the same file. Only the repository's own code counts,
each `path:line:flag` once. A new warning fails its lane; so does a fixed one until its row is lowered, and a log
that compiled nothing. Each job builds into an empty tree, so its log holds every compile; a sccache hit replays the
compiler's warnings. The Linux census steps come last, so a red census skips no test. A Windows census step is not
critical: every later build, package and stage still runs, then `Build-Windows.ps1` fails, and the x64 lane skips
its container suites. Re-measure a lane with `--write` (the file's header has the command).

The file holds no rows: every lane is at zero. CI run 45719414600 (9c18544b), before the product's fixes, held 36
own warnings over six flags in both GCC builds (25 `-Wshadow`, 5 `-Wuseless-cast`, 2 `-Wconversion`, 2 unflagged
macro redefinitions, `-Wclass-memaccess`, `-Wmissing-field-initializers`), 11 in the four clang Debug builds and 13
in clang's Profile and Release, where `_FORTIFY_SOURCE` made the `snprintf` of `CommonGuiPanels.ixx`'s inline
functions a TU-local entity they exposed (`-WTU-local-entity-exposure`). clang-cl added the CRT's deprecation of
`getenv` at eight sites, which `common/EnvironmentVariable.hpp` now reads through `getenv_s`.

**riscv64 has the errors, not the census.** The lane cross-builds the same CMake, so its product and suites build
with `-Werror` too; its fuzz targets skip themselves, as they need ASan.

## Known state (2026-10-06)

<!-- format-drift-denominator: 256 -->

**0 of 256** own sources under `Src/` and `Test/` differ from `.clang-format`
(0 of 257 with `scripts/riscv64/fp16_helpers.c`; clang-format 23.1.5,
2026-11-26), since the sweep in commit
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
