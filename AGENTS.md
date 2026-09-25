# AGENTS.md

Guidance for AI agents and new contributors working in BeschleunigerBallett
(a Vulkan graphics-engine playground with a Rust WebGPU sibling renderer:
C++23/C17, CMake presets, optional Rust).

## Start here: routing

Find your task, do the first action, read the doc. Everything below expands on
these; the [Docs](#docs) table at the end is the full ownership index.

| If you are doing this | Start with |
| --- | --- |
| Building anything on Windows | `Build-Windows-Container.ps1` — see [Fast path](#fast-path-windows-build-run-verify). Never invoke CMake/Ninja/MSBuild on the host. |
| Running the engine / seeing pixels | `scripts/windows/Invoke-ClangCl*.ps1` from the **repo root** — see [Running on the Host](#running-on-the-host-windows) |
| Changing a shader | Edit `Resources/ShadersSlang/*.slang` + `shader-manifest.json`, run `Build-SlangShaders.ps1`, run one golden. No C++ rebuild. [`docs/shader-build-pipeline.md`](docs/shader-build-pipeline.md) |
| Adding or changing a test | `Test/commit/VulkanEngine/` (CPU + GPU golden), `Test/fuzz/`, `Test/perf/`. Always in scope — see [Testing](#testing) |
| Touching render passes, barriers, frames-in-flight | Golden suites on the host GPU **and** `Invoke-SyncValidation.ps1` — [`docs/gpu-golden-testing.md`](docs/gpu-golden-testing.md) |
| Refactoring the renderer / device path | The per-unit verification loop in [`docs/gpu-golden-testing.md`](docs/gpu-golden-testing.md); log the change in [`docs/cpp-renderer-improvements.md`](docs/cpp-renderer-improvements.md) |
| "My build produced nothing" / "my deleted file still builds" | [Container reuse and delivery](#containerized-windows-builds-stevedore) — `-FreshContainer`, and the delivery check that fails the build |
| Writing a script, module, or general-purpose doc | Probably belongs upstream — [Rule: Reusable Work Belongs in ANTfrastructure](#rule-reusable-work-belongs-in-antfrastructure). Check [What ANTfrastructure owns](#what-antfrastructure-owns--links-only) before writing a procedure that may already exist |
| Bumping a submodule pin, or any dependency | `bash ./scripts/linux/renovate-local.sh` from WSL — see [Dependency upgrades](#dependency-upgrades-renovate-as-a-local-cli). Never a bare `git submodule update --remote` |
| Pushing and expecting CI to tell you something | Every lane runs on every push/PR; the three platform lanes skip docs-only commits — see [What CI runs](#what-ci-runs-and-what-it-does-not) |
| Changing the Rust WebGPU renderer | `third_party/OxidANT/crates/webgpu_renderer` — that repo owns the renderer's docs too (decision D6): [`webgpu-renderer-roadmap.md`](third_party/OxidANT/crates/webgpu_renderer/docs/webgpu-renderer-roadmap.md), which the pin bump to `ee7e5a1b` made reachable in this checkout; [on the web](https://github.com/Kataglyphis/OxidANT/blob/HEAD/crates/webgpu_renderer/docs/webgpu-renderer-roadmap.md) for a reader without the submodule |
| Touching the clouds subsystem | Pipeline shape, estimator, UBO/constants tables, queue ownership — [`docs/clouds.md`](docs/clouds.md) |

### Repo map

| Path | What lives there |
| --- | --- |
| `Src/GraphicsEngineVulkan/` | The C++ Vulkan engine (`app`, `renderer`, `vulkan_base`, `memory`, `scene`, `gui`, `window`, `common`, `util`) |
| `Src/shared/`, `Src/KomputePlayground/` | Renderer-agnostic frontend/scene/imgui/util code; the Kompute compute playground |
| `Test/commit/VulkanEngine/` | The main gtest suite (`commitTestSuite.exe`) — CPU suites plus `GoldenRender.*` / `Integration.*` |
| `Test/compile/`, `Test/fuzz/`, `Test/perf/` | Compile-time checks, FuzzTest targets, Google Benchmark suite (`perfTestSuite`) |
| `Resources/ShadersSlang/` | All shaders (Slang) + `shader-manifest.json`; compiled output under `build/` is gitignored |
| `third_party/OxidANT/crates/` | The Rust side, incl. `webgpu_renderer` and `gui` |
| `third_party/ANTfrastructure/` | The submodule that owns every reusable script, module and doc (see the rule below) |
| `scripts/windows/`, `scripts/linux/`, `scripts/agentic-loop/` | Thin project wrappers over ANTfrastructure drivers + this project's payload |
| `cmake/` | This project's build **policy** only: `ProjectOptions.cmake` (options, exceptions, CRT, C++23, modules-mandatory), `CPackOptions.cmake`, `SystemLibDependencies.cmake`. The reusable modules live in ANTfrastructure — see [CMake modules](#cmake-modules-antfrastructure-first-local-override-wins) |

### Large tracked assets

**This repository is large on purpose and it is not going to be rewritten.**
Measured 2026-09-15, outside `third_party/`: **582 MiB across 580 tracked
files**, of which **37 files account for 556 MiB** — 95% of the weight in 6% of
the files. Where it sits:

| What | Size | Why it is tracked |
| --- | --- | --- |
| 33 Wavefront meshes under `Resources/Models/` | 402 MiB | The scene library the engine's model picker offers. Four of them are test fixtures (`ShadowTest/shadow_rig.obj`, `VikingRoom/viking_room.obj`, `Dinosaurs/dinosaurs.obj`, `GltfTest/cube.glb`); `crytek-sponza/` is the only one the Release install ships. The rest are what makes a fresh clone render something without an asset-download step. |
| `Resources/Models/Sulo/New_0.9.blend` | 66 MiB | The editable source for the Sulo meshes beside it. Nothing in the build reads it; it is here so the exported `.obj` files are reproducible. |
| Textures under `Resources/` (PNG/JPG, plus one 16 MiB `.tif`) | ~87 MiB | Runtime material and IBL inputs; `Resources/Textures/` is installed with the Release build. |
| `Documents/GGD_Kit_milestone_document.pdf` | 16 MiB | Reference document, `linguist-vendored`. |
| `images/` | 11 MiB | README and homepage screenshots. |
| `docs/source/_webgpu_demo/.../kataglyphis_webgpu_renderer_bg.wasm` | 8 MiB | The prebuilt WebGPU demo the Sphinx site serves. |

The five largest single files: `buddha.obj` (89 MiB),
`Sulo/SuloLongDongLampe.obj` (82 MiB), `StanfordDragon/dragon.obj` (71 MiB),
`Sulo/New_0.9.blend` (66 MiB), `bmw/bmw.obj` (30 MiB).

**No history rewrite and no LFS** (owner decision, 2026-09-15). `filter-repo`,
`filter-branch` and BFG all invalidate every existing clone, fork and open
branch to reclaim bytes that a clone pays for once; LFS trades that for a second
thing that has to be provisioned before a checkout can build. What *is* done is
this note plus the **large-asset gate** at the bottom of `.gitignore`: DCC
project files, archives and video are ignored repo-wide, heavy image formats are
ignored under `Resources/`, and `Resources/**/*.obj` is ignored with the 33
tracked meshes listed back individually — so a 34th has to be typed rather than
swept in by `git add .`. An asset that genuinely belongs: `git add -f <path>`,
then a `!` line in `.gitignore` and a row here.

Two earlier passes already removed what was removable without touching history:
a Blender autosave twin, a duplicate `dinosaurs.obj` and a 2022 Doxygen snapshot
(`75328b2f`). Do not re-add them.

## What ANTfrastructure owns — links only

Every topic below lives in `third_party/ANTfrastructure` and is consumed from
there. **This file does not restate any of it**, and neither should the next
procedure written here: if your topic is in this table, write one sentence of
orientation and link — upstream's own instruction to consumers,
[`docs/INDEX.md` § If you are about to write a procedure in a consumer repo](third_party/ANTfrastructure/docs/INDEX.md#if-you-are-about-to-write-a-procedure-in-a-consumer-repo).

| Topic | Upstream document |
| --- | --- |
| Which repo a piece of knowledge belongs in — the *"would this still be true in a different project?"* split, with worked examples | [`INDEX.md` § Where does a piece of knowledge belong?](third_party/ANTfrastructure/docs/INDEX.md#where-does-a-piece-of-knowledge-belong) |
| The full topic → owning-document index for the whole family | [`INDEX.md`](third_party/ANTfrastructure/docs/INDEX.md) |
| Wiring another project to any of this: the loop, both container flows, launchers, CI actions | [`adopting-in-a-new-project.md`](third_party/ANTfrastructure/docs/adopting-in-a-new-project.md) |
| The one file a consumer must own itself, and why (`Resolve-BuildModule.ps1`) | [adopting § 1](third_party/ANTfrastructure/docs/adopting-in-a-new-project.md#1-the-one-file-that-cannot-live-here) |
| Agentic loop: architecture, engines, config keys, prompt composition | [adopting § 4](third_party/ANTfrastructure/docs/adopting-in-a-new-project.md#4-the-agentic-loop), [`windows-agentic-loop.md`](third_party/ANTfrastructure/docs/windows-agentic-loop.md) |
| Build matrix config, sanitizer env vars, the full matrix sweep | [`agentic-loop-build-matrix.md`](third_party/ANTfrastructure/docs/agentic-loop-build-matrix.md) |
| Calling conventions every consumer wrapper follows (`#requires`, strict mode, sourcing the hub) | [adopting § 8](third_party/ANTfrastructure/docs/adopting-in-a-new-project.md#8-calling-conventions-what-every-consumer-looks-like) |
| The Windows container image: build sequence, Stevedore setup, invariants | [`windows-builds.md`](third_party/ANTfrastructure/docs/windows-builds.md) |
| Building inside the image: transports, reuse pattern, safety rails | [`windows-container-build-performance.md`](third_party/ANTfrastructure/docs/windows-container-build-performance.md) |
| Running the Linux image locally: nerdctl, cargo cache volume, build-dir rules | [`rancher-desktop-linux-containers.md`](third_party/ANTfrastructure/docs/rancher-desktop-linux-containers.md) |
| Which CI lanes run when; the `[build-win]` / `[build-arm]` commit-message opt-ins (this repo dropped both on 2026-09-24) | [`ci-build-triggers.md`](third_party/ANTfrastructure/docs/ci-build-triggers.md) |
| Dependency upgrades family-wide: Renovate as a local CLI, what `--apply` moves and what it refuses | [`dependency-updates.md`](third_party/ANTfrastructure/docs/dependency-updates.md) |
| Reading and fixing CI status from a shell with `gh` | [`github-cli-pipeline-monitoring.md`](third_party/ANTfrastructure/docs/github-cli-pipeline-monitoring.md) |
| clang-format / clang-tidy / cmake-format rules, and the lint gates' contracts | [`code-quality-tooling.md`](third_party/ANTfrastructure/docs/code-quality-tooling.md) |
| The shared CMake modules: what each provides, how they reach `CMAKE_MODULE_PATH` | [`cmake/README.md`](third_party/ANTfrastructure/cmake/README.md) |

The [Docs](#docs) table at the end is the same list interleaved with this repo's
own pages; this section is the upstream half on its own, so *"is there already a
document for this?"* is one look rather than a scan.

---

## Fast path: Windows build, run, verify

**When building on Windows, always use Stevedore (Docker) via the container
build script.** Never invoke CMake, Ninja or MSBuild directly on the host — the
host toolchain is not what CI builds with, and host `cmake` cannot even read
this repo's presets (see below).

1. **Build** (`clangcl-debug` shown; the script defaults to all three clang-cl
   configurations):
   ```pwsh
   pwsh -ExecutionPolicy Bypass -File .\scripts\windows\Build-Windows-Container.ps1 `
     -Configurations "clangcl-debug"
   ```
   The build runs inside the reusable container `bb-build-persistent` and
   **streams the finished build trees back into the working tree** — no
   `docker cp` step is needed. Tests are skipped by default; pass `-RunTests`
   to run the CPU suites in-container.

2. **The artifacts are already on the host** at the build-directory root:
   `build-clangcl-debug\GraphicsEngine.exe`, `commitTestSuite.exe`, the fuzz
   executables, `compile_commands.json`, and `logs\`. (`bin\` holds only the
   copied ASAN runtime DLL — the executables are one level up.) Other
   configurations land in `build-clangcl-profile\` and `build-clangcl-release\`.

3. **Run on the host** — containers have no swapchain, and the working
   directory must be the **repo root** (`Resources/` is loaded via cwd-relative
   paths):
   ```pwsh
   .\scripts\windows\Invoke-ClangClDebug.ps1        # ctest + fuzz exes + launch
   .\scripts\windows\Invoke-ClangClProfile.ps1
   .\scripts\windows\Invoke-ClangClRelease.ps1
   ```
   Debug builds need the Vulkan validation layers installed on the host; see
   [Running on the Host](#running-on-the-host-windows). Profile/Release do not.

4. **If you changed rendering or the device path**, a green build proves
   nothing about behaviour — the GPU suites *skip* in the container. Run them
   on the host and follow the verification loop in
   [`docs/gpu-golden-testing.md`](docs/gpu-golden-testing.md).

---

## Build System Overview

Everything is driven by `CMakePresets.json`. Do not invent ad-hoc CMake command
lines; pick a preset.

**The repository and the CMake project are not spelled the same, on purpose.**
`GraphicsEngine` is the CMake project and executable name (`CMakeLists.txt:8`),
matching the source tree it builds, `Src/GraphicsEngineVulkan/`; Sphinx
(`docs/source/conf.py:96`) and Doxygen (`Doxyfile.in:45`) both publish under
`BeschleunigerBallett`, the repository name. Build directories, preset names and
the MSIX package (`Msix.PackageNameDefault` in
`scripts/windows/Build-Windows.config.psd1`) follow the CMake name; nothing here
is a leftover to be unified.

The version behind both is the repo-root **`VERSION.txt`**, read by
`CMakeLists.txt:4` (into `project(... VERSION ...)` and the `PROJECT_VERSION`
compile definition), `docs/source/conf.py:99` and
`scripts/windows/Build-Windows.ps1` (the MSIX package version, a hard error if
the file is missing). Bump it with `bash ./scripts/linux/bump-version.sh <x.y.z>`
and nothing else — no reader keeps a copy of the number.

> **Trap:** `cmake --list-presets` does **not** work on this host. The host
> CMake (3.29) cannot read `CMakePresets.json` (`"version": 10`,
> `cmakeMinimumRequired` 4.1) and fails with
> `CMake Error: Could not read presets ... Unrecognized "version" field`
> (verified 2026-08-02). Only the container's newer CMake can. Read the file, or
> the table below — that error is not a broken checkout.

### Windows configurations (Build-Windows.ps1)

`scripts/windows/Build-Windows.ps1` is the single entry point for Windows builds
(the container script invokes it inside the image). Its `-Configurations` names
(comma-separated) map to presets and build directories via
`scripts/windows/Build-Windows.config.psd1`:

| Configuration | Preset | Build dir | What you get |
| --- | --- | --- | --- |
| `clangcl-debug` | `x64-ClangCL-Windows-Debug` | `build-clangcl-debug` | Debug + **ASAN** + UBSan, FuzzTest fuzzing mode |
| `clangcl-profile` | `x64-ClangCL-Windows-Profile` | `build-clangcl-profile` | RelWithDebInfo + tests/benchmarks |
| `clangcl-release` | `x64-ClangCL-Windows-Release` | `build-clangcl-release` | Release + CPack packaging |
| `msvc-debug` | `x64-MSVC-Windows-Debug` | `build-msvc-debug` | MSVC (cl) builds, optional steps |
| `msvc-release` | `x64-MSVC-Windows-Release` | `build-msvc-release` | MSVC (cl) builds, optional steps |

There is also an `x64-ClangCL-Windows-Debug-ASan` preset (AddressSanitizer
without the fuzzing-mode extras; not wired into `Build-Windows.config.psd1`).
Test presets (`test-<configure-preset>`) exist for exactly three configurations:
Debug, Debug-ASan and Profile. The plain-Clang
`x64-Clang-Windows-{Debug,Profile,RelWithDebInfo}` presets were removed in
2026-07 as unused duplicates of the ClangCL set. `x64-Clang-Windows-Release`
stays: the `windows-clang-release-wix` package preset builds on it.

**Coverage is OFF for every ClangCL preset** (`myproject_ENABLE_COVERAGE` in
`x64-ClangCL-Windows-Base`, 2026-09-24). The image compiles with its patched LLVM,
which ships no `clang_rt.profile` (ANTfrastructure's `Build-LlvmFromSource.ps1`
builds it with `COMPILER_RT_BUILD_PROFILE=OFF`: the profile runtime does not
compile under clang-cl). With coverage on, the option's default, configure
stopped at `Coverage was requested, but the clang-cl profile runtime is missing`
(run 36042436962). No Windows step consumed coverage; the Linux lanes' coverage
job is where it is measured. Turn it back on once the image ships that runtime
(ANTfrastructure backlog CON9).

**No `find`/`count`/`remove` on a defaulted-`==` struct under ClangCL.** The
image's MSVC STL 14.51 takes its vectorized path for any type clang calls
trivially equality-comparable, then `static_assert`s (`unexpected size`) unless
the type is 1, 2, 4 or 8 bytes (microsoft/STL#6294, open). Run 36052511207 stopped
there on `findSampler`'s 24-byte `SamplerKey`. Use `find_if` with a predicate,
which is a plain loop. The same gate guards `count`, `remove`, `remove_copy` and
`replace` and their `ranges::` forms, so the rule covers them too.

Typical full sweep (ASAN debug, profile, release):

```pwsh
pwsh -ExecutionPolicy Bypass -File .\scripts\windows\Build-Windows.ps1 `
  -Configurations "clangcl-debug,clangcl-profile,clangcl-release" `
  -SkipFormat -SkipTidy -SkipTests -SkipPerfTests -SkipMsix
```

### Sanitizer semantics (do not guess — this is how it actually works)

- Sanitizer flags are applied **only to the Debug configuration**, and the guard
  is in **one** place: every branch of `myproject_enable_sanitizers`
  (`third_party/ANTfrastructure/cmake/Sanitizers.cmake`) emits its flags through
  `$<$<CONFIG:Debug>:...>` — the GCC/Clang `-fsanitize=` pair, the clang-cl
  compile/link/define set, and the MSVC `/fsanitize=` pair alike. Profile and
  Release builds are never sanitized.
- The *call site* is deliberately unconditional:
  `myproject_apply_sanitizers(myproject_options)` in `cmake/ProjectOptions.cmake`
  runs for every build type. That is not a contradiction of the line above and
  not a bug to "fix" — the generator expressions one level down decide. A
  `CMAKE_BUILD_TYPE` test around the call would only restate them.
- ASAN and UBSan default **ON** for Debug builds (Linux GCC/Clang, MSVC, clang-cl);
  see `myproject_default_debug_sanitizers` in `cmake/ProjectOptions.cmake`.
- Linux TSan presets (`linux-debug-tsan-clang` / `linux-debug-tsan-GNU`) set
  `myproject_ENABLE_SANITIZER_THREAD=ON` and force
  `myproject_ENABLE_SANITIZER_ADDRESS=OFF` (TSan and ASAN are mutually exclusive —
  `Sanitizers.cmake` drops TSan if ASAN is on). **These are the only builds that
  detect data races.**
- **clang-cl does not support TSan** (`x86_64-pc-windows-msvc`):
  `Sanitizers.cmake` warns and drops the request, producing a plain Debug binary.
  A Windows `clangcl-tsan` preset existed for "preset parity" until 2026-07-20
  and was removed — it silently built a duplicate of `clangcl-debug`, cost ~185 s
  per full build, and its green runs read as evidence of race-freedom.
- **On clang-cl, UBSan only works together with ASAN — never standalone.** With
  ASAN on, the UBSan handlers are folded into `clang_rt.asan_dynamic` (release
  CRT, `/MD`), and three places switch the whole Debug build to the release CRT,
  keyed on `myproject_ENABLE_SANITIZER_ADDRESS`: the `/MDd` strip and the
  `CMAKE_MSVC_RUNTIME_LIBRARY` override in `cmake/ProjectOptions.cmake`, plus the
  Rust-bridge `CXXFLAGS` in `Src/CMakeLists.txt`
  (`_myproject_configure_windows_rust_crate`). Standalone UBSan instead pulls
  `clang_rt.ubsan_standalone*`, which is built `MT_StaticRelease` (static CRT) —
  it can never link against this project's `/MD`/`/MDd` dependency mix; lld-link
  fails with `/failifmismatch` on `RuntimeLibrary`/`_ITERATOR_DEBUG_LEVEL`
  (verified 2026-07-16). So on Windows: enable UBSan only alongside ASAN, and
  turn both off together.

## Containerized Windows Builds (Stevedore)

Windows builds run inside the ANTfrastructure developer image
`ghcr.io/kataglyphis/kataglyphis_beschleuniger:winamd64` (clang-cl, CMake, Ninja,
Vulkan SDK, Rust, sccache — everything preinstalled). CI does exactly this
(`.github/workflows/windows-x64.yml`).

**All Windows-container knowledge lives in ANTfrastructure** — do not restate it
here. When you do not know which document owns a topic, start at
[`third_party/ANTfrastructure/docs/INDEX.md`](third_party/ANTfrastructure/docs/INDEX.md),
which maps topic → owning document; linking through it keeps these references
valid when upstream reorganises. For this section, two documents cover it:

- [`third_party/ANTfrastructure/docs/windows-builds.md`](third_party/ANTfrastructure/docs/windows-builds.md)
  — the image itself: build sequence, Stevedore setup, invariants.
- [`third_party/ANTfrastructure/docs/windows-container-build-performance.md`](third_party/ANTfrastructure/docs/windows-container-build-performance.md)
  — building *inside* it: **both transports and how to set each one up**
  (§ Transports), the reusable-container pattern and its safety rails, why
  sccache cannot cache a C++23 modules build, why a named volume cannot be a
  CMake build directory, the Windows path limit that silently truncates tar
  transfers, `docker exec` bypassing the entrypoint, and the wcifs teardown
  lock.

What you need in hand to not get burned:

- Default transport is a **tar-pipe into the reusable container
  `bb-build-persistent`**; `-UseBindMount` opts into a bind mount instead
  (measured slower on this Dev Drive host — measure before switching).
- **A file deleted on the host keeps building in a reused container** — sources
  are overwritten in place, never pruned. Use `-FreshContainer` after deleting
  files, and after ANY C++23 module-interface change (see the fresh-container
  rule in [`docs/gpu-golden-testing.md`](docs/gpu-golden-testing.md)).
- **A green build is not proof anything was produced or delivered.** Both halves
  have failed silently here. The script compares the executables present in the
  container against those that reached the host and **fails the build** if the
  container produced none, or if any did not arrive. Do not "fix" that check.
- The script takes `-Configurations` (comma-separated), **not** `-Preset`.
- Reset a wedged incremental build with `docker rm -f bb-build-persistent`.

This repo's measured numbers, the `KATAGLYPHIS_KEEP_BUILD_ROOT=1` contract, the
`cargo/` exclusions and the rest of the wiring:
[`docs/container-build-caching.md`](docs/container-build-caching.md).

## Linux Builds

`scripts/linux/cmake-configure-build.sh` wraps configure+build
(`--preset linux-debug-clang`, `--build-dir build`, …). TSan is selected by
preset only (`linux-debug-tsan-clang`, `linux-debug-tsan-GNU`) — there is no
script flag for it; `--use-thread-sanitizer` errors out on purpose rather than
silently no-op'ing. There is also a `linux-debug-asan-clang` preset
(AddressSanitizer + UBSan, used in CI and fuzz-test integration). Coverage,
ctest, perf, static-analysis and wasm wrappers live next to it; Vulkan SDK env
can be injected with `--vulkan-setup-script`.

The Slang precompile is `cmake-configure-build.sh`'s pre-build hook and a
failure there is **fatal** (use `--allow-prebuild-failure` only deliberately —
a silent `|| warn` once left CI green with no SPIR-V at all).

### Running the Linux build locally (Rancher Desktop)

Two things that will bite you locally, both documented with the full recipe in
[ANTfrastructure § Persisting the cargo cache](third_party/ANTfrastructure/docs/rancher-desktop-linux-containers.md#persisting-the-cargo-cache):
pass `--build-dir /tmp/...` (a build dir on the bind-mounted host tree breaks
FetchContent renames and cargo cleanup), and `--cargo-cache-dir` at a named
volume so Rust dependencies survive the container.

## Shaders: Slang (unified SPIR-V + WGSL)

All shaders are Slang under `Resources/ShadersSlang/`, compiled ahead of time
to **SPIR-V** for the C++ Vulkan renderer and **WGSL** for the Rust WebGPU
renderer. The shader list is data, in `shader-manifest.json` — add or retarget a
shader by editing the JSON, never by editing the compile scripts. Compiled
output is gitignored, so a fresh clone must run a compile script before the
engine has anything to load. Build step, staleness rules, and fast shader
iteration: [`docs/shader-build-pipeline.md`](docs/shader-build-pipeline.md);
sharing shader code between the two renderers:
[`docs/shader-sharing.md`](docs/shader-sharing.md).

## Rule: Reusable Work Belongs in ANTfrastructure

**Before writing a script, module, or doc here, ask whether another project
could use it. If yes, it goes into `third_party/ANTfrastructure` and
this repo consumes it — never a copy.**

That is the local form of the rule ANTfrastructure states canonically as *"would
this still be true in a different project?"*. Which side of the line a given
thing falls on — PowerShell, Bash, container or Windows-container knowledge, a
trap learned the hard way — is settled there, with the worked splits and the
three-broken-copies case that motivated the rule:
[`INDEX.md` § Where does a piece of knowledge belong?](third_party/ANTfrastructure/docs/INDEX.md#where-does-a-piece-of-knowledge-belong).
The destinations, so the link is not a scavenger hunt: `windows/scripts/modules/`
for PowerShell, `linux/scripts/lib/` or `linux/scripts/01-core/` for Bash,
`docs/` for everything else.

What stays here: engine code, shaders, this project's presets, and the
*payload* the shared drivers execute (build-directory names,
`Build-Windows.ps1` arguments, project-specific exclusions, the Slang
precompile hook) — plus `Resolve-BuildModule.ps1` itself, the bootstrap that
*finds* ANTfrastructure and therefore cannot live inside it
([adopting § 1](third_party/ANTfrastructure/docs/adopting-in-a-new-project.md#1-the-one-file-that-cannot-live-here)).

### The wrapper map

Almost every script under `scripts/` is a thin consumer. **When you need to know
what one actually does, read the upstream driver, not the wrapper** — the
wrapper only supplies this project's payload.

| This repo | Upstream driver (in `third_party/ANTfrastructure/`) |
| --- | --- |
| `scripts/windows/Build-Windows-Container.ps1` | `windows/scripts/modules/WindowsContainerBuild.Reuse.psm1` → `Invoke-ContainerBuild` (+ `Get-ReusableBuildContainer`, `Copy-IntoBuildContainer`, `Copy-FromBuildContainer`, `Resolve-DockerExe`, `Get-ContainerIsolationArgs`, `Test-ContainerBindMount`, `Get-SccacheContainerEnv`, `Remove-BuildContainerSafe`) |
| `scripts/linux/cmake-configure-build.sh` | `linux/scripts/lib/cmake-build.sh` |
| `scripts/windows/Invoke-ClangCl{Profile,Release}.ps1` | `windows/scripts/modules/WindowsAppRunner.Common.psm1` → `Invoke-AppRun`, `Resolve-AppExecutablePath` |
| `scripts/linux/run-{debug,profile,release}.sh` | `linux/scripts/lib/app-runner.sh` (the Bash twin of the above) |
| `scripts/linux/run-static-analysis-format.sh` | `linux/scripts/lib/code-quality.sh` |
| `scripts/linux/build-coverage-{gcovr,llvm}.sh` | `linux/scripts/lib/coverage.sh` |
| `scripts/linux/wasm-size-budget.sh` / `scripts/windows/Test-WasmSizeBudget.ps1` | `linux/scripts/lib/wasm-opt.sh` / `windows/scripts/modules/WindowsWasmOpt.Common.psm1` |
| `scripts/linux/run-cargo-tests.sh` | `linux/scripts/02-toolchain/rust/cargo_test.sh` |
| `scripts/linux/run-lint-gates.sh` | `linux/scripts/run-lint-gates.sh` (→ `lint-shell.sh`, `lint-workflows.sh`, `lint-secrets.sh`, `01-core/gates.sh`) |
| `scripts/linux/renovate-local.sh` | `linux/scripts/renovate-local.sh` (Renovate as a local CLI, plus the git half that applies what it can only detect) |
| `scripts/linux/ci-image-ref.sh` | `linux/scripts/ci-image-ref.sh`; PowerShell twin `windows/scripts/modules/WindowsContainerImage.Common.psm1` → `Get-CiImageReference` |
| `scripts/windows/Invoke-SyncValidation.ps1` | `windows/scripts/modules/WindowsVulkanValidation.Common.psm1` |
| `scripts/agentic-loop/Invoke-AgenticLoop.ps1` / `scripts/agentic-loop/Run-AgenticLoop.sh` | `windows/scripts/modules/WindowsAgenticLoop.Common.psm1` / `linux/scripts/lib/agentic-loop.sh` |
| `scripts/windows/Test-AllConfigs.ps1` | `windows/scripts/modules/WindowsBuildSweep.Common.psm1` → `Invoke-SweepStep`, `Test-LinuxContainerSupport`, `Invoke-InLinuxContainerBuild`, `Write-SweepSummary` |
| `scripts/windows/tests/Repo.GeneratedArtifacts.Tests.ps1` | `windows/scripts/modules/WindowsRepoHygiene.Common.psm1` → `Get-TrackedIgnoredFile` (the pin-drift half — `Get-SubmodulePinDrift`, `Get-SubmoduleStatusLine`, `Test-SubmoduleCommitReachable` — is consumed by the hub's own suite, see § Critical Invariant: Submodule Pins) |
| `cmake/ProjectOptions.cmake` | `cmake/*.cmake` (see [`cmake/README.md`](third_party/ANTfrastructure/cmake/README.md) there) |

`Invoke-ClangClDebug.ps1` is the exception: it keeps its own flow because it
orchestrates CTest and the fuzz executables before launching — but it still
takes `Resolve-AppExecutablePath` from `WindowsAppRunner.Common`.

### CMake modules (ANTfrastructure first, local override wins)

The root `CMakeLists.txt` puts **`cmake/` then
`third_party/ANTfrastructure/cmake/`** on `CMAKE_MODULE_PATH`, and
every module is included **by name** — `include(Sanitizers)`, never
`include(cmake/Sanitizers.cmake)`. That indirection is the whole point: a module
can live in either directory without its callers changing, and a project that
needs to override an upstream module just drops a same-named file in `cmake/`.

Upstream: the modules listed in
[`third_party/ANTfrastructure/cmake/README.md`](third_party/ANTfrastructure/cmake/README.md).

Local, because each encodes **this project's policy** rather than a reusable
mechanism:

- `cmake/ProjectOptions.cmake` — the option surface and its defaults, C++23,
  exceptions always off, C++ modules mandatory (a hard `FATAL_ERROR`, not a
  fallback). It composes the upstream modules; it does not duplicate them.
- `cmake/CPackOptions.cmake` — packaging metadata and branding.
- `cmake/SystemLibDependencies.cmake` — this engine's system dependencies.

**The clang-cl `-fms-compatibility-version` pin moved upstream** into
`CompilerBuildFlags.cmake` as `MYPROJECT_CLANG_CL_MS_COMPATIBILITY_VERSION`
(default `19.51.36231`). It names the VC Tools version ANTfrastructure's Windows
image ships, so the pin and the toolchain that motivates it now live in the same
repo — bump them together. Override the cache variable to build against a
different VC Tools.

### PowerShell module resolution (ANTfrastructure first, vendored fallback)

`Build-Windows.ps1`, the run helpers and the Pester tests resolve PowerShell
modules through `scripts/windows/Resolve-BuildModule.ps1`
(`Resolve-BuildModulePath` / `Import-BuildModule`): ANTfrastructure's
`windows/scripts/modules/` first, then the project-specific fallback
**`scripts/windows/modules/`**. `Resolve-BuildModule.ps1` is a **verbatim copy**
of upstream's `shared/windows/templates/Resolve-BuildModule.ps1` — sync it,
never hand-edit it ([adopting § 1](third_party/ANTfrastructure/docs/adopting-in-a-new-project.md#1-the-one-file-that-cannot-live-here)).

**The vendored fallback directory holds no copy of anything upstream, and a
Pester case asserts it stays that way** — `Compare-Renderer.Common.psm1` is the
only thing in it, because it is this project's own. Which module moved upstream
when, and what became a parameter in the move, is in the commit history; there
is no CHANGELOG.md here and this file is not one.

One trap is worth carrying locally because it bites at the call site:
`Import-BuildModule` imports `WindowsScripts.Shared` unconditionally, whether or
not you list it. That is not belt-and-braces — a nested `Import-Module` inside a
`.psm1` binds into *that module's* private scope and never reaches the importing
session, so a script that imports only `WindowsBuild.Common` would otherwise get
`Write-BuildLog` but **not** `Resolve-WorkspacePath`. The conventions around it
are upstream's: [adopting § 8](third_party/ANTfrastructure/docs/adopting-in-a-new-project.md#8-calling-conventions-what-every-consumer-looks-like).

### Shipping a change that spans both repos

Both repos are committed and pushed together, ANTfrastructure **first** (CI
resolves its composite actions at `@develop`), and the submodule pin is bumped in
the same change.

**Adopting any of this in another project** — the loop, both container flows,
the launchers, the CI actions — is a checklist upstream:
[`adopting-in-a-new-project.md`](third_party/ANTfrastructure/docs/adopting-in-a-new-project.md).
Read it before hand-rolling equivalents elsewhere.

## Critical Invariant: Submodule Pins

Builds are only supported against the **recorded submodule gitlinks** — the commits CI
builds green. `git submodule update --checkout --recursive` restores every pin. If a
drifted submodule is what you actually want, update the gitlink AND fix the fallout in
the same change.

Known coupling to watch when bumping pins:

- `third_party/FUZZTEST` pins its own Abseil LTS (`absl_TAG` in
  `cmake/BuildDependencies.cmake`); this repo's `ABSL_TAG` in
  `third_party/CMakeLists.txt` is declared first and wins, so it must stay >= the
  FuzzTest pin or configure fails with missing `absl::*` targets (observed:
  `absl::random_mocking_access`). Both are `20260526.0` today.

Drift itself is guarded by ANTfrastructure's repo-agnostic suite,
`third_party/ANTfrastructure/shared/windows/tests/Submodule.Pins.Tests.ps1`, run
by its own always-on workflow, `.github/workflows/submodule-pins.yml` — push
and PR on `main`/`develop`, no `[build-win]` opt-in and **no `paths-ignore`**,
because an invariant that only runs when somebody remembers to type a marker is
not a gate, and neither is one a path filter can silence (it used to live in
`windows-x64.yml` and inherited that file's `'**.md'`/`docs/**` filter, so a commit
that moved a gitlink alongside a docs page went unchecked). For **every** configured submodule it
asserts the tree is checked out, sits at its recorded gitlink, and is pinned to
a commit reachable from its remote (a pin on no remote branch cannot be
restored by a fresh clone). This repo's own copy of the suite was deleted once
the hub pin carried the promoted one; do not re-add a local fork. To run it by
hand: `$env:ANTFRASTRUCTURE_PIN_CHECK_REPO_ROOT = $PWD; Invoke-Pester
third_party/ANTfrastructure/shared/windows/tests/Submodule.Pins.Tests.ps1`. It
does **not** check the Abseil version coupling above — that one is on you.

### Dependency upgrades: Renovate as a local CLI

**Dependency upgrades go through this wrapper, not by hand:**

```bash
bash ./scripts/linux/renovate-local.sh                    # what is behind
bash ./scripts/linux/renovate-local.sh --apply --dry-run  # the plan
bash ./scripts/linux/renovate-local.sh --apply            # move the gitlinks
```

Run it from WSL — there is no node on the Windows host. How it behaves there
(why `--apply` is git rather than Renovate, the `git.exe` switch it makes for
you over a Windows checkout, that nothing is staged or committed either way) is
upstream's, not this repo's:
[`dependency-updates.md`](third_party/ANTfrastructure/docs/dependency-updates.md).

What is true of **this** repo: it moves gitlinks, and only submodules that declare a `branch =`. Any other
manager (`--managers`) is report-only; `requirements.txt` is one of those. `.github/renovate.json` is
read by this CLI and by nothing else: the Renovate GitHub App is installed on no
repository in this family and will not be (owner decision, 2026-09-09).
Rationale, the full local workflow and the GitHub-token variant:
[`third_party/ANTfrastructure/docs/dependency-updates.md`](third_party/ANTfrastructure/docs/dependency-updates.md).

## Running on the Host (Windows)

Containers cannot present a swapchain — run the built binaries on the bare host,
from the **repo root** as working directory (`scripts/windows/Invoke-ClangCl*.ps1`
wrap this). Verified 2026-07-17 on the AMD RX 9070 XT: all four clang-cl builds
render (~32 FPS ImGui overlay).

- **Debug builds require the Vulkan validation layers on the host** — without
  them the app dies at startup with exit code `-1073740791` (`0xC0000409`,
  vulkan.hpp assert after "Validation layers requested, but not available!").
  Install the Vulkan SDK (`winget install VulkanSDK`; 1.4.350.0 is what
  `Invoke-SyncValidation.ps1` defaults to), or point `VK_LAYER_PATH` at a directory
  containing `VkLayer_khronos_validation.{dll,json}` (they can be extracted from
  the ANTfrastructure image under
  `C:\Users\ContainerAdministrator\scoop\apps\vulkan\current\Bin`).
  Profile/Release builds run without validation layers.
- The ASAN debug binary needs `clang_rt.asan_dynamic-x86_64.dll`; the build
  copies it next to `GraphicsEngine.exe`, and the run helpers set `ASAN_OPTIONS`
  with a **relative** `log_path` (an absolute `C:\...` path breaks ASAN option
  parsing at the drive-letter colon).

## Testing

- C++ tests: `ctest --test-dir <build-dir> --output-on-failure` (add `-C Debug` for
  multi-config generators). `Build-Windows.ps1` runs them unless `-SkipTests`.
  Host `ctest` cannot read a container-generated CMake tree — invoke the test
  executable directly instead (`.\build-clangcl-debug\commitTestSuite.exe`).
- Benchmarks: `clangcl-profile` builds `perfTestSuite.exe`; run via
  `Build-Windows.ps1` without `-SkipPerfTests`. `scripts/windows/Compare-PerfBaseline.ps1`,
  `Compare-RendererPixels.ps1` and `Compare-RendererTimings.ps1` are local-only
  comparison tools (CI runs them in validation-only mode — the runners have no GPU).
- PowerShell module tests: Pester suites under `scripts/windows/tests/`
  (Pester 3.4 syntax; the `pester-tests` job of `.github/workflows/windows-x64.yml`
  runs them with a pinned Pester 3.4.0 on every push and PR, like the rest of
  Windows CI). **Only suites covering
  project-specific behaviour belong here.** A suite for a module that lives
  upstream goes upstream with it (2026-08-07); the eight that remain cover the
  module-resolution bootstrap (`Resolve-BuildModule`), the preset, artifact and
  shared-config guards (`CMakePresets.Integrity`, `Repo.GeneratedArtifacts`,
  `SharedConfig.Drift`), the validation-layer runner (`Invoke-SyncValidation`)
  and the three comparison tools (`Compare-PerfBaseline`,
  `Compare-RendererPixels`, `Compare-RendererTimings`).
- `scripts/windows/Test-AllConfigs.ps1` is a local one-shot gate: the three standard
  Windows container builds plus the Linux TSan build (`-SkipLinux` drops the
  latter). Not wired into CI.
- ANTfrastructure's own suites (the modules this repo imports) run via
  `third_party/ANTfrastructure/windows/scripts/tests/Invoke-Tests.ps1`
  and need **Pester >= 5** (it fails rather than silently skipping without it;
  `Install-Module Pester -MinimumVersion 5.7 -Scope CurrentUser -Force
  -SkipPublisherCheck`). Run it after changing anything upstream — and note that
  upstream has its own CI for them (`windows-scripts.yml`), so a break there is
  caught without waiting on this repo's 2-3 h Windows lane.
- **GPU tests** (`GoldenRender.*`, `Integration.*`) skip in containers and run
  only on the host — procedure, cwd requirement and the golden-writing
  cautions are in [`docs/gpu-golden-testing.md`](docs/gpu-golden-testing.md).
  Known trap: over an RDP session the swapchain reports zero images and every
  golden fails with "No synchronization frames available" — that is the
  session, not a renderer regression (see `BACKLOG.md`).
- **Synchronization validation** catches missing/incorrect barriers that no
  pixel oracle can see (it found 10 real WRITE-AFTER-WRITE hazards in July
  2026). Run it after touching render passes, barriers, or frames-in-flight:
  ```pwsh
  pwsh -ExecutionPolicy Bypass -File .\scripts\windows\Invoke-SyncValidation.ps1
  ```
  It exits non-zero iff the run log contains `SYNC-HAZARD`. Deliberately not in
  CI (needs a GPU) — details in
  [`docs/gpu-golden-testing.md`](docs/gpu-golden-testing.md).

**Adding tests is always in scope.** You do not need permission to improve
or extend the suites — a change that fixes behaviour should generally arrive
with a test that would have caught it. Prefer assertions that survive driver
and machine differences (structural pixel properties, invariants, ordering)
over exact-value comparisons, and make GPU-dependent tests skip themselves
when no adapter is present rather than fail. Ideas worth picking up live in
[`BACKLOG.md`](BACKLOG.md), alongside the sized commitments.

**Formatting and static analysis.** clang-format/clang-tidy/cmake-format
commands, the host gotchas, the container-build behavior (clang-format always
runs; clang-tidy never does), and the suggested cadence live in
[`docs/code-quality.md`](docs/code-quality.md).

**Run more than the debug loop periodically.** `clangcl-debug` is the fast
default, but `clangcl-profile` (optimized, and the only configuration where
benchmarks mean anything) and a synchronization-validation pass each catch
classes of problem the debug loop cannot. See [`BACKLOG.md`](BACKLOG.md) for
what each one is for.

<a id="there-is-no-windows-threadsanitizer"></a>
**There is no Windows ThreadSanitizer.** Use the Linux
`linux-debug-tsan-clang` preset, which CI runs, for race detection — see
"Sanitizer semantics" above.

## What CI runs, and what it does not

Every lane runs on every push/PR to `main`/`develop`. The `[build-win]` and
`[build-arm]` commit-message opt-ins are gone (owner decision 2026-09-24: Linux
x64, Linux arm64 and Windows x64 always run). The three platform lanes still skip
a docs-only commit through `paths-ignore`:

| Lane | Workflow | Trigger |
| --- | --- | --- |
| Lint gates (`lint` + `powershell-lint`) | `lint-gates.yml` — `lint` is one `uses:` of ANTfrastructure's reusable lane, with `ratchets: true` | always, **including docs-only commits** — no `paths-ignore` |
| Submodule pins | `submodule-pins.yml` — one `uses:` of ANTfrastructure's reusable lane | always, no `paths-ignore` |
| Linux x86_64 (build + test + coverage) | `linux-x64.yml` → `reusable-linux.yml` | always, minus `'**.md'`/`docs/**` |
| Windows (clang-cl/MSVC container build, Pester) | `windows-x64.yml` | always, minus `'**.md'`/`docs/**` |
| Linux ARM64 | `linux-arm64.yml` → `reusable-linux.yml` | always, minus `'**.md'`/`docs/**`; deploys nothing (the deploy jobs need `runner == 'ubuntu-26.04'`) |

The top two are their own workflows rather than jobs inside the build lanes,
and that is what makes "always" true. As tenants they inherited their host's
`paths-ignore`, so a docs-only push got no secret scan and a commit that moved a
gitlink next to a docs page got no pin check; `lint` also lived in
`reusable-linux.yml` and therefore ran once per **caller**, grading the same
runner-independent tree twice on every ARM run.

Workflow files follow the fleet naming convention (owner decision 2026-09-24):
kebab-case, one file per platform + arch (`linux-x64.yml`, `linux-arm64.yml`,
`windows-x64.yml`), repo-local reusables as `reusable-<platform>.yml`, and
display names `<Platform> <Arch> · <what>`. The job ids are unchanged, so the
check-run names a branch protection rule matches did not move; badge and
`actions/workflows/<file>` URLs did.

To re-run a lane at the branch tip without a commit (a fix to a docs path the
filter skips, say), use `workflow_dispatch` on `linux-x64.yml` or
`windows-x64.yml`. The hub's trigger rules, including the opt-in markers other
repos still use:
[`ci-build-triggers.md`](third_party/ANTfrastructure/docs/ci-build-triggers.md).
Reading pipeline status from a shell (`gh`):
[`github-cli-pipeline-monitoring.md`](third_party/ANTfrastructure/docs/github-cli-pipeline-monitoring.md).

`linux-x64.yml` and `linux-arm64.yml` both call `reusable-linux.yml`, so a fix
to the x86 lane applies to ARM automatically. No CI lane has a GPU — the golden
and synchronization suites are host-only by construction.

### The job bodies are ANTfrastructure's, resolved at `@develop`

The workflows here are mostly wiring: the actual steps come from composite
actions pulled straight from ANTfrastructure's `develop` branch (owner directive
2026-09-25; `main` lags it and still names the retired `:latest-cross` image) —
`prepare-linux-ci-host`, `run-in-linux-container`,
`prepare-windows-container-host`, `run-in-windows-container`,
`run-pester-suite`. **There is no pin: a push to ANTfrastructure `develop` changes
this repo's CI on the next run**, which is why both repos ship together with
ANTfrastructure first (see the rule above). When a lane fails inside a step whose
`uses:` points at ANTfrastructure, read the action there — it is not defined here.

### Lint gates (before anything builds)

`.github/workflows/lint-gates.yml` is a workflow of its own, not a job inside a
build lane. It pulls no image and builds nothing (~2 min), and it catches a
class the rest of CI cannot: `yaml.safe_load` proves a workflow is valid YAML,
not valid Actions, and a bash quoting or undefined-function bug only surfaces
when that line finally runs — an hour into a gcc build.

Its `lint` job is **one `uses:` line**: ANTfrastructure publishes the lane as a
`workflow_call` workflow, this repo passes `submodules: 'true'` (not the hub's
`recursive` default — only ANTfrastructure is executed here) and
`ratchets: true`. It carries the Windows-native `powershell-lint` job too
(PSScriptAnalyzer plus the mandatory parse/AST-trap gate over `scripts/`), on
`windows-2025` because that gate script resolves its own helper module by
backslash path. That one is deliberately **not** collapsed onto the hub's
equivalent: the hub's passes `-FailOnAnalyzer` unconditionally, and this tree
has one analyzer finding left (`$WebDavPassword` should be a `SecureString`),
so adopting it today would make the lane red over a change to the build
driver's public parameter contract. The file says so at the job.

**One command, and it is the same one CI runs:**

```bash
bash ./scripts/linux/run-lint-gates.sh            # the six always-on gates
bash ./scripts/linux/run-lint-gates.sh --ratchets # + doc-links and the eight measurement gates
```

That is the whole `lint` job; CI passes `--ratchets` too. The wrapper hands this repo's root to
ANTfrastructure's `linux/scripts/run-lint-gates.sh`, which owns the six gates
(shell, workflows + CI image refs, secrets, python, shared-config drift,
consumer pin-forwarding — the list and scopes are in that script's header) and
the scaffolding around them; the binaries (shellcheck, actionlint, gitleaks) are
ANTfrastructure's pinned, SHA-verified bootstraps, not a second set installed here.
The bootstrap downloads once and caches, so only the first local run is slow.

Four properties of that job are load-bearing. All four are asserted upstream
rather than assumed here, so this is one line each plus the link to the
reasoning:

- **Scopes come from `git ls-files`, never a glob** — measured 2026-09-07, the
  globs this replaced graded 19 of the 21 tracked scripts while reading as if
  they graded all of them (`scripts/agentic-loop/Run-AgenticLoop.sh` and the
  then-repo-root `bump-version.sh` were the two that fell through).
- **The consumer root is passed explicitly**, because the gate scripts live
  *inside* the submodule and a root inferred from their own location resolves to
  ANTfrastructure: [`code-quality-tooling.md` § The scan-root contract](third_party/ANTfrastructure/docs/code-quality-tooling.md#the-scan-root-contract).
- **An empty file list is a failure, not a pass** — upstream's phrasing is *zero
  files is a refusal, not a green*: [`code-quality-tooling.md`](third_party/ANTfrastructure/docs/code-quality-tooling.md).
- **The secret gate self-tests before it scans**, and matches its canary by path
  rather than by outcome: [`code-quality-tooling.md` § The secret scan scans from inside the tree](third_party/ANTfrastructure/docs/code-quality-tooling.md#the-secret-scan-scans-from-inside-the-tree).

All gates run even after one fails, and the verdict is decided once at the
end, so a triage round sees every finding instead of only the first.

**`--ratchets` is on.** It adds `doc-links` plus the eight `--root` measurement
gates (stdout-returns, masked assignments, trailing conditionals, comment size,
code size, complexity, dead functions, shellcheck warnings). Four freeze files
at the repo root hold what was over the line when the flag went on —
`comment-size.allow`, `dead-functions.allow`, `trailing-conditional.allow`,
`shellcheck-warnings.allow` — and each is **two-way**: a new offender fails, and
so does a frozen row whose offender is gone. `doc-links` has no freeze file by
design, so its findings are fixed, never frozen.

The `ubuntu-26.04` leg of the Linux lane also runs the Rust renderer crate's
own test suite (`scripts/linux/run-cargo-tests.sh`, `cargo test -p
kataglyphis_webgpu_renderer`) after the performance benchmarks step. Before
this, the crate was compiled twice in this repo (the Rust bridge and the wasm
demo) but its ~150 tests only ran in `OxidANT`'s own
workflow — so edits made to `crates/webgpu_renderer` from this working tree
got no test signal until the submodule was pushed separately.

## Code Conventions (C++ engine)

- **Exceptions are disabled project-wide** (`/EHs-`, `-fno-exceptions` in
  `cmake/ProjectOptions.cmake`; `VULKAN_HPP_NO_EXCEPTIONS` in
  `Src/GraphicsEngineVulkan/CMakeLists.txt`): vulkan.hpp calls return
  `ResultValue`; `throw`/`try` will not compile. `ASSERT_VULKAN(val, "msg")`
  (`common/Utilities.hpp`) logs critical and aborts — use it on
  creation/allocation calls only.
- Graphics pipelines are built via `kataglyphis.vulkan.pipeline_builder`
  (`vulkan_base/PipelineBuilder.ixx`) — do not hand-roll the create-info chain.
- Buffer/image memory goes through VMA (allocator owned by `VulkanDevice`);
  `VulkanBuffer`/`VulkanImage` are move-only with destructor release
  (`cleanUp()` remains for explicit early teardown and is idempotent).
- A `VkPipelineCache` persists to `pipeline_cache/kataglyphis_pipeline.cache`
  (gitignored, written on graceful shutdown only).
- Slang emits `"main"` as the SPIR-V entry point name (not the Slang function
  name), so all `pName` values in pipeline creation use `"main"`.
- Model-loading architecture (the two loaders, the async parse/upload split,
  the multi-mesh flow): [`docs/model-loading.md`](docs/model-loading.md).
- Per-unit verification pattern (container build -> direct test exe ->
  validation run): [`docs/gpu-golden-testing.md`](docs/gpu-golden-testing.md).
  The chronological log of what changed and why:
  [`docs/cpp-renderer-improvements.md`](docs/cpp-renderer-improvements.md).
  Do not restate either here — those documents are the source of truth.

## Code Conventions (PowerShell scripts)

The rules are upstream's, recorded for every consumer in
[`adopting-in-a-new-project.md`](third_party/ANTfrastructure/docs/adopting-in-a-new-project.md).
They are restated here because the next new script gets written in *this* repo,
and the exceptions below are this repo's.

- **Every `.ps1` and `.psm1` declares `#requires -Version 7.0`**, and every
  tracked file outside `third_party/` carries it — the rule, and why a version
  header beats failing deep inside a pwsh-7 construct, are upstream's:
  [adopting § 8](third_party/ANTfrastructure/docs/adopting-in-a-new-project.md#8-calling-conventions-what-every-consumer-looks-like).
- **Executable scripts are PascalCase `Verb-Noun`, with a verb `Get-Verb`
  approves** — `Build-Windows.ps1`, `Invoke-ClangClRelease.ps1`,
  `Resolve-BuildModule.ps1`, `Test-AllConfigs.ps1`, `Compare-RendererPixels.ps1`.
  Spell the case exactly when you reference one: Windows ignores it, but git
  pathspecs, ripgrep globs and the Linux lane do not, so a lowercase spelling in
  a doc or workflow matches nothing.
- **Two shapes deliberately break the plain `Verb-Noun` mould, and are not bugs.** Pester suites are
  `<Subject>.Tests.ps1` — mirroring the script under test where there is one
  (`Resolve-BuildModule.Tests.ps1`, `Invoke-SyncValidation.Tests.ps1`), or naming
  the repo property under test where there is not
  (`Repo.GeneratedArtifacts.Tests.ps1`, `SharedConfig.Drift.Tests.ps1`,
  `CMakePresets.Integrity.Tests.ps1`). Modules take the dotted noun form
  `<Area>.Common.psm1` — `scripts/windows/modules/Compare-Renderer.Common.psm1`, matching
  ANTfrastructure's `Windows<Area>.Common.psm1` — and export explicitly via
  `Export-ModuleMember`.
- **Bash is not renamed to match**: `Get-Verb` is a PowerShell notion, so the
  shell scripts stay kebab-case (`scripts/linux/run-release.sh`,
  `build-coverage-llvm.sh`). The lone exception is
  `scripts/agentic-loop/Run-AgenticLoop.sh`, spelled to sit beside its
  PowerShell twin `Invoke-AgenticLoop.ps1`.

## Agentic Loop

An autonomous planner/executor loop continuously improves the engine. The
**planner** (expensive, powerful model) analyzes the codebase and writes
detailed task entries to `BACKLOG.md`. The **executor** (cheap, fast model)
drains the queue one task at a time, building and testing as it goes. The
queue must be fully drained before the planner adds new tasks; tasks marked
`- [b]` are blocked and do not count as pending.

Two engines are selectable via `engine` in
`scripts/agentic-loop/AgenticLoop.config.json`, `-Engine`/`--engine`, or the
`AGENTIC_ENGINE` env var: **`claude`** (default — Claude Code CLI, Opus 5
planner with Fable 5 fallback, Sonnet executor) and **`opencode`** (GLM 5.2
planner, DeepSeek v4 Flash executor). Both read the same role prompt, composed
from ANTfrastructure's shared prompt plus this repo's overlays in
`scripts/agentic-loop/prompts/`.

**The loop itself is upstream** — `WindowsAgenticLoop.Common` (PowerShell) and
`agentic-loop.sh` (Bash) own it, and both role prompts are single-sourced there
too. Architecture, config keys and the module API:
[adopting § 4](third_party/ANTfrastructure/docs/adopting-in-a-new-project.md#4-the-agentic-loop) and
[`windows-agentic-loop.md`](third_party/ANTfrastructure/docs/windows-agentic-loop.md); build matrix and
sanitizer-aware tests:
[`agentic-loop-build-matrix.md`](third_party/ANTfrastructure/docs/agentic-loop-build-matrix.md).

What this repo owns is a per-role **delta**:
`scripts/agentic-loop/prompts/planner-overlay.md` and `executor-overlay.md`,
declared in the config's top-level `promptOverlays` block. At start-up the loop
composes the shared system prompt plus the overlay into one text and delivers it
to `claude` via `--append-system-prompt-file`, and to `opencode` by regenerating
`.opencode/agents/<role>.md` from the pinned hub (opencode takes no prompt file
on its command line). **`.opencode/agents/` is therefore gitignored — edit the
overlay, never the generated file.** Run the loop with
`scripts/agentic-loop/Invoke-AgenticLoop.ps1` (Windows, PowerShell 7+) or
`scripts/agentic-loop/Run-AgenticLoop.sh` (Linux, needs `jq`); what this repo
configures and where it deviates from upstream defaults:
[`scripts/agentic-loop/README.md`](scripts/agentic-loop/README.md).

## Docs

Each topic has exactly one home; link, do not copy. Reusable topics live in
ANTfrastructure (see the rule above), project-specific ones here.

| Where | Owns |
| --- | --- |
| `README.md` | Repo-level orientation, feature highlights |
| `BACKLOG.md` | **All** open work: sized commitments, then unsized ideas and recurring chores |
| `AGENTS.md` (this file) | How to build/test/run here, invariants, code conventions |
| `docs/cpp-renderer-improvements.md` | C++ engine chronological change log |
| `docs/model-loading.md` | Model-loading architecture: the two loaders, async parse/upload split, multi-mesh MeshRange flow |
| `docs/webgpu-renderer-roadmap.md` | **Pointer only.** Rust WebGPU renderer status per feature; owned by OxidANT at [`third_party/OxidANT/crates/webgpu_renderer/docs/webgpu-renderer-roadmap.md`](third_party/OxidANT/crates/webgpu_renderer/docs/webgpu-renderer-roadmap.md) (decision D6) |
| `docs/webgpu-gltf-rust-plan.md` | **Pointer only.** Original WebGPU + glTF Rust renderer plan (milestones 1–5), kept for the record; owned by OxidANT at [`third_party/OxidANT/crates/webgpu_renderer/docs/webgpu-gltf-rust-plan.md`](third_party/OxidANT/crates/webgpu_renderer/docs/webgpu-gltf-rust-plan.md) |
| `docs/shader-sharing.md` | Why/how one Slang source serves both renderers, where the two diverge, and which C++ shading path reads which `ObjMaterial` field |
| `docs/shader-build-pipeline.md` | The Slang→SPIR-V/WGSL build step: manifest, staleness rules, fast iteration |
| `docs/webgpu-srgb-audit.md` | Colour-space decisions (no known deviations) |
| `docs/code-quality.md` | clang-format / clang-tidy / cmake-format commands + cadence |
| `docs/container-build-caching.md` | This repo's container transport numbers, sccache volume, incremental-build wiring |
| `docs/gpu-golden-testing.md` | GPU golden test suites, skip-without-GPU behavior, host verification loop, synchronization validation |
| `docs/path-tracing.md` | Path-tracing mode: pipeline shape, estimator, NEE, accumulation |
| `docs/clouds.md` | Volumetric clouds: pipeline shape, estimator, UBO/constants tables, queue ownership, compositing contract |
| `docs/renderer-bounds-invariant.md` | **Pointer only.** WebGPU renderer bounds invariant; owned by OxidANT at [`third_party/OxidANT/crates/webgpu_renderer/docs/renderer-bounds-invariant.md`](third_party/OxidANT/crates/webgpu_renderer/docs/renderer-bounds-invariant.md) |
| `docs/LICENSES-README.md` | Third-party license documentation (German) |
| `docs/source/` | Sphinx pages (`README.md`, `getting_started.md`, `documentation_workflow.md`, `webgpu_demo.md`, `wsl2_vulkan.rst`, `graphviz_files.rst`) |
| `scripts/agentic-loop/README.md` | Agentic loop consumer half: what this repo configures (and what deviates from upstream defaults), its two runners, its two prompt overlays — loop architecture and usage live in the two ANTfrastructure docs below |
| `third_party/ANTfrastructure/docs/windows-builds.md` | The Windows container image: build sequence, Stevedore setup, invariants |
| `third_party/ANTfrastructure/docs/windows-container-build-performance.md` | Building inside the image: transports, reuse pattern, safety rails |
| `third_party/ANTfrastructure/docs/rancher-desktop-linux-containers.md` | Running the Linux image locally: nerdctl, cargo cache volume, build-dir rules |
| `third_party/ANTfrastructure/docs/ci-build-triggers.md` | Which CI lanes run when; the `[build-win]` / `[build-arm]` commit-message opt-ins (this repo dropped both on 2026-09-24) |
| `third_party/ANTfrastructure/docs/dependency-updates.md` | Dependency upgrades family-wide: Renovate as a local CLI, what `--apply` moves and what it refuses |
| `third_party/ANTfrastructure/docs/github-cli-pipeline-monitoring.md` | Reading and fixing CI status from a shell with `gh` |
| `third_party/ANTfrastructure/docs/adopting-in-a-new-project.md` | Wiring another project to the loop, both container flows, launchers, CI actions |
| `third_party/ANTfrastructure/docs/agentic-loop-build-matrix.md` | Build matrix config, sanitizer env vars, full matrix sweep |
| `third_party/ANTfrastructure/docs/windows-agentic-loop.md` | WindowsAgenticLoop.Common module API + config reference |
| `third_party/ANTfrastructure/cmake/README.md` | The shared CMake modules: how to put them on `CMAKE_MODULE_PATH`, what each provides, what stays project-local |

- Keep docs, scripts, and presets aligned: when you change build behavior, update
  `README.md`, `docs/source/getting_started.md`, and this file in the same change.
