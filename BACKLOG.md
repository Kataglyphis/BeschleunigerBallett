# BACKLOG

The single list of open work across the project, sized commitments and unsized ideas
together. Per-area status lives in `docs/source/` (`cpp-renderer-improvements.md`,
`shader-sharing.md`) and, for the Rust WebGPU renderer, in OxidANT's
`third_party/OxidANT/crates/webgpu_renderer/docs/webgpu-renderer-roadmap.md`; this file is
what is still to do. Finished work leaves this file; its history is in git.

Sizes: S (< half a day), M (a day-ish), L (multi-day), XL (multi-week).
Checkbox items are sized and agreed; the prose sections below the fold are unsized
candidates. A candidate graduates by acquiring a size and a decision, or gets dropped.

Priorities: **P0** broken or blocking, **P1** high value, **P2** medium, **P3** low. Sized items
sit under the heading of their priority, an area tag first (`C++`, `Rust`, `Shared`, `CI`,
`Assets`); the executor takes the first `- [ ]` in file order, so the file reads top-down by
priority. Below the fold every section is sorted the same way, each idea tagged with its level.

Checkbox states: `- [ ]` actionable (the agentic loop's executor picks these up), `- [b]`
blocked/parked (waiting on a prerequisite or an owner decision; the loop skips these and
they do not count toward its pending-task queue), `- [x]` done (pruned automatically;
history lives in git).

**GPU verification is no longer blocked on the host.** Several entries used to wait for a
console session on the RX 9070 XT. Since 2026-09-29 the Linux lane runs `GoldenRender.*` and
`Integration.*` on llvmpipe, with sync validation since 2026-10-06, and Windows x64 runs its
Release suite on lavapipe (`docs/source/gpu-golden-testing.md`). Run 37486873224 passed every test
those entries named: the path-tracing, ray-tracing and cloud goldens and
`Integration.RenderModesSelectableInGui`. Only an RX 9070 XT-specific fault still needs the
host.

## Sized

### P0: broken or blocking

No P0 item is open.

### P1: high value

- [b] `CI` **Prove the 2026-11-15 renderer integration on the lanes no Linux run covers** (S,
  **blocked on a push**). The OxidANT pin with the Rust parity work, the clouds on the shared
  `volumetric_clouds.slang`, the shared `brdf.slang` NDF fix and the C++ renderer fixes passed the
  full Linux x64 suite on llvmpipe (`run-ctest.sh --virtual-display`, 0 `SYNC-HAZARD`). Still
  unrun: both Windows lanes (`[ci windows]`: the clang-cl build, the lavapipe GPU suites, Pester),
  both arm64 lanes (`[ci arm64]`), the riscv64 GPU shards (dispatch `linux-riscv64.yml` with
  `gpu-tests`), and on the RX 9070 XT the golden suites plus `Invoke-SyncValidation.ps1`. The
  cloud goldens and the glossy-material goldens are the ones to watch: the NDF change brightens
  every highlight below roughness 0.154.
- [b] `CI` **Golden-render cases fail now and then on the arm64 device** (S–M). In
  `windows-arm64-cross.yml`'s *GPU suites + source checks (arm64, lavapipe)*,
  `EnablingCloudsChangesTheFrameAndAddsDetail` stopped after 7 s with no assertion printed
  (release dry run 38879106284, 2026-10-16), and `DeferredMatchesForwardRoughly` died after
  8 s right after `ASManager: compacted 1 BLAS` (run 39033701859 attempt 1, 2026-10-17). Each
  rerun on the same commit passed, and the 15 GPU jobs since 2026-10-07 otherwise passed both
  cases. No failure message means a crash or timeout of the case, not a pixel mismatch. On
  2026-10-27 590abe0a crashed in 3 of 4 cases over two attempts while 3857d44c passed both,
  although both test binaries differ only in their link timestamp: the crash is intermittent,
  not caused by a commit. WER LocalDumps wrote nothing on the runner, so every test executable
  now links the hub's crash handler and the job uploads its dump with the suite's PDB; the next
  crash names its thread and frame. Until then it can stop any release, since `release.yml`
  needs every lane green; rerunning the failed jobs gets past it.
  - **Suspect: `LP_NATIVE_VECTOR_WIDTH=256` on arm64.** NEON is 128 bits, so llvmpipe's AArch64 JIT
    splits every vector; at 256 it aborted OxidANT's renderer tests about one Linux arm64 run in
    five (`run-cargo-tests.sh` sets 128 there), and these crashes are on a worker thread in the
    compute-heavy cloud passes. The pin exists only for the 8-lane BVH radix sort, deleted
    upstream by Mesa `ebcfbe60` ("Delete radix_sort_vk"), which is in `mesa-26.3.0` and in no
    26.2.x tag. The dump decides it: a frame with no module (`?+0x…`, JIT code) or in
    `vulkan_lvp.dll` confirms it, one in `commitTestSuite.exe` points back at the engine.
  - **Fix applied, waiting for proof (2026-11-14):** mmozeiko/build-mesa published 26.3.0 on
    2026-11-10, so the hub moves `LAVAPIPE_VERSION` to 26.3.0 and no Windows lane sets
    `LP_NATIVE_VECTOR_WIDTH` any more (arm64 runs its native 128). Once the hub pin carrying it is
    in (it is since `b6b009604`), watch the `[ci windows]` rounds: when the arm64 GPU job stays
    green, remove the rerun advice above and tick this item. A crash at 26.3.0 is not the pin;
    read its dump. Blocked on those CI rounds, not on work.
- [b] `Assets` **The DOOM2016 skybox ships without a known licence** (S, **blocked on the owner**).
  `Resources/Textures/Skybox/DOOM2016` (six `DOOM16*.png` faces) is the one skybox the C++
  renderer loads and every package installs, but no notice, source or licence for it exists in the
  repo, and `licenses/third-party-licenses.txt` has no row. Find its origin and licence, or
  replace it with a skybox under a known licence (AssetANT, with an `ATTRIBUTION.txt`), before the
  next release.

### P2: medium

- [b] `C++` **Confirm on the RX 9070 XT that the path-tracing device loss is gone** (S, needs the
  host GPU). Cause found 2026-08-05: `path_tracing.slang`'s committed hit dropped the
  geometry index, so a hit on any mesh but a model's first read past mesh 0's index buffer.
  The kernel now adds `CommittedGeometryIndex()`, and the three tests that lost the device
  pass on llvmpipe in CI. Run
  `commitTestSuite.exe --gtest_filter='GoldenRender.PathTracing*:GoldenRender.GuiInputSweepNeverCrashesOrLosesTheDevice:Integration.RenderModesSelectableInGui'`
  on the host. Once it passes, retire the "Known issue" section of
  `docs/source/gpu-golden-testing.md`, with the `excluded=3` count and the `--gtest_filter` line
  `buildIntegritySuite.cpp` pins in it.

### P3: low

- [b] `Rust` **Indirect draws** (M, blocked until GPU culling emits indirect draw arguments).
  Current culling reads visibility back to the CPU and the render pass still calls
  `draw_indexed`; GPU-generated arguments must replace that readback first.
- [b] `Rust` **WebXR** (XL) — parked.

---

Everything below is **unsized**: ideas, owner decisions and recurring chores not yet
committed to.

## Owner decisions

- **`pointShadowMap`** was removed as dead allocation. Re-add it, or the whole omni pass,
  when the point-light shadow feature is built.

## Shared shader ideas (OxidANT `shaders/`, both renderers)

- **[P3] C++ clouds at half resolution.** OxidANT marches at half resolution on request and
  upsamples with `cloud_upsample_depth_weight`; the C++ engine would need a half-size
  `clouds_main` dispatch and an upsample pass importing it.

## Rust renderer ideas (OxidANT)

- **[P3] Texture streaming / bindless** for photogrammetry-scale scenes.
- **[P3] Golden-image CI** with reference images per GPU vendor, to catch shader regressions the
  driver-independent structural assertions miss.

## Testing and performance

- **[P3] Refresh the nine placeholder perf baseline rows** (`BM_ObjLoaderParseCpu_Suzanne`,
  `BM_GltfLoaderParseCpu_CubeTextured`, `BM_UploadCpuHalf/*`, `BM_RecordCommands*`, their `label`
  says so): they come from an aarch64 WSL host (the last four on llvmpipe under Xvfb), not the
  RX 9070 XT host the file is named for.
- **[P3] Add the RX 9070 XT to `Test/perf/baselines/gpu-timing-budgets.json`.** Its only
  profile is the aarch64 WSL llvmpipe one; record five `RendersNonBlankFrame` dumps from the
  `clangcl-profile` suite on the host and add an entry by the rule the existing one records.

## Recurring validation runs

- **`clangcl-profile` (RelWithDebInfo)** after any perf-relevant change and before a
  release: the only configuration where the benchmarks mean something, and it builds
  `perfTestSuite.exe`.
- **Host-GPU sync validation** with `scripts/windows/Invoke-SyncValidation.ps1`
  (`docs/source/gpu-golden-testing.md`). Linux and Windows x64 CI validate on llvmpipe/lavapipe on
  every push; the script covers the real GPU.
- **Release build**: the only configuration with logging compiled out and validation layers
  absent.
- **ETC2/ASTC Basis transcodes on real hardware**: OxidANT's
  `a_khr_texture_basisu_cube_draws_its_ramp_from_etc2_or_astc` (`tests/ktx2.rs`) skips on
  llvmpipe and lavapipe, which sample neither, so those targets are CPU-verified only. Run it
  on a device that samples ETC2 or ASTC (an Android or Apple GPU, or a desktop driver exposing
  either).

## Toolchain and CI notes

- **[P3] clang-tidy on the module TUs does not fit a CI runner** (measured 2026-10-06). Peak
  working set per TU reaches 10.4 GB (`VulkanRenderer.cpp`), and the 16 GB `windows-2025`
  runner died with `LLVM ERROR: out of memory` on `App.cpp` (run 37466488394), so
  `Build-Windows.ps1` keeps the hub's default skip. A host with RAM to spare can tidy them with
  `-ModuleImportPattern '(?!)'` (120 s on 32 cores, 61 GB).
- **[P3] clang-cl coverage crashes the Debug suite at exit** (2026-10-02). With ASan the profile
  runtime comes from Microsoft's ASan DLL, and `initializeValueProfRuntimeRecord`
  dereferences a bad pointer (run 36997000491). Coverage stays OFF on Windows; revisit when
  the runtime matches the compiler or value profiling can be turned off.
- **[P3] `bugprone-unchecked-optional-access` is off for `Test/`**: clang-tidy 23.1.1 segfaults in
  its dataflow on gtest bodies. Re-enable it once an LLVM release fixes the crash.
- **[P3] Coverage is clang-only** (Linux). GCC and Windows contribute nothing to Codecov.
- **[P3] The `windows-clang-release-wix` package preset is still unbuilt.** It builds on
  `x64-ClangCL-Windows-Release` since 2026-10-21; no lane runs `cpack --preset` on it.
- **[P3] The GUI input sweep runs in no CI lane**, for time (`docs/source/gpu-golden-testing.md`);
  every other suite runs on every arch lane. By design, not gaps: TSan is Linux-only (clang-cl has
  none), coverage is measured on Linux only, the Pester suites test Windows scripts on Windows x64,
  `FileReaderUnit.ReadersRefuseCharacterDevicesInsteadOfBlocking` is POSIX-only, and riscv64 runs
  four path-traced goldens in their light variant (AGENTS.md § The riscv64 lane). OxidANT's own
  lanes run the Rust renderer tests on Windows x64, arm64 and riscv64; this repo's on Linux.

## Build-time costs and papercuts

- **[P3] sccache can serve stale importers after a module-interface edit.** The released sccache
  hashes an importer without the BMIs it imports, and the shared WebDAV cache widens that to
  every build that reads it. Until mozilla/sccache#2876 ships in the image, rebuild with the
  cache bypassed after an `.ixx` edit (hub `docs/windows-container-build-performance.md`
  § *sccache on a C++23 modules build*).
- **[P3] An incremental container build can ship ODR-broken binaries after a module-interface
  change** (2026-07-20: consumers of a changed `.ixx` were not recompiled, an instant ASan
  heap-buffer-overflow at construction). Treat an ASan crash at object construction after an
  `.ixx` edit as build skew and cold-build first. Touching a source did not always recompile
  it either; consistent with the tar-transport mtimes, unverified since.
- **[P3] `-FreshContainer` strands the build cache** on the wcifs fallback path: the next build
  takes 367 s instead of 44 s.
- **[P3] Module dependency scanning** (`clang-scan-deps`) runs over all 53 `.ixx` files each
  configure; measure before assuming it is free.
- **[P3] Host `cmake` is 3.29 and cannot read `CMakePresets.json`** (`version: 10`), and host
  `ctest` cannot read the build trees; run the gtest executables directly.
- **[P3] No host LLVM** (since 2026-09-25); `docs/source/code-quality.md`'s clang-tidy commands need one.
- **[P3] Swapchain screenshots read black while the desktop session is locked**, with no error.
  Take a control capture first. The offscreen capture path the goldens use is not affected.
- **[P3] Restoring a file with `Move-Item` keeps its old mtime**, so ninja can skip the rebuild.
  Touch the file after restoring.
- **[P3] A build container can survive a successful build** (`wcifs teardown lock`). Compare the
  newest `logs/windows/build-summary-*.json` with the container start before assuming a build
  is still running.

## Recorded non-tasks

So the next sweep stops here instead of re-deriving them.

- `VulkanDevice::create_logical_device` (~290 lines) keeps its feature setup inline: it is a
  `pNext` chain of stack locals, and extracting it into returning helpers dangles them.
- The AS rebuild and descriptor refresh around `rebuildObjectDescriptions()` stay inline: the
  four scene-changed paths genuinely differ.
- The per-pass barrier pairs around each `vkCmdBeginRendering` differ (which images, which
  earlier readers, which later consumers) and are not a consolidation target beyond
  `buildImageMemoryBarrier`/`recordImageBarriers`.
- `FileReader.ixx`'s `readTextFile` and `fileExists` have no production caller but stay: the
  fuzz target drives all four readers, and `fileExists`'s comment records the
  throw-on-permission-denied finding.
- `Camera::set_near_plane`/`set_far_plane` have no GUI control on purpose: their defaults
  differ between debug and release, and `far_plane` feeds the cascade fit.
