# AGENTS.md

Guidance for AI agents and new contributors working in BeschleunigerBallett
(a Vulkan graphics-engine playground with a Rust WebGPU sibling renderer:
C++23/C17, CMake presets, Rust through Corrosion).

**Reuse before adding Bash or PowerShell code.** Search this repository and every direct
submodule, including `third_party/ANTfrastructure`, for an existing implementation. Call or
extend its canonical owner, passing project-specific values as arguments. For new behavior
needed by multiple consumers, create or extend one shared owner module and call it here.
Keep unique product logic in this repository.

## Start here: routing

Find your task, do the first action, read the doc. [Docs](#docs) and
[What ANTfrastructure owns](#what-antfrastructure-owns--links-only) are the full ownership index.

| If you are doing this | Start with |
| --- | --- |
| Building anything on Windows | `Build-Windows-Container.ps1` ([Fast path](#fast-path-windows-build-run-verify)). Never invoke CMake/Ninja/MSBuild on the host. |
| Running the engine / seeing pixels | `scripts/windows/Invoke-ClangCl*.ps1` from the **repo root** ([Running on the Host](#running-on-the-host-windows)) |
| Changing a shader | Edit Vulkan sources under `Resources/ShadersSlang/`, or shared/WebGPU sources in OxidANT's `shaders/`; run `Build-SlangShaders.ps1` and the affected renderer regression ([`docs/source/shader-build-pipeline.md`](docs/source/shader-build-pipeline.md)) |
| Adding or changing a test | `Test/commit/VulkanEngine/` (CPU + GPU golden), `Test/fuzz/`, `Test/perf/`; always in scope ([Testing](#testing)) |
| Touching render passes, barriers, frames-in-flight | Golden suites on the host GPU **and** `Invoke-SyncValidation.ps1` (Linux CI runs sync validation on llvmpipe): [`docs/source/gpu-golden-testing.md`](docs/source/gpu-golden-testing.md) |
| Refactoring the renderer / device path | The per-unit verification loop in [`docs/source/gpu-golden-testing.md`](docs/source/gpu-golden-testing.md); log the change in [`docs/source/cpp-renderer-improvements.md`](docs/source/cpp-renderer-improvements.md) |
| "My build produced nothing" / "my deleted file still builds" | `-FreshContainer`, and the delivery check that fails the build: [Container reuse and delivery](#containerized-windows-builds-stevedore) |
| Writing a script, module, or general-purpose doc | Probably upstream: [Rule: Reusable Work Belongs in ANTfrastructure](#rule-reusable-work-belongs-in-antfrastructure). Check [What ANTfrastructure owns](#what-antfrastructure-owns--links-only) before writing a procedure that may exist |
| Bumping a submodule pin, or any dependency | `bash ./scripts/linux/renovate-local.sh` from WSL ([Dependency upgrades](#dependency-upgrades-renovate-as-a-local-cli)); never a bare `git submodule update --remote` |
| Releasing | A push to `main` releases `VERSION.txt`'s version once every lane is green; first raise it and give it a `CHANGELOG.md` section ([What CI runs](#what-ci-runs-and-what-it-does-not)) |
| Pushing and expecting CI to tell you something | A push or PR runs Linux x64 (not on a docs-only commit) and the gates; the other four platform lanes run when a commit message asks with the ci token, or by hand ([What CI runs](#what-ci-runs-and-what-it-does-not)) |
| Changing the Rust WebGPU renderer | `third_party/OxidANT/crates/webgpu_renderer`; that repo owns the renderer's docs too (decision D6): [`webgpu-renderer-roadmap.md`](third_party/OxidANT/crates/webgpu_renderer/docs/webgpu-renderer-roadmap.md), reachable in this checkout since the pin bump to `d68560fe`, or [on the web](https://github.com/Kataglyphis/OxidANT/blob/HEAD/crates/webgpu_renderer/docs/webgpu-renderer-roadmap.md) without the submodule |
| Touching the clouds subsystem | [`docs/source/clouds.md`](docs/source/clouds.md): pipeline shape, estimator, UBO/constants tables, queue ownership |

### Repo map

| Path | What lives there |
| --- | --- |
| `Src/GraphicsEngineVulkan/` | The C++ Vulkan engine (`app`, `renderer`, `vulkan_base`, `memory`, `scene`, `gui`, `window`, `common`) |
| `Src/shared/`, `Src/KomputePlayground/` | Renderer-agnostic frontend/scene/imgui/util code; the Kompute compute playground |
| `Test/commit/VulkanEngine/` | The main gtest suite (`commitTestSuite.exe`): CPU suites plus `GoldenRender.*` / `Integration.*` |
| `Test/compile/`, `Test/fuzz/`, `Test/perf/` | Compile-time checks, FuzzTest targets, Google Benchmark suite (`perfTestSuite`) |
| `Resources/ShadersSlang/` | Vulkan/test Slang + `shader-manifest.json`, which imports OxidANT's shared/WebGPU manifest; compiled output under `build/` is gitignored |
| `third_party/OxidANT/shaders/` | Canonical shared math and WebGPU Slang sources consumed by both renderers |
| `third_party/OxidANT/crates/` | The Rust side, incl. `webgpu_renderer` and `gui` |
| `third_party/ANTfrastructure/` | The submodule that owns every reusable script, module and doc (see the rule below) |
| `scripts/windows/`, `scripts/linux/`, `scripts/agentic-loop/` | Thin project wrappers over ANTfrastructure drivers + this project's payload |
| `cmake/` | This project's build **policy** only: `ProjectOptions.cmake` (options, exceptions, CRT, C++23, modules-mandatory), `CPackOptions.cmake`, `SystemLibDependencies.cmake`, `FetchAssets.cmake` (the scenes, [Large tracked assets](#large-tracked-assets)). The reusable modules are ANTfrastructure's ([CMake modules](#cmake-modules-local-first-then-antfrastructure)) |

### Large tracked assets

**The scenes are not tracked; they are [AssetANT](https://github.com/Kataglyphis/AssetANT)
release assets** (owner, 2026-10-17). AssetANT keeps each scene's metadata in git (origin, license,
credit, the archive's SHA-256) and publishes its files as one reproducible `<name>.tar.xz` per
release, `<name>-v<N>`. `Resources/assets.lock` pins each archive by SHA-256 and URL;
`cmake/FetchAssets.cmake` hands the lock to the hub's
[`kataglyphis_fetch_data`](third_party/ANTfrastructure/docs/data-repositories.md#the-hubs-fetcher)
(2026-10-19), whose `fetch-data-lock.py` extracts each archive into `Resources/Models/<name>` at
configure time through a per-user cache (`KATAGLYPHIS_ASSET_CACHE`, else `kataglyphis-assets` under
`%LOCALAPPDATA%` or `~/.cache`). Every configure and `cmake -P` therefore needs Python 3.12+ on
`PATH`: `python` on Windows, `python3` elsewhere. The paths the engine loads did not move. At
`7b77802e`, the last commit that tracked them: 633 MiB tracked, 518 MiB of it the 14 scene
directories; after, 116 MiB, 62 of them `third_party/KTX`'s vendored test images.

- **A configure fetches two**: `crytek-sponza` (the Release default, the one scene the packages
  install, two goldens) and `Dinosaurs` (the Debug default, the golden scene, four `ObjParseUnit`
  fixtures, the renderer comparisons), named by `KATAGLYPHIS_DEFAULT_ASSETS` in the module.
  `-DKATAGLYPHIS_FETCH_ALL_ASSETS=ON` adds the other twelve for the model picker;
  `-DKATAGLYPHIS_FETCH_ASSETS=OFF` fetches nothing (the tests that read a scene then skip or fail).
- **Without a configure**: `cmake -P cmake/FetchAssets.cmake`. `Build-Windows-Container.ps1` runs it
  on the host, so a run from the repo root finds the scenes; the arm64 lane runs it before its
  host-side lavapipe suites. Both need Python on that host's `PATH`. x64 reads the scenes the container's configure fetched into the
  bind-mounted workspace (the `windows-2025` runner has no `cmake` on `PATH`).
- **A fetched directory holds `.assetant-sha256`**: the archive's hash and how many files it
  extracted. Only a stamp matching both makes the fetch a no-op, so a tree whose files went while
  the ignored stamp stayed is extracted again (checking out a commit that still tracked the scenes
  does exactly that). A scene directory without a stamp stops the configure instead of being
  overwritten. The packages leave the stamp out.
- **A changed scene is a new AssetANT release and a new lock line.** A published archive never
  changes; the fetch deletes a download whose hash differs and stops.
- **What stays tracked**: the test fixtures (`GltfTest/`, `ShadowTest/`, `Plane/`, `VikingRoom/`,
  and the loose `plane` and `suzanne` the perf suite parses) and `Resources/Textures/`, which the
  packages install whole: `Skybox/DOOM2016`, the one skybox the renderer loads, and the three
  1-pixel defaults (1.4 MiB). `Loading_Screen/`, the duplicated `space` skybox, `brick.png` and
  `dirt.png` (12.6 MiB, read by nothing) went on 2026-10-17.
- **The WebGPU demo is built, not tracked**: `scripts/linux/docs-build-web.sh` compiles it into
  the ignored `docs/source/_webgpu_demo/` on every docs build, which CI's release job runs. Without
  `wasm-bindgen` the docs publish without the demo (a warning in CI); without a wasm-bindgen version
  in OxidANT's `Cargo.lock` the script stops, since 2026-10-20. Until 2026-10-17 an 8 MiB snapshot
  was committed instead.

Still large: `Documents/GGD_Kit_milestone_document.pdf` (16 MiB, `linguist-vendored`), frozen with
two test fixtures in `large-files.allow` for the hub's `large-files` gate. The engine screenshots
under `images/` (11 MiB), which nothing linked, went on 2026-10-17 (owner).

**The history was rewritten on 2026-10-17; LFS is still out.** The owner reversed the 2026-09-15
"no rewrite" that day; the family-wide rewrite
([`data-repositories.md`](third_party/ANTfrastructure/docs/data-repositories.md#what-moved-and-why))
took this pack from 1.24 GiB to 146 MiB. AssetANT's published archives are now the scenes' only
copy (its `source.commit` names a commit from before); a checkout from before follows with
[the hub's adopt script](third_party/ANTfrastructure/docs/adopting-in-a-new-project.md#after-a-history-rewrite).

The **large-asset gate** at the bottom of `.gitignore` stays: DCC project files, archives and video
are ignored repo-wide, heavy image formats under `Resources/`, and `Resources/**/*.obj` with the
five tracked meshes listed back. Every directory under `Resources/Models/` except the four fixture
directories is ignored, which also keeps the fetched scenes out of `git status`. A fixture that
genuinely belongs: `git add -f <path>`, a `!` line in `.gitignore` and a mention here; a scene goes
to AssetANT. Earlier passes removed, without touching history, a Blender autosave twin, a duplicate
`dinosaurs.obj` and a 2022 Doxygen snapshot (`2987c7a3`). Do not re-add them.

## What ANTfrastructure owns — links only

Every topic below lives in `third_party/ANTfrastructure` and is consumed from there. **This file
does not restate any of it**: for a topic in this table, write one sentence of orientation and
link (upstream's instruction to consumers,
[`docs/INDEX.md` § If you are about to write a procedure in a consumer repo](third_party/ANTfrastructure/docs/INDEX.md#if-you-are-about-to-write-a-procedure-in-a-consumer-repo)).
This repo's own pages are in [Docs](#docs).

| Topic | Upstream document |
| --- | --- |
| Which repo a piece of knowledge belongs in — the *"would this still be true in a different project?"* split, with worked examples | [`INDEX.md` § Where does a piece of knowledge belong?](third_party/ANTfrastructure/docs/INDEX.md#where-does-a-piece-of-knowledge-belong) |
| The full topic → owning-document index for the whole family | [`INDEX.md`](third_party/ANTfrastructure/docs/INDEX.md) |
| Code comments: one line, only the why; API docs short; gated | [the hub rule, Comments](third_party/ANTfrastructure/AGENTS.md#comments-one-line-only-the-why) |
| Searching the tree: `rg`, not `grep -r` | [the hub rule, Searching](https://github.com/Kataglyphis/ANTfrastructure/blob/develop/AGENTS.md#searching-the-tree-ripgrep-rg) |
| Wiring another project to any of this: the loop, both container flows, launchers, CI actions | [`adopting-in-a-new-project.md`](third_party/ANTfrastructure/docs/adopting-in-a-new-project.md) |
| The one file a consumer must own itself, and why (`Resolve-BuildModule.ps1`) | [adopting § 1](third_party/ANTfrastructure/docs/adopting-in-a-new-project.md#1-the-one-file-that-cannot-live-here) |
| Agentic loop: architecture, engines, config keys, prompt composition; the `WindowsAgenticLoop.Common` module API + config reference | [adopting § 4](third_party/ANTfrastructure/docs/adopting-in-a-new-project.md#4-the-agentic-loop), [`windows-agentic-loop.md`](third_party/ANTfrastructure/docs/windows-agentic-loop.md) |
| Build matrix config, sanitizer env vars, the full matrix sweep | [`agentic-loop-build-matrix.md`](third_party/ANTfrastructure/docs/agentic-loop-build-matrix.md) |
| Calling conventions every consumer wrapper follows (`#requires`, strict mode, sourcing the hub) | [adopting § 8](third_party/ANTfrastructure/docs/adopting-in-a-new-project.md#8-calling-conventions-what-every-consumer-looks-like) |
| The Windows container image: build sequence, Stevedore setup, invariants | [`windows-builds.md`](third_party/ANTfrastructure/docs/windows-builds.md) |
| Building inside the image: **both transports and how to set each one up** (§ Transports), the reusable-container pattern and its safety rails, why sccache cannot cache a C++23 modules build, why a named volume cannot be a CMake build directory, the Windows path limit that silently truncates tar transfers, `docker exec` bypassing the entrypoint, the wcifs teardown lock | [`windows-container-build-performance.md`](third_party/ANTfrastructure/docs/windows-container-build-performance.md) |
| sccache shared with the other build hosts: this host's dufs plus an S3 bucket, handed to the reusable container's `docker exec` by name so the key reaches no command line, and the fallback when the bucket stalls; the bucket's server | [`windows-build-resources.md` § Two cache levels](third_party/ANTfrastructure/docs/windows-build-resources.md#two-cache-levels), [`linux/sccache-s3/README.md`](third_party/ANTfrastructure/linux/sccache-s3/README.md) |
| Windows arm64 cross builds: the `:winarm64` bundle, the arch gate, why Release, Profile and the Debug (ASan) build, the `container-ci-windows.yml` caller contract | [`windows-cross-builds.md`](third_party/ANTfrastructure/docs/windows-cross-builds.md) |
| Running the Linux image locally: nerdctl, cargo cache volume, build-dir rules | [`rancher-desktop-linux-containers.md`](third_party/ANTfrastructure/docs/rancher-desktop-linux-containers.md) |
| Which CI lanes run when: Linux x64 on every push, the rest on request, the release on `main`; which repository follows it | [`branch-and-release-strategy.md`](third_party/ANTfrastructure/docs/branch-and-release-strategy.md), [`ci-build-triggers.md`](third_party/ANTfrastructure/docs/ci-build-triggers.md) |
| Dependency upgrades family-wide: Renovate as a local CLI, what `--apply` moves and what it refuses | [`dependency-updates.md`](third_party/ANTfrastructure/docs/dependency-updates.md) |
| Moving every checkout to its branch tip; adopting a rewritten history | [adopting § Submodule maintenance](third_party/ANTfrastructure/docs/adopting-in-a-new-project.md#submodule-maintenance) |
| Reading and fixing CI status from a shell with `gh` | [`github-cli-pipeline-monitoring.md`](third_party/ANTfrastructure/docs/github-cli-pipeline-monitoring.md) |
| clang-format / clang-tidy / cmake-format rules, and the lint gates' contracts | [`code-quality-tooling.md`](third_party/ANTfrastructure/docs/code-quality-tooling.md) |
| The shared CMake modules: what each provides, how they reach `CMAKE_MODULE_PATH`, what stays project-local | [`cmake/README.md`](third_party/ANTfrastructure/cmake/README.md) |

---

## Fast path: Windows build, run, verify

**On Windows, always build in Stevedore (Docker) through the container build script.** Never invoke
CMake, Ninja or MSBuild on the host: the host toolchain is not what CI builds with, and host `cmake`
cannot even read this repo's presets ([the trap](#build-system-overview)).

1. **Build** (`clangcl-debug` shown; the script defaults to all three clang-cl configurations):
   ```pwsh
   pwsh -ExecutionPolicy Bypass -File .\scripts\windows\Build-Windows-Container.ps1 `
     -Configurations "clangcl-debug"
   ```
   The build runs in the reusable container `bb-build-persistent` and **streams the finished build
   trees back into the working tree**; no `docker cp` step. Tests are skipped unless `-RunTests`
   (the CPU suites, in-container).
2. **The artifacts are on the host** at the build-directory root: `build-clangcl-debug\GraphicsEngine.exe`,
   `commitTestSuite.exe`, the fuzz executables, `compile_commands.json`, and `logs\`. `bin\` holds
   only the copied ASAN runtime DLL. Other configurations land in `build-clangcl-profile\` and
   `build-clangcl-release\`.
3. **Run on the host**: containers have no swapchain, and the working directory must be the **repo
   root** (`Resources/` is loaded via cwd-relative paths):
   ```pwsh
   .\scripts\windows\Invoke-ClangClDebug.ps1        # ctest + fuzz exes + launch
   .\scripts\windows\Invoke-ClangClProfile.ps1
   .\scripts\windows\Invoke-ClangClRelease.ps1
   ```
   Debug builds need the Vulkan validation layers on the host
   ([Running on the Host](#running-on-the-host-windows)); Profile/Release do not.
4. **Changed rendering or the device path?** A green build proves nothing about behaviour: the GPU
   suites *skip* in the container. Run them on the host and follow the verification loop in
   [`docs/source/gpu-golden-testing.md`](docs/source/gpu-golden-testing.md).

---

## Build System Overview

`CMakePresets.json` drives everything: pick a preset, never an ad-hoc CMake command line.

**The repository and the CMake project are spelled differently, on purpose.** `GraphicsEngine` is
the CMake project and executable name (`CMakeLists.txt:8`), matching `Src/GraphicsEngineVulkan/`;
Sphinx (`project` in `docs/source/conf.py`) and Doxygen (`Doxyfile.in:45`) publish under
`BeschleunigerBallett`, the repository name. Build directories, preset names and the MSIX package
(`Msix.PackageNameDefault` in `scripts/windows/Build-Windows.config.psd1`) follow the CMake name;
nothing here is a leftover to be unified.

The version is the repo-root **`VERSION.txt`**, read by `CMakeLists.txt:4` (into
`project(... VERSION ...)` and the `PROJECT_VERSION` compile definition), `docs/source/conf.py`
and `scripts/windows/Build-Windows.ps1` (the MSIX package version; a missing file is a hard error).
Bump it with `bash ./scripts/linux/bump-version.sh <x.y.z>` and nothing else; no reader keeps a
copy of the number.

> **Trap:** `cmake --list-presets` does **not** work on this host. The host CMake (3.29) cannot
> read `CMakePresets.json` (`"version": 10`, `cmakeMinimumRequired` 4.1) and fails with
> `CMake Error: Could not read presets ... Unrecognized "version" field` (verified 2026-08-02).
> Only the container's CMake can. Read the file or the table below; the error is not a broken
> checkout.

### Windows configurations (Build-Windows.ps1)

`scripts/windows/Build-Windows.ps1` is the single entry point for Windows builds (the container
script runs it in the image). Its comma-separated `-Configurations` names map to presets and build
directories via `scripts/windows/Build-Windows.config.psd1`:

| Configuration | Preset | Build dir | What you get |
| --- | --- | --- | --- |
| `clangcl-debug` | `x64-ClangCL-Windows-Debug` | `build-clangcl-debug` | Debug + **ASAN** + UBSan, FuzzTest fuzzing mode |
| `clangcl-profile` | `x64-ClangCL-Windows-Profile` | `build-clangcl-profile` | RelWithDebInfo + tests/benchmarks |
| `clangcl-release` | `x64-ClangCL-Windows-Release` | `build-clangcl-release` | Release + CPack packaging |
| `msvc-debug` | `x64-MSVC-Windows-Debug` | `build-msvc-debug` | MSVC (cl) builds, optional steps |
| `msvc-release` | `x64-MSVC-Windows-Release` | `build-msvc-release` | MSVC (cl) builds, optional steps |

The hub's `Get-BuildWorkspaceConfig` selects that file and the workspace-root override;
keep `Resolve-WorkspacePath` and all project-specific settings in this driver.

- `x64-ClangCL-Windows-Debug-ASan`: AddressSanitizer without the fuzzing-mode extras; not wired
  into `Build-Windows.config.psd1`.
- Windows test presets (`test-<configure-preset>`) exist for three ClangCL configurations only:
  Debug, Debug-ASan and Profile (the other two, plus `default`, are Linux ones).
- The plain-Clang `x64-Clang-Windows-{Debug,Profile,RelWithDebInfo}` presets went in 2026-07 as
  unused duplicates of the ClangCL set. `x64-Clang-Windows-Release`, a build without Rust, went
  on 2026-10-21 (owner: Rust is always in); the `windows-clang-release-wix` package preset now
  builds on `x64-ClangCL-Windows-Release`.
- The Release package step gets its `cmake --build --target package` arguments and parallelism
  from ANTfrastructure's `Get-CmakeBuildArgs`; keep the package target here, not a copied parser.
- The ARM64 source-copy Pester suite uses the hub's parsed loop and fixture helpers;
  keep Ballett's shader list, mutation and path assertions in this repo.

**Coverage stays OFF for the ClangCL presets** (`myproject_ENABLE_COVERAGE` in
`x64-ClangCL-Windows-Base`). Switched ON on 2026-10-01 (hub CON9: the image ships
`clang_rt.profile`), the Debug suite died at process exit in the profile writer: with ASan the
profile runtime comes from Microsoft's ASan DLL, `initializeValueProfRuntimeRecord` dereferences a
bad pointer (`InstrProfilingValue.c:327`), and `commitTestSuite.exe`/`compileTestSuite.exe` exited 1
after all 654 tests had passed (run 36997000491). No Windows step reads the `.profraw` files;
coverage is measured in the Linux lanes' `static-analysis` job. Revisit when the profile runtime
matches the compiler or value profiling can be turned off. Profile keeps it OFF
(`x64-ClangCL-Windows-RelWithDebInfo-Base`, so benchmarks stay uninstrumented) and Release never
instruments (`myproject_enable_coverage`).

**No `find`/`count`/`remove` on a defaulted-`==` struct under ClangCL.** The image's MSVC STL
14.51.36231 `static_assert`s in its vectorized path
([`failure-modes.md`](third_party/ANTfrastructure/docs/failure-modes.md#static_assert-failed-unexpected-size-from-stdfind-under-clang-cl));
microsoft/STL#6294 closed with #6298 on 2026-10-18, which no VC Tools the image ships carries yet.
Run 36052511207 stopped there on `findSampler`'s 24-byte `SamplerKey`: use `find_if` with a
predicate until the image's VC Tools have the fix.

Typical full sweep (ASAN debug, profile, release):

```pwsh
pwsh -ExecutionPolicy Bypass -File .\scripts\windows\Build-Windows.ps1 `
  -Configurations "clangcl-debug,clangcl-profile,clangcl-release" `
  -SkipFormat -SkipTidy -SkipTests -SkipPerfTests -SkipMsix
```

### Sanitizer semantics

What the hub's modules do (Debug only, decided in one place; the defaults; which combinations drop
what; no TSan on clang-cl; clang-cl UBSan without ASAN traps instead of reporting):
[the hub's `cmake/README.md`](third_party/ANTfrastructure/cmake/README.md#sanitizers-what-the-modules-actually-do).
This repo's half:

- `cmake/ProjectOptions.cmake` calls `myproject_default_debug_sanitizers()` and
  `myproject_apply_sanitizers(myproject_options)` for every build type, on purpose; the
  `x64-MSVC-Windows-Debug` preset switches ASAN off again.
- **`linux-debug-tsan-clang` and `linux-debug-tsan-GNU` are the only builds that detect data
  races**: they set `myproject_ENABLE_SANITIZER_THREAD=ON` and
  `myproject_ENABLE_SANITIZER_ADDRESS=OFF`. A Windows `clangcl-tsan` preset built a plain Debug
  duplicate until 2026-07-20.
- **With ASAN on, three places move the clang-cl Debug build to the release CRT**, keyed on
  `myproject_ENABLE_SANITIZER_ADDRESS`: the hub's `/MDd` strip (`myproject_apply_compiler_build_flags`
  in `cmake/ProjectOptions.cmake`), the `CMAKE_MSVC_RUNTIME_LIBRARY` override beside it, and the
  Rust bridge's `CXXFLAGS` in `Src/CMakeLists.txt` (`_myproject_configure_windows_rust_crate`). Keep
  UBSan beside ASAN on Windows for UBSan reports.

## Containerized Windows Builds (Stevedore)

Windows builds run in the ANTfrastructure developer image
`ghcr.io/kataglyphis/kataglyphis_beschleuniger:winamd64` (clang-cl, CMake, Ninja, Vulkan SDK, Rust,
sccache preinstalled). CI does exactly this: `.github/workflows/windows-x64.yml` runs
`scripts/windows/Invoke-WindowsLane.ps1` in the container through the hub's
`container-ci-windows.yml`.

**All Windows-container knowledge lives in ANTfrastructure; do not restate it here.** Start at
[`third_party/ANTfrastructure/docs/INDEX.md`](third_party/ANTfrastructure/docs/INDEX.md), which maps
topic → owning document, so links survive upstream reorganising. This section's three documents
(the image; building inside it; the shared sccache) are rows of
[What ANTfrastructure owns](#what-antfrastructure-owns--links-only).

The two transports, the reused container's safety rails and why each exists are the hub's
([`windows-container-build-performance.md`](third_party/ANTfrastructure/docs/windows-container-build-performance.md#transports-how-to-set-up-both));
this repo's numbers and wiring are in [`docs/source/container-build-caching.md`](docs/source/container-build-caching.md).
What bites here:

- The default transport is a tar-pipe into the reusable container `bb-build-persistent`;
  `-UseBindMount` measured slower on this Dev Drive host.
- **`-FreshContainer` after a file deleted at the repo root** (the prune before each build removes
  stale top-level directories only) **and after ANY C++23 module-interface change** (the
  fresh-container rule in [`docs/source/gpu-golden-testing.md`](docs/source/gpu-golden-testing.md)).
- The script **fails the build** when the container produced no executables or one did not reach
  the host. Do not "fix" that check.
- It takes `-Configurations` (comma-separated), **not** `-Preset`.
- Reset a wedged incremental build with `docker rm -f bb-build-persistent`.

## Linux Builds

`scripts/linux/cmake-configure-build.sh` wraps configure+build (`--preset linux-debug-clang`,
`--build-dir build`, …). TSan is selected by preset only (`linux-debug-tsan-clang`,
`linux-debug-tsan-GNU`): `--use-thread-sanitizer` errors out on purpose rather than silently
no-op'ing. `linux-debug-asan-clang` is AddressSanitizer + UBSan (CI and fuzz-test integration).
Coverage, ctest, perf, static-analysis and wasm wrappers live next to it; `--vulkan-setup-script`
injects a Vulkan SDK env.

The Slang precompile is `cmake-configure-build.sh`'s pre-build hook, and a failure there is
**fatal**. Use `--allow-prebuild-failure` only deliberately: a silent `|| warn` once left CI green
with no SPIR-V at all.

**Nothing injects `--gcc-toolchain` any more** (retired 2026-09-29, hub CON37): the image's clang
and clang-tidy select the image's GCC themselves
([`linux-cross-builds.md` § Clang cross wrappers](third_party/ANTfrastructure/docs/linux-cross-builds.md#clang-cross-wrappers)),
so `run-static-analysis-format.sh` passes clang-tidy nothing.

**sccache runs through the hub's guarded launcher on Linux** (since 2026-10-06). A bare sccache
fails every compile once its server is unreachable: run 37509585546's profiling job died with
`sccache: error: Connection to server timed out` on each one. The hub's `Cache.cmake` therefore
hands both compiler launchers its `linux/scripts/01-core/sccache-launcher.sh`, which retries once,
then compiles directly and says so in the log. BB made that swap itself in
`cmake/ProjectOptions.cmake` until 2026-10-10; the hub has since 716b5226. It covers every Linux
CMake build, the riscv64 cross build included. The GCC presets use ccache and Windows keeps
`sccache.exe`: the hub has no guarded launcher for either.

### Running the Linux build locally (Rancher Desktop)

Two local traps, with the full recipe in
[ANTfrastructure § Persisting the cargo cache](third_party/ANTfrastructure/docs/rancher-desktop-linux-containers.md#persisting-the-cargo-cache):
pass `--build-dir /tmp/...` (a build dir on the bind-mounted host tree breaks FetchContent renames
and cargo cleanup), and point `--cargo-cache-dir` at a named volume so Rust dependencies survive the
container.

## Shaders: Slang (unified SPIR-V + WGSL)

Vulkan entry points and material modules live under `Resources/ShadersSlang/`; shared math and
WebGPU Slang sources belong to `third_party/OxidANT/shaders/`. This repo's `shader-manifest.json`
imports the owner's WGSL manifest, and both drivers resolve the two pinned source roots. Make
WebGPU source/generated-WGSL changes in OxidANT and move its pin here. Compiled
output is gitignored, so a fresh clone must run a compile script before the engine has anything to
load. Build step, staleness rules, fast iteration:
[`docs/source/shader-build-pipeline.md`](docs/source/shader-build-pipeline.md); sharing shader code between the
renderers: [`docs/source/shader-sharing.md`](docs/source/shader-sharing.md).
Keep the local cloud-volume module named `noise_volume.slang`; `noise.slang` would
shadow OxidANT's shared noise module and invalidate the dual-target import test.

## Rule: Reusable Work Belongs in ANTfrastructure

**Before writing a script, module, or doc here, ask whether another project could use it. If yes,
it goes into `third_party/ANTfrastructure` and this repo consumes it — never a copy.**

This is the local form of ANTfrastructure's canonical *"would this still be true in a different
project?"*. Which side a given thing falls on (PowerShell, Bash, container or Windows-container
knowledge, a trap learned the hard way) is settled there, with the worked splits and the
three-broken-copies case that motivated the rule:
[`INDEX.md` § Where does a piece of knowledge belong?](third_party/ANTfrastructure/docs/INDEX.md#where-does-a-piece-of-knowledge-belong).
Destinations: `windows/scripts/modules/` for PowerShell, `linux/scripts/lib/` or
`linux/scripts/01-core/` for Bash, `docs/` for everything else.

What stays here: engine code, shaders, this project's presets, the *payload* the shared drivers
execute (build-directory names, `Build-Windows.ps1` arguments, project-specific exclusions, the
Slang precompile hook), and `Resolve-BuildModule.ps1`, the bootstrap that *finds* ANTfrastructure
and so cannot live in it
([adopting § 1](third_party/ANTfrastructure/docs/adopting-in-a-new-project.md#1-the-one-file-that-cannot-live-here)).

### The wrapper map

Almost every script under `scripts/` is a thin consumer. **To know what one does, read the upstream
driver, not the wrapper**: the wrapper only supplies this project's payload.

| This repo | Upstream driver (in `third_party/ANTfrastructure/`) |
| --- | --- |
| `scripts/windows/Build-Windows-Container.ps1` | `windows/scripts/build/Invoke-RepoContainerBuild.ps1` (→ `WindowsContainerBuild.Reuse.psm1`'s `Invoke-ContainerBuild`, `WindowsContainerImage.Common.psm1`'s `Get-CiImageReference`) |
| `scripts/linux/cmake-configure-build.sh` | `linux/scripts/lib/cmake-build.sh` |
| `scripts/windows/Build-SlangShaders.ps1` / `scripts/linux/compile-slang-shaders.sh` | `windows/scripts/modules/WindowsSlang.Common.psm1` → `Invoke-SlangShaderCompile` / `linux/scripts/lib/slang-compile.sh` |
| `scripts/linux/run-ctest.sh` | `linux/scripts/lib/ctest-run.sh` |
| `scripts/linux/docs-build-web.sh` | `linux/scripts/lib/docs-build.sh` (the WebGPU wasm demo rebuild in front of it is this repo's) |
| `scripts/windows/Compare-PerfBaseline.ps1` | `windows/scripts/modules/WindowsPerfBaseline.Common.psm1` |
| `scripts/windows/Invoke-ClangCl{Profile,Release}.ps1` | `windows/scripts/modules/WindowsAppRunner.Common.psm1` → `Invoke-AppRun`, `Resolve-AppExecutablePath` |
| `scripts/linux/run-{debug,profile,release}.sh` | `linux/scripts/lib/app-runner.sh` (the Bash twin of the above) |
| `scripts/linux/run-static-analysis-format.sh` | `linux/scripts/lib/code-quality.sh` |
| `scripts/linux/build-coverage-{gcovr,llvm}.sh` | `linux/scripts/lib/coverage.sh` |
| `scripts/linux/wasm-size-budget.sh` / `scripts/windows/Test-WasmSizeBudget.ps1` | `linux/scripts/lib/wasm-opt.sh` / `windows/scripts/modules/WindowsWasmOpt.Common.psm1` |
| `scripts/linux/run-cargo-tests.sh` | `linux/scripts/02-toolchain/rust/cargo_test.sh` |
| `scripts/linux/run-cargo-lints.sh` | `linux/scripts/02-toolchain/rust/cargo_fmt_clippy.sh` |
| `scripts/linux/run-lint-gates.sh` | `linux/scripts/run-lint-gates.sh` (→ `lint-shell.sh`, `lint-workflows.sh`, `lint-secrets.sh`, `01-core/gates.sh`) |
| `scripts/linux/renovate-local.sh` | `linux/scripts/renovate-local.sh` (Renovate as a local CLI, plus the git half that applies what it can only detect) |
| `scripts/linux/ci-image-ref.sh` | `linux/scripts/ci-image-ref.sh`; PowerShell twin `windows/scripts/modules/WindowsContainerImage.Common.psm1` → `Get-CiImageReference` |
| `scripts/windows/Invoke-SyncValidation.ps1` | `windows/scripts/modules/WindowsVulkanValidation.Common.psm1` |
| `scripts/agentic-loop/Invoke-AgenticLoop.ps1` / `scripts/agentic-loop/Run-AgenticLoop.sh` | `windows/scripts/modules/WindowsAgenticLoop.Common.psm1` / `linux/scripts/lib/agentic-loop.sh` |
| `scripts/windows/Test-AllConfigs.ps1` | `windows/scripts/modules/WindowsBuildSweep.Common.psm1` → `Invoke-SweepStep`, `Test-LinuxContainerSupport`, `Invoke-InLinuxContainerBuild`, `Write-SweepSummary` |
| `scripts/windows/generated-artifacts.txt` | `shared/windows/tests/Repo.GeneratedArtifacts.Tests.ps1`, which reads its pathspecs from that file (run by `windows-x64.yml`'s `pester-tests` job) |
| `cmake/ProjectOptions.cmake` | `cmake/*.cmake` (see [`cmake/README.md`](third_party/ANTfrastructure/cmake/README.md) there) |

`Invoke-ClangClDebug.ps1` is the exception: it keeps its own flow, because it orchestrates CTest and
the fuzz executables before launching, but still takes `Resolve-AppExecutablePath` from
`WindowsAppRunner.Common`.

### CMake modules (local first, then ANTfrastructure)

The root `CMakeLists.txt` puts **`cmake/` then `third_party/ANTfrastructure/cmake/`** on
`CMAKE_MODULE_PATH` and includes every module by name, so a same-named file in `cmake/` overrides
an upstream one ([the hub's `cmake/README.md`](third_party/ANTfrastructure/cmake/README.md#consuming-them), which
lists the upstream modules). Local, because each encodes **this project's policy** rather
than a reusable mechanism:

- `cmake/ProjectOptions.cmake`: the option surface and its defaults, C++23, exceptions always off,
  C++ modules mandatory (a hard `FATAL_ERROR`, not a fallback). It composes the upstream modules;
  it does not duplicate them.
- `cmake/CPackOptions.cmake`: packaging metadata and branding.
- `cmake/SystemLibDependencies.cmake`: this engine's system dependencies.
- `cmake/FetchAssets.cmake`: this engine's scenes (its lock, its default set, its destination). The
  cached download-verify-extract half is the hub's `kataglyphis_fetch_data` since 2026-10-19.

**The clang-cl `-fms-compatibility-version` pin is upstream**, in `CompilerBuildFlags.cmake` as
`MYPROJECT_CLANG_CL_MS_COMPATIBILITY_VERSION` (default `19.51.36231`). It names the VC Tools version
ANTfrastructure's Windows image ships, so pin and toolchain live in one repo: bump them together.
Override the cache variable to build against a different VC Tools.

### PowerShell module resolution (ANTfrastructure first, vendored fallback)

`Build-Windows.ps1`, the run helpers and the Pester tests import modules through
`scripts/windows/Resolve-BuildModule.ps1`: ANTfrastructure's `windows/scripts/modules/` first, then
**`scripts/windows/modules/`**
([adopting § 1](third_party/ANTfrastructure/docs/adopting-in-a-new-project.md#1-the-one-file-that-cannot-live-here)).

- **It and `scripts/linux/lib/antfrastructure.sh` are copies of the hub's templates**, compared
  below their header by the shared-config gate: re-copy them after an upstream change, never edit
  them here ([`shared/config/README.md`](third_party/ANTfrastructure/shared/config/README.md#the-template-trees-are-owners-too)).
- **The fallback holds `Compare-Renderer.Common.psm1` only**, this project's own; a Pester case
  fails on a copy of an upstream module there.
- **`Import-BuildModule` imports `WindowsScripts.Shared` unconditionally**, because a module's own
  imports stay private to it
  ([adopting § 8](third_party/ANTfrastructure/docs/adopting-in-a-new-project.md#reach-for-these-before-writing-a-helper)).

### Shipping a change that spans both repos

Commit and push both repos together, ANTfrastructure **first** (CI resolves its composite actions
at `@develop`), with the submodule pin bumped in the same change. **Adopting any of this in another
project** (the loop, both container flows, the launchers, the CI actions) is upstream's checklist,
[`adopting-in-a-new-project.md`](third_party/ANTfrastructure/docs/adopting-in-a-new-project.md):
read it before hand-rolling equivalents elsewhere.

## Critical Invariant: Submodule Pins

Builds are only supported against the **recorded submodule gitlinks**, the commits CI builds green.
`git submodule update --checkout --recursive` restores every pin. If a drifted submodule is what you
actually want, update the gitlink AND fix the fallout in the same change.

The opposite move, for local work on the newest code, is the hub's `git-sync-branches.sh`
([adopting § Putting every checkout on its branch](third_party/ANTfrastructure/docs/adopting-in-a-new-project.md#putting-every-checkout-on-its-branch)).
That command keeps FUZZTEST on its declared release branch. The Renovate wrapper instead checks
every direct submodule against its remote default branch, including FUZZTEST.

**Coupling to watch when bumping pins:** `third_party/FUZZTEST` pins its own Abseil LTS (`absl_TAG`
in `cmake/BuildDependencies.cmake`); this repo's `ABSL_TAG` in `third_party/CMakeLists.txt` is
declared first and wins, so it must stay >= the FuzzTest pin or configure fails with missing
`absl::*` targets (observed: `absl::random_mocking_access`). This repo is at `20260817.0`, FuzzTest
at `20260526.0` (2026-10-07).

**Drift is guarded by ANTfrastructure's repo-agnostic suite**,
`third_party/ANTfrastructure/shared/windows/tests/Submodule.Pins.Tests.ps1`, run by its own
always-on workflow `.github/workflows/submodule-pins.yml`: push and PR on `main`/`develop`, no
`[build-win]` opt-in and **no `paths-ignore`**. An invariant that runs only when somebody remembers a
marker is not a gate, nor is one a path filter can silence: inside `windows-x64.yml` it inherited
the `'**.md'`/`docs/**` filter, so a commit that moved a gitlink alongside a docs page went
unchecked. For **every** configured submodule it asserts the tree is checked out, sits at its
recorded gitlink, and is pinned to a commit reachable from its remote (a pin on no remote branch
cannot be restored by a fresh clone). This repo's own copy of the suite was deleted once the hub pin
carried the promoted one; do not re-add a local fork. By hand:
`$env:ANTFRASTRUCTURE_PIN_CHECK_REPO_ROOT = $PWD; Invoke-Pester third_party/ANTfrastructure/shared/windows/tests/Submodule.Pins.Tests.ps1`.
It does **not** check the Abseil coupling above; that one is on you.

### Dependency upgrades: Renovate as a local CLI

**Dependency upgrades go through this wrapper, not by hand:**

```bash
bash ./scripts/linux/renovate-local.sh                    # what is behind
bash ./scripts/linux/renovate-local.sh --apply --dry-run  # the plan
bash ./scripts/linux/renovate-local.sh --apply            # move the gitlinks
```

Run it from WSL: there is no node on the Windows host. The wrapper initializes an absent
ANTfrastructure tooling submodule only for a real `--apply`; reports and plans need it checked out.
The default run checks every direct `.gitmodules` entry against its remote default branch.
`--apply` initializes missing direct checkouts and moves their gitlinks to those exact tips,
regardless of `branch =` overrides or Renovate report rows. It also rewrites the other values
the report names, including
`requirements.txt`, workflow actions and the CMake pins in `third_party/CMakeLists.txt`; a manager
it has no exact locator for is refused by name, and nothing is staged or committed.
`.github/renovate.json` is read by this CLI and nothing else (owner decision, 2026-09-09: no
Renovate GitHub App in this family). CI verifies the wrapper and config; it never applies upgrades.
How it works:
[`dependency-updates.md`](third_party/ANTfrastructure/docs/dependency-updates.md).

## Running on the Host (Windows)

Containers cannot present a swapchain: run the built binaries on the bare host, from the **repo
root** as working directory (`scripts/windows/Invoke-ClangCl*.ps1` wrap this). Verified 2026-07-17
on the AMD RX 9070 XT: all four clang-cl builds render (~32 FPS ImGui overlay).

- **Debug builds require the Vulkan validation layers on the host**; without them the app dies at
  startup with exit code `-1073740791` (`0xC0000409`, vulkan.hpp assert after "Validation layers
  requested, but not available!"). Install the Vulkan SDK (`winget install VulkanSDK`; 1.4.350.0 is
  what `Invoke-SyncValidation.ps1` defaults to), or point `VK_LAYER_PATH` at a directory containing
  `VkLayer_khronos_validation.{dll,json}` (extractable from the ANTfrastructure image under
  `C:\Users\ContainerAdministrator\scoop\apps\vulkan\current\Bin`). Profile/Release builds run
  without validation layers unless `KATAGLYPHIS_VULKAN_VALIDATION` is set (the Windows x64 lane's
  lavapipe step sets it, with the layer staged beside the suite).
- The build copies the ASAN runtime DLL next to `GraphicsEngine.exe`, and the run helpers set
  `ASAN_OPTIONS` with a relative `log_path`
  ([why both](third_party/ANTfrastructure/docs/agentic-loop-build-matrix.md#windows-asan-note)).

## Testing

- **C++ tests**: `ctest --test-dir <build-dir> --output-on-failure` (add `-C Debug` for
  multi-config generators); `Build-Windows.ps1` runs them unless `-SkipTests`. Host `ctest` cannot
  read a container-generated CMake tree: run the test executable directly
  (`.\build-clangcl-debug\commitTestSuite.exe`).
- **Benchmarks**: `clangcl-profile` builds `perfTestSuite.exe`; `Build-Windows.ps1` runs it without
  `-SkipPerfTests`. The Windows x64 lane runs it as a functional gate like Linux's: every benchmark
  must run, one that reports `error_occurred` (a model it cannot find) fails the lane, and timings
  are never gated. `scripts/windows/Compare-PerfBaseline.ps1`, `Compare-RendererPixels.ps1` and
  `Compare-RendererTimings.ps1` are local comparison tools. The lane's `Invoke-WindowsLane.ps1` runs
  the two renderer comparisons in validation-only mode, where both exit 2 when they found nothing to
  check (a warning on a GPU-less runner, never a pass); `Compare-PerfBaseline.ps1` is not in CI at
  all. The pixel comparison applies one structural verdict to each available renderer; preserve
  its three thresholds, renderer-specific messages, and failure exit code.
- **Pester** suites under `scripts/windows/tests/` (Pester 3.4 syntax): the `pester-tests` job of
  `.github/workflows/windows-x64.yml` runs them with a pinned Pester 3.4.0 whenever that lane runs.
  **Only suites covering project-specific behaviour belong here**; a suite for an upstream module
  goes upstream with it (2026-08-07). The seven left cover the module-resolution bootstrap
  (`Resolve-BuildModule`), the preset and shared-config guards (`CMakePresets.Integrity`,
  `SharedConfig.Drift`), the validation-layer runner (`Invoke-SyncValidation`) and the three
  comparison tools (`Compare-PerfBaseline`, `Compare-RendererPixels`, `Compare-RendererTimings`).
  The same job runs the hub's `shared/windows/tests/Repo.GeneratedArtifacts.Tests.ps1` over the
  pathspecs in `scripts/windows/generated-artifacts.txt` (since 2026-10-19; a wildcard ending in `/`
  matches nothing in git, so write `**/__pycache__/*`).
- `scripts/windows/Test-AllConfigs.ps1` is a local one-shot gate: the three standard Windows
  container builds plus the Linux TSan build (`-SkipLinux` drops the latter). Not in CI.
- **ANTfrastructure's own suites** (the modules this repo imports) run via
  `third_party/ANTfrastructure/windows/scripts/tests/Invoke-Tests.ps1` and need **Pester >= 5**
  (it fails rather than skips without it:
  `Install-Module Pester -MinimumVersion 5.7 -Scope CurrentUser -Force -SkipPublisherCheck`). Run it
  after changing anything upstream. Upstream's own CI runs them too (its `windows-x64.yml`, "Windows
  x64 · script tests", `windows-scripts.yml` until 2026-09-24), so a break there is caught without
  this repo's 2-3 h Windows lane.
- **GPU tests** (`GoldenRender.*`, `Integration.*`) skip in the Windows container and run for real
  on the host GPU. Linux CI's `clang-tests` job (x64 and arm64) runs them on the image's llvmpipe
  under Xvfb (`run-ctest.sh --virtual-display`), minus the GUI input sweep (too slow for a runner).
  Both Windows lanes run them on lavapipe on the runner, outside the image
  (`scripts/windows/Invoke-LavapipeTests.ps1`; x64 on the runner host after the container build,
  arm64 on `windows-11-arm`; a skip fails the run). Preserve fresh per-test JSON and stdout/stderr
  under ignored `logs/lavapipe/`; ARM64 CI uploads them on failure alongside any Windows crash
  dump and fails the upload when those evidence files are missing. A Linux host with an NVIDIA GPU
  runs them in the image through CDI. The lavapipe install, the `LP_NATIVE_VECTOR_WIDTH=256` pin, the NVIDIA recipe
  (an ASan build needs `ASAN_OPTIONS=protect_shadow_gap=0`), the cwd requirement and the
  golden-writing cautions: [`docs/source/gpu-golden-testing.md`](docs/source/gpu-golden-testing.md). "No
  synchronization frames available" on every golden was a FrameSync bug (`695db536`..`981244fd`,
  August 2026), not the RDP session it was blamed on; a commit in that range needs `981244fd` before
  any golden can draw.
- **Synchronization validation** catches missing or incorrect barriers that no pixel oracle can see
  (it found 10 real WRITE-AFTER-WRITE hazards in July 2026). Run it after touching render passes,
  barriers, or frames-in-flight:
  ```pwsh
  pwsh -ExecutionPolicy Bypass -File .\scripts\windows\Invoke-SyncValidation.ps1
  ```
  It exits non-zero when the run log contains `SYNC-HAZARD`, and with the test executable's own code
  when the run itself fails. Linux CI runs sync validation on llvmpipe since 2026-10-06
  (`run-ctest.sh --virtual-display`); this script is the host-GPU and Windows run. Details:
  [`docs/source/gpu-golden-testing.md`](docs/source/gpu-golden-testing.md).

**Adding tests is always in scope.** No permission needed to improve or extend the suites; a change
that fixes behaviour should generally arrive with a test that would have caught it. Prefer
assertions that survive driver and machine differences (structural pixel properties, invariants,
ordering) over exact-value comparisons, and make GPU-dependent tests skip themselves when no adapter
is present rather than fail. Ideas worth picking up live in [`BACKLOG.md`](BACKLOG.md), beside the
sized commitments.

**Formatting and static analysis**: the clang-format/clang-tidy/cmake-format commands, the host
gotchas, the container-build behaviour (clang-format always runs, and clang-tidy too since hub
CON10, over the TUs that import no module) and the suggested cadence:
[`docs/source/code-quality.md`](docs/source/code-quality.md).

**clang-format is a failing gate since 2026-10-06**, graded by the pinned LLVM's clang-format (the
hub's `LLVM_RELEASE`, 23.1.3 since 2026-10-10) on both lanes: the Windows x64 lane's
`Build-Windows.ps1` step and the Linux `static-analysis` job's
`run-static-analysis-format.sh --only-format-check`. The hub picks and checks the binary
(`Invoke-ClangFormatCheck`, `code_quality_require_pinned_clang_format`, since 2026-10-19), so a
distro clang-format (21.1.8 flagged 41 of the swept files) never grades. The sweep commit is in
`.git-blame-ignore-revs`; run `git config blame.ignoreRevsFile .git-blame-ignore-revs` once per
clone.

**Run more than the debug loop periodically.** `clangcl-debug` is the fast default, but
`clangcl-profile` (optimized, and the only configuration where benchmarks mean anything) and a
synchronization-validation pass each catch classes of problem the debug loop cannot; what each is
for: [`BACKLOG.md`](BACKLOG.md).

<a id="there-is-no-windows-threadsanitizer"></a>
**There is no Windows ThreadSanitizer.** For race detection use the Linux `linux-debug-tsan-clang`
preset, which CI runs ("Sanitizer semantics" above).

**`buildIntegritySuite` reads raw source, comments included.** It matches `[shader(`, `BENCHMARK(`
and `kataglyphis_add_fuzz_test(` by substring, counts calls, and looks a few lines around
`return;`, `.value` and filesystem calls. A comment quoting such text can pass or fail a gate by
itself, so grep the suite for a file's name before rewording a comment in it.

**Fuzz targets** (`Test/fuzz/`):
- **ASan.** On Linux the whole project builds with ASan (`linux-debug-asan-clang`). Abseil's
  `raw_hash_set` layout follows ASan, so ASan on the targets alone is an ODR violation that
  crashes at startup.
- **No `VulkanEngineCore`.** Never link it into a target: the renderer's static initializers
  crash a bare fuzztest main.
- **Out of ctest discovery.** Discovery executes the binaries, and they crash without
  project-wide ASan.
- **Header order.** The two abseil random headers come before `fuzztest.h`. A force-include
  breaks the module compile step.

## What CI runs, and what it does not

**A push or PR runs Linux x64 and the gates; the other four platform lanes run on request**: the
owner's rule for the family since 2026-10-15, this repo's since 2026-10-16. The tokens, hand runs,
concurrency groups and the release are the hub's
([`branch-and-release-strategy.md`](third_party/ANTfrastructure/docs/branch-and-release-strategy.md)).
Here `[ci windows]` runs both Windows lanes, `[ci arm64]` both arm64 ones, `[ci riscv64]` the
riscv64 lane, and a lane's badge shows only its runs started by hand.

**Every push to `main` is a release**
([the hub's release](third_party/ANTfrastructure/docs/branch-and-release-strategy.md#the-release)) of
`VERSION.txt`'s version. `release.yml` calls the hub's
[`release-publish.yml`](third_party/ANTfrastructure/docs/branch-and-release-strategy.md#the-reusable-release-workflow)
twice: the version check before the lanes, and the job that collects every lane's packages with
the hub's `collect-release-assets.sh` (its `collect:` block holds this repo's options) and tags.
`gh workflow run release.yml --ref develop -f dry-run=true` proves it on `develop` without
publishing.

From 2026-09-24 to 2026-10-16 every lane ran on every push; the `[build-win]` and `[build-arm]`
opt-ins before that are gone. Linux x64 still skips a docs-only commit through `paths-ignore`:

| Lane | Workflow | Trigger |
| --- | --- | --- |
| Lint gates (`lint` + `powershell-lint`) | `lint-gates.yml` — `lint` is one `uses:` of ANTfrastructure's reusable lane, with `ratchets: true` | always, **including docs-only commits** — no `paths-ignore` |
| Submodule pins | `submodule-pins.yml` — one `uses:` of ANTfrastructure's reusable lane | always, no `paths-ignore` |
| Linux x86_64 (build + test + coverage) | `linux-x64.yml` → `reusable-linux.yml` | always, minus `'**.md'`/`docs/**` |
| Windows x64 (clang-cl container build of `clangcl-debug`, `clangcl-profile` and `clangcl-release`, CPU, compile and perf suites, fuzz seeds, renderer comparisons, packaging; then the GPU suites on lavapipe on the runner host; Pester) | `windows-x64.yml` → the hub's reusable `container-ci-windows.yml`; the container half is `scripts/windows/Invoke-WindowsLane.ps1` | on request (`on-request.yml`) or by hand |
| Linux ARM64 | `linux-arm64.yml` → `reusable-linux.yml` | on request (`on-request.yml`) or by hand; builds no docs site (the docs steps need `runner == 'ubuntu-26.04'`) |
| Linux riscv64 (cross build on amd64, Debug ctest under QEMU; the CPU suites on request, the GPU suites behind the fp16 shim in 11 weekly shards; see [The riscv64 lane](#the-riscv64-lane)) | `linux-riscv64.yml` → the hub's reusable `container-ci-riscv64.yml`; the container half is `scripts/linux/run-riscv64-tests.sh` | on request (`on-request.yml`) or by hand; the GPU shards on Saturdays and on dispatch with `gpu-tests` |
| Windows ARM64 (cross build in the arm64 bundle, then the Release commit suite, the Profile perf suite and the Debug (ASan) suite on `windows-11-arm`, dda807b0; below) | `windows-arm64-cross.yml` → the hub's reusable `container-ci-windows.yml` | on request (`on-request.yml`) or by hand |

The top two are their own workflows, not jobs inside the build lanes, which is what makes "always"
true. As tenants they inherited their host's `paths-ignore`, so a docs-only push got no secret scan
and a commit that moved a gitlink next to a docs page got no pin check; and `lint`, then in
`reusable-linux.yml`, ran once per **caller**, grading the same runner-independent tree twice on
every ARM run.

**Known red on 2026-09-25, and not this repo's defect:** in `linux-arm64.yml` the two GNU jobs
(`gcc + tests + coverage`, `gcc profiling + benchmarks`) stop at configure with
`Could NOT find X11 (missing: X11_X11_LIB)` (run 36042437555): the arm64 image's GCC prints no
multiarch triplet, so CMake never searches `/usr/lib/aarch64-linux-gnu`. Fixed in hub source
(ANTfrastructure backlog CON8); it reaches this lane with the next Linux `:latest`.

**The Windows ARM64 lane cross-builds, then runs** (owner decision 2026-09-25).
`Build-Windows.ps1 -TargetArch arm64 -Configurations clangcl-release,clangcl-profile` runs in the
family image's arm64 bundle (`:winarm64`) on an amd64 runner. On a cross build it:

- refuses every other configuration: Debug links an x64-only ASan runtime and runs FuzzTest's
  grammar generator at build time, and the MSVC presets pin x64. clangcl-profile builds only with
  `-SkipPerfTests` (its benchmarks cannot run on the amd64 host), into
  `build-clangcl-profile-arm64`, for `-StageTests` to stage `perfTestSuite.exe`;
- configures with the hub's `Get-CrossConfigureArgs -Corrosion -Vulkan`, so Corrosion targets
  aarch64 and FindVulkan takes `Lib-ARM64/vulkan-1.lib` instead of the pointer-size x64 `Lib`;
- builds into `build-clangcl-release-arm64`;
- writes `dist/windows-arm64`: the portable bundle (the install tree plus its DLL closure), the
  `aarch64` MSI and ZIP, and the arm64 MSIX (manifest token `__MSIX_ARCH__`).

On `windows-11-arm` the commit suite runs whole, in two halves:

- `-StageTests` builds the Release commit suite with `KATAGLYPHIS_RELEASE_COMMIT_TESTS` and stages it
  with compiled and local Slang sources, OxidANT's pinned sources at their canonical `third_party`
  path, and test models (`Dinosaurs` included, which four `ObjParseUnit` tests parse) in
  `dist\windows-arm64-tests`. The hub's run job runs `tests.json` through its
  `Invoke-StagedTests.ps1` (hub CON43): the CPU suites (the commit suite minus the GPU suites and
  the source-reading `BuildIntegrity.*` checks), `perfTestSuite.exe` (the Profile perf suite) as an
  `exitcode` entry, and the Debug (ASan) suite the lane cross-builds too (dda807b0), whose `/MDd`
  CRT and `ucrtbased.dll` come from the runner's own VS and SDK (neither is redistributable).
- This repo's `gpu-suites` job checks the repo out, puts the staged shaders in it and runs the
  Release suite's GPU suites on lavapipe plus the source-reading `BuildIntegrity.*` checks through
  `Invoke-LavapipeTests.ps1`; its own job, because the hub's stops at 30 minutes.
- The cross build snapshots pinned OxidANT WGSL before compilation, clears mapped combined
  outputs, compares every new output to that snapshot, and stages them with both source trees.
  The ARM host compares
  all mapped outputs and sources by content; downloaded artifact and checkout mtimes are unrelated.
  The local single-tree mtime guard remains active. Upload a crash dump only when one exists.

The image's runtime bundle as a whole (GStreamer, WebRTC, HailoRT, the Python wheels) is not this
lane's to gate: since 2026-10-11 the hub proves it in its own `windows-arm64.yml`, so an image
defect the renderer never touches cannot turn this lane red or skip its GPU suites.

The hub's `Hardening.cmake` links `/CETCOMPAT` on x64 only. The run job borrows the Khronos loader
from LunarG's arm64 runtime, pinned by hash, and calls `GraphicsEngine.exe --version`: the engine
imports `vulkan-1.dll`, which a device gets from its GPU driver and the GPU-less runner has not got.
The bundle never ships the loader. The image, the arch gate over `dist/windows-arm64`, the upload
and the run job are the hub's:
[`windows-cross-builds.md` § Consumer cross lanes](third_party/ANTfrastructure/docs/windows-cross-builds.md#consumer-cross-lanes-container-ci-windowsyml).

Both Windows container callers request direct submodules; the build reads OxidANT's checked-in
sources and has no need to recurse into ANTfrastructure's cyclic documentation submodules.

**The Windows x64 lane is a thin caller too** (2026-09-26, the family's hub reusable lanes).
`windows-x64.yml` names `scripts/windows/Invoke-WindowsLane.ps1`, which a local container run
executes too:

- `Build-Windows.ps1 -Configurations clangcl-debug,clangcl-profile,clangcl-release`, the CPU-only
  suites (the GPU suites excluded by name), `compileTestSuite.exe`, the perf suite
  (`perfTestSuite.exe` from the repo root, as ctest's `perf_suite_runs`), every fuzz target's seed
  corpus (ten minutes each; the old step had one 20-minute limit), and the renderer timing and pixel
  comparisons. Every check after the build runs, and the lane fails if any did.
- `-StageTests` leaves the Release commit suite in `dist/windows-x64-tests`; the hub's
  `host-command` runs its GPU suites on lavapipe on the runner host after the container exits
  (`Invoke-LavapipeTests.ps1`, about an hour on 4 vCPUs).
- The WebDAV credentials and `MSIX_CERT_PASSWORD` reach the container through the hub's
  `CONTAINER_SECRET_ENV` secret, as an `--env-file`, and `Build-Windows.ps1` reads them from the
  environment; `KATAGLYPHIS_CI_HAS_GPU` comes from the repository variable through `container-env`.
- The product is `dist/windows-x64`, as arm64's is `dist/windows-arm64`: the portable bundle with
  its DLL closure (the hub's `New-PortableBundle` over `Get-ProductDllSearchPath`, both arches),
  the MSI, ZIP and NSIS installer, and the MSIX, which now packs that bundle. It uploads as
  `BeschleunigerBallett-windows-x64`, which replaced `windows-installers-container` and its globs
  over `build-clangcl-release`.
- `BuildIntegrity`'s three Windows-list tests read the lane script, not the workflow. The job's
  check-run name moved with it, to `Build, test, package (x64) / build (amd64)`.

**Workflow names follow the family convention** (owner decision 2026-09-24,
[adopting § Workflow file names](third_party/ANTfrastructure/docs/adopting-in-a-new-project.md#workflow-file-names-and-display-names)).
The rename kept the job ids, so the check-run names a branch protection rule matches did
not move; badge and `actions/workflows/<file>` URLs did.

To re-run a lane at the branch tip without a commit (say, a fix to a docs path the filter skips),
use `workflow_dispatch` on `linux-x64.yml`, `linux-riscv64.yml`, `windows-x64.yml` or
`windows-arm64-cross.yml` (`linux-arm64.yml` has none). The hub's trigger rules, including the
opt-in markers other repos still use:
[`ci-build-triggers.md`](third_party/ANTfrastructure/docs/ci-build-triggers.md). Reading pipeline
status from a shell (`gh`):
[`github-cli-pipeline-monitoring.md`](third_party/ANTfrastructure/docs/github-cli-pipeline-monitoring.md).

`linux-x64.yml` and `linux-arm64.yml` both call `reusable-linux.yml`, so a fix to the x86 lane
applies to ARM automatically. No CI lane has a hardware GPU: the Linux `clang-tests` job (both
arches) runs the golden suites on llvmpipe (a CPU Vulkan device,
[`docs/source/gpu-golden-testing.md`](docs/source/gpu-golden-testing.md)), both Windows lanes on lavapipe on the
runner. Both run synchronization validation there too (since 2026-10-06; Windows arm64 since the
`:winarm64` of 2026-10-10): [`docs/source/gpu-golden-testing.md`](docs/source/gpu-golden-testing.md).

**Six Linux jobs per architecture, not eleven** (2026-10-13). Every job pays the host prologue,
disk cleanup plus the image pull, before it builds anything: 8-9 min on x64 and 6-7 min on arm64
(runs 38402811139, 38368740921, 38265932848 and their arm64 twins). Six of the eleven jobs did less
work than that, so they share jobs now; nothing was dropped:

- `clang-tests`, `gcc` and the release build keep their own jobs.
- `sanitizers` runs ASan + the fuzz seeds, then TSan. It empties the one compiler-cache directory
  between the halves, so `X64-clang-asan` and `X64-clang-tsan` stay apart.
- `static-analysis` runs the format check, one `linux-debug-clang` build for both coverage and the
  analyzers (two jobs used to build that tree), then the Rust renderer tests and lints.
- `profiling` runs the clang profiling build and its benchmarks, then the gcc ones.
- `release` adds the docs and the wasm demo on x64, uploaded for [the docs site](#the-docs-site).

Each part after the first runs whatever the one before did (`if: !cancelled()`) and checks its own
build's outcome, so a red step still names the check that failed. Measured on f340258a against the
three runs before it: x64 run 38424781404 took 215 job-minutes (was 248-255), 53 of them prologue
(was 106-114); arm64 run 38424781448 took 173 (was 204-207), 39 of them prologue (was 64-68). The
longest merged job (33.8 min, arm64 `sanitizers`) stays well under `clang-tests`.

**The old layout is one switch away.** Dispatching `linux-x64.yml` or `linux-arm64.yml` with
`split-jobs` runs each merged job as a matrix of its parts: `asan`/`tsan`,
`format`/`analysis`/`rust`, `clang`/`gcc`, and on x64 `release`/`docs`; eleven jobs on x64 and ten
on arm64, through the same steps. A push runs the merged jobs, whose matrix is the single part
`all`.

### The riscv64 lane

**Cross-built on amd64, tested under QEMU**, the family's riscv64 lane (owner decision 2026-10-01,
hub CON48; why and how:
[`riscv64-cross-test-lanes.md`](third_party/ANTfrastructure/docs/riscv64-cross-test-lanes.md)).
`linux-riscv64.yml` is one `uses:` onto the hub's `container-ci-riscv64.yml`. Its container half,
`scripts/linux/run-riscv64-tests.sh`, calls `riscv64_cross_env`, precompiles the Slang shaders on
the host, builds the `linux-riscv64-cross` preset (Debug,
`Rust_CARGO_TARGET=riscv64gc-unknown-linux-gnu`) and runs ctest.

- **What runs:** 700 of the 703 Debug tests, against the image's riscv64 Vulkan loader, GLFW and
  Rust cxx bridge; the GUI input sweep is excluded everywhere and two goldens are disabled. A
  push runs the 658 CPU tests (523 s of ctest, 32 min end to end in run 38165366757), the weekly
  shards the 42 GPU tests. Measured 2026-10-01 on a 32-core host: 13 min end to end from a cold
  build at `-j8`, of which ctest is 415 s (serial, as in the x64 lane).
- **The GPU suites run weekly, in shards** (owner decision 2026-10-06). `Integration.` and
  `GoldenRender.` run serially (`-j1`) under `xvfb-run` with the `libkata_fp16_helpers.so`
  `LD_PRELOAD` built from `scripts/riscv64/fp16_helpers.c` (riscv64 glibc lacks the fp16 conversion
  helpers llvmpipe's ORC JIT calls). A shard log's `cannot be preloaded` lines are noise from the
  x86_64 `ctest` and that `env`; the riscv64 test loads the shim. On a 32-core host the suites pass
  (35/40 measured 2026-10-03 at `-j8`; the five misses were contention; a heavy path-tracing test
  took 23 min at `-j1`). One 4-vCPU runner would need about a day: the eight shards that pass below
  add up to 10.4 h of ctest. The serial lane before the shards hit its 7200 s ceiling on
  `PathTracingAntiAliasesGeometricEdges` after 5 h (run 37229293578), and every run from 6fc0beb2
  on was cancelled, so the lane gave no signal at all.
- **The shards.** `linux-riscv64.yml` has a `schedule:` (Saturdays 01:43 UTC) and a `gpu-tests`
  input on `workflow_dispatch`. Either one skips the CPU job and fans out one job per shard;
  `run-riscv64-tests.sh --list-gpu-shards` prints the matrix, so the list has one home. A
  dispatch's `only-shards` input (comma-separated names from that list) keeps only the shards it
  names, and a name off the list fails the step before any shard starts. `RISCV64_GPU_SHARD` picks
  the shard inside the container:
  - `raster`: every GPU test in no other shard (33 today, two of them disabled), 90 min per test;
  - `clouds`: the two cloud goldens, 150 min per test;
  - one solo shard each, 300 min, for the eight path-traced tests (the five
    `GoldenRender.PathTracing*`, `PathTracedMaskCardShowsItsCutout`,
    `AddedModelAppearsInPathTracing`, `ReloadedModelIsVisibleInPathTracing`) and for
    `RaytracingFrameSkipsTheRasterPass`, the costliest ray-traced one.

  The first weekly run (38015523987, 2026-10-10, at 4910c545), against a 360 min job:

  | Shard | ctest | Job | Result |
  | --- | --- | --- | --- |
  | `PathTracingAntiAliasesGeometricEdges` | 145 min | 168 min | passed |
  | `raster` (31 tests) | 134 min | 167 min | passed |
  | `PathTracingRespondsToTheDirectionalLight` | 71 min | 95 min | passed |
  | `PathTracingAccumulatesAndConverges` | 68 min | 93 min | passed |
  | `clouds` | 66 min | 90 min | passed |
  | `ReloadedModelIsVisibleInPathTracing` | 58 min | 84 min | passed |
  | `PathTracingHonorsTheQualityControls` | 50 min | 77 min | passed |
  | `RaytracingFrameSkipsTheRasterPass` | 32 min | 60 min | passed |
  | `PathTracedMaskCardShowsItsCutout` | 300 min | 334 min | timed out |
  | `AddedModelAppearsInPathTracing` | 300 min | 328 min | timed out |
  | `PathTracingPassesTheWhiteFurnaceTest` | 300 min | 327 min | timed out |

  Under QEMU a raster golden costs 11-24x its x64 time, or one to five minutes when it does little
  but start up; `RaytracingFrameSkipsTheRasterPass` cost 17x, the clouds 22-28x and the path-traced
  tests 32-200x. The path-traced frame is what does not fit: on the shadow rig (1200x768, 8 samples,
  up to 8 bounces) one took 98-189 s, against about 1 s on x64. The runner decides where in that
  range: on the four shards whose frames can be timed, a frame took 0.23-0.27 times that runner's
  own ninja build, and the same 693 build steps ran 391-731 s across the eleven runners. A ceiling
  therefore has to assume about 190 s a frame, and 300 min then hold about 90 frames. In that run
  `PathTracingAntiAliasesGeometricEdges` rendered 87 (145 min, about 280 projected on the slowest
  runner), and the three that timed out render 133, or 86 at 16 bounces for the furnace: they pass
  on the fastest runners (180-220 min) and cannot on the slowest (350-425 min).

  **So four path-traced goldens run a light variant on riscv64** (owner decision 2026-10-12).
  `run-riscv64-tests.sh` exports `KATAGLYPHIS_PT_GOLDEN_VARIANT=light` for the GPU suites and runs
  ctest with `-V`; each of the four logs the variant it ran, and no other lane sets it. They
  accumulate a shorter history and keep their thresholds:

  | Shard | Frames, full → light | Slowest runner, light |
  | --- | --- | --- |
  | `PathTracedMaskCardShowsItsCutout` | 133 → 61 | about 200 min |
  | `AddedModelAppearsInPathTracing` | 133 → 61 | about 200 min |
  | `PathTracingPassesTheWhiteFurnaceTest` | 86 → 46 | about 195 min |
  | `PathTracingAntiAliasesGeometricEdges` | 87 → 57 | about 185 min |

  The projection takes 190 s a frame, and 1.3 times that for a furnace frame at 16 bounces (the x64
  ratio); the fastest runners need about half. The x64 measurements behind each depth, with a
  negative control per test:
  [`docs/source/gpu-golden-testing.md`](docs/source/gpu-golden-testing.md) § The riscv64 light variant.

  Each shard cross-builds for itself (23-33 min before ctest starts), because the hub's reusable
  lane takes no build artifact; the 11 jobs restore the push lane's `riscv64-clang-debug` cache
  and, off a push, never save it. The CPU and GPU runs sit in different concurrency groups, so a
  push cannot cancel the weekly run. `ctest --no-tests=error` fails a shard whose filter matches
  nothing, and an unknown shard name stops before the build. `RISCV64_GPU_SHARD=all` is the old
  serial lane (CPU suites, then every GPU test), for a local host.
  `GoldenRender.GuiInputSweepNeverCrashesOrLosesTheDevice` stays excluded everywhere.
- **No sanitizers, coverage or cargo suite.** No ASan, UBSan, TSan or coverage: the cross clang has
  no riscv64 compiler-rt. That is also why `VulkanEngineCore`'s hard-wired Linux Debug
  `-fsanitize=address` skips a cross build (`CMAKE_CROSSCOMPILING`), and the fuzz targets, which
  need ASan, skip themselves. No cargo suite: OxidANT's own riscv64 lane tests that crate.
- **The Vulkan hook is switched off there.** `source_vulkan_env` would source the x86_64 SDK's
  `setup-env.sh` and undo `riscv64_cross_env`'s `/opt/vulkan/active` paths.

### The job bodies are ANTfrastructure's, resolved at `@develop`

The workflows here are mostly wiring. The steps come from composite actions pulled straight from
ANTfrastructure's `develop` branch (owner directive 2026-09-25; `main` lags it and still names the
retired `:latest-cross` image): `prepare-linux-ci-host`, `run-in-linux-container`,
`compiler-cache-restore`, `compiler-cache-save`, `clean-host-package-check`, `requested-lanes`,
`run-pester-suite`; and from five of its reusable workflows (`lint-gates.yml`,
`submodule-pins.yml`, `container-ci-windows.yml`, `container-ci-riscv64.yml`, `build-docs.yml`).
**There is no pin: a push to ANTfrastructure `develop` changes this repo's CI on the next run**,
which is why both repos ship together with ANTfrastructure first (the rule above). A lane failing in
a step whose `uses:` points at ANTfrastructure is defined there: read the action or workflow there.

### The docs site

**The release job builds it, the hub's `build-docs.yml` checks and publishes it** (2026-10-19). On
x64 the `release` job generates the Doxygen XML, runs `scripts/linux/docs-build-web.sh` and the
wasm budget, and uploads `docs/build/html/` as the `bb-docs-site` artifact. `linux-x64.yml`'s
`docs` job (`needs: build-x64`, `if: !cancelled()`, so a red test elsewhere does not hold the site
back) calls `build-docs.yml` with `artifact: bb-docs-site`: the hub's `.htaccess` and 404 page go
in, a dead internal link or anchor fails the job (`verify_site_links.py`), and the upload runs only
on a push to `develop` or `main`, never on a pull request or a dispatch. The `SERVER`, `USERNAME`
and `PW` secrets map onto its `FTP_*`. `release.yml` downloads every artifact of its run, and
the hub's collector skips both uploads of the site (`*-docs-site`, `docs-site-*`).

What follows from living in `linux-x64.yml`, which keeps `paths-ignore` for docs-only commits and
`cancel-in-progress` off `main`: a docs-only commit publishes the site with the next code push, and
a newer push to `develop` can cancel a publish in flight. The owner accepted both (2026-10-20). The
workflow and its inputs: [`docs-sites.md`](third_party/ANTfrastructure/docs/docs-sites.md).

### Lint gates (before anything builds)

`.github/workflows/lint-gates.yml` is a workflow of its own, not a job in a build lane. It pulls no
image and builds nothing (~2 min), and catches a class the rest of CI cannot: `yaml.safe_load`
proves a workflow is valid YAML, not valid Actions, and a bash quoting or undefined-function bug
only surfaces when that line runs, an hour into a gcc build.

- **`lint` is one `uses:` line** onto the lane ANTfrastructure publishes as a `workflow_call`
  workflow, with `submodules: 'recursive'` (the hub's default) and `ratchets: true`.
- **`powershell-lint`** calls the hub's reusable `powershell-quality.yml` on `windows-2025`.
  Its `analyzer-strict: false` keeps the existing `$WebDavPassword` analyzer finding advisory
  while parser and AST-trap failures remain fatal. The shared lane reads the version pin,
  installs PSScriptAnalyzer with bounded retries and reports complexity under ignored `logs/`.

**One command, and it is the same one CI runs:**

```bash
bash ./scripts/linux/run-lint-gates.sh   # the seven always-on gates, then --ratchets
```

The wrapper hands this repo's root to the hub's `linux/scripts/run-lint-gates.sh`, which owns the
seven always-on gates (shell, workflows + CI image refs, secrets, python, shared-config drift,
consumer pin-forwarding, README length), their pinned binaries and the contracts behind them
([`code-quality-tooling.md`](third_party/ANTfrastructure/docs/code-quality-tooling.md#the-scan-root-contract)).
All gates run even after one fails. The wrapper passes `--ratchets` itself since 2026-10-16; the
hub's consumer hook, once a clone opts in with
`git config core.hooksPath third_party/ANTfrastructure/shared/linux/templates/git-hooks`, runs the
seven without it. So run the wrapper before pushing a comment, size or shell change: the
comment-size ratchet once failed `Lint gates` that way (run 37486873828). The doc-link gate reads
the submodules, so a fresh clone needs `git submodule update --init` first.

**`--ratchets` adds** `doc-links`, `large-files`, `doc-dupes --against-hub` and the eight `--root`
measurement gates (stdout-returns, masked assignments, trailing conditionals, comment size, code
size, complexity, dead functions, shellcheck warnings). Their freeze files sit at the repo root
(`comment-size.allow`, `dead-functions.allow`, `trailing-conditional.allow`,
`shellcheck-warnings.allow`, `large-files.allow`, `doc-dupes-hub.allow`), each **two-way**: a new
offender fails, and so does a frozen row whose offender is gone. `doc-links` has no freeze file by
design. `large-files` measures the working tree, so on a Windows checkout with
`core.autocrlf=true` it reads `Resources/Models/suzanne.obj` at 1017 KiB against the frozen 986 and
fails; the LF checkout CI grades passes.

### Rust and Kompute in the Linux jobs

**Both Linux legs test the Rust renderer crate** (x64 and arm64, since 2026-10-01):
`scripts/linux/run-cargo-tests.sh` (`cargo test -p kataglyphis_webgpu_renderer`) in the
`static-analysis` job of `reusable-linux.yml`; its GPU tests find the image's lavapipe. On arm64 the
script sets `LP_NATIVE_VECTOR_WIDTH=128` (since 2026-10-06): the image's 256 (hub CON44) serves this
repo's BVH builds, and LLVM's AArch64 JIT aborted OxidANT's renderer tests on it about one run in
five (`LLVM ERROR: Do not know how to split this operator's operand!`). Before this, the crate was
compiled twice in this repo (the Rust bridge and the wasm demo) but its ~150 tests ran only in
`OxidANT`'s own workflow, so edits to `crates/webgpu_renderer` from this working tree got no test
signal until the submodule was pushed separately.

**The same job lints the whole OxidANT workspace** (since 2026-10-06):
`scripts/linux/run-cargo-lints.sh` runs the hub's `cargo_fmt_clippy.sh`, `cargo fmt --all --check`
plus `cargo clippy --all-targets -- -D warnings`, and fails the job like the tests do. Its clippy
scope is OxidANT's own lane's (`ci-container-steps.sh fmt-clippy`): `--workspace --locked`, since
OxidANT has no Cargo features from 3.0.0. A pin that OxidANT's lane passed therefore passes here, and an edit to any crate
from this tree gets a lint signal before the submodule is pushed. At the 2026-10-06 pin its clippy
took 46 s in `:latest` from a cold target directory on a 32-core host.

**`clang-tests` (both arches) is the only build of `Src/KomputePlayground`** (since 2026-10-06): it
configures with `-DKATAGLYPHIS_BUILD_KOMPUTE_PLAYGROUND=ON`, and its ctest step runs
`KomputePlayground.ArrayMultiply` on llvmpipe with the other suites, so the demo cannot rot unseen
again. Kompute comes from the `third_party/KOMPUTE` submodule and keeps exceptions on (its headers
throw; the playground links neither `myproject_options` nor its `-fno-exceptions`), and the SDK's
`glslangValidator` compiles the GLSL shader into the build tree. Every other build leaves the option
OFF, so none of them fetches Kompute's `fmt`.

## Code Conventions (C++ engine)

- **Exceptions are disabled project-wide** (`/EHs-`, `-fno-exceptions` in
  `cmake/ProjectOptions.cmake`; `VULKAN_HPP_NO_EXCEPTIONS` in
  `Src/GraphicsEngineVulkan/CMakeLists.txt`): vulkan.hpp calls return `ResultValue`; `throw`/`try`
  will not compile. `ASSERT_VULKAN(val, "msg")` (`common/Utilities.hpp`) logs critical and aborts;
  use it on creation/allocation calls only.
- Graphics pipelines are built via `kataglyphis.vulkan.pipeline_builder`
  (`vulkan_base/PipelineBuilder.ixx`); do not hand-roll the create-info chain.
- Buffer/image memory goes through VMA (allocator owned by `VulkanDevice`);
  `VulkanBuffer`/`VulkanImage` are move-only with destructor release (`cleanUp()` remains for
  explicit early teardown and is idempotent).
- A `VkPipelineCache` persists to `pipeline_cache/kataglyphis_pipeline.cache` (gitignored, written on
  graceful shutdown only).
- Slang emits `"main"` as the SPIR-V entry point name (not the Slang function name), so all `pName`
  values in pipeline creation use `"main"`.
- Clip-space y is down in Vulkan and up in WebGPU, so a fullscreen pass takes its uv from
  `fullscreen_vs` and inverts it with `fullscreen_uv_to_ndc` (`common/fullscreen.slang`, switched per
  target), never by hand: a copy mirrors one renderer, and a vertically symmetric scene hides it.
  [`docs/source/shader-sharing.md`](docs/source/shader-sharing.md).
- Model-loading architecture (the two loaders, the async parse/upload split, the multi-mesh flow):
  [`docs/source/model-loading.md`](docs/source/model-loading.md).
- Per-unit verification pattern (container build -> direct test exe -> validation run):
  [`docs/source/gpu-golden-testing.md`](docs/source/gpu-golden-testing.md). The chronological log of what changed
  and why: [`docs/source/cpp-renderer-improvements.md`](docs/source/cpp-renderer-improvements.md). Do not restate
  either here; those documents are the source of truth.

## Code Conventions (PowerShell scripts)

The rules are upstream's, recorded for every consumer in
[`adopting-in-a-new-project.md`](third_party/ANTfrastructure/docs/adopting-in-a-new-project.md);
they are restated here because the next new script gets written in *this* repo, and the exceptions
below are this repo's.

- **Every `.ps1` and `.psm1` declares `#requires -Version 7.0`**, every tracked file outside
  `third_party/` included. The rule, and why a version header beats failing deep inside a pwsh-7
  construct:
  [adopting § 8](third_party/ANTfrastructure/docs/adopting-in-a-new-project.md#8-calling-conventions-what-every-consumer-looks-like).
- **Executable scripts are PascalCase `Verb-Noun`, with a verb `Get-Verb` approves**:
  `Build-Windows.ps1`, `Invoke-ClangClRelease.ps1`, `Resolve-BuildModule.ps1`,
  `Test-AllConfigs.ps1`, `Compare-RendererPixels.ps1`. Spell the case exactly when you reference one:
  Windows ignores it, but git pathspecs, ripgrep globs and the Linux lane do not, so a lowercase
  spelling in a doc or workflow matches nothing.
- **Two shapes break the plain `Verb-Noun` mould on purpose.** Pester suites are
  `<Subject>.Tests.ps1`, mirroring the script under test where there is one
  (`Resolve-BuildModule.Tests.ps1`, `Invoke-SyncValidation.Tests.ps1`), or naming the repo property
  under test where there is not (`SharedConfig.Drift.Tests.ps1`, `CMakePresets.Integrity.Tests.ps1`). Modules take the dotted noun form `<Area>.Common.psm1`
  (`scripts/windows/modules/Compare-Renderer.Common.psm1`, matching ANTfrastructure's
  `Windows<Area>.Common.psm1`) and export explicitly via `Export-ModuleMember`.
- **Bash is not renamed to match**: `Get-Verb` is a PowerShell notion, so the shell scripts stay
  kebab-case (`scripts/linux/run-release.sh`, `build-coverage-llvm.sh`). The lone exception is
  `scripts/agentic-loop/Run-AgenticLoop.sh`, spelled to sit beside its PowerShell twin
  `Invoke-AgenticLoop.ps1`.

## Agentic Loop

An autonomous planner/executor loop improves the engine. The **planner** (expensive, powerful
model) analyzes the codebase and writes detailed task entries to `BACKLOG.md`; the **executor**
(cheap, fast model) drains the queue one task at a time, building and testing as it goes. The queue
must be fully drained before the planner adds new tasks; tasks marked `- [b]` are blocked and do
not count as pending.

Two engines, selected by `engine` in `scripts/agentic-loop/AgenticLoop.config.json`,
`-Engine`/`--engine`, or the `AGENTIC_ENGINE` env var: **`claude`** (default: Claude Code CLI, Opus
5 planner with Fable 5 fallback, Sonnet executor) and **`opencode`** (GLM 5.2 planner, DeepSeek v4
Flash executor). Both read the same role prompt: ANTfrastructure's shared prompt plus this repo's
overlays in `scripts/agentic-loop/prompts/`.

**The loop itself is upstream**: `WindowsAgenticLoop.Common` (PowerShell) and `agentic-loop.sh`
(Bash) own it, and both role prompts are single-sourced there too. Architecture, config keys and
the module API:
[adopting § 4](third_party/ANTfrastructure/docs/adopting-in-a-new-project.md#4-the-agentic-loop) and
[`windows-agentic-loop.md`](third_party/ANTfrastructure/docs/windows-agentic-loop.md); build matrix
and sanitizer-aware tests:
[`agentic-loop-build-matrix.md`](third_party/ANTfrastructure/docs/agentic-loop-build-matrix.md).

This repo owns a per-role **delta**: `scripts/agentic-loop/prompts/planner-overlay.md` and
`executor-overlay.md`, declared in the config's top-level `promptOverlays` block. At start-up the
loop composes the shared system prompt plus the overlay into one text and delivers it to `claude`
via `--append-system-prompt-file`, and to `opencode` by regenerating `.opencode/agents/<role>.md`
from the pinned hub (opencode takes no prompt file on its command line). **`.opencode/agents/` is
therefore gitignored: edit the overlay, never the generated file.** Run the loop with
`scripts/agentic-loop/Invoke-AgenticLoop.ps1` (Windows, PowerShell 7+) or
`scripts/agentic-loop/Run-AgenticLoop.sh` (Linux, needs `jq`). What this repo configures and where
it deviates from upstream defaults:
[`scripts/agentic-loop/README.md`](scripts/agentic-loop/README.md).

## Docs

**Write docs like a developer: short and precise** (owner, 2026-10-19). Say what a thing does,
how to run it and what breaks; no filler, no marketing, no repetition.

Each topic has exactly one home; link, do not copy. Reusable topics live in ANTfrastructure (the
rule above; its pages are listed in
[What ANTfrastructure owns](#what-antfrastructure-owns--links-only)), project-specific ones here:

| Where | Owns |
| --- | --- |
| `README.md` | The front page: what the project is, highlights, the quick start, one row per guide ([the family's README rule](https://github.com/Kataglyphis/ANTfrastructure/blob/develop/docs/repository-family.md#the-readme-rule)) |
| `BACKLOG.md` | **All** open work: sized commitments, then unsized ideas and recurring chores |
| `AGENTS.md` (this file) | How to build/test/run here, invariants, code conventions |
| `docs/source/cpp-renderer-improvements.md` | C++ engine chronological change log |
| `docs/source/model-loading.md` | Model-loading architecture: the two loaders, async parse/upload split, multi-mesh MeshRange flow |
| `docs/source/webgpu-renderer-roadmap.md` | **Pointer only.** Rust WebGPU renderer status per feature; owned by OxidANT at [`third_party/OxidANT/crates/webgpu_renderer/docs/webgpu-renderer-roadmap.md`](third_party/OxidANT/crates/webgpu_renderer/docs/webgpu-renderer-roadmap.md) (decision D6) |
| `docs/source/webgpu-gltf-rust-plan.md` | **Pointer only.** Original WebGPU + glTF Rust renderer plan (milestones 1–5), kept for the record; owned by OxidANT at [`third_party/OxidANT/crates/webgpu_renderer/docs/webgpu-gltf-rust-plan.md`](third_party/OxidANT/crates/webgpu_renderer/docs/webgpu-gltf-rust-plan.md) |
| `docs/source/shader-sharing.md` | Why/how one Slang source serves both renderers, where the two diverge, and which C++ shading path reads which `ObjMaterial` field |
| `docs/source/shader-build-pipeline.md` | The Slang→SPIR-V/WGSL build step: build wiring, manifest, staleness rules, fast iteration |
| `docs/source/webgpu-srgb-audit.md` | Colour-space decisions (no known deviations) |
| `docs/source/code-quality.md` | clang-format / clang-tidy / cmake-format commands + cadence |
| `docs/source/container-build-caching.md` | This repo's container transport numbers, sccache volume, incremental-build wiring |
| `docs/source/gpu-golden-testing.md` | GPU golden test suites, skip-without-GPU behavior, the Linux CI run on llvmpipe, host verification loop, synchronization validation |
| `docs/source/path-tracing.md` | Path-tracing mode: pipeline shape, estimator, NEE, accumulation |
| `docs/source/clouds.md` | Volumetric clouds: pipeline shape, estimator, UBO/constants tables, queue ownership, compositing contract |
| `docs/source/renderer-bounds-invariant.md` | **Pointer only.** WebGPU renderer bounds invariant; owned by OxidANT at [`third_party/OxidANT/crates/webgpu_renderer/docs/renderer-bounds-invariant.md`](third_party/OxidANT/crates/webgpu_renderer/docs/renderer-bounds-invariant.md) |
| `docs/source/LICENSES-README.md` | Third-party license documentation |
| `docs/source/` | The site: every guide above and `README.md`, `getting_started.md`, `documentation_workflow.md`, `webgpu_demo.md`, `wsl2_vulkan.rst`, `literature.md`, plus `graphviz_files.rst`, which `graphviz_generator.py` writes during the docs build. A new page goes here and into `index.rst`'s toctree |
| `scripts/agentic-loop/README.md` | Agentic loop consumer half: what this repo configures (and what deviates from upstream defaults), its two runners, its two prompt overlays; loop architecture and usage are upstream's |

Keep docs, scripts and presets aligned: when you change build behaviour, update
`docs/source/getting_started.md` and this file in the same change, and `README.md` only when its
quick start or highlights change.
