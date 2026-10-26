# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Every push to `main` releases `v<VERSION.txt's version>` once every lane is green (`release.yml`),
with this file's `## [<version>]` section as the release notes, so before a merge raise
`VERSION.txt` and turn `## [Unreleased]` into that section. The notes of v1.4.2 and earlier are on
the GitHub releases themselves.

## [Unreleased]

### Added

- Verify the Renovate wrapper and shared config in CI without applying updates.
- Shared CI quality reports, Ruff/ty analysis, PowerShell lint and complexity reporting,
  and a manual Python refactor preview. Generated reports stay under ignored `logs/`.

### Fixed

- Write the staged WGSL provenance fixture in binary mode so its explicit CRLF/LF samples keep
  the same bytes on Windows and the mutation assertions test source differences.
- Refresh the commit-suite count after the two ARM provenance tests and pin the
  Hub preflight repair.
- Replace the ARM GPU job's cross-checkout shader mtime false positive with a required
  generated-WGSL and source-provenance check; keep the local mtime guard and upload a crash
  dump only after a real crash created one.
- Reuse ANTfrastructure's Windows build config and source-copy test helpers while keeping
  this repo's path validation, shader fixture and mutation assertions.
- Share C++ and Rust pixel structure verdicts while preserving each renderer's messages,
  thresholds and failure status; test both paths with generated frames.
- Stage local and OxidANT-owned Slang sources beside ARM64 test binaries so native source checks
  resolve `rasterizer/rasterizer.slang` and `forward/forward.slang` from the downloaded artifact.
- Give the local cloud-volume shader a distinct module name so the dual-target
  noise test imports OxidANT's shared `snoise`/`fbm` module; update its SPIR-V
  path and structural tests.
- Limit Windows x64 and arm64 container checkouts to direct submodules; nested documentation
  submodules formed a cycle before either build could start.
- Use ANTfrastructure's CMake argument builder for Windows Release packaging, keeping the
  package target and parallel build setting while removing the copied argument logic.
- Annotate the shared Slang source-root array consumed by the hub driver so
  ShellCheck's warning ratchet retains its existing baseline after the migration.
- **The WebGPU demo builds from a CRLF checkout, and a lock without its version stops the docs
  build** (2026-10-20). `docs-build-web.sh` matched `name = "wasm-bindgen"` up to the line's end,
  which a Windows checkout's `\r` never reaches, so it found no version and skipped the demo without
  a word. `wasm_bindgen_lock_version` strips `\r` first; a `Cargo.lock` without a wasm-bindgen
  version, or no `Cargo.lock`, fails the script with `No wasm-bindgen version in
  third_party/OxidANT/Cargo.lock`. `scripts/linux/tests/test-docs-build-web.sh` runs the script
  against LF, CRLF, pin-less and missing locks in `linux-x64.yml`'s bare-runner job, now *Script
  suites*: 14 assertions, 9 of which fail on the old script. In `:latest` the old script skipped the
  demo on a CRLF copy (exit 0); the new one built it there and on the LF checkout (wasm-bindgen 0.2.129).
- **The docs site's WebGPU demo draws again** (2026-10-20). Chrome refused the Rust renderer's
  forward pipeline layout (`bindGroupLayoutCount (5) is larger than the maximum allowed (4)`) and its
  tonemap shader (`'@location(0)' appears multiple times`), so the canvas stayed empty.
  `forward/forward.slang` binds the tile light grid and its indices in set 3 beside the punctual
  lights (bindings 1 and 2), not in set 4. `tonemap/tonemap.slang`'s `vs_main` returns
  `FullscreenVsOut` itself: copied into a local struct, Slang emitted `FullscreenVsOut` as a plain
  struct with `SV_Position` at `@location(0)`, which naga accepts and Tint does not.
  `third_party/OxidANT` moves to 0e3f2dc, which carries the regenerated WGSL (slangc 2026.17
  reproduces the other eight files byte for byte), the one group-3 bind group, WebGPU's default
  limits on native and the demo's "PBR textures" scene loading from memory.
- **The Windows builds link again with OxidANT 20a03fb** (2026-10-19). Its ESP32 bridge enumerates COM
  ports through `serialport`, which calls SetupAPI and CfgMgr32; corrosion links only the Rust standard
  library's system libraries, so `GraphicsEngine.exe` failed with ten undefined `SetupDi*`/`CM_*`
  symbols on x64 and arm64. `Src/CMakeLists.txt` adds `setupapi` and `cfgmgr32` to the crate's list.
- **The engine draws on a Raspberry Pi 5** (2026-10-18). Its two fullscreen vertex shaders (`post`,
  `deferred`'s lighting pass) read `SV_VertexID`, which Slang lowers to `gl_VertexIndex -
  gl_BaseVertex`; `gl_BaseVertex` needs draw parameters, and Mesa's v3dv aborted at pipeline creation
  (`Unknown intrinsic ... load_first_vertex`, exit 134). They read `SV_VulkanVertexID` now, the same
  index for their `draw(3, 1, 0, 0)`. The 1.5.2 arm64 package with the two recompiled shaders renders
  on himbeere2's desktop (Debian 13, v3dv).
- **The `.deb` puts the app in the desktop's menu, and ships no GLFW development files** (2026-10-18).
  The desktop entry, the AppStream file and the icons were installed to a relative `usr/share/`, which
  the `.deb`'s `/usr` prefix made `/usr/usr/share/...`, where no desktop looks; they go to `share/`
  now, so the `.deb` has them in `/usr/share/` and CPack's AppImage still finds the entry by name.
  GLFW's install rules put its headers, `libglfw3.a`, its CMake package and `glfw3.pc` into every
  package (`/usr/include/GLFW`, `/usr/lib` in the `.deb`, a clash with `libglfw3-dev`);
  `GLFW_INSTALL` is off, the engine links it statically. `scripts/linux/check-deb-layout.sh` reads
  every lane's `.deb` after packaging (it fails on the 1.5.2 one with all four faults), and its
  synthetic-deb suite runs with Linux x64.

### Changed

- Search this repository and its direct submodules for a Bash or PowerShell helper before
  adding one; shared behavior belongs in its upstream owner.
- Use the shared Windows Git helper for WSL Renovate runs on Windows checkouts, so CRLF
  `.gitmodules` does not block the hub bootstrap or dependency update.
- The shared Renovate driver now checks every direct submodule against its remote default branch
  and initializes missing checkouts when applying. FUZZTEST's release branch remains for the
  separate branch-sync command; its old Renovate approval and release-line rules were removed.
  The wrapper can bootstrap an absent tooling hub for a real apply.
- Shared shader math and WebGPU Slang sources now belong to OxidANT. The Vulkan build imports
  its pinned manifest and source tree through ANTfrastructure's Slang drivers; source-integrity
  gates inspect both owners without retaining local source copies.

- **The README header shows BeschleunigerBallett's own logo, `images/logo.svg`** (2026-10-21,
  owner): a ballerina ant in a tutu on the RGB hello triangle, from the family set
  DocumANTation's `scripts/make_repo_logos.py` draws. `images/logo.png`, and `Engine_logo.png`,
  `Engine_logo.bmp` and `faviconNew.ico`, which the installers and the Linux desktop entry show,
  are rendered from it at their sizes (256, 256, 200 and 16 to 256 px).

- **The docs site shows BeschleunigerBallett's own logo** (2026-10-21, owner): `docs/source/conf.py`
  sets `html_logo` and `html_favicon` to `images/logo.svg` after `setup_theme()`, in place of the
  theme's shared wireframe.
- **Rust is always in, and the engine links only OxidANT's cxx bridge** (2026-10-21, owner
  2026-10-20: every Rust feature is mandatory). Gone: the `RUST_FEATURES` option, every
  `if(RUST_FEATURES)`, the `USE_RUST` define and `#if USE_RUST` in `Main.cpp`, the 15 presets'
  `"RUST_FEATURES": "ON"`, the OFF preset `x64-Clang-Windows-Release` and its build preset
  (`windows-clang-release-wix` packages `x64-ClangCL-Windows-Release` now, so the Pester suite allows
  no shared Windows `binaryDir`), the analyzer's `-DUSE_RUST=1`, and `run-cargo-lints.sh`'s
  `--features`, which OxidANT 3.0.0 refuses. `third_party/OxidANT` moves to 6aa85f8, whose
  `crates/cxx_bridge` (`kataglyphis_cxx_bridge`) depends on `cxx` alone; Corrosion imports it instead
  of the root `oxidant` staticlib, which links GStreamer and the ORT loader the engine never calls. So
  `setupapi` and `cfgmgr32` (58f1f76a) leave the Windows link, and the header is
  `oxidant_bridge/lib.h`. `RustBridgeUnit.CallsIntoRust` calls the bridge in every lane's commit suite.
  In `:latest`: `linux-debug-clang` passes 706 ctest entries, `linux-debug-GNU` 663, the riscv64 cross
  build 663 under QEMU; the release `GraphicsEngine` needs libvulkan, libstdc++, libgcc_s, libm and libc
  alone, and the WebGPU demo builds (9 030 191 bytes, in budget). In `:winamd64` clangcl-debug passes
  657 tests plus the compile suite, and the release `GraphicsEngine.exe` imports no SetupAPI, CfgMgr32
  or GStreamer DLL; `:winarm64` cross-builds the arm64 release.
- **Three stale lines** (2026-10-20). `Build-Windows.ps1`'s hub guard named the hub of 2026-09-25,
  but the build calls `Get-BuildConfigurationRow`, `New-PortableBundle` and `Get-PackageVersion
  -VersionFile`, which the hub has since 3d26fac7 (2026-10-19); it now names that commit and checks
  the three before building. AGENTS.md's wrapper map gives `Build-Windows-Container.ps1` the hub's
  `Invoke-RepoContainerBuild.ps1`, which it runs. `Documents/packaging/WelcomeFile.txt` is deleted:
  the hub's `CPackCommon` reads only `docs/packaging/WelcomeFile.txt`.
- **The docs site keeps riding `linux-x64.yml`** (owner, 2026-10-20). It keeps `paths-ignore` for
  docs-only commits and `cancel-in-progress` off `main`, so a docs-only commit publishes the site with
  the next code push, and a newer push to `develop` can cancel a publish in flight; AGENTS.md § The
  docs site records both as accepted. No workflow change.
- **OxidANT c4b2bad -> 20a03fb** (2026-10-19): the `oxidant` staticlib now builds OxidANT's ESP32
  bridge on desktop (`kataglyphis_esp32`, `serialport` without libudev); the renderer crate changed
  only in comments. In `:latest`, `linux-debug-clang` with the Kompute playground builds and ctest
  passes 705 of 705, `GraphicsEngine` links no new library, and clippy plus rustfmt pass.
- **`Build-Windows.ps1` takes its configuration rows, portable bundle and MSIX version from the
  hub** (2026-10-19): `Get-BuildConfigurationRow` replaces its `Get-BuildConfigurationSpec`, and
  `Build-Windows-Container.ps1`'s own table read, which now applies `BuildDirEnv`, hands a set one
  to the container and throws on an unknown configuration instead of skipping it.
  `New-PortableBundle` does the *Portable bundle* step through the build's logged runner, and
  `Get-PackageVersion -VersionFile VERSION.txt -Required` replaces the `Test-Path` guard. In
  `:winamd64` the rows match the old function with and without the `*Env` overrides, the version
  reads 1.5.2.0 as before while a missing `VERSION.txt` throws, and on a CPack build the new step
  returns one result object and writes the same bundle and packages as the old one.
- **The pinned clang-format is the hub's to find** (2026-10-19): `run-static-analysis-format.sh`
  calls `code_quality_require_pinned_clang_format` instead of its own lookup and `PATH` prefix, and
  `Build-Windows.ps1` no longer reads `LLVM_RELEASE` out of `versions.env`: the hub's
  `Invoke-ClangFormatCheck` requires `LLVM_WINDOWS_VERSION` by default (23.1.3, as `LLVM_RELEASE`).
  In `:latest` the gate takes `/usr/local/llvm-target/bin/clang-format` 23.1.3, passes the 217
  files, fails a planted deviation and refuses `LLVM_RELEASE=99.9.9`; in `:winamd64`
  `Build-Windows.ps1 -SkipBuild` passes the same 217 and `LLVM_WINDOWS_VERSION=99.9.9` refuses
  `C:\llvm-patched`'s 23.1.3.
- **`scripts/linux/lib/common.sh` loads the hub's core modules, with no fallbacks** (2026-10-19):
  `logging.sh`, `platform.sh`, `verify.sh`, `parallelism.sh`, `vulkan-env.sh`, `tool-checks.sh` and
  `load-versions-env.sh` through `antfrastructure_source`. Its own module search (`source_module`,
  `have_module`), the logging fallback, `get_build_jobs` and `antfrastructure_version` are gone:
  the bootstrap fails without the hub, so none of them could run. `cmake-configure-build.sh` and
  `run-debug.sh` read `VULKAN_VERSION` with `load_versions_env` in a subshell, so an exported
  `VULKAN_VERSION` now also picks `cmake-configure-build.sh`'s default setup script. In `:latest`
  the old and the new library define the same helpers, and the hub's `cmake_build_jobs 4000`
  (7), the default Vulkan setup script and `run-debug.sh`'s SDK version come out the same.
- **The generated-artifacts suite is the hub's** (2026-10-19): `windows-x64.yml`'s `pester-tests`
  job runs `third_party/ANTfrastructure/shared/windows/tests/Repo.GeneratedArtifacts.Tests.ps1` over
  the pathspecs in `scripts/windows/generated-artifacts.txt`, and this repo's copy of the suite is
  gone. Its `**/__pycache__/` matched no file in git; the list says `**/__pycache__/*`. Under Pester
  3.4.0 the suite passes this tree (2/2) and fails a fixture repo with a tracked `__pycache__` file.
- **The scenes come through the hub's fetcher** (2026-10-19, hub CON98): `cmake/FetchAssets.cmake`
  calls `kataglyphis_fetch_data(LOCK Resources/assets.lock DEST Resources/Models EXTRACT ONLY
  crytek-sponza Dinosaurs)`, without `ONLY` under `KATAGLYPHIS_FETCH_ALL_ASSETS`, so its own
  download, hash, cache and stamp code is gone (245 lines to 36). A configure and `cmake -P` now need
  Python 3.12+ on `PATH`: `:winamd64` and `:winarm64` have `C:\temp\cpython\PCbuild\amd64\python.exe`
  (3.15.0), `:latest` `/opt/venv/bin/python3`. In all three images the old and the new module
  extracted the same 44 files and stamps and each took the other's stamps as up to date; a real
  `cmake --preset linux-debug-clang` in `:latest` fetched the same 44.
- **The agentic-loop runners are watched copies of the hub's templates** (2026-10-19):
  `.antfrastructure-shared.manifest` declares `agentic-loop-sh` and `agentic-loop-ps1`, and
  `sync-shared-config.sh --check` compares their bodies. `Run-AgenticLoop.sh` takes the loop name
  from the checkout's directory (`LOOP_NAME_OVERRIDE` wins) and sets `-euo pipefail` after its header,
  so `--help` prints lines 2-29 (`head -29 | tail -28`), the same text as before;
  `Invoke-AgenticLoop.ps1` gains the template's comment line. The previous copies fail the check.
- **`test-check-deb-layout.sh` runs on the hub's `lib/test-harness.sh`** (2026-10-19): the same six
  cases as `t_assert_rc`, so a failing `.deb` must exit 1, not any non-zero code, and a miss prints
  the gate's output. `linux-x64.yml`'s `deb-layout` job fetches the hub submodule for it. In
  `:latest` it counts 6 assertions, as the old suite did 6 checks, and fails when the gate never
  fails, skips the stray-file check or stops requiring the icon.
- **`release.yml` calls the hub's `release-publish.yml`** (2026-10-19, hub c3fc7585): `stage: check`
  before the lanes, `stage: publish` after all five, with this repo's collector options in its
  `collect:` block. `scripts/linux/collect-release-assets.sh` and its suite are gone, and so is the
  suite's step in `linux-x64.yml`, whose job runs the `.deb` layout suite alone (`deb-layout`). Over
  uploads shaped like this repo's, in `:latest`, the hub's collector with those options gathered
  the same 15 files as the old script.
- **The docs site publishes through the hub's `build-docs.yml`** (2026-10-19). The x64 `release` job
  uploads `docs/build/html/` as `bb-docs-site` after the wasm budget, and `linux-x64.yml`'s new `docs`
  job hands it to the hub's reusable lane: the hub's `.htaccess` (HTTPS redirect, HSTS, the FTP state
  file hidden) and 404 page go in, a dead internal link or anchor fails the job, and the upload runs
  on a push to `develop` or `main` only. Before, the release job uploaded on every event but a
  dispatch, pull requests included, with `continue-on-error: true`. The `SERVER`/`USERNAME`/`PW`
  inputs left `reusable-linux.yml` and its callers (hub 89b098c9). `release.yml` downloads every
  artifact of its run, so `collect-release-assets.sh` skips both uploads of the site; its suite
  proves it. Built in `:latest` from an LF clone and staged by the hub's `docs-site.sh`: 486
  documents, 0 dead links or anchors.
- **The README is a short overview** (2026-10-19, owner: every README in the family): header,
  badges, one paragraph, six highlights, the quick start (cloned over https) and one row per guide,
  after ANTfrastructure's README rule. What left it moved into the docs: the scene licence table to
  `docs/source/getting_started.md` § Scenes, the full feature list, the repository map and the
  roadmap to `docs/source/README.md`, the literature list to the new `docs/source/literature.md`,
  and the WSL2 pointer to the troubleshooting section. Contributing, contact and licence are the
  family page's (`docs/repository-family.md` in the hub).
- **The family's ant replaces the old logos** (2026-10-17, owner: one icon in every repository):
  `images/logo.png` in the README, and `Engine_logo.png`, `Engine_logo.bmp` and `faviconNew.ico`,
  which the installers and the Linux desktop entry show. `faviconNew.ico` now holds 16 to 256 px,
  where it had 16 px only, and `Engine_logo.png` is 256 px, the size of the `hicolor/256x256`
  folder it is installed into (it was 200 px).
- **`docs/source/wsl2_vulkan.rst` is in English** (2026-10-19), with every command and fact kept,
  and without the chat assistant's closing offer it ended with. `getting_started.md` clones over
  https, as the family's repositories and submodules do.
- **`docs/LICENSES-README.md` is in English** (2026-10-19). Every licence, pin, version, path and
  date is unchanged; the translation changes no fact.
- **The docs link the hub instead of restating it** (2026-10-19). `AGENTS.md` keeps this repo's
  half of the CI rule, Renovate, the container transports, the sanitizer modules, the branch sync,
  the history rewrite and the riscv64 lane, and links ANTfrastructure (pinned at a447ef3c) for the
  rest; `docs/container-build-caching.md` keeps its measurements. Three stale facts went with it:
  `--apply` rewrites `requirements.txt`, actions and CMake pins too, not only gitlinks;
  microsoft/STL#6294 is closed (#6298, not yet in the image's VC Tools); the agentic loop needs
  OpenCode v2. `/plan` and `/execute` include the hub's task prompts instead of a copy, and
  `doc-dupes-hub.allow` turns the hub's `doc-dupes --against-hub` gate on.
- **The site uses the family theme's `setup_theme()`** (2026-10-19) instead of loading DocumANTation's
  frozen `conf_base.py` by path: Markdown links and GitHub heading anchors resolve on the site,
  links to unpublished repository files go to GitHub, and the logo, favicon and title are the
  family's. The `custom.css` symlink went; `beschleunigerballett.css` keeps the dark hero. The API
  tree view's 176 dead anchors (Exhale links its labels with their case kept, docutils lowercases
  the ids) now point at the ids, through the theme's own hook since DocumANTation a58ae23 (hub
  89b098c9); `conf.py`'s copy of it went.
- **The fourteen hand-written guides are on the site** (2026-10-19). `docs/*.md` (path tracing,
  clouds, the shader pipeline, GPU golden testing, …) moved into `docs/source/` and its toctree,
  where Sphinx never read them before. Every link and code pointer to them follows, the
  `BuildIntegrity` checks that read their markers included; their links into the hub go to GitHub.

## [1.5.2] - 2026-10-17

### Added
- **Windows arm64 ships a setup `.exe` too** (2026-10-17), as x64 always did. NSIS has no arm64 stub,
  so it is the same x86 stub, which Windows on Arm runs emulated; the arm64 build used to drop it
  because the hub's arch gate refused that stub. The gate now reports an NSIS installer instead of
  failing it, and the `windows-11-arm` job installs it silently, checks that every installed binary
  is ARM64, starts `bin/GraphicsEngine.exe --version` from the install directory and uninstalls it again
  (`installed-run-command`). The hub moves to 6ea38c48. Owner rule of the same day: every release ships
  Windows arm64 installers next to the x64 ones.

### Changed

- 2026-10-17 — **The scenes are AssetANT release assets, not tracked files.** The fourteen scene
  directories under `Resources/Models` (518 of the 633 MiB HEAD tracked) left the tree for
  [AssetANT](https://github.com/Kataglyphis/AssetANT), one reproducible `<name>.tar.xz` per release.
  `Resources/assets.lock` pins each by SHA-256 and URL, and `cmake/FetchAssets.cmake` extracts
  `crytek-sponza` and `Dinosaurs` into `Resources/Models` at configure time through a per-user cache,
  so the paths the engine loads did not move. `-DKATAGLYPHIS_FETCH_ALL_ASSETS=ON` fetches all
  fourteen, and `cmake -P cmake/FetchAssets.cmake` fetches without a configure. The test fixtures
  (`GltfTest`, `ShadowTest`, `Plane`, `VikingRoom`, `plane.obj`, `suzanne.obj`) and
  `Resources/Textures` stay tracked, and the history keeps every scene: nothing was rewritten.

## [1.5.1] - 2026-10-17

### Fixed

- 2026-10-16 — **The Linux packages start outside the build image.** 1.5.0's `.deb`, tarball and
  AppImage need GCC 16.2's `libstdc++` (`GLIBCXX_3.4.36`, newer than any distro's) and carry
  none; the `.deb` claimed `libstdc++6 (>= 13.1)`. ANTfrastructure's
  `kataglyphis_install_linux_closure` now puts the compiler's runtime in `lib/graphicsengine/`
  together with the image's loader and glibc 2.43, and `bin/GraphicsEngine` is a launcher that
  starts the program through them (`LOADER`), so an older glibc (Debian 13, Ubuntu 24.04) is no
  obstacle: the arm64 `.deb`, tarball and AppImage start on a Debian 13 Raspberry Pi 5 (measured
  2026-10-17). The `.deb` takes only the Vulkan loader from the distro. GraphicsEngine finds its
  resources from the working directory, which the launcher leaves alone.
- 2026-10-16 — **A clean-host job proves it**: after each Linux lane, `ubuntu-26.04` and
  `ubuntu-26.04-arm` install the `.deb` with apt, check every installed ELF with `ldd`, and start
  `GraphicsEngine --version` from the `.deb`, the unpacked tarball and the AppImage.

## [1.5.0] - 2026-10-16

### Changed

- 2026-10-16 — **A push runs Linux x64 and the gates; Linux arm64, Linux riscv64, Windows x64 and
  Windows arm64 run on request** (`on-request.yml`, the ci token in a commit message, or each
  lane's Run workflow button), the family's rule in [ANTfrastructure's
  `branch-and-release-strategy.md`](https://github.com/Kataglyphis/ANTfrastructure/blob/develop/docs/branch-and-release-strategy.md).
- 2026-10-16 — **Every push to `main` releases `v<version>`, and only when all five lanes are
  green** (`release.yml`). It carries the `.deb`, AppImage, flatpak (named per arch) and tarball of
  both Linux arches and the MSI, MSIX and zip of both Windows arches plus x64's installer, gathered
  by `scripts/linux/collect-release-assets.sh`; its suite runs with Linux x64 on every push. The
  Linux packages upload as `linux-packages-<arch>`, since one release run holds both arches.
  `gh workflow run release.yml --ref develop -f dry-run=true` runs it all on `develop` and
  publishes nothing.
