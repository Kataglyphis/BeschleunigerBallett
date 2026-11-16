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

- [ ] `Rust` **Bin punctual lights conservatively, then stop an empty tile from iterating every light**
  (M, a correctness bug before a cost). `third_party/OxidANT/shaders/forward/forward_lighting.slang`'s
  `punctual_lighting` falls back to all lights when a tile's
  binned count is 0, so empty screen costs up to 256 iterations per fragment. The fallback is
  load-bearing: `tile_rect_for_light`'s 7-point AABB under-covers a sphere's true screen
  silhouette by up to `D / sqrt(D² - r²)`, even with every point in front of the camera.
  Near-plane straddling is handled, and since OxidANT `bfa19de3` a tile past
  `MAX_LIGHTS_PER_TILE` lists none, so it shades every light too. A non-empty tile still shades
  only its binned lights, so a light the AABB under-covers is cut off there: a visible error.
  Next: a projection-aware sphere screen bound (Mara & McGuire 2013), then a completeness test
  with unconstrained random ranges that fails today, only then remove the fallback.
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

- [b] `C++` **Renderer-level RAII cleanup consolidation** (M, **blocked on being testable**) —
  `VulkanRenderer`'s hand-ordered `cleanUp()`
  and the device-lost special case in `App.cpp`, which skips `scene->cleanUp()` and
  `gui->cleanUp()` when the device is lost. Device loss cannot be induced here, so removing
  that guard would be an untestable change to a path that only runs after a failure. First
  add a way to simulate device loss (a device-simulation layer, or fault injection behind a
  debug flag).
  Related, owned by this entry: `Scene`'s destructor undoes that guard. `scene` is declared
  before `vulkan_renderer` (`App.cpp`), so `~Scene()` runs its `cleanUp()` after
  `VulkanRenderer::cleanUp()` has destroyed the VMA allocator and the logical device. Benign
  only because the leaf `cleanUp()`s are idempotent.
- [b] `Rust` **Basis ETC1S/UASTC transcoding** (M, **blocked on a transcoder and a test asset**).
  KTX2 BCn passthrough is done; supercompressed files are rejected with a clear error
  (`ktx2_loader.rs`). The viable transcoder is the `basis-universal` crate, a C++ binding,
  and the repo has no Basis-compressed KTX2 test asset. Do it as a deliberate cycle: vendor
  `basis-universal`, generate an ETC1S and a UASTC `.ktx2` with `toktx`/`basisu`, then
  transcode to a BCn `CompressedFormat` on desktop and to ETC2/ASTC on the web path.
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

- **[P2] A receiver-plane depth bias for the PCF taps.** `cascaded_shadow.slang` compares every
  tap against the centre texel's depth with one bias, and OxidANT's
  `forward/forward_lighting.slang` (`shadow_visibility`) copies that kernel and its bias since the
  2026-11-15 pin, so on a receiver sloped against the light the outer taps shadow it in both
  renderers. On the shadow rig, lit surfaces far from any shadow darken from radius 2 (the
  default: 15 % of them by 3+ levels), growing with the radius
  (docs/source/gpu-golden-testing.md § The PCF radius bound, measured). Offsetting each tap's
  reference depth by the receiver's depth slope removes it. Change both kernels together; the
  shadow goldens will move, and `DISABLED_PcfRadiusSweepMeasuresTheShadowRig` shows the effect.
- **[P3] The path tracer's GGX uses the direct-light Smith remap.** `brdf.slang`'s
  `geometry_smith` takes Epic's `k = (r + 1)^2 / 8`, meant for analytic lights; for sampled light
  Epic uses `k = alpha / 2`. Both path tracers evaluate the shared BRDF for their bounces too, so
  a smooth metal over-masks at grazing angles: a white metal at roughness 0.3 returns 0.668 of a
  uniform sky at 60 degrees (`PathTracingPassesTheWhiteMetalFurnaceTest`). A shared change in
  OxidANT, for both renderers; the furnace bounds move with it.
- **[P3] One path estimator for both path tracers.** OxidANT's `tracer/path_estimator.slang` is
  a port of `path_tracing.slang`'s RNG, orthonormal basis, visible-normal GGX sampling, its pdf
  and the lobe probability; the C++ kernel keeps its own copies. Moving the estimator into
  `common/` and importing it from `path_tracing.slang` makes a fix land in both, with a
  dual-target guard like `tests/brdf_test.slang`.
- **[P3] Decouple `cloud.radius` from `cloud.scale`** (`volumetric_clouds.slang`'s `cloud_unpack`:
  `radius = cloudMeshScale * scale * 10`). It resizes the rendered volume, so the cloud goldens
  of both renderers move.
- **[P3] Clouds at half resolution**: a quality trade-off, still open.
- **[P3] Animate the cloud volume** (needs a time uniform and a deterministic override for the
  goldens).

## C++ engine ideas

- **[P3] A `vkCheck()` helper** for the `auto r = device...createX(info); ASSERT_VULKAN(...);`
  idiom (52 sites, about 20 files). A whole-codebase sweep, so it wants a deliberate moment.
- **[P3] `GUI*` is a mutable cross-cutting dependency**: `VulkanRenderer` reads GUI state each
  frame. An app-owned settings struct passed by const reference would decouple them.
- **[P3] A stage registry**, so adding a pass does not mean editing `VulkanRenderer`. (A
  `SwapchainTarget` extraction was investigated 2026-07-23 and rejected: recreation is
  inherently cross-cutting.)

## Rust renderer ideas (OxidANT)

- **[P2] `obj_to_gltf` drops `map_d`.** glTF has no opacity-texture slot, so matching the C++
  MASK cut-out means compositing `map_d`'s red channel into `map_Kd`'s alpha and writing a
  new image. The renderer depends on `image` (PNG) since the 2026-11-15 pin, so the codec is
  there; the editor's library opens the OBJ scenes through this converter.
- **[P2] Render-graph v2**: automatic barrier placement and transient-resource aliasing; the
  deferred, cloud and traced passes have grown the pass count.
- **[P3] `qem.rs::into_primitive` drops morph targets without the comment** its sibling
  `lod.rs::simplify_primitive` carries. Harmless (morphed primitives are excluded from LOD).
- **[P3] Texture streaming / bindless** for photogrammetry-scale scenes.
- **[P3] Golden-image CI** with reference images per GPU vendor, to catch shader regressions the
  driver-independent structural assertions miss.

## Testing and performance

- **[P2] Few input-level tests for the editor.** The GUI sweep writes the shared vars directly, so
  flows such as "a library click picks, Enter loads" (`GUI::renderModelLibrary`) are checked by
  reading ImGui's source, not by a test. `Integration.RasterSegmentCancelsAPendingTracingPick`
  shows one way: it rebuilds a control's ID from its ID-stack scope and presses it with
  `ImGui::ActivateItemByID`. A click versus a double-click needs `ImGuiIO`'s mouse events instead,
  with the row rects taken from ImGui's item data.
- **[P3] Headless assertions for the shadow path** beyond the single darkened-pixel ratio, and for
  the post-processing chain. Both need careful oracle design (`docs/source/gpu-golden-testing.md`
  on why the tonemap is hard to isolate).
- **[P3] Benchmarks still missing**: `record_commands` wall time per render mode at a fixed scene
  and camera, and the upload path (`createBufferAndUploadVectorOnDevice`) for a few payload
  sizes. Gate the pure-CPU ones in CI; GPU timings are machine-dependent.
- **[P3] No budget on the GPU timings.** `KATAGLYPHIS_GPU_TIMING_JSON` and
  `Compare-RendererTimings.ps1` make per-pass timings comparable, but nothing asserts a budget
  for any `GpuTimedPass`.
- **[P3] `resolveModelPath` is about 4x slower on a miss** (8 parent-directory probes;
  `Test/perf/baselines/win-9070xt-32core.json`). Fine at startup, bad in a loop.

## Recurring validation runs

- **`clangcl-profile` (RelWithDebInfo)** after any perf-relevant change and before a
  release: the only configuration where the benchmarks mean something, and it builds
  `perfTestSuite.exe`.
- **Host-GPU sync validation** with `scripts/windows/Invoke-SyncValidation.ps1`
  (`docs/source/gpu-golden-testing.md`). Linux and Windows x64 CI validate on llvmpipe/lavapipe on
  every push; the script covers the real GPU.
- **Release build**: the only configuration with logging compiled out and validation layers
  absent.

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
