# BACKLOG

The single list of open work across the whole project — sized commitments and
unsized ideas together. Detailed per-area status lives in `docs/`
(`cpp-renderer-improvements.md`, `webgpu-renderer-roadmap.md`,
`shader-sharing.md`); this file is what is still to do.

Sizes: S (< half a day), M (a day-ish), L (multi-day), XL (multi-week).
Checkbox items are sized and agreed; the prose sections below the fold are
candidates that have not been sized yet. A candidate graduates by acquiring a
size and a decision, or gets dropped.

Checkbox states: `- [ ]` actionable (the agentic loop's executor picks these
up), `- [b]` blocked/parked (waiting on a prerequisite or an owner decision —
the loop skips these and they do not count toward its pending-task queue),
`- [x]` done (pruned automatically; history lives in git).

> Merged from the former `ROADMAP.md` on 2026-07-20. There is no longer a
> separate roadmap file — one list, so a stale entry in one place cannot
> contradict a fresh one in the other. That had already happened: the roadmap
> still described cascaded shadows as completely broken a day after they were
> fixed.

## C++ Vulkan engine

> **The "two instruments disagree" entry that used to be here was my own
> error, and the mistake is worth keeping.** The golden test appeared to
> report 11.60% darkened / mean 63.70 -> 47.77 while an independent
> measurement of the same states showed no change. The golden numbers had
> been taken with **stale probe SPIR-V still compiled in** - a forced
> full-coverage triangle in the shadow geometry shader plus a forced return
> in `calc_cascaded_shadow`. That is why the figure matched the forced-1.0
> ceiling to two decimal places: it *was* the forced probe.
> `BuildIntegrity.CompiledShadersAreNotOlderThanTheirSources` caught the
> staleness minutes later and I did not connect it to the measurement I had
> just taken. Both instruments now agree. **After touching any shader,
> recompile and re-run the integrity tests BEFORE trusting a rendered
> measurement - including one taken moments earlier.**
- [b] **Renderer-level RAII cleanup consolidation** (M, **blocked on being
  testable**) — the stage-level work landed 2026-07-19; `VulkanRenderer`'s
  hand-ordered `cleanUp()` and the device-lost special-casing in `App.cpp`
  are what is left.

  Deliberately not attempted 2026-07-20. The whole point of the change is the
  device-lost path — `App.cpp` skips `scene->cleanUp()`/`gui->cleanUp()` when
  the device is lost — and device loss cannot be induced here, so removing
  that guard would be an untestable behaviour change to the one path that
  only runs when things have already gone wrong. The payoff is code
  cleanliness, not a user-visible defect. Get a way to simulate device loss
  first (a device-simulation layer, or a deliberate fault injection behind a
  debug flag); then the refactor is safe and its correctness is checkable.

- Animate the cloud volume (needs a time uniform + a deterministic override for the goldens).

## Rust WebGPU renderer (`third_party/OxidANT`)

- [b] **Basis ETC1S/UASTC transcoding** (M, **blocked on a transcoder + a test
  asset**) — KTX2 BCn passthrough is done; supercompressed files are already
  rejected with a clear error (`ktx2_loader.rs`: "supercompression … not supported
  yet"). Two concrete blockers surfaced 2026-07-21: (1) the viable transcoder is
  the `basis-universal` crate, a **C++ binding** (build.rs compiles the upstream
  basisu — a build-system dependency, not pure Rust; no mature pure-Rust
  transcoder exists), and (2) there is **no Basis-compressed KTX2 test asset**
  in-repo (only `tests/assets/red_bc1.ktx2`, plain BC1), so an implementation
  can't be verified headlessly. Do it as a deliberate cycle: vendor `basis-universal`,
  generate an ETC1S + a UASTC `.ktx2` via `toktx`/`basisu`, then transcode to a
  BCn `CompressedFormat` on desktop and to ETC2/ASTC on the web path.
- [b] **Indirect draws** (M, blocked on GPU occlusion culling producing draw
  arguments) — instancing landed without them. Indirect only
  pays once draw arguments come from the GPU (culling compute, batched
  submission); with CPU-side instance counts it adds a buffer round trip for
  nothing. Revisit alongside GPU occlusion culling below, which is what would
  produce those arguments.
- [b] **WebXR** (XL) — parked.
- [b] **Colosseum demo scene** (blocked on you) —
  pick a licensed photogrammetry scan, keep the asset out of git.

  This entry used to claim "LOD + KTX2 machinery is ready" while the LOD
  subsystem was library-and-tests-only, called by no render pass at all. That
  is now genuinely true for LOD (see the render-path item above) — set
  `lod_enabled` before `upload_scene`. **KTX2 is still not ready**: Basis
  ETC1S/UASTC transcoding is unimplemented (`asset/ktx2_loader.rs` rejects any
  supercompression), so a scan shipping Basis-compressed textures will not
  load.

## Cross-renderer

---

Everything below is **unsized**: ideas and recurring chores that have not been
committed to.

## Performance testing

- **Benchmarks still missing**, in rough value order:
  - `record_commands` wall time per frame for each render mode (forward,
    deferred, RT, path tracing) at a fixed scene + camera — the closest
    proxy to "did a refactor make the frame path slower".
  - Upload path: `createBufferAndUploadVectorOnDevice` for a few payload
    sizes, now that the staging buffer is reused (guards against a
    regression back to per-upload create/destroy).
  - Pure-CPU units are the ones worth gating in CI; anything touching the
    GPU is machine-dependent and belongs in the "run it locally" bucket.
  - **glTF parse is now benchmarked** (2026-07-21): `BM_GltfParse_CubeGlb` and
    `BM_GltfParse_CubeTextured` (`Test/perf/perfSuite.cpp`) mirror the OBJ pair -
    parse the document, load buffers, walk every POSITION accessor. They drive
    `cgltf` DIRECTLY with a local `CGLTF_IMPLEMENTATION`, exactly as the OBJ
    benchmarks drive tinyobj, and deliberately NOT `GltfLoader::parseCpu`:
    importing the engine loader would pull `Device`/`Model`/`Texture` (and their
    Vulkan-touching global ctors) into this headless binary - the same
    headless-global-ctor hazard that took the fuzzer down - and also re-collides
    the duplicate `TINYOBJLOADER_IMPLEMENTATION` that only stays benign while
    `ObjLoader.cpp.o` is never linked in. The `.glb` (inline binary buffer) and
    `.gltf` (base64 data-URI) exercise different `cgltf_load_buffers` decode
    paths. Verified building + running under `linux-profile-GNU` (the CI
    benchmark config); host figures belong in the baseline table below, taken on
    a clean host run rather than a container (wcifs I/O dominates the wall time).
- **GPU-side numbers already exist**: per-pass timestamps land in
  `GUIRendererSharedVars::gpuTimings` (GUI "GPU timings" header). ~~A headless
  mode that renders N frames and dumps the per-pass averages as JSON would
  turn them into a comparable artifact~~ — **that exists** (corrected
  2026-08-01): the `KATAGLYPHIS_GPU_TIMING_JSON` export in
  `GpuTimingSubsystem.ixx:211` writes one JSON object keyed by
  `FrontendShared::GPU_TIMED_PASS_EXPORT_NAMES`, the Rust side mirrors it via
  the `dump_gpu_timings` example, and `scripts/Compare-RendererTimings.ps1`
  drives both and prints them side by side. What is still missing is the
  *assertion*: nothing sets a budget for `GpuTimedPass::ShadowCascades` (or
  any other pass), so the artifact is comparable but ungated.
- **Regression tracking**: Google Benchmark can emit JSON
  (`--benchmark_out=... --benchmark_out_format=json`); storing one baseline
  per machine and diffing beats eyeballing console output. **Done
  (2026-07-31):** `scripts/Compare-PerfBaseline.ps1` diffs a fresh JSON run
  against the checked-in `Test/perf/baselines/win-9070xt-32core.json`, flags
  any benchmark that regressed beyond a configurable tolerance (default
  +25%), and is deliberately not wired into CI (see the "measured baseline"
  table below for why: machine-dependent numbers).

### Measured baseline (2026-07-31, clangcl-profile, 32-core 4.3 GHz)

`Test/perf/baselines/win-9070xt-32core.json` is the machine-readable source of
truth (checked in, diffed by `Compare-PerfBaseline.ps1`); the table below is a
human-readable summary of it. Keep the two in sync — a 2026-08-01 re-run
confirmed the `BM_ComputeCascadeData` rows below (the JSON previously had them
wrong by ~3x; fixed to match this table, which a fresh run agreed with).

| Benchmark | Time |
| --- | --- |
| `BM_CameraViewMatrix` | 10.1 ns |
| `BM_ProjectionAndInverses` | 30.3 ns |
| `BM_CameraKeyControl` / `MouseControl` | ~34 ns |
| `BM_AvailableModelPaths` | 2.72 ns (was 859 ns before the `std::span` return, 2026-07-31) |
| `BM_ResolveModelPath_Hit` | 4.1 us |
| `BM_ResolveModelPath_Miss` | 15.7 us |
| `BM_ObjParse_Plane` (1 KB) | 23 us |
| `BM_ObjParse_Suzanne` (1 MB) | 7.1 ms |
| `BM_GltfParse_CubeGlb` | 13.2 us |
| `BM_GltfParse_CubeTextured` | 15.7 us |
| `BM_ComputeCascadeData/1` (`std::array` corners) | 143 ns |
| `BM_ComputeCascadeData/3` (`std::array` corners) | 329 ns |
| `BM_FrustumCull/64` | 339 ns |
| `BM_FrustumCull/512` | 2.93 us |
| `BM_FrustumCullShadowCaster/64` | 326 ns |
| `BM_FrustumCullShadowCaster/512` | 2.77 us |

Two things this baseline already tells us:

- **Asset loading blocks for a long time.** 1 MB of OBJ costs ~7 ms of
  pure parsing; `dinosaurs.obj` is 27 MB, so a load is plausibly ~200 ms
  of frozen main thread. That is the concrete case for the async
  asset-loading item above — it was previously argued from first
  principles only.
- **`resolveModelPath` is ~4x slower when it misses** (8 parent-directory
  probes). Fine once at startup, bad in a loop.

## Recurring validation runs

Debug-only builds are the default working loop (fast, sanitized). Things
that are *not* exercised that way and should be run periodically:

- ~~**Multiview `viewMask` validation warning (observed 2026-07-23)**~~ —
  **CONFIRMED RESOLVED (re-checked 2026-08-01).** The blocker this bullet was
  waiting on (`#2106`, "multiview feature is not enabled") was root-caused and
  fixed on 2026-07-31: `VulkanDevice.cpp` set `features2.pNext = nullptr`,
  severing the whole Vulkan11/12/13 feature chain, so `features11.multiview`
  never reached `vkCreateDevice` (see "GPU host verification" below). The
  follow-up RX 9070 XT run recorded there states the `multiview` VUIDs are
  "completely gone from the log". Both instruments therefore agree and there is
  nothing left to re-run. Historical detail kept below because the
  `clampCascadeCount` machinery it describes is still live code. `VulkanDevice`
  now queries
  `VkPhysicalDeviceVulkan11Properties::maxMultiviewViewCount`
  (`getMaxMultiviewViewCount()`) and both cascade-count decision points
  (startup init, `handleShadowResolutionChange`) route through a new
  `clampCascadeCount()` free function (`CascadedShadowMap.ixx/.cpp`, CPU-tested:
  `ClampCascadeCountRespectsBothLimits` / `ClampCascadeCountFloorsToOne`), so a
  device with `maxMultiviewViewCount < MAX_CASCADES` can no longer create a
  non-conformant render pass. **Could not confirm on the GPU host that the
  original two VUIDs are gone**: the RX 9070 XT run instead hit the
  `multiview`-feature-not-enabled abort tracked in #2106 below (`viewMask is
  0x7, but multiview feature is not enabled` —
  `VUID-VkSubpassDescription2-multiview-06558`) before the render pass this fix
  touches gets far enough to show the original VUIDs one way or the other.
  `maxMultiviewViewCount` itself did read correctly (6, logged at device
  selection). Re-run the golden suite once #2106 is fixed and strike this
  bullet if the original VUIDs are confirmed gone.
- **`clangcl-profile` (RelWithDebInfo) once in a while** — optimized code
  paths differ from debug: different inlining, different UB exposure,
  and it is the only configuration where the benchmarks are meaningful
  (debug timings are noise). Run it after any perf-relevant change and
  before a release; it also builds `perfTestSuite.exe`.
- **`clangcl-tsan` does NOT detect data races** — checked 2026-07-20 by
  building it and inspecting the result. `third_party/ANTfrastructure/cmake/Sanitizers.cmake` warns
  "clang-cl ThreadSanitizer is not supported for target
  x86_64-pc-windows-msvc" and drops the request, so the preset produces a
  plain debug build: no `-fsanitize=thread` in `build.ninja`, no `__tsan_*`
  symbols in the binary. The suite passes 40/40 under it and that result
  means nothing. This entry previously read "data races only show up here;
  nothing runs it today", which was wrong in a way that would have made a
  green run look like evidence.

  Race coverage on Windows is therefore unavailable today. `ThreadSanitizer`
  works on Linux (`linux-debug-tsan-clang`), which CI runs. **Resolved
  2026-07-20: the `clangcl-tsan` preset was removed entirely** (verified
  2026-07-31 — no `tsan` preset remains in `CMakePresets.json` outside the two
  Linux ones; AGENTS.md § "There is no Windows ThreadSanitizer" records it).
  This bullet stays as the record of why a green Windows "TSan" run meant
  nothing.
- **Synchronization validation** — run
  `pwsh -ExecutionPolicy Bypass -File .\scripts\windows\Run-SyncValidation.ps1`
  (documented in `docs/gpu-golden-testing.md`). It sets
  `khronos_validation.validate_sync = true` via `scripts/vk_layer_settings.txt`,
  copied next to the executable for the run, and exits non-zero on any
  `SYNC-HAZARD` in the log. This found 10 real WRITE-AFTER-WRITE hazards in
  July 2026; it is still not part of any automated run (needs a GPU), so it
  needs a deliberate pass after touching render passes, barriers, or
  frames-in-flight — the script just makes that pass one command instead of
  a hand-written layer settings file.
- **Release build** — the only configuration with logging compiled out and
  validation layers absent; behavioral surprises hide there.

## Test coverage ideas

> **This section was rewritten on 2026-07-20 because it had rotted.** An audit
> of every open item against the code found the stale ones clustered almost
> entirely here: two of its three bullets were fully done and the third was
> substantially overtaken. They rotted for a structural reason worth
> remembering — each was completed *as part of a sized item elsewhere* (the
> fuzzing `[x]` above, the GUI round-trip in Completed), and nobody walked back
> up to the unsized prose to strike it. **Any unsized prose that shadows a
> sized item will rot the same way.** Prefer extending the sized item.

- ~~Fuzz the shader file reader and KTX2/texture loading~~ — **done**;
  `Test/fuzz/shader_file_reader_fuzz_test.cpp` and
  `Test/fuzz/texture_loading_fuzz_test.cpp` exist and are registered. KTX2 was
  deliberately descoped: the C++ engine does not use it.
- ~~GUI-state round-trip test~~ — **done**;
  `Test/commit/VulkanEngine/guiSceneVarsRoundTripSuite.cpp`, and it is in the
  Windows CI filter.
- Headless offscreen assertions in the C++ engine — **re-scoped, largely done.**
  `goldenRenderSuite.cpp` now carries 29 runnable GPU tests (forward/deferred raster,
  shadows, frustum culling, RT, path tracing incl. white furnace, textures,
  mask alpha, double-sided, KHR texture transform, second-model load). What is
  still genuinely missing is coverage of the *shadow* path beyond the single
  darkened-pixel ratio, and of the post-processing chain (both need careful
  oracle design — see `docs/gpu-golden-testing.md` on why the tonemap is hard
  to isolate).

- **GUI-input coverage — DONE (2026-07-23).** The question "is every possible
  user input tested?" was NO: the fuzz tests cover only the FILE inputs reached
  via the GUI (models/paths/shaders/textures), and the golden/integration
  suites set only a handful of GUI vars. `GoldenRender.GuiInputSweepNeverCrashes`
  now drives EVERY user-touchable control across its allowed ImGui slider/combo
  range - all ten cloud params, light dir/colour, shadow cascade count (1..8) /
  resolution / distance, PT sample/bounce counts, mode (forward/deferred/RT/PT)
  - in 16 random combinations plus the deterministic all-maximum worst case,
  asserting no crash / device-lost / validation error (debug build runs
  validation + ASan). It is a property test (no pixel oracle). Verified that
  `num_shadow_cascades` 4..8 (slider max 8 > `MAX_CASCADES` 3) is SAFE -
  `VulkanRenderer.cpp` clamps `active_cascades` before the SceneUBO write. Also
  added `GltfParseUnit.TriangleFanIsTriangulated...` and the
  `MeshRangeSlice.*` unit suite.

**Always dump the picture, not just the number** (2026-07-20).
`GoldenRender.DISABLED_DumpsFrameToPng` writes the captured frame, the same
frame with the effect disabled, and an amplified difference, to PNG:

    KATAGLYPHIS_FRAME_DUMP=out ./commitTestSuite.exe \
      --gtest_also_run_disabled_tests --gtest_filter=*DumpsFrameToPng*

A count says how much changed; only the shape says whether what changed is
the effect. This exists because measurement alone twice produced confident
wrong calls on shadows — a shadow baked into the model reported as cast, and
a classifier that only ever saw the ImGui overlay. It immediately earned its
keep by contradicting the golden shadow metric (see the open item above).

**Caution learned the hard way** (2026-07-19, cost most of a day): captures
are **tonemapped**, and the ImGui overlay is composited into them. A pixel
classifier written against raw scene colours (`r < 60 && b < 60`) silently
measured only the overlay and produced two confident, wrong conclusions
("numCascades reads 0 in the shader", "a CPU/GPU UBO race") that had to be
retracted. Any new pixel assertion needs a liveness check and an
unconditional control capture before its output is believed.

## Code quality (see `docs/code-quality.md` for the commands)

- **Decide on the formatting sweep.** **140 of 211** own sources under
  `Src/` and `Test/` do not match `.clang-format` (measured 2026-08-04; see
  `docs/code-quality.md` "Known state" for the commands and the
  `format-drift-denominator` marker it pins). Every container build already
  runs a non-destructive `clang-format --dry-run -Werror` pass and logs this
  count, but does not fail the build on it on purpose - with a backlog this
  size a failing gate gets switched off within a day. Fixing the drift is one
  enormous commit that will collide with anything in flight, so it wants a
  deliberate moment (right after a merge point) plus a
  `.git-blame-ignore-revs` entry. Alternative: format-on-touch only, and let
  the drift shrink over time. **Owner decision, not an agent's.** `-SkipTidy`
  is still passed unconditionally in container builds, so clang-tidy remains
  uncovered (and cannot see module TUs anyway - see below).
- **clang-tidy cannot see C++23 module TUs** (module BMIs reference the
  container layout). Either run tidy inside the container, or accept that
  coverage is limited to the non-module surface.

### Deep code-review pass (2026-07-23)

Three subsystem reviews (renderer, scene/loaders, vulkan_base+memory+app),
each finding verified against the source before any change. **Applied this
pass** (all non-`.ixx`, one build+test cycle):

- **BUG `VulkanInstance.cpp` extension check** — `strcmp(...) != 0 != 0`
  parsed as `(strcmp != 0) != 0`, so `check_instance_extension_support` set
  `has_extension = true` on the first *non-matching* extension and effectively
  always returned true. Fixed to `== 0`.
- **BUG `VulkanSwapChain.cpp` swapchain usage** — `eSampled | eStorage` were
  requested unconditionally while only `eTransferSrc` was capability-guarded.
  `eStorage` is an optional swapchain usage; on a surface that lacks it
  `createSwapchainKHR` fails and `ASSERT_VULKAN` aborts. Now masks the optional
  bits against `supportedUsageFlags` (like `eTransferSrc`). No behaviour change
  on hardware that supports them; portability fix elsewhere.
- **BUG texture-index misalignment (both loaders)** — `GltfLoader`/`ObjLoader`
  `uploadParsed()` skipped `addTexture` on a decode/load failure, but each
  material's `textureID` is a dense index; a skipped slot shifted every later
  texture down one and could index past the descriptor array (malformed embedded
  image / missing `.mtl` file). Now fills the failed slot with the default
  texture to keep alignment. Valid-texture path (golden suite) unchanged.
- Dead/duplicate removals: `ASManager` double-assigned
  `instanceShaderBindingTableRecordOffset`; `VulkanRenderer` unused
  `[[maybe_unused]] toVkResult`; `Mesh` dead identity-matrix `transpose_transform`
  block; `Mesh` reserved-identifier `__vbm` alias.

**DONE — wave 2 (commit `76d05179`, `.ixx`-touching, FreshContainer + 119 tests green):**

- `memory/Allocator` is now move-only + RAII (was copyable with a
  non-releasing destructor — double-`vmaDestroyAllocator` risk). New
  `allocatorOwnershipSuite` pins the move-only contract at compile time.
- Removed dead members/resources: `Mesh::objectDescriptionBuffer`,
  `Model::texture_list` (+ getter), `VulkanImage`/`GUI` `commandBufferManager`,
  `compute_command_pool`, and the `pointShadowMap` allocation (omni shadow map
  system deleted entirely 2026-07-24, see item #12).
- `VulkanDevice` const getters; stray `eIndexBuffer` bit dropped from the two
  material storage buffers.

**DONE — wave 3 (commit `519c2329`, container-verifiable refactors, FreshContainer green):**

- `GltfLoader::parseCpu` → private `processPrimitive()` (the parse suites cover
  the decomposed path); `Mesh::create*Buffer` → one `uploadDeviceLocalBuffer<T>`;
  `VulkanDevice::getQueueFamilies()` delegates to the physical-device overload;
  `CascadedShadowMap` drops dead `getFrustumCornersWorldSpace` and stores/reuses
  the chosen depth format; `Rasterizer` single `OFFSCREEN_FORMAT`; deleted the
  dead pre-module `ObjLoader.hpp` + 3 stale IDE-filter refs.

**GPU verification is now available** (see `[[host-gpu-golden-verification]]`):
the container-built `commitTestSuite.exe` runs the golden suite on the host
RX 9070 XT from the repo root. The render-path refactors below are therefore
verified, not blind — each was confirmed by the full GPU golden suite passing
(the count as of those dates; see `docs/gpu-golden-testing.md` for the current
tally).

**DONE — wave 4 (commit `c43af69b`, GPU-verified):** `VulkanRenderer::record_commands`
decomposed into `recordRasterPass` + `recordRaytracingOrPathTracing` (verbatim,
command order preserved); the 5-site offscreen-texture ternary collapsed to
`activeOffscreenTexture()`.

**DONE — wave 5 (commit `f1032540`, GPU-verified + new test):**
`VulkanImage::transitionImageLayout` (device overload delegates to the
command-buffer overload — barrier logic in one place); `DeferredRasterizer`
hoists the set-0 bind out of the per-mesh loop and drops the lighting pass's
dead dummy vertex input; new `TriangleFanIsTriangulatedAroundTheHubVertex`
parse test pins the fan winding.

**DONE — wave 6 (commit `fba308d7`, GPU-verified):** the `MeshRange` slice loop
+ the two near-identical structs are unified into a shared
`kataglyphis.vulkan.mesh_range` module (`sliceMeshRange` helper), `export
import`ed by both loaders. **Wave 7 (commit `780bc6bd`, GPU-verified):**
extracted `VulkanRenderer::rebuildObjectDescriptions()` for the identical
cleanUp+recreate pair at the four scene-changed sites.

**REMAINING (deliberately left, with reasons):**

- `VulkanDevice::create_logical_device` (~290 lines): **not decomposing the
  feature setup.** It is an interlinked `pNext` chain of stack locals feeding
  `createDevice`; extracting it into returning helpers dangles those pointers,
  and a dangling `pNext` can pass golden *by luck* (freed stack not yet
  clobbered). Only the queue-create-info build is safely extractable — modest
  gain, low priority.
- The AS-rebuild + descriptor-refresh steps around `rebuildObjectDescriptions()`
  are **intentionally inline, not duplication**: the four scene-changed paths
  genuinely differ (createASForScene vs createTLAS vs none; runs before or after
  the object descriptions; updateAllDescriptorSets vs
  updateTexturesInSharedRenderDescriptorSet). A unified helper would
  misrepresent four sequences as one.

**Owner decision:** `pointShadowMap` allocation was removed as dead; re-add it
(or the whole omni pass) when the point-light shadow feature is built.

## CI and release gaps

- **Latent: a `VulkanEngineCore` global constructor faults in a headless
  process** (found 2026-07-21, unsized). Surfaced by the fuzz SEGV above: some
  engine global ctor null-derefs when it runs without the app's `main()` having
  initialised GLFW/Vulkan first. The shipping app is fine (its init order holds),
  and the fuzz fix above stops *linking* it into the fuzzer, so this is not
  blocking — but a global that assumes app init is a real fragility (it would
  bite any future headless/tool use of the engine). Worth symbolizing once (build
  `scene_config_fuzz_test` the old way in the ASan container and run under
  `llvm-symbolizer`) to name the exact ctor, then either make it lazy or guard it.

- [b] **Windows CI: the `:winamd64` image is 54 GB and exhausts the runner**
  (blocked on the owner building/pushing a slim image)
  (root-caused 2026-07-21). `Build/Test/Package` failed after ~58 min: `docker
  pull` of `:winamd64` died repeatedly with `hcsshim::ImportLayer ... not enough
  space on the disk (0x70)` — it imports 54.4 GB of layers into Docker's data-root
  and the runner runs out of room; it never compiled anything. `cleanup-disk-space`
  runs but `docker system prune` reclaims 0B on a fresh runner. A **disk
  diagnostic** now prints per-drive free space + the data-root before the pull and
  fails fast if the data-root drive can't hold ~54 GB (owner chose "diagnostic
  first").

  **What's in the 54 GB** (ANTfrastructure `windows/build.ps1`): the chain is
  `base → nvidia (CUDA+cuDNN+TensorRT ~50 GB) → toolchain (clang/cmake) → media
  (ONNX/GenAI+OpenCV+FFmpeg+LiteRT+TVM+GStreamer) → final`. **The graphics engine
  + wgpu renderer need NONE of it** — verified: `RUST_FEATURES=ON` builds the whole
  RustProjectTemplate workspace, but `crates/media` and `crates/inference` both
  have `default = []` (gstreamer/onnx/CUDA are optional, non-default), so the build
  pulls zero media/ML system libs; it needs only clang-cl + cmake + Vulkan + the
  Rust toolchain.

  **Fix (owner builds the image) — command CORRECTED 2026-07-22 against
  ANTfrastructure `windows/build.ps1`:** the stage chain is
  `base → sdk → toolchain → media → final`, and `toolchain` builds
  `FROM windows-sdk`, so `sdk` cannot be skipped - but on the CPU lane (no
  `-Gpu`) the `sdk` stage is just `docker tag windows-base windows-sdk` (no
  CUDA/nvidia). So the exact command is:

  ```pwsh
  .\windowsuild.ps1 -Stages base,sdk,toolchain   # NO -Gpu
  ```

  That produces `local/kataglyphis:windows-toolchain` (build.ps1:632) - the
  clang-cl + cmake + Vulkan + Rust image with NO media/ML stack. Tag and push
  it as the slim consumer image:

  ```pwsh
  docker tag local/kataglyphis:windows-toolchain ghcr.io/kataglyphis/kataglyphis_beschleuniger:winamd64-toolchain
  docker push ghcr.io/kataglyphis/kataglyphis_beschleuniger:winamd64-toolchain
  ```

  Then repoint `WINDOWS_CONTAINER_IMAGE` in BOTH consumers - the superproject's
  `Windows.yml` and RustProjectTemplate's `rust_windows2025.yml` (and the
  superproject `Build-Windows-Container.ps1` default) - at `:winamd64-toolchain`.
  Mirrors the Linux `:toolchain` split. Relocating Docker data-root only helps
  if a drive has >54 GB free (the diagnostic says); the slim image is the
  reliable fix. **The previously-written `-Stages base,toolchain` would throw
  "requires existing image not found: windows-sdk" - do not use it.**

- **Windows CI runs the CPU-only tests** (since 2026-07-20): 36 tests across
  BuildIntegrity, CameraUnit, SceneConfigUnit, CascadedShadowMapUnit,
  GuiSceneVarsRoundTrip and HelloTestCommit, plus the three fuzz targets'
  seed corpora. Runs in ~14 ms. **The GPU suites (Integration, GoldenRender)
  still do not run anywhere except locally** - they are excluded by name
  rather than left to self-skip, because the container ships the Vulkan
  loader and `SKIP_WITHOUT_GPU` only asks `glfwVulkanSupported()`, which can
  answer yes with no device present and then abort during device creation.
  Closing that gap needs a self-hosted runner with a GPU. **A suite added to
  the repo does not run in CI unless it is added to the filter in
  `Windows.yml`.**

  **And none of it runs by default.** `Windows.yml` is gated on
  `if: contains(github.event.head_commit.message, '[build-win]')`, so the
  whole workflow — build included — is skipped unless a commit message opts
  in. That predates this work and is presumably a runner-cost decision, but
  it means "Windows CI passes" is usually a statement about a workflow that
  never ran. Worth deciding deliberately: run on PRs to `main`, run nightly,
  or keep it opt-in and stop treating a green tick as Windows coverage.
- **Packaging paths are only half exercised** (corrected 2026-08-01 — the
  previous "never exercised" was stale). Linux CI *does* package: `Linux.yml`
  runs a `linux-release-clang` configure/build followed by a
  `--build-target package` step and uploads `*.deb` / `*.tar.gz` / `*.tgz` /
  `*.AppImage` / `*.flatpak`. Still unexercised: **WiX**
  (`windows-clang-release-wix`), which nothing builds anywhere, and **MSIX**,
  which `Windows.yml` collects (`**/*.msix`) but only inside the workflow
  that is itself gated on `[build-win]` — so in practice neither Windows
  packaging path runs unless a commit message opts in.
- **Coverage is clang-only** (Linux). GCC and Windows contribute no
  coverage data, which skews what Codecov reports.
- ~~**Docs builds are unverified.**~~ — **done** (2026-07-31): `docs-build-web.sh`
  now runs `make html` with `SPHINXOPTS="-W --keep-going"` and a follow-up
  `make linkcheck` (also `-W`), so a broken internal link, missing toctree
  page, or malformed rST title fails the step and blocks the FTP deploy after
  it. External URLs are excluded via `linkcheck_ignore` in `conf.py` (they
  flake; this gate is for OUR pages). Local warning count was 6 before the
  fix (below the "don't add `-W` if the count is large" threshold): a
  toctree/xref gap for `graphviz_files` (generator wasn't run locally), the
  same for `api/library_root` (only exists once Exhale runs, which needs
  Doxygen XML nothing currently produces — `conf.py` now writes a placeholder
  page when that XML is absent, mirroring the existing `graphviz_files.rst`
  fallback), a docutils title-underline-too-short warning in
  `wsl2_vulkan.rst`, and a stray `docs/source/_static/VULKAN.md` that the
  `linkcheck`/`latex` builders read as an orphan page even though the `html`
  builder silently excludes `html_static_path` (fixed by adding
  `_static/**` to `exclude_patterns`, so every builder agrees).
- **Docs placement audited** (2026-07-21) - the split is intentional and clean:
  `docs/source/` is the Sphinx site (every source page is in the `index.rst`
  toctree, none orphaned); `docs/*.md` at the repo root are deep dev-reference
  docs, linked FROM the site (e.g. `webgpu_demo.md` -> `webgpu-renderer-roadmap.md`)
  but not built into it. Open CHOICE, not a defect: those root dev-docs
  (roadmaps, cpp-renderer-improvements, shader-*, sRGB audit) are invisible on
  the published site; decide per-doc whether any should move into `source/` +
  the toctree to be surfaced.
- **Golden-image CI** for the Rust renderer: the headless tests already
  render; storing reference images per GPU vendor would catch shader
  regressions that structural assertions miss (they were designed to be
  driver-independent, which is also their blind spot).

## Startup and build-time costs

- **Build transfers dominate (~17 GB/build).** Incremental builds work
  (~230 s vs ~360-480 s cold) but 8.5 GB moves each way. A long-lived build
  container with source-only re-sync would remove both transfers entirely;
  needs lifecycle handling and a way to extract executables for host tests.
- **Outbound `Artifact extraction failed (exit 1)`** is still reported even
  with the cargo subtree excluded. Artifacts do arrive (verified), but a real
  failure here would leave stale host binaries — worth a proper fix.
- **sccache: every write fails, and modules bypass it entirely** (measured
  2026-07-20; corrects the previous "writes nothing (0 bytes)" note, which was
  wrong - the cache holds 981 KiB and simply never grows).

  Two independent problems, worth separating:

  1. **Every attempted write errors.** Reproduced twice: 66 write errors from
     66 misses in one build, then 1 from 1 in a single-file rebuild. The cache
     size does not move between runs. Cause still unknown - `SCCACHE_ERROR_LOG`
     and `SCCACHE_LOG` are now passed to the container (they were not), the
     server was stopped so it would restart and pick them up, and **no error
     log file appeared**. Next thing to try: run sccache by hand inside the
     container against a trivial TU, outside the build orchestration, so the
     failure is not buried in ninja output.
  2. **C++23 module TUs never reach sccache at all.** Module BMI compiles
     invoke `clang-cl.exe` directly rather than through
     `CMAKE_CXX_COMPILER_LAUNCHER`, so most of this build is uncacheable
     regardless of (1). Even a perfect fix to the write errors leaves the hit
     rate bounded by the non-module surface. That reframes the whole item: it
     is worth much less than "20 GiB cache, 0% hit rate" suggests.

  Related gotcha found while trying to force a rebuild: **touching a source
  file usually does NOT cause the container to recompile it.** One touch
  produced a rebuild, three later ones produced none. That makes "touch and
  rebuild" unreliable as a workflow here and is consistent with the tar
  extraction issue already recorded below - worth pinning down, since it also
  means a real edit could in principle be missed.
- **`-FreshContainer` strands the build cache** on the wcifs fallback path:
  the next build takes 367 s instead of 44 s.
- **Module dependency scanning** (`clang-scan-deps`) runs over all 53
  `.ixx` files each configure; measure before assuming it is free.

## Developer-experience papercuts (all hit during the 2026-07 campaign)

- Host `cmake` is 3.29 and **cannot read this repo's `CMakePresets.json`**
  (`version: 10`); only the container's newer CMake can. Anyone running
  `cmake --list-presets` on the host gets a confusing parse error. Host
  `ctest` cannot read the build trees either — run the gtest executables
  directly.
- **LLVM is not on `PATH`** despite being installed — see
  `docs/code-quality.md` for the absolute paths.
- **`run-clangcl-debug.ps1` sets `VK_LAYER_PATH = ''`**, which crashes the
  app at startup with `0xC0000409`. Launch with
  `VK_LAYER_PATH='C:\VulkanSDK\1.4.350.0\Bin'`.
- **Swapchain screenshots read black while the desktop session is
  locked**, with no error — a capture path that silently lies. Always
  take a control capture of a known-good app before believing a black
  frame is a regression. The offscreen capture path used by the golden
  tests does *not* have this problem.
- **Restoring a file from a backup can defeat ninja.** `Move-Item` restores
  the original mtime, so if the backup is older than the compiled object,
  the rebuild is skipped and you test the old binary while believing you
  reverted. Touch the file after restoring.
- **Build containers occasionally survive a successful build**
  (`wcifs teardown lock`); a stale container makes it look like a build is
  still running. Compare the newest `logs/windows/build-summary-*.json`
  timestamp against container start before assuming.
- `scripts/windows/Build-Windows-Container.ps1` takes `-Configurations`,
  not `-Preset`; passing the wrong one silently builds **all four**
  configurations.
- ~~A source file deleted on the host keeps building inside the reusable
  container~~ — **done** (`37a7fdbf`, 2026-07-24). Reproduced 2026-07-19: added
  a probe test, built (it ran), deleted the file, rebuilt — the test still
  ran, and the `.cpp` was still present at `C:\ws\...` inside the container.
  `Invoke-TarPipeBuild` in `Build-Windows-Container.ps1` now prunes the
  reusable container's source tree before streaming in fresh sources on every
  reused-container build, keeping only `build`/`build-*`/`build_*`, `logs` and
  `sccache-local`.

## Architecture debt not yet sized

- **The glTF material fetch is duplicated across three fragment shaders** —
  **DONE (ee05b636)**: extracted into `common/material_fetch.glsl`
  (`fetch_object_description(objectIndex)` + `fetch_material(obj_res)`), replacing
  the identical `object_description`/`MaterialIDs`/`Materials` walk in the forward,
  deferred, and shadow fragment shaders. Also dropped the dead Vertices/Indices
  buffer_reference decls the forward + deferred shaders carried but never used.
  GLSL only, behaviour-preserving: all three recompile, 15/15 goldens unchanged,
  validation-clean (no C++ rebuild needed).

- **`VulkanRenderer` is still the hub.** PipelineBuilder (-416 lines) and
  DescriptorSetGroup (-617) shrank it a lot, but it still owns the
  swapchain, sync objects, UBOs, five stages and four foreign pointers
  (`Window*`, `Scene*`, `GUI*`, `Camera*`). Candidate extractions:
  `FrameSync` (fences/semaphores/frame index), `SwapchainTarget`
  (swapchain + framebuffers + recreation), a stage registry so adding a
  pass does not mean editing the renderer.
  - **FrameSync — DONE 2026-07-23 (`e7e7579d`, module `kataglyphis.vulkan.frame_sync`; the golden suite as of that date green).** For reference, the state was
    `current_frame`, `image_available` + `in_flight_fences` (sized per
    frame-in-flight / `MAX_FRAME_DRAWS`), and `render_finished_by_image` +
    `images_in_flight_fences` (sized per SWAPCHAIN IMAGE). **The per-image vs
    per-frame split is load-bearing and the whole reason to be careful:**
    render-finished is per-image (a per-frame render-finished semaphore can be
    waited on before it is signalled across a swapchain recreate). A `FrameSync`
    class owns these five members + `MAX_FRAME_DRAWS`, with
    `create(device, image_count)` / `cleanUp()` mirroring today's exact sizing,
    `waitForCurrentFrame()`, `advance()`, and handle accessors for the
    acquire/submit/present path; `drawFrame` keeps its acquire→submit→present
    flow verbatim but reads handles from the instance. Do NOT collapse
    `render_finished_by_image` to per-frame. GPU-verifiable: sync mistakes
    surface as validation errors / device hangs — run the full golden suite on
    the RX 9070 XT (`docs/gpu-golden-testing.md`). Touches `VulkanRenderer.ixx` →
    FreshContainer.
  - **SwapchainTarget — investigated 2026-07-23, NOT a clean extraction (skip it).**
    First added the missing recreation coverage (`SwapchainRecreationKeepsRendering`,
    `9b202b68`). Then reading `recreateSwapChain` end to end showed the suggestion
    doesn't hold: the swapchain is *already* a `VulkanSwapChain` class, the
    framebuffers belong to the **stages** (postStage/rasterizer/deferred/skybox
    each own theirs), and `recreateSwapChain` is inherently cross-cutting
    orchestration — it destroys+recreates the stage framebuffers, the GPU-timing
    query pool, the path-tracing accumulation target, and, on an image-count
    change, the UBO vectors + descriptor pools/sets + RT descriptors. A
    "SwapchainTarget" owning "swapchain + framebuffers + recreation" would have
    almost nothing to own and would still need every stage handle, so it does not
    decouple anything. Coordinating recreation IS the hub's job. Better hub-shrink
    targets: the descriptor/UBO re-provisioning could move behind a
    `reprovisionPerImageResources()` helper, but that is a small readability win,
    not a class extraction. (**Done** — verified 2026-07-31: the helper exists at
    `VulkanRenderer.cpp:635-647` and the image-count branch of
    `recreateSwapChain` calls it at `:708-713`; nothing left to extract here.) Aside: the cascade `# cascades` GUI slider still
    advertises 1..8 though the engine clamps to `MAX_CASCADES`(3) (`c49fd8b4`);
    left as-is because GUI has no clean access to `MAX_CASCADES` and the clamp
    already fixes the bug.
- **Device-lost teardown is special-cased in `App.cpp`** (scene/GUI
  cleanup is skipped) — a symptom of ownership living in the wrong place.
  Full RAII up the stack would remove the special case entirely.
- **`GUI*` is a mutable cross-cutting dependency**: both `Scene` and
  `VulkanRenderer` read GUI state each frame. A plain settings struct
  owned by the app, passed by const reference, would decouple them.
- **Multi-object rendering works** (2026-07-20). The shaders index
  `object_description.i[pc_raster.objectIndex]` rather than hard-coding 0,
  `Scene::loadAdditionalModel` / `VulkanRenderer::addModel` can add a model
  without replacing the scene, and `GoldenRender.SecondModelLoadsAndRenders`
  loads a second model at index 1 and asserts it reaches the draw loop and
  changes the frame.

  Still not isolated: the test proves a second model loads, is counted and
  contributes pixels, but does not prove the index ARITHMETIC - two models
  whose materials differ enough to tell apart driver-independently would be
  needed for that. The layout contract is guarded in `pushConstantSuite.cpp`.

  The scene still loads one model by default; a multi-model debug scene is a
  separate decision about what the app should open on.

## Rust renderer ideas (unsized)

- ~~**`self.depth` (the resolved single-sample depth buffer) appears to read as
  wrong/empty on the RX 9070 XT, breaking every one of its consumers**~~ —
  **root-caused and fixed (2026-08-01), two independent bugs, not one.** Batch
  IV named the first correctly (Slang's WGSL backend not honouring `SV_Depth`,
  emitting a colour `@location(0)` instead of `@builtin(frag_depth)`) and that
  fix alone was applied first — it did **not** turn the tests green. An
  isolated repro (a synthetic MSAA depth texture cleared to a known value, run
  through the actual `depth_resolve` pipeline in complete isolation from the
  rest of the renderer) showed the resolve pass itself was correct once
  patched, yet the full renderer still resolved every pixel to 0.0. Second bug
  found by that repro: the forward pass's `depth_msaa` attachment used
  `store: wgpu::StoreOp::Discard` (`forward.rs`, comment read "Discard MSAA
  depth; the resolve pass copies to `depth`") — but the resolve pass is a
  **separate** render pass recorded *after* the forward pass ends, not a
  subpass, so the store op already discarded the content before resolve ever
  read it. Changed to `StoreOp::Store`. With both fixes: `a_cube_hidden_behind_
  another_reads_back_zero_samples`, `two_side_by_side_cubes_are_both_visible`,
  `an_occluded_primitive_is_actually_skipped_in_the_opaque_pass`,
  `loading_a_new_scene_does_not_inherit_the_old_scene_visibility` and
  `ssao_darkens_geometry` are now green. `alpha_modes_blend_and_mask`, also
  flagged here as unexplained by this bug, is green too as of 2026-08-01 (came
  back passing on its own after the `cascade_splits` double-duty fix, before
  any targeted investigation into this specific test) — the "blend pass
  doesn't touch `self.depth`" note above was never confirmed as its actual
  cause. The two GPU-culling-specific tests
  (`an_occluded_primitive_is_skipped_with_gpu_culling`,
  `two_visible_cubes_are_both_drawn_with_gpu_culling`) are now green, recorded
  passing under `1594a4a0` (see the batch XIV planner notes below).
- **Render-graph v2**: the current graph validates declared read/write
  wiring but does not schedule or alias resources. Automatic barrier
  placement and transient-resource aliasing are the natural next steps —
  worth it only when pass count grows again.
- **Texture streaming / bindless**: the renderer binds per-primitive sets;
  a bindless array plus streaming would be needed for photogrammetry-scale
  scenes (the Colosseum case).
- ~~**wgpu timestamp queries** to mirror the C++ per-pass GPU timings~~ —
  **done**: `render/gpu_timing.rs` (`TimedPass`, per-pass averaged ms) and the
  `dump_gpu_timings` example + `scripts/Compare-RendererTimings.ps1` already
  compare timings across renderers, not just pixels.
- ~~**Wasm size budget**~~ — **done**: `scripts/linux/wasm-size-budget.sh`
  (CI) / `scripts/Test-WasmSizeBudget.ps1` (local) build wasm32-unknown-unknown
  release, run `wasm-opt -Oz`, and fail above a 12 MiB budget; wired into
  `Linux.yml` ahead of the docs deploy. The ~3.7 MB figure was stale — measured
  post-opt size is ~8.3 MiB, never previously enforced.

## Housekeeping candidates

- The `x64-Clang-Windows-Release` preset survives only because
  `windows-clang-release-wix` packages from it; if WiX packaging moves to
  ClangCL, that preset can go too.
- `imgui.ini` is tracked and changes whenever a window is dragged — **DONE
  (2026-07-24)**: added to `.gitignore`; `git rm --cached` stopped tracking
  the existing file. It is user state, not a shared layout.

---

## 2026-07-22 deep-dive candidates

Found by a full read of both renderers against the existing backlog (nothing here
duplicates the sections above). Ordered by value-per-effort within each renderer.
Evidence is `file:line` at the time of writing.

### C++ Vulkan engine

**These became CI-testable on 2026-07-22.** The Windows container lane now
builds the engine and runs the CPU test suite in CI (it previously never got past
`docker run`), so items below that were "needs a container build to verify" can
now be proven by pushing with `[build-win]` instead of only on a dev box. That
matters most for #1 (cascade near-plane clipping) and #9 (cascade texel
snapping), which the survey flagged as provable with pure CPU gtests in
`cascadedShadowMapSuite` - they can now go red-then-green in CI like any other
test. Note the fuzz step runs there too, so #14 (cgltf fuzzing) has a home.


1. **Shadow casters in front of a cascade's near plane are clipped away** —
   **DONE (7a1a4ade)**: fixed via DEPTH CLAMP (PipelineBuilder::setDepthClamp
   + guarded depthClamp device feature on the CSM pipeline), not near-plane
   extension - the latter was tried and REGRESSED ShadowsDarkenSomePixels
   because the shader bias is constant in normalized depth, so widening the
   range scaled it in world units and ate contact shadows. Original text: —
   `isVisibleAsShadowCaster` deliberately drops the near plane so tall geometry
   still casts (`Frustum.cpp:85-88`, with a test asserting it), but the cascade
   ortho near plane is fitted from camera-frustum corners with a fixed 10-unit pad
   (`CascadedShadowMap.cpp:225-227`) and the shadow pipeline sets
   `depthClampEnable = VK_FALSE` (`PipelineBuilder.cpp:115`). A ceiling/overhang
   further than the pad casts NO shadow: the CPU keeps it, the rasterizer deletes
   it. Fix: enable `depthClamp` (needs the feature, not requested at
   `VulkanDevice.cpp:487-494`) or extend the near plane to scene bounds along the
   light axis. Test: pure CPU in `cascadedShadowMapSuite` — a point 30 units toward
   the light must transform to NDC `z >= 0`; fails today.
2. **Lit target is `R8G8B8A8_UNORM`** — **DONE (2026-07-22, with the #8
   lighting fix in one unit)**: FP16 offscreen (Rasterizer x2, DeferredRasterizer
   offscreen+finalFormat, rgen+PT storage qualifiers). With diffuse finally
   scaling by the light, the whole scene exceeded 1.0 and the UNORM target
   clamped flat - the two only work together, exactly as the null-result
   sequencing predicted. Post's Reinhard now does real work; PT's 186 ceiling
   is gone (its light golden jumped 0.027 -> 0.751). Historical note below. — implemented and REVERTED after three oracles showed the format
   change is indistinguishable on this scene: whole-frame mean moves ~+0.76 for
   a radiance 2->8 sweep on UNORM and FP16 alike; bright-pixel counts (>200,
   the post-Reinhard UNORM ceiling is ~186) are flat on BOTH at radiance 8 AND
   at radiance 25. Root cause found in the process: `pbrBook.glsl:85` - the
   Lambertian diffuse term is `LambertDiffuse(ambient) * CosTheta(L,N)` and
   NEVER multiplies light_color or light_intensity, so radiance only enters
   via the small specular term and scene luminance never approaches 1.0.
   Sequencing: fix #8 (lighting actually consuming intensity/diffuse/roughness)
   FIRST, then the FP16 targets + rgen rgba16f (sites known: Rasterizer.cpp
   x2, DeferredRasterizer offscreen+finalFormat, raytrace.rgen:26), proven by
   a bright-pixel-delta golden across a radiance sweep that crosses the 186
   ceiling - the whole-frame-mean oracle is measured useless for this.
   Original item: —
   `post.frag:32` applies Reinhard, but the offscreen target is UNORM
   (`Rasterizer.cpp:206`, `:328`, `DeferredRasterizer.cpp:88`) while the G-buffer
   correctly uses `R16G16B16A16Sfloat` (`:89-90`). The radiance slider does nothing
   above the clip point and the whole tonemap/bloom stage is decorative. Test:
   `GoldenRender` at radiance R vs 2R — mean luminance must rise; today it does not.
3. **Only model 0's textures bind, and only the first 24** — **DONE
   (2026-07-22)**: per-model texture_offset in ObjectDescription + all four
   shader fetch sites + flattened binding across all models (warn on cap
   overflow). Golden: sponza-as-second-model must show texture DETAIL in the
   crop (0.045 green vs exactly 0 with the model-0-only binding) - a colour
   oracle was measured blind twice: the dinosaur's mtl ships NO textures at
   all, and sponza's bricks are near-greyscale. En route: untextured .mtl
   materials got textureID 0 (not -1), so the bundled dinosaur rendered its
   Kd colours as flat white for the engine's whole life - fixed in the
   loader with a CPU red/green test. Original text: —
   `updateTexturesInSharedRenderDescriptorSet` hard-codes `getTextures(0)`
   (`VulkanRenderer.cpp:1644`) and clamps to `MAX_TEXTURE_COUNT = 24` (`:1650`).
   The release default scene is Sponza (`SceneConfig.cpp:121`), which has far more
   materials — everything past 23 renders with the wrong texture, and a second
   `addModel` is textured with the first model's array. `runtimeDescriptorArray`
   is already enabled (`VulkanDevice.cpp:591-593`), so a per-scene flat table with
   an offset in `ObjectDescription` is the natural fix.
4. **Moving the model in the GUI never rebuilds the TLAS** — **DONE
   (2026-07-22)**: the transform path rebuilds the TLAS only (BLAS geometry
   untouched) before the descriptor update, which binds the new handle and
   resets PT accumulation. Golden RaytracedWorldFollowsTheModelTransform:
   green 0.397 swung fraction, stale-TLAS red EXACTLY 0 (deterministic RT).
5. **`raytrace.rchit` lights in object space and transforms the normal with `w=1`** (S) —
   ~~`:88` uses `vec4(normal_hit, 1.0)` (picks up translation; a normal needs the
   inverse-transpose), and `:103-104` mix object-space `N`/`hit_pos` with
   world-space `L`/`cam_pos`.~~ **DONE (PT correctness batch, 2026-07-22)** —
   plus the untextured-material clamp-to-slot-0 fetch, same batch. ~~STILL OPEN: `:130-131` hard-codes light colour/intensity~~ **DONE
   (2026-07-22, forward-lighting unit)** - rchit now reads sceneUBO.dirLight;
   the item is fully closed.
6. **glTF is unreachable from the GUI, and `reloadModel` is OBJ-only + null-unsafe** —
   **DONE (2026-07-22)**: the scan accepts .obj/.gltf/.glb case-insensitively
   (red: the list test fails "OBJ-only again"); reloadModel dispatches by
   extension; add_model null-guards (red: SEH 0xC0000005 access violation on
   the shipped binary - the crash was real). Original text: —
   `scanAvailableModels` filters `== ".obj"` (`SceneConfig.cpp:96`, case-sensitive),
   so the in-tree `cube.glb` can never be picked; `Scene::reloadModel` constructs
   `ObjLoader` directly (`Scene.cpp:177`) instead of `loadModelByExtension`, and
   passes the result to `add_model` with no null check (`:179` vs `:153`) — a
   malformed asset is a null-deref. Test: CPU-only, all three behaviours.
7. **Base-colour textures upload as UNORM, then post applies gamma again** —
   **DONE (2026-07-22)**: eR8G8B8A8Srgb for texture uploads (real + default);
   the hardware decodes to linear at sample time. A/B census on the default
   scene: 16.5k pixels shift (3.5%, exactly the textured skeleton), max delta
   243; forward/deferred parity held at 0.20 through the change. Whole-frame
   channel means were BLIND to it (skeleton too small) - the A/B frame dump
   was the instrument. Original text: —
   `Texture.cpp:120`, `:194` use `eR8G8B8A8Unorm` for sRGB-encoded PNG/JPG, then
   `post.frag:34` does `pow(.,1/2.2)`. Albedo is systematically too bright and mips
   average in the wrong space. Narrow fix: `eR8G8B8A8Srgb` for base colour only
   (normal/ORM must stay UNORM).
8. **Forward shading ignores material diffuse and roughness** — **DONE
   (2026-07-22)**: both raster paths consume material.diffuse (untextured
   fallback - the clamp-to-slot-0 defect PT/RT had) and map shininess ->
   roughness (Beckmann), replacing the hard-coded 0.9 that DEFERRED also
   wrote into its own G-buffer (lighting.frag "reading the material" was an
   illusion). Found underneath: tinyobj's -1 no-material face id was cast to
   0xFFFFFFFF, so every shader material fetch on every untextured model was
   an OUT-OF-BOUNDS buffer-device-address read - fixed in ObjLoader with a
   CPU red/green test. Rig lit luminance 166.9 -> 158.1 proves the whole
   loader->slot0->diffuse chain live. Original text: —
   `shader.frag:86-91` builds ambient from the texture alone, leaves `diffuse`
   commented at `:89` and hard-codes `roughness = 0.9` at `:91`, nullifying the
   glTF material mapping in `GltfLoader.cpp:106-129`. The deferred path reads both
   (`lighting.frag:48-49`), so the two raster paths disagree on materials.
9. **Cascades refit per frame with no texel snapping — shadow edges crawl** —
   **DONE (5aca1c2b)**: world-fixed light basis + box sized from the slice
   bounding radius + centre snapped to whole texels + one-texel pad; opt-in
   via a shadowMapResolution param that updateCascades passes. Three tests
   incl. a legacy red control (the old path must crawl or the assertions are
   vacuous). Shadow golden IMPROVED to 9.78% darkened. Original text: —
   `computeCascadeData` derives the box from exact frustum corners each frame
   (`CascadedShadowMap.cpp:201-216`) so it translates AND resizes; the
   stabilisation ingredient (`radius`, `:187-190`) is already computed but only
   used for eye placement. Fix: size from `radius`, snap origin to whole texels.
   Test: two camera positions a fraction of a texel apart — origins must differ by
   an exact texel multiple and box width must be identical. Both fail today.
10. **A `Model` can hold exactly one `Mesh`** (L) — **✅ DONE (loader split 1d40e176,
    render golden f008b501)**: a multi-primitive glTF now loads as one Model with
    one Mesh per primitive, end-to-end. `GltfLoader::parseCpu` records a per-primitive
    `MeshRange` (turnkey approach below — flat getters + GltfParseUnit untouched);
    `uploadParsed` slices each range into its own `add_new_mesh`, and `adoptParsed`
    moves the ranges so the async path builds the meshes too. Verified: the
    `MultiPrimitiveGltfLoadsAsMultipleMeshes` render golden loads two_primitives.gltf
    and asserts `visibility.meshes_total == 2` through the real render loop, plus
    `MultiPrimitiveGltfRecordsPerPrimitiveMeshRanges` covers the CPU parse; 14/14
    goldens + 13/13 GltfParseUnit green, validation-clean. The RT/PT
    gl_GeometryIndexEXT>0 case is correct-by-inspection (raster golden exercises the
    flat objectIndex; an RT-mode colour oracle is a possible future hardening). The
    remaining optional refinement is per-mesh material subsets (each mesh currently
    shares the full materials array — correct, just not minimal). Historical detail
    below.

    **FOUNDATION DONE (f20b1234)**:
    `Model` now holds `std::vector<Mesh>` (getMeshCount → size, getMesh(i) →
    meshes[i], add_new_mesh appends, cleanUp/getPrimitiveCount iterate), and
    `Scene::add_model` flattens one object description PER MESH. Deliberate no-op
    today (loaders make one mesh/model, so flat mesh index == model index);
    102/102 CPU + 15/15 goldens unchanged, validation-clean. Model is now
    multi-mesh-CAPABLE. **objectIndex now the FLAT mesh index (84e4fd45)**: the
    forward/deferred/shadow record loops push it per mesh (running count, advancing
    for culled meshes too, push moved after the visibility test), so the RASTER
    render path is fully multi-mesh-correct; 15/15 goldens unchanged, no-op today.
    Per-mesh culling already falls out (the loops iterate getMeshCount + per-mesh
    AABBs). **REMAINING**: split a loader's primitives into separate meshes - the
    load-bearing step (`GltfLoader::parseCpu` flattens all primitives, `loadModel:79`
    calls add_new_mesh once → restructure to per-primitive geometry + loop
    add_new_mesh; ripples to the loader API + the GltfParseUnit tests asserting on
    the flat getVertices/getIndices + the async loader). **AS + RT/PT DONE**: the
    AS already built one BLAS per MODEL with one GEOMETRY per mesh (`:53-70`,
    iterates getMeshCount), and the RT/PT kernel material fetch now uses
    `instanceCustomIndex (= model's first-mesh flat index, a running base in
    createTLAS) + gl_GeometryIndexEXT` (1e5d0a35), so the WHOLE render path (raster
    + AS + RT/PT) is multi-mesh-correct; 15/15 goldens unchanged. **So the ONLY
    remaining #10 work is the loader split** - once a loader emits >1 mesh per
    Model, everything downstream already handles it (and it is what finally
    EXERCISES the multi-mesh path - today it is verified no-op + correct-by-
    inspection, since two separate models drive the flat objectIndex but never the
    RT/PT gl_GeometryIndexEXT != 0 case).

    **TURNKEY loader-split approach (2026-07-23, cleaner than the "API/test ripple"
    fear - it does NOT touch the flat getters, so GltfParseUnit is unaffected):**
    `parseCpu`'s primitive loop already has `base` (the primitive's vertex-base
    offset) and `primIndexStart` (`GltfLoader.cpp:314,335`). Record a per-primitive
    range `{vertex_base=base, vertex_count, index_start=primIndexStart, index_count,
    tri_start}` in a new member vector; keep the flat vertices/indices/materialIndex/
    materials exactly as they are (getters + tests unchanged). Then in `loadModel`
    (and the async `uploadParsed`/`adoptParsed`) loop the ranges: per primitive,
    slice `vertices[vb:vb+vc]`, `indices[is:is+ic]` re-based by `-vb`,
    `materialIndex[ts:ts+tc]`, share the full `materials`, and call `add_new_mesh`
    per primitive. Add a 2-primitive test glTF (two quads, two materials) + a golden
    asserting getMeshCount()==2 and both render (distinct materials, and one in
    RT/PT mode to hit gl_GeometryIndexEXT=1). ABI-skew only if the range vector goes
    in a .ixx-exposed type. Original state: `getMeshCount()` returned literal
    `1` and `getMesh()` ignored its index (`Model.ixx:38-39`); `add_new_mesh`
    overwrote (`Model.cpp:47`). Culling is all-or-nothing on one scene-sized AABB
    (`Rasterizer.cpp:122-141`), and there is nothing to attach LOD to.

    **DESIGN NOTE (2026-07-22, prepared so the L does not start cold):** the
    2026-07-22 texture_offset work already established the pattern this needs -
    per-DRAW identity flows through ObjectDescription + pc_raster.objectIndex,
    and the flattened-resource binding (textures) generalizes to meshes. Plan:
    (1) Model holds `std::vector<Mesh>`; add_new_mesh appends;
    object descriptions become one PER MESH (objectIndex = flat mesh index,
    offsets computed exactly like texture_offset in
    create_object_description_buffer); (2) per-mesh AABB from the loader,
    culling iterates meshes not models - the all-or-nothing cull falls out
    immediately; (3) AS: one BLAS per mesh (ASManager already loops a blas
    vector; feed it meshes), instances keep model transform; (4) LOD attaches
    per mesh afterwards. Biggest ripple: everything indexing model_list[i]
    1:1 with object_descriptions[i] (the texture_offset loop among them) -
    grep `getObjectDescriptions` consumers first. Suite guards: parity +
    multi-model + transform-follow goldens all exercise the flattening
    invariants already. This is the
    enabling change for several already-wanted features.

    **CALLER/CONSUMER MAP (2026-07-23, makes it mechanical):** (1) `Model` holds
    a single `Mesh mesh` (`Model.ixx`); `getMeshCount()` hardcodes 1, `getMesh()`
    ignores its index, `getObjectDescription()` returns `mesh.getObjectDescription()`.
    ABI skew (Model is a module-interface class embedded in Scene) -> delete-build +
    -FreshContainer. (2) `add_new_mesh` is called EXACTLY ONCE per Model - only from
    `GltfLoader.cpp:79` and `ObjLoader.cpp:129` - so switching it from overwrite to
    append is safe (no existing double-call to break). (3) The object-description
    flattening lives in `Scene::add_model` (`Scene.cpp:159`):
    `object_descriptions.push_back(model->getObjectDescription())` - change to loop
    the model's meshes and push each mesh's OD, so `Scene::object_descriptions`
    becomes one-per-MESH. (4) `VulkanRenderer::create_object_description_buffer`
    (`:1463`) consumes `scene->getObjectDescriptions()` unchanged; what changes is
    the meaning of `objectIndex` - it must become the FLAT mesh index (sum of prior
    models' mesh counts + local k), pushed per draw in the forward/deferred/shadow
    record loops (`Rasterizer.cpp:115` `pushConstant.objectIndex = m`,
    `DeferredRasterizer.cpp:468`, and CSM's `makeShadowPush(..., m)`). (5) AS: one
    BLAS per mesh (ASManager already loops a blas vector). Do it as: data structure
    + per-mesh OD first (goldens must stay green - still 1 mesh/model so it is a
    no-op), THEN split a loader's primitives into multiple meshes to actually
    exercise it.

    **OBJ PARALLEL — ✅ DONE (09dadaa5, 2026-07-23):** `ObjLoader` used to flatten
    every OBJ shape (`o`/`g` group) into ONE mesh (the pre-#10 glTF limitation);
    now each shape becomes its own Mesh, so #10's whole downstream
    (objectIndex/culling/AS/RT-PT) applies to OBJ too. The glTF turnkey
    `MeshRange {vertexBase, vertexCount}` slice did NOT transfer directly: glTF
    primitives parse into disjoint contiguous vertex ranges, whereas ObjLoader's
    `vertices_map` dedup was declared ONCE OUTSIDE the shape loop, so vertices
    were deduped GLOBALLY - a shape's indices could reference a vertex first
    emitted by an earlier shape. Fix taken (approach a): reset `vertices_map` per
    shape so each shape's vertices form a contiguous block, at the cost of
    duplicating any vertex shared across distinct shapes (rare between separate
    objects). KEY INSIGHT that de-risked it (the original "GPU goldens must be
    re-baselined" fear was WRONG): per-shape dedup produces PIXEL-IDENTICAL
    geometry (same triangles, vertices just stored per-shape not globally shared),
    so pixel goldens do NOT shift from the vertex-count change - only CPU
    count-assertions (which use the preserved flat getters, so unaffected) and
    per-mesh culling could, and the sponza golden (373 shapes) held at detail
    0.031 > 0.02. parseCpu keeps the flat arrays; uploadParsed slices one
    add_new_mesh per range; adoptParsed moves the ranges (async path). Verified:
    ObjParseUnit multi-shape (dinosaurs, contiguity/coverage/in-block invariants)
    + single-shape (shadow_rig), 14/14 goldens, 106/106 suite, 12s validation- +
    ASan-clean run loading 3-shape dinosaurs as a multi-mesh model. Optional
    future: per-mesh material subsets (shared with the glTF split's follow-up).
11. **glTF loader gaps** (M) — skinned-node transforms are applied though the spec
    says ignore them (`GltfLoader.cpp:231`); ~~missing `NORMAL` becomes a
    constant `(0,1,0)`~~ **NORMAL fallback DONE (2026-07-22)**: absent normals
    now get per-triangle flat normals from world-space positions (matching the
    OBJ path), degenerate triangles skipped; CPU red/green with an XY-plane
    triangle whose true normal is +/-Z. ~~non-triangle primitives silently
    skipped~~ **strip/fan triangulation DONE (2026-07-22)** - triangle strips
    and fans are now triangulated (points/lines stay skipped, undrawable
    here); CPU test on a 4-vertex strip -> 2 triangles. STILL OPEN in this
    item: `alphaMode`/`doubleSided`/`KHR_texture_transform`/texcoord index all
    ignored, so transparent glTF renders opaque.

    **SCOPING NOTE (2026-07-22, so this does not start cold — re-sized M -> the
    MASK slice is S/M, the full item is L):** the four gaps split cleanly by cost.
    - *Infrastructure already present:* `PipelineBuilder` has a working
      src-alpha/one-minus-src-alpha blend state (`PipelineBuilder.cpp:144-149`,
      gated by its blend flag), so a BLEND pipeline variant is a builder call, not
      new Vulkan. What is missing is entirely material + shader + loader side.
    - *`ObjMaterial` is a SHARED C++/GLSL struct* (`Src/shared/scene/ObjMaterial.hpp`,
      `#ifdef __cplusplus`) read directly by the fragment shaders. It already carries
      `dissolve` (OBJ `d`/opacity, default 1.0). Adding any field (an `alphaCutoff`
      float + a mode int, or reuse `dissolve`+`illum`) crosses the module boundary =
      **ABI skew: delete build-clangcl-debug + `-FreshContainer`** (see
      [[cpp-renderer-fix-campaign]]).
    - *Increment 1 — alphaMode MASK cutoff (S/M)* — **SHADING DONE, verified
      (5d8523f8)**: `ObjMaterial` got a trailing `float alphaCutoff` (-1 = not MASK);
      `GltfLoader` sets it from `cgltf_alpha_mode_mask`/`alpha_cutoff`; the forward
      (`shader.frag`) and deferred (`geometry.frag`) shading passes `discard` when
      `alphaCutoff >= 0` and the sampled base-colour alpha is below it. Safe-by-default
      (every OBJ/OPAQUE material is -1, so bit-unchanged). CPU-tested
      (`GltfParseUnit.{MaskAlphaModeSetsTheCutoff,OpaqueMaterialHasNoCutoff}`, red
      without the loader change) + that suite added to the Windows CI filter (it was
      missing). **1b (shadow-pass MASK) DONE (6b7350b4)**: the directional cascade
      pass now alpha-tests MASK casters (VS forwards UV + moves light matrices to
      set 1 + repurposes the push slot as objectIndex; the empty FS replicates the
      forward material walk and discards; CSM binds the shared set at set 0). ABI
      skew (a CascadedShadowMap.ixx member) - verified with a fresh rebuild after
      the incremental build tripped the documented stale-BMI ASan overflow; 15/15
      goldens unchanged + validation-clean (the material fetch runs for every caster,
      proving the path end to end). ~~and a GPU-host visual golden of the cut-out~~
      **VISUAL GOLDEN DONE (MaskCardDiscardsCutoutTexelsVisually)**: differential
      oracle (base scene A vs +card B, changed-fraction in the bbox of changed pixels,
      scanned only in the GUI-free upper-right so the ImGui panel/FPS text/floor shadow
      are excluded); discard ON measured 0.37 (checkerboard, holes leave background),
      discard OFF 0.78 (red-proven by disabling the FS discard), 0.55 gate between.
      REMAINING for increment 1: only the RT/PT kernels (1c) still trace the solid quad.
      (Attempted the visual
      check via `KATAGLYPHIS_MODEL_OVERRIDE=Models/GltfTest/mask_card.gltf` +
      `DISABLED_DumpsFrameToPng`: the default camera frames the ImGui panel over a
      tiny/edge-on 1x1 card, so it shows nothing useful — the visual golden needs a
      controlled camera + a scaled card rig like `shadow_rig`, same lesson as the
      shadow golden. The shading logic is already CPU- + regression-verified, so this
      is confirmation-only.) The Rust renderer's MASK path (RPT `forward.wgsl`) is
      the reference.

      *ORACLE PARTLY DE-RISKED (2026-07-23): fixture ready + rig mechanism found, but
      the OBVIOUS metric is a trap.* Decoded `mask_card.gltf`'s embedded 8x8 PNG: a
      PERFECT 50/50 CHECKERBOARD (32/64 texels opaque, 32 alpha=0 cut-out,
      nearest-filtered). Rig mechanism: no new fixture needed - reuse
      `EngineHarness::renderer->addModel(path, placement)` (as the sponza golden does)
      to add the card over the default scene with a scale+translate placement that
      faces it to the camera and fills a known crop (the earlier tiny/edge-on failure
      was the DEFAULT placement; an explicit matrix fixes it), then LOOK at a
      DumpToPng before trusting the crop bounds.
      **METRIC TRAP (do NOT use detail/edge-fraction here):** with discard OFF the
      cut-out texels still carry RGB and render, so the card is a checkerboard in BOTH
      states - opaque-vs-background with discard ON, opaque-vs-cutoutRGB with discard
      OFF - and an edge/detail-fraction metric stays HIGH either way, so the red state
      PASSES (vacuous, exactly the "swung-pixel counts do not discriminate" lesson).
      **BEST ORACLE — DIFFERENTIAL, needs NO uniform background (2026-07-23, this is the
      one to build):** render the scene WITHOUT the card (capture A), `addModel` the
      card (capture B), and measure the fraction of CHANGED pixels *within the bounding
      box of the changed pixels*. Discard ON -> only the ~32 opaque texels change (holes
      leave the background = A, unchanged) -> ~50% inside the box. Discard OFF -> the
      solid card covers everything -> ~100% inside the box. The bounding box
      auto-locates the card, so this is FRAMING-INDEPENDENT (no exact crop to tune) and
      needs no uniform background (it compares B against the SAME background in A), which
      dissolves both the metric trap above and most of the framing risk. Assert e.g.
      `0.25 < changed_fraction < 0.80`; red proof (remove the forward FS `discard`,
      recompile spv) pushes it to ~1.0 and fails high. The only real setup is a
      placement that makes the card VISIBLE and not edge-on: the default camera is at
      `(0,6,26)` looking down -Z (Camera.cpp ctor), and the card's +Z normal faces it,
      so `translate({0,~5,~8}) * scale(~12)` in the card's own XY plane is a good first
      guess - LOOK at a DumpToPng once to confirm it fills a reasonable region and isn't
      back-face-culled (doubleSided is not honoured yet, increment 2, so the +Z side
      must face the camera). Estimate: one golden + ~1-2 look-and-tune cycles. 1c reuses
      this exact rig in PT/RT mode (its differential is even cleaner: background geometry
      visible through the holes vs occluded).

      *1b design note (M, so it starts warm — the shadow pass is a CORE system,
      give it a dedicated cycle):* the CSM shadow pass
      (`scene/light/directional_light/CascadedShadowMap.cpp`) is deliberately
      minimal — its own descriptor set holds ONLY the light-space matrices uniform
      (`recordCommands` binds `{descriptorSet}`, not the forward pass's material/
      texture set), the push constant is just `{model, cascadeIndex}`, and the VS
      (`directional_shadow_map.vert`) reads only `pos` (location 0). To alpha-test
      MASK casters: (1) VS — add `layout(location=N) in vec2 uv` (N = the UV slot in
      the engine `Vertex` layout) + pass it through; (2) push constant — add
      `material_address`, `material_index_address`, `texture_offset` (the forward
      pass already pushes these via ObjectDescription); (3) descriptor set — add the
      texture array + sampler bindings the forward set has (or bind the shared
      forward material/texture set as set 1); (4) the empty shadow FS
      (`directional_shadow_map.frag`) — sample base-colour alpha, `discard` when
      `alphaCutoff >= 0 && alpha < alphaCutoff`. Simplest first cut: ONE pipeline,
      discard for all (OPAQUE = -1 skips), accepting a wasted texture sample per
      opaque shadow fragment; the RPT-style SECOND alpha-testing pipeline with
      per-primitive routing (avoids that cost) is the optimisation after. Multiview
      is orthogonal (gl_ViewIndex stays in the VS). Golden: a MASK card caster over a
      plane — shadowed-pixel count drops vs a solid-quad control, red-proven by
      removing the discard (mirror the RPT alpha-shadow test's oracle). The
      `mask_card.gltf` fixture (a cut-out card, `GltfParseUnit.MaskCardFixture…`) is
      the caster.

      *Tractability found while scoping (makes it cheaper than the "M, core surgery"
      warning suggests, and NOT an ABI-skew change — `alphaCutoff` already exists):*
      (a) the shared material/texture set is a REUSABLE layout object
      `sharedRenderDescriptors.getLayout()` — the forward/deferred/clouds/skybox/RT
      pipelines all consume it — so the shadow pipeline layout just adds it as a
      second set; no new layout, and `recordCommands` ALREADY receives
      `sharedRenderDescriptors.sets()` (as `rasterizer_descriptor_sets`), it simply
      binds only its own today. (b) The shadow push constant's second field is
      `cascadeIndex`, explicitly "retained for layout stability; unused" (the VS uses
      `gl_ViewIndex`) — REPURPOSE it as `objectIndex` (same mat4+uint32 size, no
      push-constant resize) to reach `ObjectDescription[objectIndex].material_address`
      exactly as the forward FS does. So the real work is: shadow VS reads UV (Vertex
      loc 3) + passes it; the empty shadow FS #includes the forward binding defs +
      samples base colour + discards; set-index juggling (put the shared set at set 0
      to match the forward binding decorations, move light matrices to set 1 —
      currently set 0 binding 1); `recordCommands` binds the shared set + pushes the
      object index. Not ABI-skew, so a normal incremental build verifies it. The one
      genuinely delicate part is the set-index juggling — a wrong descriptor-set
      layout on this CORE pass means validation storms, so do it with the
      `ShadowsDarkenSomePixels` golden watching.

      *Turnkey numbers (investigation done — implementation is now mechanical):* the
      forward shared set is SET 0 with `OBJECT_DESCRIPTION_BINDING 2` /
      `TEXTURES_BINDING 3` / `SAMPLER_BINDING 4` (`hostDevice/host_device_shared_vars.hpp`);
      `objectIndex` in the forward path is just the mesh-loop index `m`
      (`Rasterizer.cpp:115`, `DeferredRasterizer.cpp:468`), and the CSM draw loop
      already iterates the same `m`. So: (VS) `directional_shadow_map.vert` — add
      `layout(location=3) in vec2 tex_coords;` + `layout(location=0) out vec2 fragUV`,
      change the light-matrices UBO to `set = 1, binding = UNIFORM_LIGHT_MATRICES_BINDING`,
      rename the push field cascadeIndex→objectIndex (same slot); (FS)
      `directional_shadow_map.frag` — `#include` host_device_shared_vars.hpp +
      ObjectDescription.hpp + ObjMaterial.hpp + Vertex.hpp + PushConstantRasterizer.hpp,
      declare the SET 0 bindings 2/3/4, do `obj_res = objDesc.i[objectIndex]` →
      `materials.m[materialIDs.i[gl_PrimitiveID]]` → sample `tex[textureID]` at fragUV
      → `if (alphaCutoff >= 0 && a < alphaCutoff) discard`; (C++) CSM pipeline layout
      becomes `{ sharedRenderDescriptors.getLayout(), lightMatricesLayout }`, plumb the
      shared layout through `CascadedShadowMap::init`, and in `recordCommands` bind the
      shared set (already received as `rasterizer_descriptor_sets`) at set 0 + push
      `objectIndex = m`. NOT ABI-skew, so a normal incremental build + the mask_card
      shadow golden verify it.

      *1c — RT/PT MASK any-hit (M/L, dedicated cycle; design done 2026-07-23 by reading
      the kernels).* Today both trace the SOLID quad for MASK: `path_tracing.comp` opens
      the ray query with `gl_RayFlagsOpaqueEXT` (line ~218) and an EMPTY
      `while(rayQueryProceedEXT)` loop, and the RT pipeline has no any-hit shader.

      **ATTEMPTED 2026-07-23 — PT shader half written + STRUCTURALLY verified, then
      REVERTED because the positive proof is BLOCKED by a separate limitation (see the
      new "added models absent from the RT/PT AS" item below).** The PT candidate
      alpha-test (a `candidatePasses(rayQuery)` helper: fetch the candidate material via
      the committed=false buffer-reference walk, OPAQUE/untextured pass immediately, MASK
      samples base-colour alpha at the barycentric UV vs the cutoff; the primary + NEE
      shadow queries drop `gl_RayFlagsOpaqueEXT` and confirm passing candidates) was
      IMPLEMENTED and is no-regression clean: all 4 PT goldens pass and the white FURNACE
      stays 186.005 (ideal 186) unbiased, proving the confirm logic is correct for opaque
      hits. The POSITIVE MASK-through-holes proof could not be built at the time because a
      mask_card `addModel`'d in PT was INVISIBLE even with the opaque flag forced (RED ==
      GREEN to the digit, 0.0353734) - which turned out to be the SEPARATE addModel-AS
      bug, NOW FIXED (see the ✅ item above). Reverted the 1c shader to stay at the
      "red/green proven" bar. **NOW UNBLOCKED**: with the addModel AS rebuild landed, an
      addModel'd card DOES trace in PT (`AddedModelAppearsInPathTracing` proves it). To
      finish 1c PT next: re-apply the `candidatePasses` shader change (from this session's
      transcript), addModel the mask_card in PT, and detail-fraction the card crop - with
      1c a checkerboard of opaque texels vs scene through the holes (high detail), without
      it a solid quad (low). ONE oracle caveat: mask_card's opaque RGB is uniform WHITE,
      so for a crisp signal use a HIGH-CONTRAST fixture (dark opaque texels) or a dark
      background - white-opaque vs grey-sky is weak. The shader change is written-and-correct.
      **2ND ATTEMPT (2026-07-23) hit a NEW blocker, also reverted:** re-applied the shader +
      built a high-contrast `mask_card_hc.gltf` (BLACK opaque texels + alpha checkerboard,
      valid 8x8 RGBA PNG). With the AS fix the added card now traces, BUT it renders SOLID
      WHITE, not a black checkerboard - i.e. `textureID == -1` (the color path falls back to
      diffuse/baseColorFactor white) AND `candidatePasses` returns true for everything
      (untextured MASK passes). So the ADDED MASK card's base-colour texture is not being
      sampled in PT even though the same-shaped `uv_transform_card` (opaque, added) DID
      sample its texture. Debug that first (why does an added MASK card get textureID -1 /
      wrong texture_offset in the traced path, when an added opaque card does not?) before
      the oracle. BISECT STEP 1 DONE (2026-07-23): the HC PNG is STRUCTURALLY VALID
      (decoded: colortype 6 RGBA, alpha checkerboard 32/64 opaque, RGB all-black) and the
      HC gltf is structurally identical to the golden-tested mask_card.gltf (only the
      material name + image bytes differ), so the FIXTURE is ruled out - this is very
      likely a real ENGINE bug in the added-textured-model path, NARROWER than the AS bug
      (only a second added TEXTURED model, seen in PT). Next bisect: a CPU parse test on
      the HC fixture to check textureID extraction. BISECT STEP 2 DONE (2026-07-23):
      `GltfParseUnit.HighContrastMaskCardExtractsItsTexture` PASSES - CPU extraction is
      clean (textureID >= 0, one image, cutoff 0.5). So the blocker is GPU-side: the
      texture extracts but is not sampled in the TRACED path for the added MASK card (it
      renders diffuse-fallback white). The opaque `uv_transform_card` added the same way
      DID sample its texture in PT, so the variable is MASK-material or RGBA-texture. NEXT
      bisect: render mask_card_hc in FORWARD/raster via the harness - black checkerboard
      there means upload is fine and the bug is PT-sample-specific; white there too means
      the GPU upload/`createFromMemory` of this RGBA texture failed. Then compare
      `uploadParsed`'s texture append / `texture_offset` for the added model against the PT
      `texture_id = texture_offset + textureID` read.
      CLEAN SUMMARY (2026-07-23, superseding my earlier flip-flopping notes above - the
      raster bisect was INCONCLUSIVE, default-camera framing on the 1x1 card; disregard the
      "raster renders black" and "likely candidatePasses" claims):
      - PROVEN: the CPU parse sets `textureID >= 0` for mask_card_hc
        (`GltfParseUnit.HighContrastMaskCardExtractsItsTexture` passes), and the added
        OPAQUE uv_transform_card samples its texture correctly in PT
        (`AddedModelAppearsInPathTracing`, committed path).
      - SYMPTOM: with the mask card added in PT the card renders SOLID WHITE (not
        transparent) = the diffuse fallback, i.e. the PT shader reads `textureID` as -1 for
        the added card even though the CPU material has it >= 0. A CPU<->GPU mismatch for the
        ADDED textured model in the traced path (upload or the traced material/texture-offset
        read), NOT a shader-logic bug I can pin by inspection.
      - NEEDS GPU DEBUG (RenderDoc / shader printf): dump the actual GPU ObjMaterial
        `textureID` + `texture_offset` for the added card as the PT kernel reads them, vs
        what the raster path reads. Extraction is clean; the mismatch is upload- or
        descriptor-side for a runtime-added textured model under RT/PT. Possibly related to
        the addModel path (which the AS fix just touched) not fully re-flattening the
        material/texture arrays the RT descriptors read.

      Two code paths, shared alpha-test logic:
      - *PT (ray_query, the easier half — NO new shader/SBT):* drop `gl_RayFlagsOpaqueEXT`
        from BOTH the primary query (~line 218) and the NEE shadow query (~line 263).
        In each `rayQueryProceedEXT` loop body, when
        `rayQueryGetIntersectionTypeEXT(rayQuery,false)==gl_RayQueryCandidateIntersectionTriangleEXT`,
        fetch the CANDIDATE's material+UV — same buffer-reference walk `getObjectHitInfo`
        already does but with the `committed=false` variants
        (`...InstanceCustomIndexEXT/GeometryIndexEXT/PrimitiveIndexEXT/BarycentricsEXT(rayQuery,false)`),
        interpolate `tex_coords` via barycentrics (copy lines ~96-98 of `raytrace.rchit`),
        sample base-colour alpha; call `rayQueryConfirmIntersectionEXT(rayQuery)` when
        `alphaCutoff < 0` (OPAQUE) OR `alpha >= alphaCutoff`, else skip (leave it a
        candidate). The primary query keeps the closest CONFIRMED hit; the shadow query
        (terminate-on-first-hit) then correctly passes through holes.
      - *RT pipeline (rgen/rchit — needs a new any-hit shader):* add `raytrace.rahit`
        bound to the hit group, replicating rchit's ObjectDescription/Vertices/Indices/
        MaterialIDs/Materials setup + the UV interpolation it ALREADY computes (rchit
        lines 69-98 are copy-ready), sample alpha, `ignoreIntersectionEXT()` on MASK
        fail (OPAQUE returns immediately = accept). Drop the per-geometry/instance opaque
        flag in `ASManager` (currently set so any-hit is skipped) and wire the rahit into
        the SBT/hit-group in the RT pipeline builder. rchit is unchanged (only committed
        hits reach it).
      - *Oracle (NOISE-ROBUST refinement 2026-07-23 — the rig now EXISTS):* the
        `addModel(mask_card, placement)` rig that `MaskCardDiscardsCutoutTexelsVisually`
        and the doubleSided goldens already use is directly reusable in PT mode
        (`renderer_vars.pathTracing = true`). Prefer a DETAIL-fraction over a differential
        here — PT is noisy and a base-vs-card differential is polluted by per-frame noise
        in the holes (separate accumulation after the AS-rebuild reset). Instead render the
        card over the SKY and measure the detail-fraction in the card's screen box after
        enough accumulation frames (the accumulation golden converges ~45): with 1c the
        card is a high-contrast checkerboard of white opaque texels vs sky through the
        holes → HIGH detail; without 1c the card is a SOLID uniform white quad (mask_card's
        RGB is uniform white — verified while building the KHR fixture) → LOW detail.
        Red-proven by restoring `gl_RayFlagsOpaqueEXT`. Same detail-fraction metric as the
        forward MASK + KHR goldens (`goldenRenderSuite.cpp`), so no new oracle machinery -
        just point it at the PT capture and pick the accumulation depth. The card-over-sky
        framing (env-tunable MASK_X/Y/Z/SCALE) is already dialled in from the forward golden.
      - ABI: shader-only for PT; the RT half adds a shader file + SBT wiring (no C++
        struct change, so not ABI-skew) but DOES touch `ASManager` geometry flags.
    - *Increment 2 — doubleSided — ✅ FORWARD DONE (2026-07-23):* forward pass now
      honours `material.doubleSided` via dynamic cull state exactly as the plan below
      described - `PipelineBuilder::setDynamicCullMode` (opt-in `VK_DYNAMIC_STATE_CULL_MODE`,
      only the forward Rasterizer enables it) + per-draw `setCullMode(eNone/eBack)`;
      the flag rides glTF material.double_sided -> MeshRange -> Mesh via add_new_mesh,
      exposed by `Scene::isMeshDoubleSided`. Verified: `MaskCardDoubleSidedRendersFromBehind`
      (card rotated 180deg, back to camera - visible 12779px ONLY with doubleSided,
      RED-proven 0px forced single-sided), 16/16 goldens unchanged, validation-clean.
      **DEFERRED ALSO DONE**: the G-buffer geometry pass got the same opt-in dynamic
      cull + per-draw setCullMode (`.cpp`-only, no ABI-skew);
      `MaskCardDoubleSidedRendersFromBehindDeferred` confirms the back-facing card
      reaches the G-buffer (12779px), 17/17 goldens, validation-clean. **SHADOW PASS
      NEEDS NOTHING**: the CSM pipeline already builds with `setCullMode(eNone)`
      (`CascadedShadowMap.cpp:644`), so it renders both faces and a doubleSided caster
      already casts correctly - no back-face culling to disable. So doubleSided is now
      COMPLETE across every pass (forward + deferred honour it, shadow never culled).
      The design below is retained as reference. *Original plan:* per-material cull-mode. *Concrete plan
      (2026-07-23): use DYNAMIC cull state, not a second pipeline.* `vkCmdSetCullMode`
      is core Vulkan 1.3 (the engine targets 1.4), so add `VK_DYNAMIC_STATE_CULL_MODE`
      to the raster PipelineBuilder and, per mesh in the draw loop,
      `vkCmdSetCullMode(cmd, mesh.double_sided ? eNone : eBack)` - no pipeline variant,
      no per-draw pipeline switch. Per-MESH cull works exactly post-#10 because each
      glTF mesh is now one primitive = one material (OBJ has no doubleSided concept, so
      always cull). Plumbing: add `bool doubleSided` to `GltfLoader::MeshRange` (parseCpu
      reads `cgltf primitive->material->double_sided`), carry it to a new `bool
      double_sided` on `Mesh` via an `add_new_mesh` param (ABI-skew: Model.ixx +
      GltfLoader.ixx -> fresh container), draw loop reads `getMesh(k)->double_sided`.
      Keeps doubleSided OUT of the GPU ObjMaterial struct. Oracle is DETERMINISTIC (raster,
      not the noisy PT of 1c): reuse the mask_card rig but rotate the card 180deg (back to
      camera) - INVISIBLE today (culled), VISIBLE once doubleSided is honoured; the
      differential-in-bbox metric from `MaskCardDiscardsCutoutTexelsVisually` transfers
      directly, red-proven by forcing single-sided. ~8 files, one fresh build.
    - *Increment 3 — BLEND + sorting (M/L):* sorted transparent pass through the
      blend pipeline above; this is the genuinely L part (back-to-front ordering,
      a second draw list). Defer until a real transparent asset needs it.
      *(Parity note found 2026-07-23:* the DEFERRED geometry pass discards any
      textured texel with `alpha < 0.1` for NON-MASK materials too
      (`deferred/geometry.frag:51`, `alpha_cull = alphaCutoff>=0 ? alphaCutoff : 0.1`),
      whereas the FORWARD pass only discards for MASK (`rasterizer/shader.frag:78`).
      So an OPAQUE material whose texture carries a real low-alpha channel would drop
      those texels in deferred but keep them in forward - a low-impact but real
      forward/deferred inconsistency, and a landmine to reconcile when BLEND lands
      since deferred cannot blend and would silently cull instead. Confirm intent
      before touching: the deferred 0.1 default may be a deliberate "kill fully
      transparent texels the G-buffer can't blend" guard.)*
    - *Increment 4 — KHR_texture_transform ✅ DONE (2026-07-23) + texcoord index (M/L, NOT S
      — corrected 2026-07-23):* **KHR_texture_transform DONE**: ObjMaterial gained trailing
      uv_scale/uv_offset (identity default, scalar-layout-safe), the shared
      `transform_uv` helper applies `uv*scale+offset` in the forward/deferred/shadow FS,
      and GltfLoader reads the extension's scale/offset from the base-colour texture
      (rotation not yet applied). Verified: `ReadsKhrTextureTransformScale` +
      `MaterialWithoutTextureTransformIsIdentity` CPU tests + `KhrTextureTransformTilesTheTexture`
      golden (8x8 RGB checkerboard scaled 4x tiles to 32x32, detail 0.552 vs 0.079
      red-proven), 18/18 goldens, validation-clean. uv scale/offset uniform and a per-slot
      uv-set selector — RPT does both via `uv_set_mask`. Cost split re-checked in the C++
      engine: *KHR_texture_transform* is the genuinely small half — a per-texture scale/offset
      (a material-struct field, ABI-skew) applied to the UV in the fragment shaders, no vertex change. *texcoord index*
      is NOT small: the engine `Vertex` (`scene/Vertex.hpp`, `getVertexInputAttributeDesc`
      returns 4 attributes) carries a SINGLE `texture_coords`, so a second UV set means
      adding `TEXCOORD_1` to the Vertex layout (5th attribute) + every mesh vertex buffer +
      every shader that reads UV + the loaders — a Vertex-layout ABI change touching the
      whole pipeline, M/L not S. Do KHR_texture_transform first; defer texcoord index until
      a real dual-UV asset needs it.
**✅ FIXED (2026-07-23): a model added via `addModel` after the initial load was ABSENT
from the RT/PT acceleration structure** — CONFIRMED by code (addModel rebuilt the
object-description buffer + RT descriptor sets but never the AS) and FIXED (addModel now
calls `createASForScene` when hardware RT is supported). Red/green-proven by
`GoldenRender.AddedModelAppearsInPathTracing` (added card visible in PT: detail 0.108 with
the rebuild vs 0.035 without). *Follow-up optimisation (not a bug, correct as-is):* the fix
(and the existing `reloadModel` path) call `createASForScene`, a FULL rebuild that clears and
re-builds EVERY model's BLAS - O(N) BLAS builds to add ONE model. Fine for the few-model
scenes today, a hitch for many-model or interactive add. Incremental version: an ASManager
`appendBlas(model)` that builds only the new model's BLAS and pushes it, then `createTLAS`
(the TLAS rebuild is unavoidable and cheap, it just re-lists instances). Guard the RT
goldens + `AddedModelAppearsInPathTracing`. NOTE (assessed 2026-07-23): `createBLAS` is a
BATCHED full build - one shared scratch buffer sized across ALL models, compaction across
all, and it clears+rebuilds the whole `blas` vector - so `appendBlas` means carefully
extracting the single-model build path (its own scratch + compaction, append not clear);
it is a real refactor of the AS build, not a 10-line change. Budget for it + verify RT
carefully (AS bugs are the delicate kind). Original report: a `mask_card.gltf` added with `renderer->addModel(path, placement)`
    while in PATH-TRACING mode does not appear in the traced image at all: forcing
    `gl_RayFlagsOpaqueEXT` (a solid quad that MUST occlude if present) leaves the render
    bit-identical (RED == GREEN, detail 0.0353734), i.e. no rays hit it. The same
    `addModel` renders fine in the FORWARD raster goldens (`SecondModelShadesWithItsOwnTextures`),
    so the geometry loads - it just isn't traced. Likely the same class as the
    TLAS-follows-transforms fix (`99c62471`) which was TLAS-only "BLAS untouched": adding
    a NEW model needs a new BLAS built AND the TLAS rebuilt to reference it, and the
    add-model path may only update object descriptions / the TLAS instance list without
    the new BLAS. Confirm by tracing a scene where a second model is added post-load (RT
    and PT) and checking it appears; if real, the fix is to build the added model's BLAS
    and rebuild the TLAS in `ASManager` on add. This BLOCKS positively testing 1c PT via
    addModel (use a default-model MASK fixture meanwhile). If confirmed it is a real
    correctness bug for any runtime-added geometry under RT/PT, not just MASK.
12. **Point lights are wired on the GPU but never fed; `OmniDirShadowMap` renders
    nothing** (M) — **DONE (Option A — DELETE, 2026-07-24)**: removed
    OmniDirShadowMap (.ixx + .cpp, 176 lines), `Light.ixx` (entire file —
    Light/DirectionalLight/PointLight all unreferenced), the
    `omni_shadow_map.*` shader trio+spv (6 files), `point_light.glsl`
    (never included), and the pointLight fields from SceneUBO +
    `host_device_shared.hpp` + `bindings.hpp` + `lighting.frag` (deferred
    loop). Frees the 1024x1024 cube depth allocation per run, shrinks
    SceneUBO, kills the parity trap. Re-adding later costs the same M as
    finishing now — nothing rots.
13. **Cascades cost 3 render passes + a pass-through geometry shader** —
    **DONE (2026-07-22)**: single multiview pass (viewMask over all cascades,
    one full-array framebuffer), gl_ViewIndex selects the light matrix in the
    vertex shader, geometry stage deleted (file + spv). Union caster culling
    preserves the old test's safety property. Measured: shadow coverage
    identical (12.276% vs 12.267% darkened), ShadowCascades 0.0477 ->
    0.0408 ms (-14%) on the rig run. 94/94, validation-clean.
14. **cgltf is an unfuzzed untrusted-input surface** — **DONE (2026-07-22)**:
    all three hardened - buffer-view fit checked against buffer->size, base64
    length rejected unless a positive multiple of 4 (the underflow source),
    cgltf_validate() gates the walk. New gltf_parsing_fuzz_test (self-
    contained cgltf, wired into both CI lanes) plus two CPU regression tests
    (malformed JSON rejected; sub-quad base64 URI yields no texture instead of
    an underflowed read). 96/96, validation-clean.
15. **Swapchain recreate destroys before creating; surface-lost unhandled** —
    **DONE (2026-07-22, the two live defects)**: recreate now keeps the old
    swapchain alive as the oldSwapchain handoff (destroyImageViews split out
    of cleanUp; old handle destroyed AFTER the new one is created), and
    createSwapchainKHR's result is checked (ASSERT_VULKAN - it silently stored
    null before). Surface-lost was already distinct in the current code: acquire/
    present route eErrorOutOfDate -> recreate and everything else (incl.
    eErrorSurfaceLost) -> abort_frame_with_fatal_error, so no change needed
    there. The resize path is not headless-testable; the always-run half (init
    + the result check) is validation-clean every launch.
16. **Dynamic rendering + synchronization2 are hard-disabled** (L) —
    `VulkanDevice.cpp:451`, `:454`. Already in use: RT pipelines, ray query, AS,
    BDA, descriptor indexing, scalar block layout, multiview, pipeline cache, VMA,
    timestamps. Dynamic rendering would delete the framebuffer rebuild in
    `recreateSwapChain` (`VulkanRenderer.cpp:592-621`) where the image-count edge
    cases live. Test: run the whole golden suite under both paths behind a toggle.

### Path tracing survey (2026-07-22)

Full read of PathTracing.cpp/.ixx, all 241 lines of path_tracing.comp, the
dispatch path and the RT pipeline. The kernel is an RTIOW/nvpro-style port.
Two load-bearing facts drive most items: the RNG seed is `res.x*y + x` with NO
frame dimension (`path_tracing.comp:150`) - every frame is bit-identical, so
nothing ever converges; and the environment-radiance line is COMMENTED OUT
(`:225`) with clearColor black (`PathTracing.cpp:92`) - the scene is lit by an
accidental constant-white furnace and the GUI light provably does nothing.

1. **Use the precomputed inverse matrices** (S, top value/effort) - the sample
   loop calls `inverse(view)`/`inverse(projection)` 24x per pixel per frame
   (`:162,:168,:173`) while GlobalUBO ALREADY carries inv_view/inv_projection
   (`GlobalUBO.hpp:26-29` - added for the clouds pass with a comment calling
   inverse() "ruinously expensive"). raytrace.rgen:41-44 has the same waste.
   Verify: GpuTimedPass::Main JSON before/after.
2. **Temporal accumulation + camera-move reset + per-frame RNG** (M, headline)
   - **DONE (2026-07-22)**: rgba32f history image (one, persistent), running
   mean in the kernel, frame index folded into the seed, resets on camera
   move / resize / AS rebuild (model load). Golden proves differ+converge
   with an exact red (frame term removed -> changed fraction exactly 0).

   NEW items found while proving it:
   - ~~**PT dispatches before the TLAS exists** (S)~~ **DONE**: the RT/PT record
     branch is now guarded on `asManager.getTLAS()` (plus the descriptor-set index
     bound) in `VulkanRenderer.cpp:872-873`, so the async model-load window no
     longer dispatches against never-written TLAS/output/accumulation descriptors.
     Original text: with PT enabled during the async model load, the kernel
     dispatches against never-written descriptor sets (20 validation errors in the
     pre-load window of the golden run). Guard the PT/RT record branch on a built TLAS.
   - ~~**Pipelines consume the PREVIOUS run's SPIR-V** (S/M)~~ **DONE
     (2026-07-22)**: PathTracing/PostStage reordered to compile-then-read
     (the other stages already had the right order; Clouds deliberately
     consumes prebuilt spv only). BUT the reorder exposed a deeper layer,
     still OPEN:
   - ~~**Runtime shader compilation is a silent no-op for container-built
     binaries** (M)~~ **DONE (2026-07-22)**: glslc resolves at call time
     (baked path when it exists -> VULKAN_SDK/Bin -> PATH), and the system()
     return is checked with a loud error naming the stale spv that will be
     served. Proven on the host: touching a kernel source and running a
     golden regenerates the spv mid-run (mtime flip) - GLSL iteration no
     longer needs manual compile-shaders.ps1 (the script remains the bulk /
     CI path; ShaderIncludes already had its own runtime fallback).
3. **Wire actual light transport** (M) - **DONE (2026-07-22)**: NEE toward
   the directional light (one shadow ray per bounce) + deliberate soft
   gradient sky on miss (the accidental radiance-1 furnace is gone). Golden
   `PathTracingRespondsToTheDirectionalLight` (shadow rig scene, swung-pixel
   fraction in the panel-free crop): green 0.027, pre-NEE kernel exactly 0.
   Estimator constants (1/pi, PDFs) still item 9.
4. **Degenerate scatter guard** (S) - **DONE (2026-07-22)** near-zero scatter
   falls back to the normal, RTIOW 9.4 style.
5. **Hit normal transformed with w=1, no inverse-transpose** (S) - **DONE
   (2026-07-22)** row-multiply by `worldToObject` (inverse-transpose, handles
   non-uniform scale); same fix applied to `raytrace.rchit` which shared the
   defect verbatim, plus its object-space `N`/`V` BRDF inputs.
6. **Material diffuse fallback commented out** (S) - **DONE (2026-07-22)**
   `textureID < 0` now uses `material.diffuse` in BOTH kernels (the old
   clamp sent -1 to texture slot 0, not black as first written here).
7. **Russian roulette + GUI spp/depth** (S/M) - **DONE (2026-07-22)**: GUI
   sliders (spp 1-64, bounces 1-16) through new push-constant fields; RR from
   the 4th segment (survivors reweighted, unbiased); a quality change resets
   the accumulation (mean over two estimators is biased). Golden: bounces
   8-vs-1 swung fraction 0.132 green, hardcoded-bounds kernel 7.3e-5 (fails).
8. **Self-intersection epsilon** (S) - **DONE (2026-07-22)** t_min raised
   0.0 -> 0.001 to match the rgen; the 1e-4 normal offset stays as the
   secondary guard.
9. **Estimator bias** - **DONE (2026-07-22)**: the NEE term now carries the
   Lambertian 1/pi (measured: rig lit crop 208.5 -> 188.2; forward on the
   same rig is 158.1, remaining gap = PT's indirect sky which forward
   lacks). The BOUNCE path needed nothing: cosine-weighted sampling of a
   Lambertian cancels pi and cosine exactly - the "no PDF division" reading
   was wrong for that half. The furnace golden is DONE (2026-07-22 later
   the same day): KATAGLYPHIS_PT_FURNACE uniform-env + albedo-1 mode;
   green mean 186.005 vs ideal 186 at uniformity 1.0; red (spurious
   bounce 1/pi) crashes to 136.9/0.25.
10. **PT goldens** - **DONE by accumulation of shipped units (2026-07-22)**:
    non-black + variance-decreases live in PathTracingAccumulatesAndConverges,
    light response in PathTracingRespondsToTheDirectionalLight, quality
    wiring in PathTracingHonorsTheQualityControls, transform-follow in
    RaytracedWorldFollowsTheModelTransform, and the furnace in
    PathTracingPassesTheWhiteFurnaceTest - five red-proven goldens where the
    survey found only device-not-lost.
11. **Decorrelate the RNG seed** - **CLOSED as a measured NULL RESULT
    (2026-07-22)**: lag-1 autocorrelation of the depth-2 noise field is
    -0.012 (lag-16: +0.015) - no neighbour correlation exists; the LCG
    pre-step + PCG output hash already decorrelate adjacent linear seeds.
    The survey's claim was an assumption. The instrument stays as a logged
    diagnostic in the accumulation golden.

Trivial rider: ~~path_tracing.comp includes the BRDF headers (`:15-19`) and
never calls them - delete the dead includes.~~ **DONE (2026-07-22).**

### C++ Vulkan engine — second survey (2026-07-22, app/GUI/RT/deferred internals)

**CORRECTION (2026-07-22, after implementation):** second-survey items 1-3 cited
`rasterizer/g_buffer_{geometry,lighting}_pass.frag` - those files are DEAD: the
DeferredRasterizer loads `Resources/Shaders/deferred/{geometry,lighting}.*`
(cwd + RELATIVE_RESOURCE_PATH, DeferredRasterizer.cpp:315-323), and the live
pair has none of the three defects (no tonemapping in lighting - raw linear
out; bindless texture sampling in geometry; subpass-input albedo/material).
`clouds/CloudsRectangle.frag` is likewise referenced by nothing. The real
defects found instead while proving this: the GUI mode radios stomped
programmatic mode changes every frame (item #11, FIXED), and the post-pass
input descriptor was written once at init so a mode switch presented a stale
forward image (NEW, FIXED with a rebind on mode change). Items #2/#8's
G-buffer-format concerns apply to the LIVE pass's attachments only where they
actually exist there.

**DONE 2026-07-22 - dead shader set deleted** (20 files: g_buffer_* pair+spv, CloudsRectangle, noise_texture_{32,128}_res, loading_screen/ entire). `omni_shadow_map.*` were also deleted 2026-07-24 with the point-light system (item #12). `generated/` is NOT dead - those are the Rust renderer WGSL->GLSL exports. Original item text: `rasterizer/g_buffer_*`,
`clouds/CloudsRectangle.frag` (+ audit for further unreferenced shaders by
grepping each Resources/Shaders file against Src). They cost this survey its
three headline findings and several verification cycles; BuildIntegrity also
recompiles them forever. Deleting is safe only after a liveness grep per file -
the loader resolves paths at runtime, so a filename appearing in NO source
file is the deletion criterion.


A second deep pass over the subsystems the first survey covered least. Verified
against source; nothing duplicates the first list. Ruled out on inspection (so
nobody re-chases them): the rgen "missing Y-flip" comment is stale, not a bug
(the flip is baked into the projection at VulkanRenderer.cpp:179); the async
model loader is race-clean; GUI/renderer state is single-threaded.

**The deferred path is broken three independent ways** - do these together:

1. **Deferred lighting tonemaps + gamma-corrects, then post.frag does BOTH again**
   — **PHANTOM (see the CORRECTION above): `g_buffer_lighting_pass.frag` is a
   DEAD file referenced by no code; the LIVE `deferred/lighting.frag` writes
   raw linear and is healthy. No fix needed.** Original text:
   (S) - `g_buffer_lighting_pass.frag:215-216` ends with Reinhard + gamma, then
   `post.frag:32-34` re-applies both. Forward writes raw color and is correct,
   so deferred renders crushed/washed vs forward. Fix: delete the two lines.
   Test: tighten `GoldenRender.DeferredMatchesForwardRoughly` to mean-luminance
   tolerance; fails today.
2. **G-buffer material-id is UNORM, every index collapses to 0/1** — **PHANTOM
   (dead `g_buffer_geometry_pass.frag`; the live geometry pass has no material-
   id attachment). No fix needed.** Original text: (S) - the
   geometry pass writes `g_material_id = vec3(mat_ID)` into `eR8G8B8A8Unorm`
   (`DeferredRasterizer.cpp:92,:209`), so `mat_ID >= 1` clamps to 1.0 and
   `SKYBOX_MATERIAL_ID = 35` / `CLOUDS_MATERIAL_ID = 36` can never round-trip -
   sky and cloud pixels get lit as geometry in deferred mode. Fix: `eR8Uint`/
   `usampler2D` (or normalize by MAX_MATERIALS+2).
3. **G-buffer never samples albedo textures** — **PHANTOM (dead
   `g_buffer_geometry_pass.frag`; the live geometry pass samples the base
   colour - see the materials unit 2840cc9a). No fix needed.** Original text:
   (S) - the texture fetch is
   commented out on the assignment line in `g_buffer_geometry_pass.frag`
   (`g_albedo = materials[mat_ID].diffuse;//texture(...)`), so Sponza renders
   flat per-material color in deferred while forward shows textures.

**CPU-testable robustness/coverage (the now-green Windows CI can gate these):**

4. **First-frame delta_time is unbounded** — **DONE (c8d3e26e)**: `FrameInput.ixx`
   now has a pure `clamp_frame_delta` (caps at 0.1 s / 10 FPS, negatives → 0) that
   `update_frame_timing` applies, so the first frame's whole-startup-wallclock
   delta — and any later hitch — becomes slow motion instead of a camera teleport.
   Guarded by `FrameInputUnit.{FirstFrameStartupSpikeIsClamped,OrdinaryFrameDeltasPassThroughUnchanged,NegativeDeltasBecomeZero}`
   in `frontendInputSuite.cpp`, wired into the Windows CI filter (`Windows.yml:211`).
   Original text: — `last_time` starts at 0.0 (`App.cpp:32-33`) so the first
   `update_frame_timing` returns the whole startup wall-clock (seconds); a key held
   during load lurches the camera. Seed or clamp; pure gtest.
5. **Single-time command buffers are never freed** — **DONE**:
   `endAndSubmitCommandBuffer` now calls `device.freeCommandBuffers` after the
   submission is fence-waited (or `queue.waitIdle()` in the fallback paths), so the
   buffer is provably not pending when freed (`CommandBufferManager.cpp:124`), plus
   a matching free on the submit-failure error path (`:96`). This stops every
   texture/buffer upload, layout transition and AS build from leaking a command
   buffer for the process lifetime (GUI model reloads leaked dozens more). The
   per-submit fence create/destroy is a separate, smaller inefficiency left as-is.
6. **Input handling + frame timing have ZERO tests** — **DONE (c8d3e26e)**:
   `frontendInputSuite.cpp` adds 8 device-free gtests — `FrameInputUnit.*` (the #4
   clamp) and `WindowInputUnit.*` (key press/release tracking, out-of-range keys
   ignored, first-mouse-move does not jump the camera, axis-delta consume+reset,
   and the ImGui-capture gate swallowing input). All eight run in Windows CI
   (`Windows.yml:211-212`). Original text: — `WindowInputCallbacks.ixx:24-83` and
   `FrameInput.ixx:9-21` are pure, device-free, and route all input into the camera;
   no suite in Test/commit references them. This is also where #4 gets its
   regression guard.

**Build hygiene / perf / RT:**

7. **Kompute sandbox target with exceptions enabled** — **DONE (2026-07-22,
   the gate option)**: KATAGLYPHIS_BUILD_KOMPUTE_PLAYGROUND (default OFF)
   wraps both the playground target AND kompute's configure/subdirectory -
   default builds ship no exceptions-enabled binary and skip the kompute
   dependency entirely (its headers throw, so it can never link the
   project's no-exceptions options; conforming it was measured impossible,
   gating is the honest park). Verified: zero kompute lines in the gated
   configure; 93/93 unchanged. The ON path re-enables exactly the previous
   add_subdirectory wiring.
8. **G-buffer stores full world position + an RGBA8 for a scalar id** -
   **DONE (2026-07-22, position half)**: the rgba16f world-position target is
   gone; the lighting subpass reconstructs position from the DEPTH input
   attachment it already bound (inv_view * inv_projection * (uv*2-1, depth),
   background = depth >= 1.0). Correctness: deferred-vs-forward parity 0.200
   (was 0.205 - the reconstruction agrees with the stored positions). Timing
   on the parity run: Main 0.0647 -> 0.0639 ms - the test scene is too small
   for bandwidth wins to register; the win is one full-res 8-byte/px
   write+read removed per frame. The material-id RGBA8 packing remains open
   (S) if a real scene ever measures it.
9. **Acceleration structures are never compacted** - **DONE (88a3d4fa)**:
   BLAS built with eAllowCompaction, compacted-size query + copy after the
   synchronous build, originals destroyed; TLAS built after picks up the new
   addresses. Default scene: 15.98 MB post-compaction. (TLAS-only compaction
   deliberately skipped - it gains little.)
10. **RT output image is `rgba8`** - **DONE (2026-07-22, with the HDR unit
    e25eca80)**: raytrace.rgen + path_tracing.comp storage images are rgba16f
    and the offscreen targets R16G16B16A16Sfloat, so the traced result reaches
    post's Reinhard un-clamped (the RT analogue of the UNORM-lit-target fix).
11. **GUI render-mode radios live in function-local statics** - **DONE
    (961a9a7a)**: the radios derive from the shared vars each frame instead of
    holding their own static int, so a programmatic mode change is no longer
    stomped (the defect that made the deferred path unreachable end-to-end).

### Rust WebGPU renderer

**Bake wasm32-unknown-unknown into the :latest-cross image.** ~~The CI lane added
2026-07-22 cannot add the target itself~~ **FIX UPSTREAM (2026-07-22,
ANTfrastructure `3cff632`)**: install-rust.sh now adds the wasm target on the
STABLE toolchain (it only had it on the pinned nightly), not behind try_ so a
regression fails the image build. REMAINING: once the rebuilt :latest-cross
publishes, flip the RPT wasm step from skip-if-missing back into a hard gate.

**Add a wasm32 CI lane (found the hard way 2026-07-22).** `wasm_demo.rs` is
entirely behind `#[cfg(target_arch = "wasm32")]`, and nothing in the loop builds
that target - cargo test, clippy and every CI lane are native - so it had not
compiled since the wgpu 29 migration (it still used `wgpu::SurfaceError`, which
no longer exists). The web demo was simply broken and no check could see it.
A `cargo check --target wasm32-unknown-unknown` is seconds of compute and would
have caught it at the migration commit.

The recurring bug class behind many of these is written up in
`docs/renderer-bounds-invariant.md` - read it before touching anything that
moves geometry.

**Status 2026-07-31 (supersedes the 2026-07-24 status) — re-verified against the
crate source:**
  - #4 texture dedup: DONE (a `texture_cache` HashMap keyed by `Arc::as_ptr`
    dedupes at upload, forward.rs:1053-1129).
  - #12 uniform split: **the split itself is DONE** — `FrameUniforms`
    (forward.rs:55-64, written once per frame at :1780-1799) vs `PrimUniforms`
    (:68-78), punctual lights in a storage buffer (:1801-1805). STILL OPEN: the
    per-primitive `write_buffer` of the full `PrimUniforms` every frame with no
    dirty gate (:1844-1867) and the per-frame `model.inverse().transpose()`
    recompute (`normal_matrix_of` at :3080-3087) — see the 2026-07-31 batch task.
  - #13 render bundles: **DONE in shape** — the caster list records into a
    `wgpu::RenderBundle` (:1891-1944) executed per cascade (:1968-1970). STILL
    OPEN: the bundle is rebuilt every frame (a local, never cached) and
    per-cascade caster culling was disabled to make the bundle possible
    (`_light_frustum` discarded at :1952, stats fudged at :1939/:1972-1976) —
    see the 2026-07-31 batch task.
  - #15 lower-value trio: strip/fan triangulation DONE
    (gltf_loader.rs:206-227, :387-397), 16-bit PNG down-conversion DONE
    (:262-268). STILL OPEN: orthographic cameras are warn-and-dropped
    (:155-160) — see the 2026-07-31 batch task.
  - #8 MSAA remains open and design-heavy (the depth-resolve blocker above).


1. **Occlusion culling deletes, then flickers, any primitive the camera is inside** (S) —
   when the eye is inside a primitive's AABB the proxy box's front faces are
   near-plane clipped and only back faces rasterise, which fail `LessEqual`
   (`occlusion.rs:171-177`, `occlusion_bbox.wgsl:29-69`), so the query returns 0 →
   skipped next frame → depth empty → passes → returns. A ~30 Hz strobe on the
   object filling the screen. `cull_mode: None` and the 2% margin do not address
   near-plane clipping. Fix: force-visible when the expanded AABB contains the eye.
2. **`set_instances` never widens `scene_bounds`, so cascades stay fitted to the
   un-instanced scene** (S) — the 8th case of the stale-bookkeeping pattern.
   `set_instances` updates `aabb_min/max` and `world_center` but `scene_bounds` is
   written only in `upload_scene` (`forward.rs:923`) and `set_animation_time`
   (`:1913`), and it is the ONLY input to cascade fitting (`:1920-1924`). Instances
   scattered over ±50 units fall outside every cascade, so they neither receive nor
   cast shadows.
3. **`world_center` silently changes metric on first animation** (S) — at upload it
   is the vertex centroid (`forward.rs:1183`), afterwards the AABB centre (`:696`,
   `:725`, `:1906`). So `set_animation_time(0.0)` with NO movement can flip the LOD
   level across a switch distance and reorder the transparent draw list. Consumers
   at `:145` (LOD) and `:1489-1491` (blend sort) are documented as needing one
   agreed metric — they agree, but the value changes definition underneath them.
4. **Every primitive uploads its own copy of every material texture** (M) —
   `create_material_texture` is called inside the per-primitive loop
   (`forward.rs:997-1013`), so 200 primitives sharing one atlas do 200 CPU mip
   chains (`generate_mips` does a per-texel `powf`, `:2620-2657`) and 200 GPU
   uploads. The CPU side is already `Arc<CpuTexture>`, so the cache key is free.
   This is the VRAM ceiling blocking the Colosseum scene.
5. **Instanced normals use the instance matrix, not its inverse-transpose** (S/M) —
   `forward.wgsl:136-140` applies the raw instance matrix on top of a normal matrix
   built from `prim.model` alone (`forward.rs:1296`). Wrong for any non-uniform or
   mirrored instance scale — i.e. exactly the scattered/squashed instances that
   instancing exists for. The tangent path is correct, which hides the asymmetry.
6. **`COLOR_0` vertex colours are silently dropped** — **DONE (2026-07-22, RPT
   abb46c1, push held for the Windows lane)**: Vertex gains a linear-RGBA
   colour (white default), the loader reads COLOR_0, the shader multiplies it
   into albedo per spec, QEM blend interpolates it. Attribute location 6 for
   colour shifted the instance buffer to 7-10. Green-vertex-quad red/green
   test (r=g=b=177 white in the red state).
7. **Every texture slot is forced onto TEXCOORD_0** — **DONE (2026-07-22, RPT
   0f715e1, push held)**: Vertex carries uv1 (TEXCOORD_1); the loader builds a
   per-material uv_set_mask (bit per slot) from each textureInfo.texCoord;
   packed into material_flags.y, the shader selects uv0/uv1 per slot;
   KHR_texture_transform applies to the base slot's chosen set; QEM blend
   interpolates uv1. texCoord >= 2 falls back to UV0 with a warning. Base-on-
   UV1 red/green test (flat red at mask 0, red/blue split at mask 1). Rider
   (KHR_texture_transform on non-base slots) still open.
8. **No anti-aliasing anywhere** (M — **bigger than it looks; DESIGN NOTE
   2026-07-22**) — `MultisampleState::default()` on all four forward pipelines
   and `sample_count: 1` on the HDR target. The most visible quality defect in
   the browser demo. Color MSAA + resolve to the HDR view is easy; the BLOCKER
   is DEPTH. A render pass requires its color and depth attachments to share
   sample_count, so MSAA colour forces a MSAA depth buffer - but `self.depth`
   is read as a plain single-sample texture by BOTH SSAO (position
   reconstruction) AND the occlusion-cull bbox pass, and **wgpu has no native
   depth resolve** (only color `resolve_target`). So the real work is one of:
   (a) a manual depth-resolve pass (MSAA depth -> single-sample depth via a
   fullscreen min/max blit) feeding SSAO + occlusion, or (b) make SSAO and the
   occlusion pass MSAA-depth-aware (per-sample loads). Not a mechanical
   sample_count bump - budget a dedicated session. A clean test exists: a
   diagonal edge produces intermediate-colour "partial" pixels under MSAA and
   only hard fg/bg pixels without.
9. **Degenerate/NaN input poisons the whole frame** (S/M) — **DONE (2026-07-24)**:
    `compute_world_transforms` now guards non-finite node scale/translation/rotation
    and the resulting matrix (falling back to identity), catching zero-scale
    hide-nodes and NaN-export bugs before they propagate through the scene graph.
    `update_cascades` guards scene_radius against NaN (belt-and-suspenders behind
    the vertex-level is_finite skips in `primitive_local_aabb`/`primitive_world_aabb`).
    Three new CPU tests: `a_zero_scale_node_produces_a_finite_identity_transform`,
    `a_nan_translation_produces_a_finite_identity_transform`,
    `a_non_finite_scale_produces_a_finite_transform`. (Items #2, #3, #5 were already
    fixed by the 2026-07-22 batch; the compute_world_transforms guard was the last
    remaining NaN vector.)
10. **`KHR_materials_unlit` is ignored** (S) — the `gltf` crate exposes
    `material.unlit()`; the loader never asks (`gltf_loader.rs:435-496`). Every
    Sketchfab flat-colour export and most mobile/AR assets get a full GGX response
    with IBL and shadows — exactly what the extension exists to prevent.
11. **Anisotropic filtering is never requested** (S) — `create_sampler` leaves
    `anisotropy_clamp` at 1 (`forward.rs:2589-2597`). The mip chain is already
    correct, so this is the cheapest visible win: grazing-angle floors/walls are
    over-blurred by several mip levels. Must fall back to 1 when the glTF sampler
    asked for `Nearest` (wgpu validates this).
12. **~784 bytes of identical uniform data + a bind group rewritten per primitive
    per frame** (M) — `Uniforms` mixes per-frame data (view_proj, 3 cascade
    matrices, 16 vec4 of lights ~700 B) with per-primitive data, and the whole
    struct is rewritten for every primitive every frame (`forward.rs:1292-1323`);
    at 1000 primitives that is ~780 KB/frame plus 1000 buffers and bind groups.
    Same loop recomputes `model.inverse().transpose()` per frame though `model`
    only changes in `set_animation_time`; and `VsOut.light_space_pos` is
    interpolated but never read (`forward.wgsl:116`, `:146` vs `:364-473`).
13. **Render bundles for the three shadow cascades** (M) — the identical draw list
    is re-recorded three times per frame (`forward.rs:1349-1397`). `RenderBundle` is
    WebGPU-core (works on the web, unlike the parked indirect-draw item). Per-cascade
    culling changes the set, so invalidation is the real design question.
14. **Bloom and SSAO run at full cost when their strength is 0** (S) — `encode` is
    unconditional (`forward.rs:1539-1542`); the strengths are only consulted by the
    tonemap composite, so a slider at 0 still pays for a half-res depth pass, a 3x3
    blur, a brightpass and a separable Gaussian.
15. Lower value, noted: orthographic glTF cameras are dropped
    (`gltf_loader.rs:139`); `TriangleStrip`/`TriangleFan` primitives are skipped
    entirely (`:290-297`); one 16-bit PNG aborts the whole file instead of
    down-converting (`:197`).


## 2026-07-24 batch — sized, unblocked

### C++ Vulkan engine

### Rust WebGPU renderer

### Cross-renderer

### Container / build


## 2026-07-28 batch — refactor (dead code, duplication, test gap)

Found by a full read of the largest `Src/GraphicsEngineVulkan/` files against the
current source (nothing here duplicates the surveys above; all verified against
the code at the time of writing). Ordered non-ABI-skew first so the executor can
drain the cheap wins on an incremental build before the ABI-skew one needs a
`-FreshContainer`.

## 2026-07-28 batch II — subsystem extraction & CI coverage

Found by a structural read of `VulkanRenderer.cpp` (still the 78 KB / 1771-line
hub) and the test/CI wiring. None duplicate the dead-code batch above or the
sized items in the 2026-07-24 batch. Ordered non-ABI-skew first.

## 2026-07-28 batch IV — dead code, dead params, naming, test gap

Found by reading `SkyBox.cpp`, `Scene.ixx`/`Scene.cpp`, `Clouds.cpp`/`.ixx`,
and `ASManager.cpp`/`.ixx` against the current source. None duplicate the
batches above; all verified against the code at the time of writing (every
"dead" claim is backed by a repo-wide grep returning only the declaration
and/or definition). Ordered non-ABI-skew first so the executor drains the
cheap wins on an incremental build before the three ABI-skew tasks share one
`-FreshContainer`.

## 2026-07-30 batch — planner (validation conformance, dead plumbing, Slang follow-ups)

All five verified against the tree on 2026-07-30 (post-Slang-migration state,
`86ee9532`). The Rust deep-dive candidates were re-checked first: occlusion
eye-inside guard, bloom/SSAO zero-strength skip, MSAA, `KHR_materials_unlit`
and anisotropic filtering are ALL already implemented in the crate — do not
pick them up from the older prose above.

### C++ Vulkan engine

### Build / scripts

### Rust WebGPU renderer (`third_party/OxidANT`)

### Cross-renderer / docs

## 2026-07-30 batch II — planner (Slang-migration test fallout, push-constant budget, GPU-verification blocker)

Originally three tasks, verified against the tree on 2026-07-30, from the
verification notes of the "`GUI*` mutable cross-cutting dependency" task
above. The BuildIntegrity rewrite and the push-constant budget shrink
(`PushConstantRasterizer` now 116 bytes, `PushConstantRasterizerUnit.*` all
green) are both done; the GPU-verification blocker below is what remains.

### GPU host verification

**ROOT CAUSE FOUND AND FIXED (2026-07-31).** The GPU-golden host run is
restored. Two independent in-repo bugs were causing the reported abort, not
an environmental/driver regression:

1. `VulkanDevice.cpp:465-466` set `features2.pNext = nullptr;` instead of
   `&features11`, silently severing the entire Vulkan11/12/13 + ray-tracing
   feature chain (`features11 -> features12 -> features13 ->
   acceleration_structure_features -> ray_tracing_pipeline_features`) from
   ever reaching `vkCreateDevice` (`device_create_info.pNext = &features2` at
   `:619`). Every feature the code carefully queried and conditionally
   enabled on those structs - `multiview`, `bufferDeviceAddress`,
   `scalarBlockLayout`, `shaderDrawParameters`, `accelerationStructure`,
   `rayTracingPipeline`, `rayQuery` - was never actually requested from the
   driver, despite the startup log claiming otherwise. `git blame` traces
   this to commit `f9bdeb15` (2026-02-18), five months before the Slang
   migration that was originally suspected. Forward/deferred/shadow goldens
   still reported `[ OK ]` before the fix only because the AMD driver renders
   anyway despite the VUID spam; RT/PT hard-aborted because losing
   `bufferDeviceAddress` is fatal there. Fixed: `features2.pNext =
   &features11;`.
2. `Resources/ShadersSlang/raytracing/raytrace.rchit.slang:73` used
   `.Sample(...)` (implicit LOD) in the closest-hit shader. `closesthit` maps
   to the `RayClosestHitKHR` execution model, which has no fragment
   derivatives, so `spirv-val` rejects `OpImageSampleImplicitLod` there
   (`VUID-VkShaderModuleCreateInfo-pCode-08737`, "ImplicitLod instructions
   require Fragment, GLCompute, MeshEXT or TaskEXT"). Only surfaced once (1)
   was fixed and the device stopped rejecting `rayTracingPipeline` outright.
   The old pre-Slang GLSL shader used the same implicit `texture()` call and
   never hit this because glslang auto-lowers `texture()` to explicit-LOD
   outside fragment/compute stages; Slang does not. Fixed:
   `.SampleLevel(textureSamplers[textureId], texCoords, 0.0)`. Shaders
   recompiled via `scripts/windows/compile-slang-shaders.ps1`.
   `path_tracing.slang`'s identical-looking `.Sample()` calls are unaffected -
   that shader is `[shader("compute")]`, which IS in the allowed list.

   **Verified on the RX 9070 XT** (container-built `clangcl-debug`,
   `commitTestSuite.exe --gtest_filter=GoldenRender.*`): the
   `multiview`/`bufferDeviceAddress`/`ImplicitLod` VUIDs are completely gone
   from the log (previously every frame logged them). 15 of 17 non-PT-
   accumulation goldens pass clean, including real ray-tracing ones that
   previously never got the chance to run:
   `RaytracedWorldFollowsTheModelTransform`, `AddedModelAppearsInPathTracing`,
   `ForwardLightingRespondsToTheDirectionalLight`, both mask-card and
   KHR-texture-transform tests, both multi-model tests.

**NEW, narrower blocker isolated: path-tracing compute dispatch hits
`VK_ERROR_DEVICE_LOST` with zero preceding validation errors.**
`GoldenRender.PathTracingAccumulatesAndConverges` still hard-aborts
(`ASSERT_VULKAN`) on `vkQueueSubmit` returning `-4` (`ErrorDeviceLost`) at
`frame=2`; `GoldenRender.GuiInputSweepNeverCrashesOrLosesTheDevice` hits the
identical failure (`renderMode=path_tracing`, `vk::Result=-4`) at `frame=0` -
so it is not timing-dependent, it is the `path_tracing.slang` compute kernel
itself (RayQuery-based inline ray tracing, `Src/.../renderer/PathTracing.cpp`)
faulting on the GPU. Because the process aborts on the first hit, every test
ordered after the first PT-exercising one in `goldenRenderSuite.cpp` never
runs - this still includes the GUI-sweep SEH-crash concern from the original
report, which remains unevaluated. Not attempted here: this needs a GPU
capture (RenderDoc/PIX or AMD's crash-dump tooling) or a bisection of
`path_tracing.slang` (RayQuery flags, SBT/accumulation-image binding,
buffer-device-address indexing) to localize - out of scope for this pass.
`docs/gpu-golden-testing.md` should be updated once that lands.

## 2026-07-30 batch III — planner (shader-load consolidation, per-frame copies, docs drift)

Found by a refactor-focused read of the shader-loading call sites, the GUI
frame path, and the agentic-loop docs. Deliberately only three tasks: the
open queue is already deep, and GPU-golden host verification is blocked (see
the diagnosis task above), so nothing here needs a golden run to verify.
None duplicate the 2026-07-24/28/30 batches.

### C++ Vulkan engine

### Docs

  **Test:** None (docs-only). Verification is the grep in step 4 plus
  checking every path named in the rewritten section exists in the tree.

  **Build:** None (docs-only change; no source touched).

  **Context:** Documentation-drift item from the refactor mandate. AGENTS.md
  is the file every agent session loads, so a wrong prompt-file path there
  multiplies into wrong edits. Coordinate with the in-flight working-tree
  changes to `scripts/agentic-loop/*` — base the wording on the files as they
  are at execution time, and keep AGENTS.md the summary, README the detail.

## 2026-07-31 batch — planner (perf coverage, resurrected-docs cleanup, Rust frame path)

All five verified against the tree on 2026-07-31. Re-checks done first so
nobody chases stale prose: `reprovisionPerImageResources()` is already
extracted, the `clangcl-tsan` preset is already gone, strip/fan triangulation
and 16-bit PNG down-conversion are already in the Rust loader, and the shadow
cascades already use a render bundle — none of those are tasks (the prose
above was corrected in this pass). The three Rust tasks live in the
`third_party/OxidANT` submodule: commit there AND bump
the superproject gitlink in the same change (AGENTS.md § Critical Invariant:
Submodule Pins).

### C++ Vulkan engine

### Docs / repo hygiene

### Rust WebGPU renderer (`third_party/OxidANT`)

- **Pre-existing (found 2026-07-31, unrelated to the work below): every GPU-touching
  test in `kataglyphis_webgpu_renderer` fails headless on this host.**
  `Device::create_shader_module` for `forward_shader` rejects `forward.wgsl` at
  line 435: `textureSampleCompareLevel(shadowMap_0, shadowSampler_0, ...)` types
  as `vec4<f32>`, not the `f32` a compare-sample returns, because `shadowMap_0`
  is declared `texture_2d_array<f32>` (`forward.wgsl:60`) instead of
  `texture_depth_2d_array` — WGSL's compare-sample builtins require a depth
  texture type. Reproduced on a clean `git stash` (pre-dates this session's
  changes): 22/30 `tests/headless.rs` cases fail identically, plus the crate's
  own `render::forward::tests::set_animation_time_recomputes_and_dirties_the_cached_normal_matrix`
  lib test. `forward.wgsl` reads as Slang-generated (obfuscated `_S37`/`sum_1`
  names) — likely a naga/wgpu version bump started enforcing a rule the shader
  was already violating, rather than a new regression in the WGSL itself. Not
  attempted here: fixing the texture declaration (and whatever depends on
  `texture_2d_array<f32>` sampling of the same view elsewhere in the file)
  needs its own careful pass and re-verification of every shadow golden, out of
  scope for the bundle-caching task below, which could only be verified via
  `cargo build` + the unaffected lib tests as a result.

## 2026-07-31 batch II — planner (WGSL depth-texture fix, PT device-lost, glTF skinning, docs gate, perf diffing)

All five verified against the tree on 2026-07-31 before writing. Re-checks done
first so nobody chases stale prose: the Rust `PrimUniforms` dirty gate is DONE
(`uniforms_dirty` at `forward.rs:1890`), the shadow-caster bundle cache is DONE
(`c2c2fe4` in the submodule), the wasm size-budget CI step is already a hard
gate (`wasm-size-budget.sh` does its own `rustup target add` and is fatal), and
the Slang shaders already use CPU-precomputed inverses (the PT "use the
precomputed inverse matrices" survey item is moot post-Slang). Suggested order:
the WGSL fix first — it unblocks headless verification for every future Rust
renderer task.

### Rust WebGPU renderer (`third_party/OxidANT`)

**`headless.rs` wrong-pixels investigation — DONE (2026-07-31), 16/33 → 32/33.**
Two systemic root causes, not 17 independent bugs:

1. **Vertex attribute `@location` mismatch.** `Vertex::LAYOUT` /
   `InstanceRaw::LAYOUT` in `scene/mod.rs` numbered attributes sequentially
   (0-7, instances 8-11), but the Slang WGSL backend assigns its OWN
   `@location`s to `forward.wgsl`'s `vertexInput_0/1/2` structs
   (position=0, uv1=1, instance columns=2/3, normal=4, uv=5, tangent=6,
   joints=7, weights=8, color=9, instance columns cont'd=10/11) — a
   completely different numbering the Rust side never matched post-migration.
   Every primitive was reading position/normal/uv/color from the wrong slots.
   Fixed by renumbering both `vertex_attr_array!` calls to Slang's actual
   assignment (entries stay in FIELD order; only the `@location` numbers
   attached to each changed). This alone fixed 13 of the 17: everything except
   `alpha_modes_blend_and_mask`, `auto_exposure_brightens_a_dark_scene_over_successive_frames`,
   `caster_culling_engages_and_shadows_survive`, and `ssao_darkens_geometry`
   (`renders_cube_headless`'s `[137, 128, 124, ...]` grey was garbage vertex
   data landing in the wrong per-vertex slots, not a camera/lighting bug).
2. **Inverted shadow factor.** `forward.wgsl`'s `shadow_factor_0()` returns
   *visibility* (1.0 = lit/unoccluded, via `CompareFunction::LessEqual` PCF —
   confirmed by its own out-of-cascade early return of `1.0`, which only
   makes sense as "fully visible outside the map"). The lighting call site
   nonetheless computed `directLight * (1.0 - shadow_factor_0(...))`, i.e.
   multiplied direct light by *occlusion* instead of visibility: fully lit
   ground got zeroed direct light (ambient-only, dim) and the actual shadow
   footprint got full direct light on top of ambient (brighter than its
   surroundings) — a shadow that visibly glows instead of darkens. Fixed by
   dropping the `1.0 -` inversion. This fixed `alpha_modes_blend_and_mask`
   (the scene was reading uniformly dark because direct light was zeroed
   almost everywhere) and `ssao_darkens_geometry` (SSAO's per-pixel multiply
   can only ever darken; the "SSAO increased total energy" symptom was the
   inverted shadow glow being brighter than the correctly-shadowed baseline
   it was compared against, not an SSAO bug).
   `caster_culling_engages_and_shadows_survive`'s pixel classifier also
   needed a fix alongside: it required *neutral* grey for "shadowed", but
   with analytic IBL a correctly-shadowed patch is blue-tinted (sky-lit
   ambient only), not neutral-dark — `shadow_darkens_plane_under_cube`
   already knew this and used a blue-tint-aware classifier;
   `caster_culling`'s comment claimed to be "the same structural check" but
   wasn't. Brought in line with the proven-correct classifier.

Verified: `cargo test -p kataglyphis_webgpu_renderer --test headless` 32/33,
`--lib` 113/113. Diagnosed via a scratch frame-dump/pixel-diff example
(removed after use, per "always dump the picture, not just the number").

**Remaining: `auto_exposure_brightens_a_dark_scene_over_successive_frames`
still fails — likely a stale test assumption, not a renderer bug (S,
needs an owner decision on the fix shape).** Reproduces stably: manual
mean 171.07, auto converges (within 2 frames, as expected for
`speed=3.0, dt=0.5`) to mean 166.48 — auto exposes *down* ~2.7% instead of
up 8%+. The CPU auto-exposure math (`render::auto_exposure`, 113 unit
tests) and the GPU `cs_reduce_exposure` mirror of it both look correct and
match the working `manual_exposure_still_controls_brightness` test. The
scene (`cube.gltf`, default `OrbitCamera`, dim sun + near-zero ambient) is a
small cube against a procedural sky that fills most of a 128×128 frame and
is *not* attenuated by the test's dim light settings (only the cube's direct
+ ambient terms are) — confirmed visually via a frame dump: the cube is a
small fraction of the image. The geometric-mean-luminance auto-exposure
metric is dominated by that already-well-exposed sky, so it correctly holds
near EXPOSURE_KEY rather than brightening for the small dim subject. The
docstring's historical baseline ("182.7 vs 163.1 auto, a 12% lift") was almost
certainly measured before the vertex-attribute fix above, i.e. on a broken
frame where the cube's real shape/shading didn't dominate either — not a
result that exposure fix #1 preserves. Two honest fixes, either needs a call
the executor loop shouldn't make alone: (a) reframe the test scene so the
dim subject actually dominates the frame (tighter FOV / closer camera /
darker sky), which is what "auto-exposure brightens an underlit scene" is
actually meant to exercise; or (b) accept that a sky-dominated frame
legitimately shouldn't brighten much and lower the threshold. Left failing
and unchecked rather than guessed at.

### C++ Vulkan engine

- [b] **(M) Localize (and fix if cheap) the path-tracing compute
  `VK_ERROR_DEVICE_LOST` on the RX 9070 XT** — **two real bugs fixed
  2026-07-31, the crash on the large-mesh reproducer is NOT; root-causing it
  further needs GPU capture tooling (RenderDoc/PIX/AMD crash dumps), which is
  not available here.**

  **Fixed this pass, both verified on the RX 9070 XT (container-built
  `clangcl-debug`, host GPU run):**
  1. `rayQuery.Proceed()` was called exactly once
     (`Resources/ShadersSlang/path_tracing/path_tracing.slang`). Per
     `GL_EXT_ray_query`/DXR RayQuery semantics it must be looped
     (`while (rayQuery.Proceed()) {}`) until it returns false — a single call
     can leave BVH traversal incomplete, so `CommittedStatus()` and the
     committed-hit accessors read undefined state. The pre-Slang GLSL kernel
     (`path_tracing.comp`, still present in packaged release output) looped
     this correctly; the Slang port dropped the loop. Fixed and kept.
  2. `PathTracing.ixx`'s `SpecializationData` defaulted to
     `(specWorkGroupSizeX=16, specWorkGroupSizeY=8)`, feeding
     `PathTracing.cpp`'s dispatch work-group-count math — but the Slang kernel
     hardcodes `[numthreads(8, 8, 1)]` rather than consuming these as SPIR-V
     specialization constants (Slang does not wire them up), so the dispatch
     under-covered the image width by ~2x every frame. Fixed the default to
     `(8, 8)` to match.

  Neither fix alone, nor both together, resolves the device-lost on the
  original reproducer (below) — they were necessary correctness fixes found
  along the way, not the root cause.

  **NOT fixed — remaining device-lost, narrowed far past the original
  report:** `GoldenRender.PathTracingAccumulatesAndConverges` (scene:
  `Models/Dinosaurs/dinosaurs.obj`, hundreds of thousands of vertices) still
  hard-aborts on `vkQueueSubmit` returning `-4`, zero preceding validation
  errors, within 0-2 frames. Bisection (12+ shader-recompile/host-GPU-run
  rounds, no GPU capture tooling used) found:

  - Consuming ONLY the material-fetched `hitColor` (texture sample or
    `material.diffuse` — never touches the vertex buffer's `.position`/
    `.normal` fields, only `.texture_coords` on the dead branch, see below)
    from the RayQuery-committed hit is safe: proven across 40+ frames and an
    8x-per-invocation RayQuery loop.
  - The instant the caller also consumes a value derived from
    `v0.position`/`v0.normal` (fetched via the `Vertices*` buffer-device-
    address pointer indexed by `rayQuery.CommittedPrimitiveIndex()`-derived
    indices) — **either field alone, in any combination, with or without the
    `CommittedObjectToWorld3x4()`/`CommittedWorldToObject3x4()` transform,
    with or without `sceneUBO` access, with or without `brdf_direct()`,
    whether factored into a separate function taking `RayQuery` by value and
    returning a struct or fully inlined into `path_tracing_main`** — the
    device is lost within 0-2 frames.
  - `dinosaurs.obj`'s materials are untextured (`material.textureID < 0` for
    every primitive — confirmed elsewhere in this file), so the
    `.texture_coords`-consuming branch (texture `.Sample()`) is dynamically
    never taken for this scene; whether reading `.texture_coords` at scale
    would ALSO fault could not be confirmed or ruled out.
  - **Mesh-size correlation, not yet exploited:** every PT/RT golden test that
    reads `v0.position`/`v0.normal` via the identical `Vertices`/`Vertex`
    buffer-device-address struct and does NOT crash
    (`GoldenRender.RaytracedWorldFollowsTheModelTransform` via the RT
    *pipeline* rchit shader, `GoldenRender.AddedModelAppearsInPathTracing`,
    `GoldenRender.PathTracingPassesTheWhiteFurnaceTest`,
    `GoldenRender.PathTracingHonorsTheQualityControls`) uses the tiny
    `Models/ShadowTest/shadow_rig.obj` rig. No existing golden test exercises
    `raytrace.rchit.slang`'s identical `v0.position`/`.normal` read pattern
    against the large dinosaur mesh — so it is UNKNOWN whether this is scoped
    to RayQuery/compute (as the entry originally assumed) or is a more
    general large-mesh vertex-buffer-device-address bug that the RT pipeline
    has simply never been tested against.

  **RESOLVED 2026-07-31 by `GoldenRender.RaytracedLargeMeshDoesNotLoseTheDevice`
  (container-built `clangcl-debug`, verified on the RX 9070 XT):** the RT
  *pipeline* (`raytrace.rchit.slang`) raytraces the same dinosaur mesh over 10
  frames, with a real (non-blank) capture, and does NOT lose the device. It
  reads `v0.position`/`v0.normal` through the identical buffer-device-address
  `Vertices*` path as the faulting PT compute kernel. That rules out the
  shared vertex-upload/BDA path (`ASManager`, `GltfLoader`/`ObjLoader` upload,
  CPU/Slang `Vertex` stride/alignment) as the cause — the bug is confirmed
  scoped to `path_tracing.slang`/RayQuery compute specifically. Full 21-test
  `GoldenRender.*:Integration.*` run (minus the two still-known PT
  device-lose reproducers) stayed green, so this is not a false negative from
  some other suite-wide breakage. Whoever picks this back up next should look
  inside `path_tracing.slang` itself (RayQuery flags, SBT/accumulation-image
  binding, or a compute-specific buffer-device-address indexing bug) rather
  than the shared upload path.

  **Side effect of the two fixes above — previously-invisible bugs newly
  exposed, both unrelated to device-lost, NOT attempted here (out of scope
  for this entry):** with the crash no longer shadowing every test after the
  first PT one, two more PT goldens now run to completion and fail on VALUE
  assertions, not aborts: `GoldenRender.PathTracingHonorsTheQualityControls`
  ("the bounce-cap slider did not change the path-traced image") and
  `GoldenRender.PathTracingPassesTheWhiteFurnaceTest` ("furnace converges LOW
  ... not uniform"). Root cause for both is almost certainly the same thing:
  `path_tracing.slang` never implements a bounce loop at all — `pc_ray.
  max_bounces` is read into the push constant on the C++ side
  (`PathTracing.cpp`) but never referenced in the kernel, which only ever
  evaluates ONE ray-query hit per sample (no recursive/iterative bounce),
  unlike the pre-Slang GLSL kernel's `for (tracedSegments < 8)` loop. That is
  a real feature gap (single-bounce direct lighting only, no GI), separate
  from the device-lost above and worth its own sized entry if picked up.

  **Still true, unevaluated:** `GoldenRender.GuiInputSweepNeverCrashesOrLosesTheDevice`
  and `Integration.RenderModesSelectableInGui` both still device-lose (they
  sweep render modes over the default dinosaur scene) — same reproducer as
  above, not a new failure.

  **Files:**
  - `Resources/ShadersSlang/path_tracing/path_tracing.slang` — the RayQuery
    kernel; the crash-frontier comment block above the hit-info extraction
    records the bisection result in place.
  - `Src/GraphicsEngineVulkan/renderer/PathTracing.ixx`/`.cpp` — dispatch,
    descriptor bindings, the now-fixed specialization-constant defaults.
  - `Test/commit/VulkanEngine/goldenRenderSuite.cpp` —
    `PathTracingAccumulatesAndConverges` (the reproducer),
    `RaytracedWorldFollowsTheModelTransform` (the small-mesh RT counterpart
    that would need a large-mesh variant per "Next step" above).
  - `docs/gpu-golden-testing.md` — how to run container-built binaries on the
    host GPU.

  **Build:** `clangcl-debug` via
  `pwsh -ExecutionPolicy Bypass -File .\scripts\windows\Build-Windows-Container.ps1 -Configurations clangcl-debug -FreshContainer`
  (`-FreshContainer` needed here because `PathTracing.ixx` changed), then host
  GPU runs per `docs/gpu-golden-testing.md`. Shader-only iterations:
  `pwsh scripts/windows/compile-slang-shaders.ps1`, no C++ rebuild needed.

  **Verified 2026-07-31:** 18/20 in
  `--gtest_filter='GoldenRender.*:Integration.*:-GoldenRender.PathTracingAccumulatesAndConverges:GoldenRender.GuiInputSweepNeverCrashesOrLosesTheDevice:Integration.RenderModesSelectableInGui'`
  pass on the RX 9070 XT (the 2 failures are the pre-existing bounce-loop gap
  above, not crashes) — no regression from the two fixes, and strictly more
  of the suite runs than before (previously the abort shadowed everything
  after the first PT test in file order).

## 2026-07-31 batch III — planner (submodule hygiene, PT bounce loop, RT diagnosis, CI filter, frustum perf)

All five verified against the tree on 2026-07-31 before writing. Findings behind
them: the RPT submodule working tree currently REVERTS the committed
shadow-factor fix (see the first task — do it before any other Rust work);
`path_tracing.slang` confirms the batch-II suspicion (no bounce loop at all,
`max_bounces` declared at line 21 and never referenced again); the old GLSL
`Resources/Shaders/` tree is fully deleted (Slang-only now), so the reference
kernel must come from git history; `PushConstantRasterizerUnit` is the ONLY
CPU suite missing from the Windows CI filter (checked every `TEST(` suite name
against `Windows.yml:209-229`); the ANTfrastructure commit the RPT working tree
points at (`1de9aff`) is already on ANTfrastructure `origin/main`, so committing
that bump is safe.

### CI and release gaps

## 2026-07-31 batch IV — planner (refactor: dead code, depth-format consolidation, host/device layout pins)

All three verified against the tree on 2026-07-31 (every "dead" claim below was
re-grepped across `Src/` + `Test/` this pass; the depth-format divergence and
the missing layout pins were confirmed by reading the sites). Candidates found
but deliberately NOT tasked this cycle (queue discipline — next planner cycle
should re-verify and size them): README.md:213-220 names two files that do not
exist (`ShaderIncludes.hpp`, `CompileShadersToSPV.cmake` — the single most
actionable doc drift); `docs/cpp-renderer-improvements.md:66-72,119-121` still
describes runtime-glslc recompilation that was deleted with the Slang
migration; `docs/webgpu-renderer-roadmap.md:121` advertises the naga WGSL
export that `docs/shader-sharing.md:133` documents as retired; per-frame heap
allocations in `CascadedShadowMapMath.cpp` (measurable via
`BM_ComputeCascadeData`); sampler creation triplicated with drifted values
(`PostStage.cpp:187`, `Model.cpp:72`, `Texture.cpp:234`); the 4× duplicated
`DescriptorSetGroup` write prologue; `FileReader.ixx`/`Texture::loadTextureData`
error paths have fuzz-only coverage and no deterministic unit tests.

### C++ Vulkan engine

## 2026-07-31 batch V — planner (docs drift, per-frame allocations, sampler drift, error-path tests)

The actionable queue was empty when this batch was written (only `- [b]` entries
remained). All five tasks below are the candidates batch IV deliberately deferred
"for the next planner cycle to re-verify and size" — each one was re-checked
against the tree this pass:

- README.md:213-220 — **confirmed**: neither
  `Src/GraphicsEngineVulkan/vulkan_base/ShaderIncludes.hpp` nor
  `Src/GraphicsEngineVulkan/cmake/CompileShadersToSPV.cmake` exists anywhere in
  the repo (`find` returns nothing).
- `docs/cpp-renderer-improvements.md` — **confirmed**: :66-72 still describes
  "runtime glslc resolution" and "the loader no longer recompiles GLSL", :119-121
  still tells you to edit a GLSL source and rely on `compile-shaders.ps1`. Only
  `scripts/windows/compile-slang-shaders.ps1` exists; there is no runtime shader
  compilation at all (AGENTS.md § Shaders).
- `docs/webgpu-renderer-roadmap.md:121` — **confirmed**: advertises the naga
  WGSL→SPIR-V export as a shipped feature while `docs/shader-sharing.md:133`
  documents the same route under "Historical note: the retired naga/WGSL-export
  route".
- `CascadedShadowMapMath.cpp:29-48` — **confirmed**: `frustumCornersWorldSpace`
  returns a `std::vector<glm::vec4>` built by 8 `push_back`s from empty, once per
  cascade.
- Sampler triplication — **confirmed** at `PostStage.cpp:188`, `Model.cpp:76`,
  `Texture.cpp:237`, with real drift between them (see the task).
- `FileReader.ixx` / `Texture::loadTextureData` — **confirmed**: the only
  coverage is `Test/fuzz/`; no deterministic suite exercises either.

Candidates found but NOT tasked this cycle (queue discipline; re-verify next
pass): the 4× duplicated `checkWritePreconditions`/`findBinding` prologue in
`DescriptorSetGroup.cpp:191,217,241,267` — three lines each, and hoisting it
needs an out-param, so the consolidation is likely worse than the duplication;
`VulkanRenderer.cpp:1194` copies `scene->getObjectDescriptions()` by value
(needs a check of whether that path is per-frame or scene-change-only before it
is worth a task); `third_party/NLOHMANN_JSON` gitlink is drifted in the working
tree again (`eaedec85` → `2222d386`) — the recurring drift recorded in
`[[submodule-pin-drift]]`, an owner call rather than an executor task.

### Docs

### C++ Vulkan engine

## 2026-08-01 batch — planner (texture-slot cap, redundant raster in RT/PT, per-frame allocs, Rust tile binning)

The actionable queue was empty when this batch was written (only `- [b]` entries
remained across the whole file). Every claim below was read out of the tree this
pass, with the numbers measured rather than asserted:

- **`MAX_TEXTURE_COUNT` is 24 and the release default scene needs 33.**
  `Src/GraphicsEngineVulkan/common/host_device_shared_vars.hpp:8` and
  `Resources/ShadersSlang/common/scene_types.slang:8` both say `24`.
  `SceneConfig.cpp:131` selects `Models/crytek-sponza/sponza_triag.obj` under
  `NDEBUG`; its `.mtl` declares **24 materials referencing 33 distinct texture
  files** (counted: `grep -c '^newmtl'` = 24, unique `map_*` targets = 33). So
  the shipping release build overflows the cap on its own default scene — the
  warning at `VulkanRenderer.cpp:1427` fires and 9 textures never get bound.
  This is the part of the 2026-07-22 item #3 that was deliberately left: the
  flattening + `texture_offset` landed, the cap did not.
- **The whole binding block is duplicated host↔shader with nothing pinning it.**
  `host_device_shared_vars.hpp` and `common/scene_types.slang` carry the same
  seven constants (`MAX_TEXTURE_COUNT`, `globalUBO_BINDING` … `SHADOW_MAP_BINDING`,
  `TLAS_BINDING`, `OUT_IMAGE_BINDING`, `ACCUMULATION_IMAGE_BINDING`) as two
  independent literals. `pushConstantSuite` pins push-constant *layouts*; nothing
  pins these.
- **The raster pass runs and is thrown away every RT/PT frame.**
  `record_commands` calls `recordRasterPass` unconditionally
  (`VulkanRenderer.cpp:897`) and then `recordRaytracingOrPathTracing`
  (`:899`). `activeOffscreenTexture()` (`:819`) returns *the same* texture the
  rasterizer just rendered into, and the rgen/PT dispatch writes every pixel of
  it. Confirmed by the barrier at `Raytracing.cpp:94`, whose `oldLayout` is
  hard-coded to `eShaderReadOnlyOptimal` precisely *because* the raster render
  pass ran first (`PathTracing.cpp:83` is identical).
- **Per-frame heap allocations survive in three record paths.**
  `DeferredRasterizer.cpp:487` allocates a `std::vector<vk::Buffer>` **per mesh
  per frame**; `SkyBox.cpp:457` one per frame; `CascadedShadowMap.cpp:448`
  (`cascadeFrusta`) and `:484` (`shadowDescriptorSets`) one each per frame.
  `Rasterizer.cpp:154-156` and `CascadedShadowMap.cpp:519-521` already use the
  pointer form, so the fix is the pattern the file itself establishes.
- **The Rust tiled-lighting binning ignores light range.**
  `forward.rs:228-327` projects each light's *position* to one pixel and bins it
  into exactly ONE `16×16` tile; `light.range` is packed at
  `packed[base+1][3]` (`forward.rs:212`) and never read by the binner. The
  shader honours the grid (`src/shaders/forward.wgsl:448,468,484`) and only
  falls back to all-lights when `tileW == 0`. Lights whose centre is off-screen
  or behind the camera are dropped entirely (`:308-312`), and a
  `CpuLightKind::Directional` entry (kind `3.0`, no meaningful position) is
  binned by its position like a point light.

Candidates found but NOT tasked this cycle (queue discipline; re-verify next
pass): `VulkanRenderer.cpp:1196` copying `getObjectDescriptions()` by value is
**not** a per-frame cost — it sits in `create_object_description_buffer()`,
which only runs on scene change, so the batch-V deferral resolves to "leave it";
`kind` is bound and never used in both loops of `build_tile_light_grid`
(`forward.rs:250,306`) — a warning-level nit that the range-aware rewrite below
will consume anyway; `forward.rs` is 4084 lines and is the Rust-side equivalent
of the `VulkanRenderer` hub, but no clean extraction was identified this pass.

### C++ Vulkan engine

### Rust WebGPU renderer (`third_party/OxidANT`)

## 2026-08-01 batch II — planner (refactor: dead generality, extension-dispatch triplication, span-ify layout params)

The actionable queue was empty when this batch was written (only `- [b]` entries
remained across the whole file). Every claim below was read out of the tree this
pass:

- **`CascadedShadowMap` still carries the pre-multiview per-cascade
  containers.** `framebuffers` and `shadowMapLayerViews`
  (`CascadedShadowMap.ixx:133,137`) are `std::vector`s that `createFramebuffers`
  `resize(1)`s (`.cpp:169-170`) and only ever indexes at `[0]` (`:184,189,196`,
  and `:461` in `recordCommands`). The comment at `:165-168` says so outright:
  "The per-layer views and per-cascade framebuffers are gone." The accessor
  `getFramebuffers()` (`.ixx:92`) is **dead** — grepped across `Src/` and
  `Test/`, zero callers.
- **Model-file extension dispatch exists in three independent copies, and has
  already drifted once.** `SceneConfig.cpp:100-105` lowercases
  `path().extension()` and tests `== ".obj" || ".gltf" || ".glb"`;
  `Scene.cpp:38-43` (`loadModelByExtension`) lowercases the **whole path** and
  tests `ends_with(".gltf") || ends_with(".glb")`;
  `AsyncModelParse.ixx:125-132` (`isGltfPath`) is a third, byte-identical copy
  of the `Scene.cpp` logic. The comment at `SceneConfig.cpp:97-99` records the
  drift that already shipped: that filter was `== ".obj"`, so `cube.glb` could
  never be picked and `.OBJ` was invisible. Adding a format today means editing
  three places, and the two `ends_with`-on-full-path copies also misclassify a
  path whose *directory* ends in `.glb`.
- **Five stages take pipeline-layout arrays as `const std::vector<...>&` while
  the record path has already moved to `std::span`.** `Rasterizer`,
  `DeferredRasterizer`, `PostStage`, `Raytracing` and `PathTracing` each declare
  three functions (`init`, `shaderHotReload`, `create*Pipeline*`) taking
  `const std::vector<vk::DescriptorSetLayout> &`. Inside, the parameter is used
  **only** via `.size()` / `.data()` into `vk::PipelineLayoutCreateInfo`
  (verified at `DeferredRasterizer.cpp:328,358`, `PathTracing.cpp:206,209`,
  `PostStage.cpp:303`, `Rasterizer.cpp:424`, `Raytracing.cpp:254`) — a drop-in
  `std::span` substitution. Six caller-side `std::vector` temporaries of 1-2
  elements exist purely to satisfy the parameter type
  (`VulkanRenderer.cpp:101,102,117,420,423` and the RT pair at `:123,428`).
  Commit `6e4d0204` already did exactly this for descriptor *sets* across the
  record path; this is the same convention applied to the init/hot-reload path.

Candidates found but NOT tasked this cycle (queue discipline; re-verify next
pass): the `auto r = device...createX(info); ASSERT_VULKAN(...); h = r.value;`
idiom appears at **52** sites and could collapse into a `vkCheck()` helper, but
that is a whole-codebase sweep touching ~20 files in one commit — wants a
deliberate moment, not an executor session; `PostStage.cpp:272` is the only
`createRenderPass` call using the C-style `(&info, nullptr, &out)` overload
while the other four use the `ResultValue` form (a one-line consistency nit,
folded into the `vkCheck()` sweep if that ever happens); the per-stage
`createRenderPass` bodies are genuinely different (1, 2 and 3 subpass
dependencies, different attachment sets) and are **not** a consolidation
target.

## 2026-08-01 batch III — planner (dead GPU-culling path, unchecked Vulkan results, shadow/CI test gaps)

The actionable queue was empty when this batch was written (only `- [b]` entries
remained across the whole file). Every claim below was read out of the tree this
pass:

- **The Rust GPU compute culling path is wired to nothing.** `gpu_culling` is
  referenced at exactly five places in `forward.rs` (`:562, :573, :1117, :1119,
  :2314-2316` — grepped). `GpuCulling::cull()` runs the compute dispatch and
  copies the visibility buffer to `readback_buffer` (`gpu_occlusion.rs:240-247`),
  but **`GpuCulling::readback()` (`:251`) has no caller anywhere in the crate**,
  so `GpuCulling::visibility` stays an empty `Vec` forever. The draw loop's skip
  is gated on `self.occlusion_queries_enabled && !self.occlusion.visible(i)`
  (`forward.rs:2212-2216`) and never consults `gpu_culling` at all. Net effect:
  setting `gpu_culling_enabled = true` costs a compute dispatch plus a buffer
  copy every frame and culls nothing — and because the dispatch sits in an
  `if/else if`, turning it on also *disables* the hardware-query path that does
  work. Zero tests reference `gpu_culling_enabled` (`tests/occlusion.rs` covers
  only the query path). `inv_view_proj` is passed as `Mat4::IDENTITY` with a
  `// placeholder` comment (`:2328`); harmless today only because
  `gpu_cull.wgsl` declares the field (`:9`) and never reads it.
- **The doc comment on `occlusion_queries_enabled` is two increments stale.**
  `forward.rs:566-569` says "this increment only DETECTS occlusion … it does not
  yet skip any draw, so the frame renders exactly as before whether it is on or
  off". It does skip: `:2212`. The test
  `an_occluded_primitive_is_actually_skipped_in_the_opaque_pass`
  (`tests/occlusion.rs:164`) exists precisely because it skips.
- **~20 Vulkan creation calls take `.value` with no `ASSERT_VULKAN`.** Measured
  by scanning every engine `.cpp` for `.value` with no `ASSERT_VULKAN` in the
  preceding six lines. The worst is `VulkanInstance.cpp:79`,
  `instance = vk::createInstance(create_info).value;` — and it is reached even
  when the extension check *failed*: `:70-73` logs `spdlog::error("VkInstance
  does not support required extensions!")` and then falls through to create the
  instance anyway. On a driver missing an extension that is a guaranteed
  `VK_ERROR_EXTENSION_NOT_PRESENT`, a null `VkInstance`, and
  `VULKAN_HPP_DEFAULT_DISPATCHER.init(instance)` on it — a crash with no
  diagnostic, one line after the diagnostic was already known. `Model.cpp:84-88`
  carries a `TODO` admitting the same pattern for `createSampler`.
- **Shadow-path GPU coverage is still one number.** `goldenRenderSuite.cpp` has
  25 tests; the only shadow oracle is `ShadowsDarkenSomePixels` (:565), a
  darkened-pixel *ratio*. `CascadedShadowMapUnit.CascadesRespondToLightDirection`
  (:288) pins the same property on the CPU, but nothing checks end-to-end that
  the light direction reaching the shadow pass actually moves the rendered
  shadow — the exact gap that produced the "shadow baked into the model reported
  as cast" retraction recorded above.
- **The Windows CI test filter is a hand-maintained list that has already
  drifted twice.** `.github/workflows/Windows.yml:209-237` enumerates 27
  `<Suite>.*` globs. `Test/commit/VulkanEngine/*.cpp` defines 29 suite names, of
  which `GoldenRender` and `Integration` are the deliberate GPU exclusions — so
  the list is complete *right now*, but only because commit `eb077041` had to
  add `PushConstantRasterizerUnit` by hand after it was missed. AGENTS.md and
  this file both already warn "a suite added to the repo does not run in CI
  unless it is added to the filter"; nothing enforces it.

Candidates found but NOT tasked this cycle (queue discipline; re-verify next
pass): `Model::addSampler` creates one `vk::Sampler` per texture with identical
parameters except `maxLod`, so Sponza allocates 33 near-duplicate samplers
against a device `maxSamplerAllocationCount` — dedup is real but small;
`Model::cleanUp` (`Model.cpp:26-37`) does not `meshes.clear()` while it clears
every other container, so a second `cleanUp()` re-walks already-cleaned meshes
(idempotent in practice, inconsistent on its face); the `# cascades` GUI slider
still advertises 1..8 against `MAX_CASCADES` 3 — now cheaply fixable since
`host_device_shared_vars.hpp` is a plain header the GUI could include, but the
engine-side clamp already makes it a cosmetic lie rather than a bug.

### Rust WebGPU renderer (`third_party/OxidANT`)

## 2026-08-01 batch IV — planner (depth-resolve root cause, generated-shader gates)

The actionable queue was empty when this batch was written (only `- [b]` entries
remained across the whole file). Every claim below was read out of the tree this
pass.

**The depth-resolve regression from batch III is ROOT-CAUSED.** Batch III recorded
it as "not root-caused … needs a dedicated session", with three second-order
symptoms (occlusion counts always 0, SSAO adding energy, 0 composited alpha
pixels) and the MSAA depth resolve named as prime suspect. It is the resolve, and
the mechanism is exact:

- `Resources/ShadersSlang/depth_resolve/depth_resolve.slang:20` declares
  `float fs_main(FullscreenVsOut In) : SV_Depth` — correct.
- The Slang **WGSL backend does not honour `SV_Depth`**. Both the generated
  `Resources/ShadersSlang/build/combined_depth_resolve.wgsl:21` and the
  checked-in crate copy
  `crates/webgpu_renderer/src/shaders/depth_resolve.wgsl:19-22` emit
  `struct pixelOutput_0 { @location(0) output_0 : f32 }` — a **colour** output,
  not `@builtin(frag_depth)`. Confirmed globally: `grep -o '@builtin([a-z_]*)'`
  across every `src/shaders/*.wgsl` yields `position`, `vertex_index`,
  `global_invocation_id`, `local_invocation_index` — **zero** `frag_depth`.
- `create_depth_resolve_pipeline` (`forward.rs:2860-2880`) declares
  `targets: &[]` and the pass declares `color_attachments: &[]`, so that
  `@location(0)` output is silently discarded. Depth is therefore written from
  the *rasterized* `svPosition.z`, which `vs_main` hard-codes to `0.0`
  (`depth_resolve.slang:14`), under `depth_compare: Always, depth_write_enabled:
  true`. **`self.depth` ends up 0.0 at every texel, every frame.**
- That single fact explains all three symptoms without needing three bugs. The
  occlusion AABB pipeline is `depth_compare: LessEqual` with write off
  (`occlusion.rs:173-174`) against `self.depth`: against a uniform 0.0 every
  AABB fragment fails, so **every** primitive reads 0 samples whether occluded
  or not — precisely the reported symptom. SSAO reconstructs position from the
  same buffer; the tonemap/alpha composite reads it too.
- Both compile scripts already carry a **post-emit patch table** for exactly
  this class of Slang-WGSL-backend deficiency —
  `$DepthTexturePatches` (`scripts/windows/compile-slang-shaders.ps1:231-249`)
  and the `sed -i -E` block (`scripts/linux/compile-slang-shaders.sh:183-196`),
  both of which already patch `depth_resolve.wgsl`. So the fix has an
  established home, and this is the same failure mode as commit `64f5053e`
  ("fix Slang WGSL-backend depth-texture emission blocking all Rust GPU tests").

Two gate gaps let it through, both tasked below: the two patch tables are
independent hand-written copies with nothing pinning them, and `depth_resolve`
is one of three `src/shaders/*.wgsl` files absent from `tests/shader_export.rs`'s
hard-coded `SHADERS` list — the checked-in generated WGSL has **no** staleness or
validity gate on the Rust side at all (`BuildIntegrity`'s
`CompiledShadersAreNotOlderThanTheirSources` /
`EveryShaderSourceHasCompiledBinary` walk `build/spirv` only).

Also verified this pass and folded into the tasks rather than tasked separately:
`depth_resolve.slang:26` loops `i < 4u` while `GetDimensions` already wrote
`samples` into an unused local (silent wrongness if `MSAA_SAMPLE_COUNT` ever
moves off 4); `forward.rs:379-385`'s doc comment on `occlusion_queries_enabled`
still claims "it does not yet skip any draw" — the twin at `:566` was corrected,
this one was missed, and `:2218` does skip.

Candidates found but NOT tasked this cycle (checked, then rejected with a
reason — do not re-propose without new evidence): **the RT/PT barrier
`oldLayout`** after commit `60729e06` skipped the raster pass is *correct* — that
commit moved both `Raytracing.cpp` and `PathTracing.cpp` to `eUndefined` and ran
a `validate_sync` pass; **`Model` sampler dedup** (Sponza allocates 33 samplers
differing only in `maxLod`) is not worth it — `modelTextureSamplers[t]` is
positionally indexed by the descriptor write (`VulkanRenderer.cpp:1484`) and the
destroy loop (`Model.cpp:32-34`) would double-destroy shared handles, so dedup
costs an index indirection plus a separate owned-unique list to buy headroom
against a limit whose spec minimum is 4000; **`Texture::textureSampler` is not a
duplicate** of the `Model` sampler — `createTextureSampler` has five callers
(clouds/shadow map/skybox) and none of them are model textures; **extracting the
animation samplers** (`keyframe_lerp_indices`/`cubic_spline_weights`/`sample_*`,
`forward.rs:3188-3324`) out of the 3968-line hub would be pure code motion — they
already have inline unit tests at `forward.rs:3711-3940`; **a C++ headless
per-pass GPU-timing JSON dump** already exists (`KATAGLYPHIS_GPU_TIMING_JSON`,
consumed by `scripts/Compare-RendererTimings.ps1`).

### Rust WebGPU renderer (`third_party/OxidANT`)

### Build / scripts

### C++ Vulkan engine

## 2026-08-01 batch V — planner (refactor pass)

The actionable queue was empty again when this batch was written (only `- [b]`
entries across the whole file, and batch IV's three subsections above are drained).
Every claim below was read out of the tree this pass; the three tasks are all
C++ Vulkan engine, all found by a dead-code / API-consolidation / doc-drift sweep.

Verified this pass and folded into the tasks rather than tasked separately:
batch IV's two "folded" fixes **did land** — `depth_resolve.slang:26` now loops
`i < samples` from `GetDimensions`, and `forward.rs`'s `occlusion_queries_enabled`
doc comment no longer claims "it does not yet skip any draw". Nothing to re-do there.

**One build-system fact that all three tasks depend on, checked this pass:**
`kataglyphis_collect_module_interfaces` (`third_party/ANTfrastructure/cmake/KataglyphisCMakeHelpers.cmake:10-13`)
uses a plain `file(GLOB_RECURSE ... *.ixx)` with **no `CONFIGURE_DEPENDS`** — unlike
the commit-test glob (`Test/commit/VulkanEngine/CMakeLists.txt:9`), which has it
precisely because a new file was otherwise silently never compiled. So **adding or
deleting any `.ixx` requires a CMake re-configure**, and combined with the recorded
module-BMI skew hazard ("Incremental container builds can ship ODR-broken binaries")
every task below wants `-FreshContainer`, not an incremental build.

Candidates found but NOT tasked this cycle (checked, then rejected with a reason —
do not re-propose without new evidence): **unifying the CSM shadow-pass draw loop**
(`CascadedShadowMap.cpp:499-528`) into task 3's helper — it ORs visibility across
`cascadeFrusta` via `isVisibleAsShadowCaster` and pushes a *different* push-constant
type, so folding it in would need a predicate + a type parameter and would
misrepresent two genuinely different loops as one; **`Scene::getObjectDescriptions()`
returning `std::vector` by value** (`Scene.ixx:110`) — one caller
(`VulkanRenderer.cpp:1217`, inside `rebuildObjectDescriptions`), which runs on scene
change, not per frame, so the `std::span` treatment that paid off for
`BM_AvailableModelPaths` buys nothing measurable here; **a shared framebuffer-creation
helper** across the five `vk::FramebufferCreateInfo` sites (Rasterizer, DeferredRasterizer,
PostStage, CascadedShadowMap, SkyBox) — they differ in attachment count, layer count
and multiview, so the helper would be almost all parameters.

### C++ Vulkan engine

## 2026-08-01 batch VI — planner (Slang-source shadow inversion, GPU-cull root cause, generated-artifact gate)

The actionable queue was empty again when this batch was written (only the eight
`- [b]` entries across the whole file; batch IV's and batch V's subsections are
drained). Every claim below was read out of the tree this pass.

**The headline finding: the inverted shadow factor has now been "fixed" twice and
reverted twice, because both fixes were applied to a GENERATED file.**
`git log -L 365,365:Resources/ShadersSlang/forward/forward.slang` returns exactly
one commit — `40b1cbe3 complete slang migration` — so the Slang source has read
`directLight *= 1.0 - shadow;` since the day it was written and has never been
touched. Meanwhile `1dd2ebd` (Jul 31, "fix … inverted shadow factor", 16/33 →
32/33 headless) and `9acdc0a6` (Jul 31, "restore reverted RPT shadow-factor fix")
both edited only `crates/webgpu_renderer/src/shaders/forward.wgsl`, a
`compile-slang-shaders` output. The RPT working tree is dirty again right now with
the regenerated — i.e. re-inverted — WGSL, and the three-line warning comment
`1dd2ebd` added ("Do not wrap it in `1.0 - ...`") is gone with it. That comment is
itself the proof of the anti-pattern: Slang's WGSL backend emits zero comments, so
a `//` in a generated file can only be a hand-edit awaiting deletion.

Two mechanisms let this recur, both tasked below:

- **The convention is inverted between the two renderers under near-identical
  names.** `Resources/ShadersSlang/common/cascaded_shadow.slang:10-11`'s
  `calc_cascaded_shadow` returns OCCLUSION ("0 = fully lit"), and its two C++ call
  sites (`rasterizer.slang:82`, `deferred.slang:133`) correctly write
  `color *= 1.0 - shadow * intensity`. `forward.slang:192`'s `shadow_factor`
  returns VISIBILITY (line 207 early-returns `1.0` for out-of-cascade;
  `SampleCmpLevelZero` returns 1 where the fragment passes the depth compare), so
  `1.0 - shadow` at `:365` is wrong. Copying the C++ idiom onto the Rust helper is
  the whole bug, and the names give no warning.
- **Nothing automated can catch it.** `BuildIntegrity.CheckedInWgslIsNotOlderThanItsSlangSource`
  (`buildIntegritySuite.cpp:807`) compares mtimes only — a regenerate makes the
  destination *newer*, so it passes while carrying the regression (and after a
  fresh CI checkout mtimes carry no staleness information at all). On the Rust
  side, `shadow_darkens_plane_under_cube` *would* catch a full inversion (it
  demands `lit_plane > 1000` neutral-bright pixels, which the inversion destroys),
  but `rust_ubuntu24_04.yml` runs `cargo_test.sh` in a GPU-less container where
  every `headless.rs` test hits its `GpuContext::new_headless()` early return and
  prints `SKIP`.

**Second root cause found this pass: the GPU-culling compute path culls every
primitive, and the reason is in the shader.** The `- [b]` "Make `gpu_culling_enabled`
actually cull" entry above closed with "needs a fresh look at the compute-shader
culling path itself"; this is that look. `Resources/ShadersSlang/gpu_cull/gpu_cull.slang:31-44`
projects **only the AABB centre** and keeps the primitive when
`depth <= sampledDepth`. For any opaque object that rendered itself, the centre of
its own bounding box sits *behind* its own front face, so its projected depth is
strictly greater than the depth the object wrote at that texel and the test fails
— every visible primitive is culled. That is exactly the reported symptom
(`two_visible_cubes_are_both_drawn_with_gpu_culling` returning `(0, 2)`). The
hardware-query path it was told to mirror does not have this property because it
rasterizes the *box* (front face nearer than the object) with `LessEqual`.
Lines 37-38 add a second over-cull: a primitive whose centre falls off-screen or
behind the near plane is culled outright, so a large ground plane disappears.

Verified this pass and folded into the tasks rather than tasked separately: the
`alpha_modes_blend_and_mask` failure that batch IV recorded as "isolated, needs its
own investigation" is very likely the same inversion — its classifier requires
`g > 140 && r > 70 && b > 60` over a "bright white plane", and zeroing direct light
everywhere outside a cascade takes the frame to ambient/IBL only, which reads as
the reported 0 composited pixels. Task 1 states the expected outcome rather than
assuming it. Also confirmed: `parseCpu`'s texture extraction (`GltfLoader.cpp:438-441`)
is NOT a second alignment hazard — `textureID` is assigned from
`textureImages.size()` at push time, so a dropped image shifts nothing.

Candidates found but NOT tasked this cycle (checked, then rejected with a reason —
do not re-propose without new evidence): **`histogram.wgsl` being the one
hand-written WGSL** — AGENTS.md § Shaders already records why (Slang has no
`InterlockedAdd` on `RWStructuredBuffer` for WGSL) and it is Rust-only, with no
C++ counterpart to share; **README/`shader-build-pipeline.md`/`webgpu-renderer-roadmap.md`
shader drift** that batch IV deferred — re-read this pass and all three are now
correct (`shader-build-pipeline.md:49` explicitly frames the GLSL tree as a
"Historical note", the roadmap marks the naga route retired); **the `Y`-flip
divergence** between `cascaded_shadow.slang:29` (`proj.xy * 0.5 + 0.5`) and
`forward.slang:206` (`0.5 - proj.y * 0.5`) — the two renderers build their cascade
matrices differently and both shadow goldens pass, so there is no evidence either
is wrong; **`CheckedInWgslIsNotOlderThanItsSlangSource`'s missing mtime tolerance**
— a real sharp edge, but task 3's content gate makes the mtime test's vacuousness
moot rather than needing its own fix.

## 2026-08-01 batch VII — planner (multi-model texture offsets, unsynchronized clouds pass, cascade-count drift)

The actionable queue was empty again when this batch was written (the eight `- [b]`
entries are the only checkboxes left in the file; batches IV, V and VI are drained).
Every claim below was read out of the tree this pass, with the `file:line` given.

**The headline finding: `create_object_description_buffer` indexes a PER-MESH array
with a PER-MODEL counter.** `Scene::add_model` (`Scene.cpp:150-153`) pushes *one
object description per mesh*, flattened across models — its own comment says so
("objectIndex (the per-draw push constant) indexes this list"). But
`VulkanRenderer.cpp:1222-1226` walks that list with `i < scene->getModelCount()`
and accumulates `scene->getTextureCount(i)`, i.e. it treats slot `i` as *model* `i`.
Its comment ("descriptions are pushed one per model, in model order") is stale and
is what makes the bug read as correct. Task 1 fixes it. It is latent with the
default single-model scene (every offset is legitimately 0) and bites the moment a
multi-mesh model is followed by a second model — exactly the case
`GoldenRender.SecondModelLoadsAndRenders` exercises without asserting the index
arithmetic (the "Multi-object rendering works" note above already flags that gap).

**Second finding: the clouds compute pass has no synchronization with its consumer.**
`VulkanRenderer.cpp:871` dispatches the cloud compute shader, which writes
`cloudOutputTexture` as a storage image; `VulkanRenderer.cpp:926` runs the post pass,
whose descriptor set samples that same image (`VulkanRenderer.cpp:1155-1159`).
Between them there is nothing: `grep pipelineBarrier Src/.../VulkanRenderer.cpp`
returns **zero hits**, and PostStage's only subpass dependency is
`eColorAttachmentOutput -> eColorAttachmentOutput` from `VK_SUBPASS_EXTERNAL`
(`PostStage.cpp:250-256`), which orders the swapchain colour attachment and says
nothing about a compute write. The comment at `VulkanRenderer.cpp:917-920` is about
that swapchain barrier specifically and does not cover this hazard. Tasks 2 and 3.

**Third finding: `MAX_CASCADES` has a third, ungated copy.** The pin added by
`766bd89c` compares `host_device_shared_vars.hpp` against `scene_types.slang` only
(`buildIntegritySuite.cpp:626-667`), but
`Resources/ShadersSlang/rasterizer/shadows/shadow_map.slang` declares its own
`static const int NUM_CASCADES = 3` (`:8`) *and* a literal `ConstantBuffer<float4x4[3]>`
(`:10`), neither of which the gate can see. Task 4.

Candidates found but NOT tasked this cycle (checked, then rejected with a reason —
do not re-propose without new evidence): **`gpu_cull.slang:79`'s comment calling the
maximum sampled depth "nearest-to-camera"** — the comment is backwards for a standard
0-near depth buffer, but the *math* (`cull iff aabbNear > maxSampled`) is the correct
conservative HZB test, so this is a one-word comment nit not worth a task on its own;
**`Texture::generateMipMaps`'s `[[maybe_unused]] uint32_t in_mip_levels`**
(`Texture.cpp:299`, loop reads the member `mip_levels` at `:323` instead) — real dead
parameter, but the single call site passes exactly that member, so it is cosmetic;
**the `# cascades` GUI slider advertising 1..8 against `MAX_CASCADES` 3** — already
recorded as a deliberate cosmetic lie in batch III's prose above, with the engine-side
clamp making it harmless; **checking in the generated SPIR-V under a content gate like
the WGSL one** — unnecessary, `git ls-files Resources/ShadersSlang/build` returns
nothing, so no SPIR-V or WGSL artifact is tracked in this repo and there is no
hand-edit surface to guard.

### Rust WebGPU renderer (`third_party/OxidANT`)

## 2026-08-01 batch VIII — planner (refactor: unread shadow stats + dead accessors, skybox view plumbing, descriptor-write duplication)

The actionable queue was empty again when this batch was written (the eight `- [b]`
entries are the only checkboxes left in the file; batches IV–VII are drained).
Every claim below was read out of the tree this pass, with the `file:line` given.

**The headline finding: the shadow pass computes cull statistics every frame and
throws them away, while the GUI already has the panel that would show them.**
`CascadedShadowMap::recordCommands` resets `castersDrawn`/`castersConsidered`
(`CascadedShadowMap.cpp:433-434`) and increments them per mesh (`:505`, `:515`).
Their only readers are `getCastersDrawn()`/`getCastersConsidered()`
(`CascadedShadowMap.ixx:112-113`), and a grep of `Src/` + `Test/` for both names
returns **only those six lines** — nothing calls the getters. Meanwhile
`GUI.cpp:247-252` already renders a "Frustum culling" checkbox plus
`Culled: %u (%.1f%%)` from `guiRendererSharedVars.visibility.meshes_drawn` /
`meshes_total`, which `VulkanRenderer.cpp:1008-1011` publishes from the *raster*
path. So the identical diagnostic exists for one pass and is computed-then-discarded
for the other. The Rust renderer already resolved this the same way
(`c2c2fe4` "cache the shadow-caster render bundle across frames, make caster stats
honest").

**Same finding, second half: the comment describing the culling toggle is wrong.**
`GUIRendererSharedVars.ixx:59-64` documents `frustum_culling_enabled` as the switch
you flip "when something goes missing" to learn "whether culling is the cause", and
signs off with "Never applied to the shadow pass; see Rasterizer::recordCommands."
The flag is read at exactly one site (`VulkanRenderer.cpp:931-934`, building the
optional camera frustum) — but `CascadedShadowMap::recordCommands` culls
*unconditionally* against the cascade frusta (`:507-514`), gated by nothing. The
sentence is therefore true about the flag and false about the pass, and it reads as
the latter. A missing *shadow* cannot be diagnosed with that checkbox today, which is
precisely the failure mode the comment claims the checkbox exists to rule out.

**Second finding: the skybox framebuffer views are built twice, and half of each
build is a vector of N copies of one value.** `VulkanRenderer.cpp:133-138` and
`:695-700` are the same six lines verbatim (init and `recreateSwapChain`). Both fill
`skyboxDepthViews[i] = postStage.getDepthBufferImageView()` in a loop — the same
handle for every element, because `PostStage` owns a single depth buffer
(`PostStage.ixx:31`, one `depthBufferImage`). `SkyBox::createFramebuffers` /
`recreateFrameResources` (`SkyBox.ixx:29,33`) take both as
`const std::vector<vk::ImageView>&` plus a redundant `size_t count` that is always
`imageViews.size()`. This is the same shape commit `39486995` already collapsed for
`CascadedShadowMap` ("per-cascade framebuffer vectors to scalars"), and the same
`std::span` conversion `7cff9cc0`/`6e4d0204` applied to the record path.

**Third finding: `DescriptorSetGroup`'s four write helpers share a copy-pasted
prologue and epilogue.** `writeBuffer`, `writeImage`, `writeImageArray` and
`writeAccelerationStructure` (`DescriptorSetGroup.cpp:171,197,223,249`) each open
with the identical `checkWritePreconditions` + `findBinding` + null-check pair, then
each fill the identical five `vk::WriteDescriptorSet` fields
(`dstSet`/`dstBinding`/`dstArrayElement`/`descriptorType`/`descriptorCount`) and
close with the identical `updateDescriptorSets(1, &descriptor_write, 0, nullptr)`.
Only the payload pointer differs (`pBufferInfo` / `pImageInfo` / `pNext`), plus one
`descriptorCount` that comes from the binding rather than being 1. This was recorded
as a deferred candidate in batch IV ("the 4× duplicated `DescriptorSetGroup` write
prologue") and never tasked.

**Build-system fact all three tasks depend on** (re-checked this pass, unchanged
since batch V): `kataglyphis_collect_module_interfaces`
(`third_party/ANTfrastructure/cmake/KataglyphisCMakeHelpers.cmake:10-13`) globs `*.ixx` **without**
`CONFIGURE_DEPENDS`, and the recorded module-BMI skew hazard ("Incremental container
builds can ship ODR-broken binaries") applies to any edited module interface. All
three tasks below edit a `.ixx`, so all three want `-FreshContainer`.

Candidates found but NOT tasked this cycle (checked, then rejected or deferred with a
reason — do not re-propose without new evidence): **`Raytracing`'s duplicated
swapchain pointer** — `init` stores the parameter in `this->vulkanSwapChain`
(`Raytracing.cpp:31`), `recordCommands` takes a *second* `VulkanSwapChain*`
(`:47`) and picks between them with `swapchain ? swapchain : this->vulkanSwapChain`
(`:114`), while the sole call site passes `&vulkanSwapChain`
(`VulkanRenderer.cpp:1046`) — the same object `init` was given, never null. Both the
member (`Raytracing.ixx:42`) and the parameter carry a stale `[[maybe_unused]]`
though line 114 uses them. Real dead generality worth a task, deferred purely for
queue discipline (three-task cap) — **pick this up next cycle**;
**`PathTracing.ixx:48`'s `[[maybe_unused]] vk::PushConstantRange pc_range{..., 0, 0}`**
— zero-sized, grep-confirmed unreferenced (the `pc_ranges` hits are `Raytracing`'s
separate, live member), but it is a single line with no behaviour attached, so it
rides along with the Raytracing task rather than earning its own; **the `vkCheck()`
sweep over the 52 `createX` + `ASSERT_VULKAN` + `.value` sites** — still the same
whole-codebase, ~20-file commit batch II deferred, still wanting a deliberate moment
rather than an executor session; **`Scene::getObjectDescriptions()` returning
`std::vector` by value** — re-verified this pass, still one caller on the
scene-change path, still not worth a `std::span`, exactly as batch V concluded.

### C++ Vulkan engine

## 2026-08-01 batch IX — planner (dead RT/PT plumbing, a queue-family transfer that never happens, cross-frame clouds WAR, shadow-cull toggle, occlusion strobe)

The actionable queue was empty again when this batch was written (the eight `- [b]`
entries are the only checkboxes left in the file; batches IV–VIII are drained).
Every claim below was read out of the tree this pass, with the `file:line` given.

**Task 1 is the item batch VIII explicitly deferred with "pick this up next
cycle"** — re-verified unchanged: `Raytracing::init` stores its swapchain
parameter (`Raytracing.cpp:31`), `recordCommands` takes a *second* one
(`:47`, still marked `[[maybe_unused]]`) and picks between them at `:114`, while
the sole call site (`VulkanRenderer.cpp:1050`) passes `&vulkanSwapChain` — the
same object `init` was handed. `PathTracing` is NOT the same shape: its
`vulkanSwapChain` parameter is genuinely read (`PathTracing.cpp:121`); only the
zero-sized `pc_range` member (`PathTracing.ixx:48`) is dead, and it rides along.

**Second finding: PathTracing's two image barriers declare a queue-family
ownership transfer that no second barrier ever completes.**
`PathTracing.cpp:79-80` sets `srcQueueFamilyIndex = graphics_family`,
`dstQueueFamilyIndex = compute_family`, and `:105-106` sets compute→compute —
but this command buffer is recorded into the frame's graphics command buffer and
submitted to the graphics queue, and the `dispatch` that consumes the image
follows on that same queue. A release with no paired acquire leaves the contents
undefined for the acquiring queue. It is invisible today only because
`getQueueFamilies` breaks as soon as one family satisfies graphics + compute +
present (`VulkanDevice.cpp:683`), which on every desktop AMD/NVIDIA part is
family 0 for all three, so `src == dst` and Vulkan ignores both fields. Every
other barrier in the engine already uses `VK_QUEUE_FAMILY_IGNORED`
(`Raytracing.cpp:100-101`, `VulkanRenderer.cpp:881-882`); PathTracing is the
outlier. Task 2.

**Third finding: the cross-frame WAR on `cloudOutputTexture` is still open, and
the code says so.** `VulkanRenderer.cpp:898-906` documents it precisely
("Follow-up, not fixed here") and nothing has closed it since:
`cloudOutputTexture` is a single image (`Clouds.cpp:286-289`, one texture, not
per-frame-in-flight), `MAX_FRAME_DRAWS == 3`, so frame N's post-pass read and
frame N+1's compute write are ordered by nothing. The barrier at `:878-896`
closes only the same-frame RAW. Since `5ccaca80` there is now a one-command way
to check it (`scripts/windows/Run-SyncValidation.ps1`), and the fix is cheap and
deterministic either way — a barrier orders against *all* previously submitted
commands on the queue, including earlier submissions, so a WAR barrier before
the dispatch closes it without duplicating the image. Task 3. Note
`clouds_enabled` defaults to **false** (`GUISceneSharedVars.ixx:45`), so any
verification run must turn clouds on.

**Fourth finding: batch VIII fixed the comment about the culling toggle but not
the gap it described.** `GUIRendererSharedVars.ixx:69-73` now correctly states
that `frustum_culling_enabled` gates only the raster paths and that "the cascade
pass culls unconditionally … this switch does not turn that off". That is now an
accurate description of a real diagnostic hole: the checkbox exists so that "when
something goes missing, [you can tell] in one click whether culling is the cause"
(`:66-68`), and a missing *shadow* is exactly the case it cannot answer.
`CascadedShadowMap::recordCommands` culls against the cascade frusta at
`CascadedShadowMap.cpp:507-514` gated by nothing. The caster counters the same
batch published (`VulkanRenderer.cpp:918-919`) make the fix assertable. Task 4.

**Fifth finding (Rust): the hardware-occlusion path still strobes any primitive
the camera is inside.** This is item #1 of the 2026-07-22 Rust survey, re-checked
and still open — `OcclusionQueries::record` takes only `view_proj`
(`occlusion.rs:227-233`) and there is no eye position anywhere in the module, so
nothing can force-visible a box containing the camera. `cull_mode: None`
(`:163-165`) is present and its comment names the situation, but back faces still
fail `LessEqual` against the geometry's own depth while front faces are
near-plane clipped, so the query reads 0. The consumer is live:
`forward.rs:2049-2050` skips the draw for `!self.occlusion.visible(i)`, which
empties the depth there, which makes the next query pass — a ~30 Hz flicker on
the object filling the screen. Task 5.

Verified-and-closed while surveying (do NOT re-propose these; they are done, and
the 2026-07-22 Rust survey text above is stale for them): #2 `set_instances`
widening `scene_bounds` — `recompute_scene_bounds` now exists
(`forward.rs:2581-2597`) and is called from all three mutation sites (`:1000`,
`:1034`, `:2578`); #3 `world_center` metric drift — every site is the AABB centre
now, including upload (`:1605`); #5 instanced normals — `instance_cofactor_0`
is emitted and applied (`forward.wgsl:129-131`, `:181`); #10
`KHR_materials_unlit` — loaded (`gltf_loader.rs:600`), plumbed
(`forward.rs:1834`) and demoed (`wasm_demo.rs:48`); #11 anisotropy —
`anisotropy_for` plus a unit test (`forward.rs:3319`, `:3676`); #14 zero-strength
bloom/SSAO — gated at `forward.rs:2192`, `:2196`.

Candidates found but NOT tasked this cycle (checked, then rejected or deferred
with a reason — do not re-propose without new evidence): **`Texture::generateMipMaps`'s
dead linear-blit branch** — `spdlog::error("...does not support linear blitting!")`
at `Texture.cpp:303-306` can no longer fire, because the only caller already
consulted `supportsLinearBlit` and forced `mip_levels = 1` on failure
(`:129-132`), so the function is never entered in that case; it also re-queries
`getFormatProperties` for nothing and carries the dead `[[maybe_unused]] uint32_t
in_mip_levels` parameter (`:299`, the loop reads the member at `:323`). Real dead
code, deferred purely for the five-task cap — **pick this up next cycle**;
**`Model::addSampler` creating one `vk::Sampler` per texture** (`Model.cpp:66-82`)
— identical except `maxLod`, so a Sponza-class model burns 33 sampler objects
against a `maxSamplerAllocationCount` floor of 4000; they *are* destroyed
(`:32-35`), so this is headroom, not a leak, and not worth a task yet;
**`docs/gpu-golden-testing.md:121` claiming the suite is 21 tests** when
`goldenRenderSuite.cpp` now holds 28 — one-number doc drift, fold it into
whichever task next touches that file; **`VulkanDevice::getQueueFamilies`
breaking as soon as all three indices are set** (`:683`), so a dedicated
async-compute or transfer family can never be selected — deliberate given nothing
uses a second queue today, and task 2 removes the only code that pretended
otherwise.

## 2026-08-01 batch X — planner (an over-subscribed uniform slot in the Rust forward shader, dead mip branch, script + baseline coverage)

The actionable queue was empty again (the eight `- [b]` entries are the only
checkboxes left; batches IV–IX are drained). Every claim below was read out of
the tree this pass with the `file:line` given.

**The headline finding is task 1, and it is worse than a naming problem.**
`Resources/ShadersSlang/forward/forward.slang:22` documents its uniform as
`float4 cascade_splits; // x,y: splits, z: count, w: tile_h` — five values
asked of a `vec4`. The shader then reads `.z` twice, for two different things:
as the **cascade count** at `:199-200` (`int count = int(frame.cascade_splits.z);
if (cascade > count - 1) cascade = count - 1;`) and as the **tile-grid width**
at `:270` (`uint tileW = uint(frame.cascade_splits.z);`). The Rust host resolves
the conflict in favour of tiles — `forward.rs:1736-1741` writes
`[cascade_splits[0], cascade_splits[1], tile_counts.0 as f32, tile_counts.1 as f32]`
— so the `CASCADE_COUNT as f32` that `update_cascades` puts in
`self.cascade_splits[2]` (`:2619`, and the initializer at `:930`) is written and
then thrown away, and the cascade clamp is fed a tile width.

That alone is only latently wrong (a 256px target gives `tileW = 16`, and
`min(cascade, 15)` never bites). The part that bites is the ordering:
`tile_counts` starts `(0, 0)` (`:910`) and is assigned at `:1809`, **after** the
frame-uniform write at `:1743-1747`. So the value the shader sees is always one
frame stale, and on the first frame it is zero — making the clamp compute
`count - 1 == -1` and force `cascade = -1` for every fragment in the frame.
**Every headless test renders exactly one frame** (`render_to_pixels`), so the
entire Rust GPU suite runs in that state; the shadow tests pass only because a
negative array index degrades to cascade 0's matrix, which is the right cascade
for a near-camera fixture. The same stale frame recurs after every resize.

Task 2 (the still-red `alpha_modes_blend_and_mask`) is sequenced **after** task
1 on purpose: a blend quad shadowed by the wrong cascade is a live hypothesis
for why its green pixels miss the `g > 140` threshold, and re-diagnosing it
before task 1 lands risks chasing a symptom of task 1.

Task 3 is the item batch IX deferred with "pick this up next cycle", re-verified
unchanged. Task 5 folds in the `docs/gpu-golden-testing.md` count drift batch IX
asked to fold into "whichever task next touches that file", and adds a second
instrument disagreement found this pass: the BACKLOG "Measured baseline" table
says `BM_ComputeCascadeData/1` is 142 ns and `/3` is 324 ns, while
`Test/perf/baselines/win-9070xt-32core.json` — captured the same day
(`context.date` 2026-07-31T19:36, same host, same 32 cores) — records 372.82 ns
and 1012.79 ns. 2.6x and 3.1x apart is not desktop noise.

Candidates found but NOT tasked (checked, then rejected — do not re-propose
without new evidence): **`ForwardRenderer::light_space_matrix`**
(`forward.rs:2655-2680`) is `#[allow(dead_code)]` and a near-duplicate of
`light_matrix_for` — real dead code, but it is inside the file task 1 edits, so
delete it there rather than spending a task on it; **the Rust cascade fit has no
texel snapping** (`light_matrix_for:2634-2652` anchors `look_at_rh` at a
continuously moving `center`, which is exactly the shimmer the C++ engine fixed
on 2026-07-31 in `CascadedShadowMapMath.cpp:153-204`) — a genuine cross-renderer
parity gap and the C++ code is a ready-made template, but it needs its own cycle
and its own CPU oracle, so it is recorded here rather than half-specified;
**`Model::addSampler` burning one `vk::Sampler` per texture** (`Model.cpp:66-82`)
— re-checked, still headroom rather than a leak, still not worth a task.

### C++ Vulkan engine

### Build / scripts

### Docs

## 2026-08-01 batch XI — planner (refactor: dead parameters, Clouds setup triplication, forward.rs geometry extraction)

The actionable queue was empty again (the eight `- [b]` entries are the only
checkboxes left in the file; batches IV–X are drained). Every claim below was
read out of the tree this pass with the `file:line` given.

**Re-verified as genuinely closed while surveying — do NOT re-propose these.**
`ForwardRenderer::light_space_matrix` is gone (batch X task 1 deleted it as
instructed; the only `allow(dead_code)` left in the crate are four
`cfg_attr(target_arch = "wasm32", ...)` on `forward.rs:233-244`, which are
platform gating, not dead code). `update_cascades` no longer writes a cascade
count into `cascade_splits[2]` — `forward.rs:2620-2622` now carries the comment
"z, w (tile_w, tile_h) are supplied by the per-frame uniform write in `render`".
Batch II's three items all landed: `Src/GraphicsEngineVulkan/scene/ModelFileKind.ixx`
now owns the extension dispatch behind `isGltfModelPath`/`isSupportedModelPath`
(with tests at `cameraSceneConfigSuite.cpp:206-215`), zero
`const std::vector<vk::DescriptorSetLayout> &` parameters remain anywhere in
`Src/`, and `CascadedShadowMap`'s dead `getFramebuffers()` is gone.
`docs/gpu-golden-testing.md:121-122` is **not** drifted despite the suite now
holding 30 `TEST(GoldenRender, ...)`: 30 − 1 `DISABLED_` − 3 excluded by the
documented filter + 2 `Integration` tests = the 28 it claims. That arithmetic is
non-obvious, so do not "fix" the number.

**Task 1's four items are the last `[[maybe_unused]]` markers in `Src/`** —
grepped this pass, there are exactly four and every one of them marks something
that should simply be deleted rather than annotated. `Texture::generateMipMaps`
kept `vk::Format image_format` when commit `c80e7503` removed its two *other*
dead parameters (`physical_device`, `in_mip_levels`); the format is now
`[[maybe_unused]]` in the definition (`Texture.cpp:293`) but plain in the
declaration (`Texture.ixx:96`), which is the tell that it was overlooked rather
than kept deliberately.

**Task 2: `Clouds` writes the same two sequences three and two times.** The
"create a storage `Texture` + view + sampler, then transition it to `eGeneral`
through a one-shot command buffer" block appears at `Clouds.cpp:33-41`,
`:44-53` and — byte-for-byte identical to the second — at `:286-293` inside
`recreateFrameResources`. The "load SPIR-V, fill a `vk::PipelineShaderStageCreateInfo`,
create a pipeline layout, create the compute pipeline, destroy the module"
sequence appears at `:174-200` and `:203-228`. Together that is ~50 lines of
duplication in a 355-line file, and the `recreateFrameResources` copy is exactly
the shape that drifts: a format or usage flag changed in `createTextures` and not
there produces a resize-only bug. **This is GPU-verifiable** — `clouds_enabled`
defaults to `false` (`GUISceneSharedVars.ixx:45`), but two golden tests turn it
on: `GoldenRender.CloudsAcrossManyFramesDoesNotLoseTheDevice`
(`goldenRenderSuite.cpp:2665`, 30 frames past the frames-in-flight cycle) and
the all-maximum case of `GuiInputSweepNeverCrashesOrLosesTheDevice` (`:2514`).

**Task 3: `forward.rs` is 3740 lines and ~255 of them are pure-CPU geometry that
touches no wgpu type at all.** `Frustum` (`:3007-3068`), `normal_matrix_of`
(`:3070`), `aabb_contains_point` (`:3086`), `instanced_bounds` (`:3103`),
`widen_bounds_for_skin` (`:3128`), `primitive_world_aabb` (`:3150`),
`primitive_local_aabb` (`:3189`), `transform_aabb` (`:3220`) and
`compute_world_bounds` (`:3241`) form one contiguous, self-contained block whose
only imports are `glam`, `crate::scene::*` and two symbols from
`crate::render::occlusion`. They are already documented as a group by
`docs/renderer-bounds-invariant.md` ("Consumers — everything that reads
bounds"), which names them by symbol and never by file, so the move costs no doc
churn. The precedent is `c85ef931`, which pulled animation sampling out of the
same file into `render/animation.rs`; `render/mod.rs` is the one-line registry.

Candidates found but NOT tasked this cycle (checked, then rejected or deferred
with a reason — do not re-propose without new evidence):
**`Clouds::recordComputeCommands` taking `std::span<const vk::DescriptorSet>`
and reading only `[0]`** (`Clouds.cpp:264-271`) — it looks like dead generality
but is not: `VulkanRenderer.cpp:861` builds one `std::array<vk::DescriptorSet, 1>`
and hands the same span to the rasterizer, deferred, skybox and post record
paths, so Clouds is following the house convention, not deviating from it;
**`Model::addSampler` burning one `vk::Sampler` per texture** (`Model.cpp:66-82`)
— re-checked a third time, still headroom against a 4000 floor rather than a
leak, still not worth a task (stop re-checking it);
**the `vkCheck()` sweep over the 52 `auto r = createX(...); ASSERT_VULKAN(...);
h = r.value;` sites** that batch II deferred — re-confirmed as still the right
call to defer: it is a ~20-file single commit that wants a deliberate moment;
**the Rust cascade fit still has no texel snapping** (`light_matrix_for:2637-2660`
anchors `look_at_rh` at a continuously moving `center`, the shimmer the C++ side
fixed in `CascadedShadowMapMath.cpp`) — a real cross-renderer parity gap, but it
is a behaviour change needing its own CPU oracle, not a refactor, and task 3
deliberately does **not** touch it (`light_matrix_for` stays in `forward.rs`
because it reads `self.light_dir_ambient`).

## 2026-08-01 batch XII — planner (Rust cascade fitting, texture-upload round trips, shadow comparison sampler, a drifted comparison script)

The actionable queue was empty again — batches IV–XI are drained and the only
checkboxes left in the file were `- [b]`. Every `file:line` below was read out
of the tree this pass.

**One `- [b]` closed while surveying, not re-proposed.** The
`gpu_culling_enabled` entry above is now `- [x]`: its blocker ("needs a fresh
look at the compute-shader culling path itself") was removed by `1594a4a0`,
whose commit message records both
`an_occluded_primitive_is_skipped_with_gpu_culling` and
`two_visible_cubes_are_both_drawn_with_gpu_culling` as passing. Blocker gone
*and* work done, so the entry was pruned to a stub per this file's own rules.
Two stale prose claims were corrected in place for the same reason (packaging
in CI, the headless GPU-timings JSON) — see those bullets.

**Tasks 1 and 2 both edit Rust cascade fitting and task 2 builds on task 1's
extraction. Do them in order.** They are split rather than merged because they
fix different things: task 1 is a correctness bug (the near cascades cover a
depth band nothing is ever assigned to), task 2 is a stability gap (shimmer).

**Task 1 is a real bug, and the C++ engine already proves the invariant that
catches it.** `update_cascades` (`forward.rs:2610-2635`) sets
`cascade_splits = [near_radius * 2.0, mid_radius * 2.0, 0.0, 0.0]` — values
derived from the *scene* radius — while `forward.slang:201-202` selects a
cascade by comparing them against `In.viewDepth`, which
`forward.slang:151` defines as `distance(worldPos, camera_position)`: an
**eye distance**, not a scene-radius fraction. The two are unrelated
quantities. Cascade 0's box is centred at `camera.target.lerp(camera.eye(),
0.15)` with half-extent `near_radius`, i.e. it covers eye distances roughly
`0.85 * camera.radius ± near_radius`, but it is *selected* for eye distances
`< 2 * near_radius`. For any framing where the camera sits more than about
`3.5 * near_radius` from its target — the normal case — those two intervals do
not overlap at all, so every fragment routed to cascade 0 lands outside
cascade 0's ortho box, `forward.slang:210` sees `uv` outside `[0,1]` and
`return 1.0` (fully lit). The failure is silent by construction: shadows still
appear, because everything far enough out falls through to cascade 2, which
does cover the scene. The symptom is "the near cascades never do anything and
shadow resolution is always the coarsest cascade's", not "shadows are
missing" — which is exactly why no existing test catches it. The C++ engine
has the right oracle already: `CascadedShadowMapUnit.EachCascadeCoversItsOwnFrustumSlice`
(`cascadedShadowMapSuite.cpp:109`) and `StabilizedCascadesStillCoverTheirSlice`
(`:430`) assert precisely this property against `computeCascadeData`, and the
C++ splits come from real view-depth distances (`CascadeData::splitDepth =
cascadeSplits[i + 1]`). Port the assertion, then make it pass.

**Task 2 is the parity gap batch XI deliberately deferred**, re-proposed here
with the new evidence batch XI said it needed: task 1 gives it a pure-CPU home
(`render/cascades.rs`) and a working oracle to extend, which is the "own CPU
oracle" that deferral was waiting on. `light_matrix_for`
(`forward.rs:2637-2655`) anchors `Mat4::look_at_rh` at a continuously moving
`center`, so the ortho box translates *and* the light basis rotates every
frame; the C++ side fixed exactly this in `CascadedShadowMapMath.cpp:154-201`
and documents the three necessary ingredients in the comment there.

**Task 3: `Texture::uploadRgba` costs three full GPU round trips per texture.**
`CommandBufferManager::endAndSubmitCommandBuffer`
(`CommandBufferManager.cpp:57-125`) creates a fence, submits, and
`waitForFences(..., UINT64_MAX)` before returning — every call is a
synchronous stall. `uploadRgba` (`Texture.cpp:151-172`) makes three of them
back to back: `transitionImageLayout`, `vulkanBufferManager.copyImageBuffer`,
then either `generateMipMaps` (which begins/ends its own,
`Texture.cpp:288-378`) or a second transition. They are all recordable into
one command buffer — `VulkanImage::transitionImageLayout` already has a
command-buffer overload (wave 5), and `copyImageBuffer` only needs one.
Related and in the same file: every `Texture` **owns** a `VulkanBufferManager`
(`Texture.ixx:100`) purely to reach `copyImageBuffer`, which touches no member
state; the manager's whole reason to exist is its reusable staging buffer, and
the texture path ignores it and creates a fresh `VulkanBuffer` per texture
(`Texture.cpp:134-138`). One texture-heavy model pays both costs per texture.

**Task 4: the cascade shadow map is bilinear-filtered and then compared, which
is not PCF.** `CascadedShadowMap.cpp:57` builds the sampler with
`vk::Filter::eLinear`, and `common/cascaded_shadow.slang` declares it as a
plain `Sampler2DArray`, calls `.Sample()`, and does the depth compare in the
shader (`occluded += (currentDepth - bias) > mapDepth ? 1.0 : 0.0`). Averaging
four *depths* and then thresholding the average is the textbook wrong order:
at any silhouette the blended depth is a value no surface actually has, so the
comparison answers about geometry that does not exist. Hardware comparison
samplers exist for this, and the Rust renderer already uses one —
`forward.slang:215` calls `shadowMap.SampleCmpLevelZero(shadowSampler, ...)`.
This is a cross-renderer parity fix with a free quality win (the 2x2 hardware
PCF comes with the comparison sampler).

**Task 5 is a defect in a checked-in tool, found by reading it against the two
enums it claims to track.** `scripts/Compare-RendererTimings.ps1:34` defaults
`$RustExpectedPasses` to
`@('Forward','ShadowCascades','Ssao','Bloom','Histogram','Post')`, and
`Assert-PassesExist` (`:49-63`) sets `exitCode = 1` for any expected pass the
JSON does not carry. **There is no `Post` pass on the Rust side.**
`TimedPass::name()` (`gpu_timing.rs:83-93`) emits `ShadowCascades`, `Forward`,
`OcclusionCull`, `Bloom`, `Ssao`, `Histogram`, `ExposureReduce`, `Tonemap` —
the tonemap pass is called `Tonemap`. So the script fails on every run for a
pass that cannot exist, and simultaneously never notices that
`ExposureReduce`, `Tonemap` and `OcclusionCull` are unchecked. The C++ list
(`:32`) does currently match `GPU_TIMED_PASS_EXPORT_NAMES`
(`GUIRendererSharedVars.ixx:26-32`), but nothing keeps it that way. This is
the same class the repo already gates elsewhere (`a63edf10` self-enforcing
Windows CI test filter, `28887db1` / `41b76ab8` generated-WGSL gates), and the
Pester precedent is `scripts/windows/tests/Compare-PerfBaseline.Tests.ps1`.

Candidates found but NOT tasked (checked, then rejected — do not re-propose
without new evidence): **`Frustum::from_view_proj` in `render/bounds.rs:21-35`
does not normalize its planes** while the C++ `extractFrustumPlanes`
(`Frustum.cpp:26-53`) does — but normalization divides by a strictly positive
length, so the sign test both sides perform is unaffected, and the C++
normalization exists only so its `1e-4` epsilon is scale-independent (the Rust
version has no epsilon). Cosmetic, not a defect. **`intersects_aabb_as_caster`
(`bounds.rs:52`) has no production caller** — grepped the whole crate, only
its own test — but this is deliberate and documented: `forward.rs:1877-1884`
disabled per-cascade caster culling when the shadow draw list became a cached
`RenderBundle`, and re-enabling it needs culling-aware bundle invalidation,
which that comment explicitly defers as a design decision. Do not delete the
function and do not re-enable culling without that design. **`GltfLoader.cpp:299`
says "Placeholder for now"** about flat normals that `:352-372` actually
computes — one stale sentence, too small to task; fold it into the next edit
that touches the file. **The `Model::addSampler` per-texture sampler** — batch
XI's instruction to stop re-checking this still stands; it was not re-checked.

## 2026-08-01 batch XIII — planner (a divide-by-zero on the sync error path, a rotted comparison script, a CI GPU probe that always says yes, the Rust cascade oracle batch XII asked for, dead stateless-class members)

Batch XII is fully drained (`50c0304b`, `add6f17c`, `d277fa99`, `9bfe48b4`,
`092f166d`); every checkbox left in the file was `- [b]`. Every `file:line`
below was read out of the tree this pass.

**Task 1 is a real crash on an untested error path, traced end to end.**
`FrameSync::advanceFrame()` (`FrameSync.ixx:121`) is
`current_frame = (current_frame + 1) % frame_sync_count`, and `create()` sets
`frame_sync_count = 0` on *any* creation failure (`:65` for the
semaphore/fence loop, `:82` for the render-finished loop). `drawFrame` does
guard on it — but at `VulkanRenderer.cpp:442`, **before** it can re-enter
`recreateSwapChain()` at `:450`/`:484`, and `recreateSwapChain()` ends with
`createSynchronization()` → `frameSync.create(...)`
(`VulkanRenderer.cpp:1484-1487`). So the count can go to 0 *after* the only
guard. Walk the second failure path (`FrameSync.ixx:82`, render-finished
semaphore): `image_available` and `in_flight_fences` are already fully
populated, so `currentFrame() >= inFlightFenceCount()` (`:455`) passes and
`!inFlightFence()` (`:461`) passes; `render_finished_by_image` was resized to
`imageCount` so `renderFinishedCount()` (`:501`) passes, and
`!renderFinishedSemaphore(image_index)` only fires if the *acquired* image is
one of the ones that failed. Acquire image 0 when image 2 was the failure and
the frame runs all the way to `advanceFrame()` at `:613` → `% 0`. The class
should defend its own invariant instead of relying on a caller check that
runs too early.

**Task 2: `scripts/Compare-RendererPixels.ps1` cannot complete a run.**
`$bmp.Dispose()` is called and the *next* statement passes `$bmp.Width` /
`$bmp.Height` as arguments (`:204-205`, and again at `:223-224`).
`System.Drawing.Image.Width` on a disposed bitmap throws, and the script sets
`$ErrorActionPreference = 'Stop'` at `:37`, so Phase 3 dies the moment a C++
frame exists — the exact path the script is for. Two more defects in the same
file, all of the "a green run means nothing" class this repo keeps
rediscovering: `-ValidationOnly` enumerates `*.png` at `:92` and then
*ignores* that list, testing `Test-Path $cppPng` where `$cppPng` is the
hard-coded `cpp-vulkan.png` (`:103`) while the capture at `:129-135` actually
writes `cpp-vulkan-<suffix>.png` — so validation mode always finds nothing;
and with neither renderer producing a frame, `$exitCode` stays 0 and the
script prints `PIXEL COMPARISON PASSED`. It is also the only one of the three
`scripts/Compare-*.ps1` tools with no Pester suite. Sibling precedent for
everything this needs: `scripts/windows/tests/Compare-RendererTimings.Tests.ps1`
(child-process invocation + fixture files, Pester 3.4.0 dash-less syntax).

**Task 3 is the half of batch XII task 1 that did not land.** That entry said
"The C++ engine has the right oracle already:
`CascadedShadowMapUnit.EachCascadeCoversItsOwnFrustumSlice`
(`cascadedShadowMapSuite.cpp:109`) … Port the assertion, then make it pass."
`add6f17c`/`50c0304b` shipped the fit and five good tests in
`render/cascades.rs:161-344` — splits growing with the camera, the
scene-radius fallback, finiteness, texel snapping, box-size invariance — but
**not the coverage assertion**, and the gap it was meant to catch is still
there. `fit_cascades` (`cascades.rs:91-96`) gives cascade 0 a sphere centred
at `camera.target.lerp(camera.eye(), 0.15)` with radius `0.35 * d`, i.e. it
covers eye distances `[0.5d, 1.2d]`, while `forward.slang:198-201` selects it
for every eye distance below `splits[0] = 0.7d`. Everything closer than
`0.5d` is routed to a cascade whose box does not contain it, and
`forward.slang:212` answers that with `return 1.0` — **fully lit, silently**.
Do NOT "fix" this by re-deriving the boxes from the camera frustum: the
module doc at `cascades.rs:26-34` records that an earlier attempt at exactly
that regressed `shadow_darkens_plane_under_cube` to zero shadowed pixels.

**Task 4: the Windows CI GPU probe asks a question that is not about the
GPU.** `Windows.yml:315` runs `commitTestSuite.exe --gtest_list_tests |
Select-String "GoldenRender"` and prints `GPU_AVAILABLE` if it matches — but
gtest lists registered test names without touching an adapter, so it matches
on every runner that has the binary, and `:318` prints `GPU_NOT_AVAILABLE`
unconditionally right after. `$test -match 'GPU_AVAILABLE'` at `:320` filters
the array and gets a non-empty result, so `$hasGpu` is always true and `:323`
resolves `$validationFlag` to `''` — the step runs
`Compare-RendererTimings.ps1` **without** `-ValidationOnly` on a GPU-less
hosted runner, which is the opposite of what its own comment (`:296-301`)
says it does. Same family as the swapchain screenshot that reads black while
the session is locked: a probe that cannot say no.

**Task 5 (refactor): `CommandBufferManager` has no state, and six objects
carry one anyway.** The class (`CommandBufferManager.ixx:8-22`) has exactly
two members, both `static`, and an empty `private:` section. Six types hold a
zero-information instance of it: `ASManager.ixx:52`, `Texture.ixx:97`,
`DeferredRasterizer.ixx:74`, `VulkanRenderer.ixx:204`,
`VulkanBufferManager.ixx:69`, `Rasterizer.ixx:78` — plus
`DeferredRasterizer.cpp:39` (`commandBufferManager = CommandBufferManager();`,
a no-op assignment) and `Texture.cpp:31`/`:47`, which move the dead member
through `Texture`'s move constructor and move assignment. `d277fa99` deleted
the per-`Texture` `VulkanBufferManager` for the same reason and left this one
behind. Every call site in the tree already uses the static form
(`Texture.cpp:152`, `VulkanBufferManager.cpp:21`, `:34`, `:62`, `:72`).

Candidates found but NOT tasked (checked, then rejected — do not re-propose
without new evidence): **`Texture::createImage` never assigns
`this->mip_levels`** (`Texture.cpp:203-218`), so a texture built that way
reports `getMipLevel() == 0` and `createTextureSampler` gives it
`maxLod = 0.0`. Harmless today — every one of the seven external callers
passes `in_mip_levels = 1` (`DeferredRasterizer.cpp:81`/`:100`,
`PostStage.cpp:174`, `Rasterizer.cpp:301`/`:320`, `Clouds.cpp:33`,
`CascadedShadowMap.cpp:53`, `SkyBox.cpp:112`, `VulkanRenderer.cpp:1269`) and
`maxLod = 0` is correct for one mip — but it is a trap for the first mipped
image created without going through `uploadRgba`. Fold the one-line
assignment into the next change that touches `Texture`. **The PCF radius
slider is 1..20** (`GUI.cpp:199`) and `cascaded_shadow.slang:39-52` loops
`(2r+1)^2` comparison taps, so the maximum is 1681 taps per shadowed
fragment; now that the comparison sampler adds hardware 2x2 filtering the
useful range is much smaller, but narrowing a user-facing slider is an owner
decision, not an agent's. **`Frustum::from_view_proj` plane normalization**
and **`intersects_aabb_as_caster` having no production caller** — batch XII
already rejected both with reasons; they still hold.

## 2026-08-01 batch XIV — planner (refactor: a copy-pasted shader-stage block in six pipelines, forward.rs's texture helpers, model-loading doc drift)

Batch XIII is fully drained (`62e56684`, `24baeeef`, `7b8c3ccd`, `ea464598`,
`3fd2f217`); every checkbox left in the file was `- [b]`. Every `file:line`
below was read out of the tree this pass.

**Task 1: six pipelines each hand-write the same vertex+fragment stage
block, and one of them leaks it across 48 lines.** The sequence "load two
`.spv` into `vk::ShaderModule`, fill two `vk::PipelineShaderStageCreateInfo`
with `pName = "main"`, pack them into a two-element array, build the
pipeline, destroy both modules" appears verbatim at `Rasterizer.cpp:354-370`
+ `:402-403`, `PostStage.cpp:286-300` + `:319-320`,
`DeferredRasterizer.cpp:312-317` + `:348-349` and `:352-357` + `:380-381`,
`SkyBox.cpp:332-345` + `:385-386`, and
`CascadedShadowMap.cpp:335-350` + `:435-436`. Two tells that this is
copy-paste rather than convergent style: `CascadedShadowMap.cpp:350` names
its local `std::array skyStages` — a name that only makes sense in
`SkyBox.cpp:345`, where it was copied from — and `Rasterizer` puts 32 lines
of vertex-input and layout setup between loading its modules and destroying
them, so any early return added in that window leaks two `vk::ShaderModule`.
`Raytracing.cpp` (four modules), `PathTracing.cpp` (one compute module) and
`Clouds.cpp` (compute, already deduped by `9aac4cb2`) are deliberately out of
scope: different stage counts, different shapes.

**Task 2: `forward.rs` carries ~280 lines of texture/sampler code that no
other renderer file needs and no test can currently reach.** `create_sampler`
(`:2977`), `anisotropy_for` (`:3014`), `srgb_to_linear` (`:3022`),
`linear_to_srgb` (`:3031`), `generate_mips` (`:3042`),
`compressed_wgpu_format` (`:3081`), `create_compressed_texture` (`:3098`),
`create_material_texture` (`:3149`), `create_depth_texture` (`:3217`) and
`create_hdr_texture` (`:3236`) form one contiguous block; grepping the crate
confirms **not one of them is referenced outside `forward.rs`** (the four
other `create_sampler` hits in `bloom.rs`/`ibl.rs`/`tonemap.rs` are
`wgpu::Device::create_sampler`, a different function). Three of them —
`srgb_to_linear`, `linear_to_srgb`, `generate_mips` — are pure CPU with zero
wgpu types and **zero tests**, even though `generate_mips` is the box filter
every material texture's whole mip chain is built from and the sRGB pair is
what keeps colour textures from being averaged in the wrong space. The
precedents are `c85ef931` (`render/animation.rs`) and `0378178e`
(`render/bounds.rs`); `render/mod.rs` is the one-line registry.

**Task 3: `docs/model-loading.md` describes a `MeshRange` that no longer
exists and points at a refactor that already landed.** `:71` lists the struct
as six fields, but `scene/MeshRange.ixx:18-30` has a seventh — `bool
doubleSided`, which `uploadParsed` forwards to `add_new_mesh` so glTF
`doubleSided` materials disable back-face culling per mesh. The doc never
mentions that the struct moved into its own `kataglyphis.vulkan.mesh_range`
module (commit `fba308d7`, wave 6) or that the re-basing arithmetic it warns
about at `:111-115` now lives in exactly one shared function,
`sliceMeshRange`, with a `MeshSlice` return type and its own test suite
(`Test/commit/VulkanEngine/meshRangeSliceSuite.cpp`). Worse, `:118-120` still
tells the reader that deduplicating "that loop" is a queued refactor and "a
natural moment" — that IS `fba308d7`. `:36-37` also predates
`scene/ModelFileKind.ixx`, which now owns the extension dispatch that both
`AsyncModelParse.ixx:55` and `Scene.cpp:36` call through
(`isGltfModelPath`).

## 2026-08-01 batch XV — planner (tiled-lighting binning is upside down, an "empty tile" fallback that defeats tiling, an unpinned light-packing layout, a CI fuzz list that cannot say no, stale golden-suite counts)

Batch XIV is fully drained (`3fd2f217`, `f97712f4`, `e76a860f`, `7b38ac3e`);
every checkbox left in the file was `- [b]`. Every `file:line` below was read
out of the tree this pass.

**Tasks 1 and 2 are both in the tiled punctual-lighting path and task 2 is
unsafe before task 1 lands. Do them in order.** Task 1 is a correctness bug
(tiles are indexed upside down); task 2 is the fallback that has been hiding
it, and removing that fallback while the binning is still mirrored would turn
a hidden bug into a visible one.

**Task 1: the CPU tile binning and the shader disagree about which way is
up.** `tile_rect_for_light` projects a light's candidate points and converts
clip space to a screen fraction with `let uv = ndc * 0.5 + 0.5;`
(`tile_grid.rs:79`), then derives tile rows from `uv.y`
(`tile_grid.rs:94`/`:96`). NDC y is **up** (`+1` = top of the viewport), so
`uv.y == 1.0` lands in the LAST tile row. The consumer indexes the same grid
from the fragment's framebuffer position: `uint tileY = uint(fragCoord.y) /
16u` (`forward.slang:293`, `In.svPosition` = `SV_Position` at `:393`/`:120`,
which is `@builtin(position)` in the emitted WGSL — y **down**, `0` = top),
and `uint tileIndex = ty * tileW + tx` (`:300`). So the row a light is written
to is the mirror of the row that reads it; x is fine (NDC `-1` and
`fragCoord.x == 0` are both the left edge). The observable defect needs two
lights at different screen heights: a top-half tile that receives some
*other* light's mirrored entry gets `count > 0`, takes the per-tile list, and
therefore **misses the light actually over it**. None of the six existing
tests in `tile_grid.rs:200-357` can catch this — every one of them puts its
light at the origin dead-centre, uses a directional light, or asserts only
"at least one tile is lit", all of which are symmetric under a vertical flip.

**Task 2: an empty tile iterates every light in the scene, which is the exact
opposite of what tiling is for.** `forward.slang:305` reads
`if (count <= 0 || tileW == 0u) tileCount = totalLights;`. `tileW == 0` is the
documented "the grid was never built" fallback (`tile_grid.rs:8-9` says so),
but `count <= 0` is a *different* fact — "the CPU binned no light into this
tile" — and the shader conflates the two. With `MAX_PUNCTUAL_LIGHTS = 256`
(`forward.rs:32`), every tile the lights do not reach pays a 256-iteration
loop instead of zero, so a scene gets *slower* the more empty screen it has.
It is not a correctness bug today (iterating all lights is the physically
correct answer; binning is the optimisation), which is why nothing has caught
it — and it is also why it silently absorbs task 1's mirroring. Before the
`count <= 0` branch can go, the binning has to actually be conservative, and
today it is not in one case: `tile_rect_for_light` builds its screen AABB from
only those of the seven candidate points with `clip.w > 0.0`
(`tile_grid.rs:74-84`), so a light sphere **straddling the near plane** — centre
in front, part of the sphere behind, or vice versa — gets a footprint computed
from a subset of its extent and can under-cover. That must be fixed in the
same change or task 2 will delete light.

**Task 3: `pack_punctual_lights` is a 4×`vec4`-per-light host/device contract
with zero tests.** `forward.rs:188-222` writes `[pos.xyz, kind]`,
`[colour*intensity, range]`, `[dir.xyz, cos_inner]`, `[cos_outer, 0, 0, 0]`;
`forward.slang:311-314` reads exactly those four rows back and decodes `kind`
with float thresholds (`kind > 2.5` → directional, `kind > 1.5` → spot,
`forward.slang:317`/`:334`). Nothing pins the two together, and the packer is
pure CPU — no wgpu types, no adapter — so there is no reason for it to be
untested. It also feeds task 1's binner, which reads `packed[base][3]` as
`kind` and `packed[base + 1][3]` as `range` (`tile_grid.rs:130-135`), a third
consumer of the same unwritten contract. The extraction precedents are
`c85ef931` (`render/animation.rs`), `0378178e` (`render/bounds.rs`) and
`e76a860f` (`render/texture.rs`); `render/mod.rs` is the one-line registry.

**Task 4: the Windows CI fuzz step re-hand-maintains the list that
`EveryCpuSuiteIsInTheWindowsCiFilter` already exists to stop being
hand-maintained.** `Windows.yml:277` hard-codes
`@('obj_parsing_fuzz_test','gltf_parsing_fuzz_test','scene_config_fuzz_test','shader_file_reader_fuzz_test','texture_loading_fuzz_test')`
while the targets themselves are declared by seven
`kataglyphis_add_fuzz_test(...)` calls in `Test/fuzz/CMakeLists.txt:106-164` —
a new fuzz target is silently not run in CI, which is the same drift class
`BuildIntegrity.EveryCpuSuiteIsInTheWindowsCiFilter`
(`buildIntegritySuite.cpp:682`) was written for. Worse, `Windows.yml:279`
reads `if (-not (Test-Path $exe)) { Write-Host ('missing ' + ...); continue }`
— a fuzz target that stops being built makes the step print one line and pass.
The step only runs after a successful `clangcl-debug` build in which fuzzing
mode is on, so a missing executable is a build regression, not an
environment difference: this is another instance of the probe-that-cannot-say-no
that `ea464598` just removed from the GPU check three steps below it.

**Task 5: the golden-suite counts are stale in four places and one of them
contradicts a line 75 lines further down the same file.**
`docs/gpu-golden-testing.md:46-47` says "As of 2026-07-23 the baseline is 19
`GoldenRender` + 2 `Integration` tests, ~44 s", while `:121-122` of the same
document records a 2026-08-01 run of "28 tests from 2 test suites" with three
excluded by name. The tree says 30 `TEST(GoldenRender...)` in
`goldenRenderSuite.cpp` (one of them `DISABLED_DumpsFrameToPng`) plus
`Integration.RenderModesSelectableInGui` and `Integration.VulkanEngine`, i.e.
**29 runnable `GoldenRender` + 2 `Integration` = 31**, and 31 − 3 excluded = the
28 the later line reports. So the later line is right and the earlier one is
stale by ten tests. BACKLOG repeats the old figure three more times
(`:258` "22 GPU tests", `:372` "all 19-21 GPU tests passing", `:626`/`:639`
"22 golden"). Separately, BACKLOG `:709-714` still calls
`an_occluded_primitive_is_skipped_with_gpu_culling` and
`two_visible_cubes_are_both_drawn_with_gpu_culling` "Still red" while
`:3001-3007` in the same file records both as passing under `1594a4a0` and
prunes the entry that owned them.

Candidates found but NOT tasked (checked, then rejected — do not re-propose
without new evidence): **`CascadedShadowMap::recordCommands` binds the wrong
set on an unreachable path** (`CascadedShadowMap.cpp:483-491`) — when
`descriptorSets` is empty it binds `descriptorSet` (built from the *light*
layout) at set 0, which is layout-incompatible with the pipeline layout's set
0; the only caller (`VulkanRenderer.cpp:956`) always passes a non-empty span,
so this is dead defensive code, not a live defect. **`generate_mips` underflows
on a zero-dimension texture** (`texture.rs:86-88`, `pw - 1` on `pw == 0`) —
unreachable, every `CpuTexture` comes from a successful decode. **`Texture::
createImage` never assigns `this->mip_levels`** — batch XIII already rejected
this with reasons that still hold; fold the one-liner into the next change
that touches `Texture`.

### Rust WebGPU renderer (`third_party/OxidANT`)

- [b] **Stop an empty tile from iterating every light in the scene** —
  **step 1 done (2026-08-01), step 2 blocked on a deeper, newly-found gap.**
  `tile_rect_for_light` (`tile_grid.rs:47`) now returns the whole grid when a
  light's seven candidate points straddle the near plane (some `clip.w > 0`,
  some `<= 0`), same as the directional fallback — the case the original task
  described. New test `a_light_straddling_the_near_plane_covers_the_whole_grid`
  pins it (camera `(0,0,5)`, point light at `(0,0,4.9)` range `2.0`, every tile
  must list it).

  **The shader's `count <= 0 || tileW == 0u` fallback in
  `forward.slang`'s `punctual_lighting` was deliberately NOT removed.** Only
  the safe half of step 3 (moving the `tileLightGrid` read so it does not fire
  on the `tileW == 0` path) was applied; the `count <= 0` → iterate-everything
  branch stays exactly as before.

  **Why: the "genuinely conservative" premise was wrong in a second, larger
  way the original task text did not anticipate.** A completeness test
  (`build_tile_light_grid_is_complete_for_random_lights` — sample many points
  on each light's actual sphere surface, project each independently of
  `tile_rect_for_light`, assert the grid lists the light for every tile a
  sample lands in) was written per the task's step 4 and **failed on lights
  with no near-plane involvement at all**. Root cause, confirmed by direct
  calculation: the 7-candidate AABB (`pos` plus `pos ± range * axis`) is not
  a bound on the sphere's true screen-space silhouette even when every
  candidate is in front of the camera. The silhouette's tangent points
  combine a lateral *and* a depth-axis offset — no single axis-aligned
  candidate sits there — and the true extent exceeds the naive one by a
  factor of `D / sqrt(D² - r²)` (`D` = eye-to-centre distance, `r` = range),
  worse as range grows relative to distance. Repro: light at
  `pos=(1.13,-2.07,0.77)`, `range=0.74` (`D≈4.84`, so `r/D≈0.15`, a small
  ratio) still under-covered by a full tile row. This is documented in
  `tile_grid.rs`'s `tile_rect_for_light` doc comment now.

  A uniform scalar inflation using that factor was considered and rejected:
  it is only exactly correct when the light lies on the view axis (the
  aligned 2D case verified above) — for a light off to the side, on an
  asymmetric or non-square-aspect projection, the correct bound is
  direction-dependent and needs a projection-matrix-aware formula (see Mara &
  McGuire, "2D Polyhedral Bounds of a Clipped, Perspective-Projected 3D
  Sphere", 2013). Shipping the scalar-inflation shortcut would have looked
  fixed while still under-covering in untested configurations — the same
  "green because untested" trap this task's own context note warned about,
  one level deeper. `build_tile_light_grid_is_complete_for_random_lights` was
  therefore **not** added to the tree (it would either fail or need range
  bounds tight enough to be dishonest about what it tests); the straddle test
  is the only one that survived.

  **Next step for whoever picks this up**: implement a real
  projection-matrix-aware sphere screen-bound (Mara & McGuire or equivalent),
  re-add the completeness test with unconstrained random ranges, confirm it
  passes, *then* remove the shader's `count <= 0` fallback. Until that
  lands, the fallback is load-bearing correctness, not just a performance
  safety net — do not remove it as a drive-by.

  Verified: `cargo test --workspace --locked` from
  `third_party/OxidANT` (all `tile_grid` tests green;
  the pre-existing `auto_exposure_brightens_a_dark_scene_over_successive_frames`
  and the 11 `ibl` test failures reproduce identically on unmodified `develop`,
  confirmed by stashing this change — unrelated headless-GPU-adapter issues in
  this environment, not caused by this change). Shaders recompiled
  (`compile-slang-shaders.ps1`); `clangcl-debug` container build green;
  `commitTestSuite.exe --gtest_filter='BuildIntegrity.*'` shows the
  shader-staleness/no-hand-edit gates (`CompiledShadersAreNotOlderThanTheirSources`,
  `CheckedInWgslIsNotOlderThanItsSlangSource`, `CheckedInWgslHasNoHandEdits`)
  green. Two unrelated, pre-existing `BuildIntegrity` failures surfaced in the
  same run and are NOT caused by this change (confirmed by file scope —
  neither touches anything this change edited):
  `NoShaderRedeclaresTheCascadeCount` (`forward.slang:15`'s
  `CASCADE_COUNT`, untouched by this change) and `VulkanCreationResultsAreChecked`
  (`Clouds.cpp:227`, `ShaderHelper.cpp:38` — files this change never opened).
  Worth their own task. The full `commitTestSuite.exe` GPU golden suite was
  also tried and failed broadly with "No synchronization frames available" —
  looks like GPU/session contention from the three build containers already
  running in this environment (matches the known "swapchain reads black
  while the session is locked" papercut), not a rendering regression; the
  task's own build instructions only called for the `BuildIntegrity.*` filter,
  which is green.

## 2026-08-01 batch XVI — planner (two RED BuildIntegrity gates that fail Windows CI, a ray-tracing SBT that reads past its miss region, an unpinned manifest pair, unpinned Rust/Slang constants)

Batch XV is fully drained (`8b28543c`, `cb669253`, `3a0a49c7`, `c9e920b7`,
`e2767bb1`); every checkbox left in the file before this batch was `- [b]`.
Every `file:line` below was read out of the tree this pass.

**Tasks 1 and 2 are the priority: both are `BuildIntegrity` tests that are RED
right now, and `'BuildIntegrity.*'` is in the Windows CI filter
(`Windows.yml:210`), so any `[build-win]` commit fails the Test step.** Batch
XV's executor notes spotted both as "unrelated, pre-existing failures … worth
their own task" and correctly did not chase them mid-task; this is that task.
Neither needs a GPU — they are pure file-scanning CPU gtests.

**Task 1 is a stale line-number allowlist, not a real unchecked result.**
`VulkanCreationResultsAreChecked` (`buildIntegritySuite.cpp:1386`) exempts three
`.value` reads via `kCheckedResultAllowlist` (`:1312-1327`), keyed on
`{relative file, 1-based line}`. Two of the three line numbers have drifted out
from under it: the allowlist says `vulkan_base/ShaderHelper.cpp` **37** but the
`.value` read is now at **38**, and `scene/atmospheric_effects/clouds/Clouds.cpp`
**249** but the read is now at **227**. (`vulkan_base/VulkanDevice.cpp` 231 is
still correct.) Both written justifications still hold verbatim — I read both
sites: `ShaderHelper.cpp:30-38` still does `spdlog::critical` + `std::abort()`
above the read, and `Clouds.cpp:222-227` still logs and `return`s on failure
rather than aborting. So the test is reporting a bookkeeping drift as a
correctness gap, which is exactly the failure mode that trains people to ignore
a red gate. A line-number key cannot survive an edit anywhere above it in the
file; the fix is to anchor the exemption to something that moves *with* the
code.

**Task 2 is a scope bug: the gate scans a shader the Vulkan engine does not
consume.** `NoShaderRedeclaresTheCascadeCount` (`:1094`) walks every `.slang`
under `Resources/ShadersSlang/` outside `build/`, and fails on any
`static const int <name containing "cascade"> = <MAX_CASCADES>`. It fires on
`Resources/ShadersSlang/forward/forward.slang:15`
(`static const int CASCADE_COUNT = 3;`). That constant is **not** a stale copy of
the C++ engine's `MAX_CASCADES` — `forward/forward.slang` is the Rust/WebGPU
forward shader, WGSL-emit only (`compile-slang-shaders.ps1` /
`.sh` list it with `Targets = wgsl` only), and no `forward.*.spv` appears
anywhere in the engine's SPIR-V load list (checked: the engine loads
`rasterizer.*`, `deferred.*`, `shadow_map.*`, `skybox.*`, `post.*`,
`raytrace.*`, `shadow.rmiss.*`, `path_tracing.*`, `compute/*` and nothing else).
Its owner is `crates/webgpu_renderer/src/render/forward.rs:43`
(`pub const CASCADE_COUNT: usize = 3;`), and the shader's own comment at `:13`
says so. The two constants are equal at 3 by coincidence of both renderers
choosing three cascades. Raising the C++ `MAX_CASCADES` cannot make
`forward.slang` stale, which is the only thing this gate is for. Widening the
gate to "any cascade-ish 3 anywhere" made it fire on a shader it has no claim
over.

**Task 3 is a real Vulkan spec violation in the ray-tracing shader binding
table, latent on this GPU and wrong on a device with
`shaderGroupHandleAlignment > shaderGroupHandleSize`.** Three coupled defects in
`Raytracing.cpp`, all verified by reading `createSBT` (`:289-334`),
`recordCommands` (`:45-73`), the group table (`:210-250`) and the two
`TraceRay` call sites. The pipeline has four groups — 0 raygen, **1 and 2 both
miss** (`rmiss_main`, `shadow_rmiss_main`), 3 triangles-hit — and
`raytrace.rchit.slang:98-103` traces its shadow ray with **miss index 1**
(`0u, 0u, 1u,   // sbt offset, stride, miss index (shadow miss)`), while
`raytrace.rgen.slang:36-43` uses miss index 0. But `recordCommands` sets
`miss_region.stride = handle_size_aligned; miss_region.size =
handle_size_aligned;` (`:65-66`) — a region declared to hold **one** record,
from which the shader reads record **1**. Separately, `createSBT` copies handles
out of the `getRayTracingShaderGroupHandlesKHR` blob at the *aligned* stride
(`handles.data() + handle_size_aligned`, `handles.data() + handle_size_aligned *
3`, `:332-333`), but that call writes handles **tightly packed at
`shaderGroupHandleSize`** — the aligned stride is the SBT's *device-side* record
stride, not the blob's. And the two miss handles are then written back-to-back
at `handle_size` stride (`memcpy(mapped_miss, …, handle_size * 2)`) into a
buffer allocated `2 * handle_size` (`:319`), while the device reads them at
`handle_size_aligned` stride. All three are identities when
`shaderGroupHandleAlignment == shaderGroupHandleSize` (32/32 on the RX 9070 XT
and most desktop drivers), which is why every RT golden test passes today.

**Task 4 pins a manifest pair that has not drifted yet but has no gate**, and
task 5 pins two Rust/Slang constants that do. Both follow precedents already in
the tree (`SlangWgslPatchTablesAgree`, `EveryFuzzTargetIsInTheWindowsCiFuzzList`,
and batch XV's `render/lights.rs` layout pin).

Candidates found but NOT tasked (checked, then rejected — do not re-propose
without new evidence): **the Windows and Linux Slang manifests currently
disagree on `histogram/histogram.slang`** — false alarm, that row is *commented
out* in `compile-slang-shaders.ps1:109` (the hand-written `histogram.wgsl`
documented in AGENTS.md); the 49 live rows match exactly, which is why task 4 is
written as a preventive gate and not a bug fix. Note the same comment trap for
whoever implements it. **The GUI `# cascades` slider advertising 1..8** — stale
backlog claim, `GUI.cpp:196` already reads
`ImGui::SliderInt("# cascades", …, 1, MAX_CASCADES)`. **`Clouds::
recordComputeCommands` indexes `descriptorSets[0]` with no emptiness check**
(`Clouds.cpp:246`) — same dead-defensive-path class batch XV already rejected for
`CascadedShadowMap::recordCommands`; the only caller always passes a non-empty
span.

### C++ Vulkan engine

- [b] **(M) Fix the ray-tracing SBT: the miss region declares one record while the shadow ray reads record 1, and handle offsets assume the device stride** — a spec violation that is invisible only because `shaderGroupHandleAlignment == shaderGroupHandleSize` on this GPU.

  **Status (2026-08-01): code fix implemented and committed, GPU verification blocked.**
  All steps 1-5 below are done: `sbt_handle_source_offset`/`sbt_record_offset`
  extracted into `MemoryHelper.hpp`, `createSBT` fixed to source at
  packed `handle_size` and write records at aligned `handle_size_aligned`
  offsets, `miss_region.size` widened to `2 * handle_size_aligned`, and the
  two new `MemoryHelperUnit` CPU tests (plus a no-op-on-this-hardware
  regression test) added and green in the `clangcl-debug` container build.
  **GPU-verify could not be run**: the host RDP session (`jonas`, session 2)
  is currently disconnected, and *every* `GoldenRender.*` test fails
  identically — including `GoldenRender.RendersNonBlankFrame`, which has
  nothing to do with ray tracing — with `No synchronization frames
  available; skipping draw frame.` from frame 0 and `frame.empty() == true`.
  This reproduces with or without this change (confirmed via baseline test)
  and persists across repeated runs and a 20s wait, so it is not the known
  `PathTracingAccumulatesAndConverges` device-lost issue recovering slowly —
  it looks like swapchain presentation is unavailable to a disconnected
  session on this box. `tscon 2 /dest:console` failed with access denied
  (needs admin). Next executor with a live/attached GPU session: rerun
  `.\commitTestSuite.exe --gtest_filter='GoldenRender.Raytrac*:GoldenRender.*Raytraced*:GoldenRender.PathTracing*:GoldenRender.AddedModelAppearsInPathTracing'`
  and `Run-SyncValidation.ps1` from repo root to close this out, then delete
  this entry.

  **Files to read:**
  - `Src/GraphicsEngineVulkan/renderer/Raytracing.cpp:289-334` (`createSBT`),
    `:45-73` (the three `vk::StridedDeviceAddressRegionKHR` set-ups in
    `recordCommands`), `:210-250` (the four-group table: 0 raygen, 1 miss,
    2 shadow miss, 3 triangles-hit).
  - `Resources/ShadersSlang/raytracing/raytrace.rchit.slang:98-103` — the shadow
    `TraceRay` with **miss index 1**; `raytrace.rgen.slang:36-43` — miss index 0.
  - `Src/GraphicsEngineVulkan/common/MemoryHelper.hpp:6` — `align_up`, and
    `Test/commit/VulkanEngine/memoryHelperSuite.cpp` for the existing pure-CPU
    test pattern to extend.
  - `Src/GraphicsEngineVulkan/vulkan_base/VulkanDevice.cpp:560-580` — where
    `shaderGroupBaseAlignment`/`shaderGroupHandleAlignment` are already read and
    logged at device selection.

  **Steps:**
  1. **Source offsets.** `getRayTracingShaderGroupHandlesKHR` writes handles
     tightly packed at `shaderGroupHandleSize`. Change the three `memcpy` sources
     at `:331-333` to index by `handle_size`, not `handle_size_aligned`
     (group 0 at `0`, groups 1-2 at `handle_size`/`2 * handle_size`, group 3 at
     `3 * handle_size`).
  2. **Destination layout.** Size the three SBT buffers in units of
     `handle_size_aligned`, not `handle_size` (`:317-322`): raygen
     `handle_size_aligned`, miss `2 * handle_size_aligned`, hit
     `handle_size_aligned`. Copy each record into its own aligned slot — the two
     miss handles must land at offsets `0` and `handle_size_aligned`, replacing
     the single `handle_size * 2` block copy.
  3. **Region sizes.** In `recordCommands`, set
     `miss_region.size = 2 * handle_size_aligned` (stride stays
     `handle_size_aligned`) so the declared region covers the miss index 1 the
     closest-hit shader actually uses. Leave raygen and hit at
     `size == stride == handle_size_aligned` (the spec requires
     `size == stride` for the raygen region specifically).
  4. **Make the arithmetic testable.** Extract the two offset formulas as
     `constexpr` free functions next to `align_up` in
     `common/MemoryHelper.hpp` — e.g.
     `sbt_handle_source_offset(groupIndex, handleSize)` and
     `sbt_record_offset(recordIndex, handleSizeAligned)` — and call them from
     `createSBT` instead of inline multiplications. Keep them header-only and
     free of Vulkan types so the headless test binary can use them.
  5. Add a short comment at `createSBT` recording *why* the two strides differ
     (blob is packed, device records are aligned) — that distinction is the whole
     bug and the next reader will otherwise re-collapse them.

  **Test:** Add to `Test/commit/VulkanEngine/memoryHelperSuite.cpp` (suite name
  is **`MemoryHelperUnit`**, already in the Windows CI filter at
  `Windows.yml:239` — no workflow edit needed):
  `MemoryHelperUnit.SbtSourceOffsetsArePackedAtHandleSize` and
  `MemoryHelperUnit.SbtRecordOffsetsUseTheAlignedStride`, both exercising the
  **`handleSize = 32`, `alignment = 64`** case (so `aligned = 64 != 32`) where
  every current formula is wrong: source offsets must be `0, 32, 64, 96`, record
  offsets `0, 64`. Also assert the degenerate `alignment == handleSize` case
  still yields today's values, so the change is provably a no-op on this
  hardware. These are pure CPU. If you do add a new suite name,
  `BuildIntegrity.EveryCpuSuiteIsInTheWindowsCiFilter` will tell you to register
  it.

  **Build:** `clangcl-debug` for the CPU tests. **Then GPU-verify on the host RX
  9070 XT** — this is a render-path change and the CPU tests cannot see it:
  `.\commitTestSuite.exe --gtest_filter='GoldenRender.Raytrac*:GoldenRender.*Raytraced*:GoldenRender.PathTracing*:GoldenRender.AddedModelAppearsInPathTracing'`
  from the repo root (`RaytracedWorldFollowsTheModelTransform`,
  `RaytracingFrameSkipsTheRasterPass`, `RaytracedLargeMeshDoesNotLoseTheDevice`,
  and the five path-tracing tests). A wrong handle offset shows up as the wrong
  shader being invoked — expect visibly broken RT output, not a subtle shift, if
  step 1 or 2 is done wrong. Also run
  `pwsh -ExecutionPolicy Bypass -File .\scripts\windows\Run-SyncValidation.ps1`;
  the validation layers check SBT region sizes against the miss indices used, so
  step 3 is directly observable there.

  **Context:** See `docs/gpu-golden-testing.md` for the host verification loop
  and `[[host-gpu-golden-verification]]`. This is deliberately three fixes in one
  change because they are one bug — the code conflates the packed *source* stride
  with the aligned *device record* stride, and fixing either half alone leaves the
  SBT self-inconsistent. Do not "simplify" by asserting
  `shaderGroupHandleAlignment == shaderGroupHandleSize`: that is exactly the
  portability assumption being removed, and the RX 9070 XT would never fail the
  assert.

### Build / scripts

### Rust WebGPU renderer (`third_party/OxidANT`)

## 2026-08-01 batch XVII — planner (a dangling pointer in swapchain creation, an incomplete blit-capability gate, three coverage gaps)

The actionable queue was empty again: the only checkboxes left in the whole file
are `- [b]` entries, and batch XVI is drained (`e8b1db52`, `f1a67217`,
`74aaee23`, `302faa90`, `ee4abb24`). Every `file:line` below was read out of the
tree this pass.

**The headline finding is task 1 and it is undefined behaviour, not a style
nit.** `VulkanSwapChain::initVulkanContext` declares
`uint32_t queue_family_indices[]` **inside** the
`if (indices.graphics_family != indices.presentation_family)` block
(`VulkanSwapChain.cpp:95-96`), stores its address into
`swap_chain_create_info.pQueueFamilyIndices` at `:100`, and the block closes at
`:101`. `createSwapchainKHR` reads that pointer at `:117` — sixteen lines after
the array's lifetime ended. It is invisible here for one reason only: on this
box `graphics_family == presentation_family`, so the branch never executes and
the `else` at `:102-105` writes `nullptr`/count 0. On a device that presents
from a different queue family the driver reads freed stack. This is the same
shape as the SBT stride bug batch XVI fixed — correct-by-luck on one machine,
wrong by spec everywhere.

Two smaller real gaps in the same pass. `Texture.cpp`'s `supportsLinearBlit`
(`:59-64`) tests only `eSampledImageFilterLinear`, but `vkCmdBlitImage` also
requires `eBlitSrc` on the source and `eBlitDst` on the destination — and since
`c80e7503` deleted the (then-unreachable) in-loop fallback,
`supportsLinearBlit` is the **only** gate left in front of
`generateMipMaps`, so an incomplete check now means no check at all on a device
that lacks those bits. And `AsyncModelParse` — the engine's only threaded class,
driven every time the GUI loads a model — has **zero** tests, on a platform with
no ThreadSanitizer (AGENTS.md § "There is no Windows ThreadSanitizer").

Candidates found but NOT tasked (checked, then rejected — do not re-propose
without new evidence): **extracting the eight bind-group-layout and four
pipeline-layout descriptors out of `ForwardRenderer::new`**
(`forward.rs:431-745`, ~330 lines of inline descriptor construction) — this is
pure code motion, which batch XIII already rejected for the same file and the
same reason (the animation samplers); **`map_format` collapsing `*_SRGB_BLOCK`
onto the Unorm `CompressedFormat`** is not itself a bug — `compressed_wgpu_format`
(`render/texture.rs:111-125`) picks the sRGB wgpu variant from the *usage* flag,
which is what glTF specifies; what is missing is a diagnostic when the container
disagrees, which is task 5 and deliberately not a behaviour change;
**`docs/gpu-golden-testing.md:47-48` vs `:122-123`** currently agree
(30 defined − 1 `DISABLED_` = 29 runnable, + 2 `Integration` = 31 total, minus
the 3 named exclusions = 28) — task 4 is therefore a preventive gate, not a
drift fix.

### C++ Vulkan engine

### Rust WebGPU renderer (`third_party/OxidANT`)

## 2026-08-01 batch XVIII — planner (refactor: a "kept in lockstep" guard that is written twice, untested texture-slot flattening, a depth-aspect rule with four hand-rolled copies)

Batch XVII is fully drained (`ad836844`, `0705aa7f`, `2df83793`, `8e0d8294`,
`73192584`); every checkbox left in the file before this batch was `- [b]`.
Every `file:line` below was read out of the tree this pass.

**All three tasks are refactors and all three are pure-CPU verifiable.** Tasks 1
and 2 both edit `renderer/VulkanRenderer.cpp`, so do them in either order but
not concurrently.

**Task 1 is duplication the code itself already flags as dangerous.**
`VulkanRenderer.ixx:174-180` documents `raytracingOwnsFrame` as "the single
predicate deciding whether `recordRaytracingOrPathTracing` will actually
dispatch this frame … if they do [disagree], a frame renders nothing" — and yet
`recordRaytracingOrPathTracing` does **not call it**: it re-writes the same
three-term condition inline at `VulkanRenderer.cpp:1082-1083`, with its own
copy of the rationale comment at `:1078-1081` and a second copy at `:1064-1068`.
Separately, the three descriptor writes that bind TLAS / output image /
accumulation image exist verbatim twice — `:798-801` (per-image, from the frame
path) and `:1368-1371` (inside the all-images loop). `update_raytracing_
descriptor_set` is also declared **public** (`VulkanRenderer.ixx:81`) while its
only caller in the tree is `VulkanRenderer.cpp:547`, inside the class.

**Task 2 is the largest block of untested pure logic left in the renderer.**
`updateTexturesInSharedRenderDescriptorSet` (`VulkanRenderer.cpp:1518-1592`)
mixes four Vulkan writes with ~45 lines of index arithmetic — flatten every
model's textures in model order, stop at `MAX_TEXTURE_COUNT`, pad the remaining
fixed-size slots with slot 0 — and that arithmetic must agree slot-for-slot
with `assignTextureOffsets` (`scene/ObjectDescription.ixx:21-36`), which stamps
the `texture_offset` the shaders add to model-local `textureID`s. If they ever
disagree, every model past the first samples the wrong images; that is the
exact bug the offsets were introduced to fix (`ObjectDescription.hpp:14-19`).
`assignTextureOffsets` has three CPU tests
(`Test/commit/VulkanEngine/objectDescriptionOffsetsSuite.cpp`); its counterpart
has **none**, because it is welded to `DescriptorSetGroup::writeImageArray`.
The extraction also fixes a real (minor) reporting defect: the exhaustion
warning at `:1558-1563` reports `image_info_textures.size() + (model_texture_
count - t)`, which counts only up to the model that overflowed and **ignores
every later model's textures** — so the number it prints is not the total the
scene asked for, which is the one number the message exists to give you.

**Task 3 is one rule with four hand-rolled implementations and no test.**
Every depth image in the engine takes its format from
`chooseDepthFormat` (`common/FormatHelper.hpp:43-49`), whose preference list
contains two combined depth/stencil formats. Deriving the aspect mask from that
format is then done four different ways: `Rasterizer.cpp:30-35` has a private
anonymous-namespace `hasStencilComponent` and builds `eDepth | eStencil`
(`:329-341`, correct — that view is also the subject of a layout transition,
where a combined format must carry both aspects), while
`DeferredRasterizer.cpp:99`, `PostStage.cpp:183` and
`CascadedShadowMap.cpp:56`/`:181` each hard-code `eDepth`. Two of those
hard-codes are **required** to stay single-aspect and must not be "fixed"
(details in the task). Nothing states the rule in one place and nothing tests
it, so the next depth image added is a coin flip.

Found, real, and deliberately **deferred to a later batch** (not rejected — do
not re-derive, just pick up): (a) the two cloud-output `vk::ImageMemoryBarrier`
blocks at `VulkanRenderer.cpp:876-894` and `:908-926` are 20 lines of identical
field setup differing only in stage/access masks, and the ~19-line rationale at
`:928-936` **repeats `:869-874` almost verbatim** — only the 2026-08-01
measurement note at `:937-946` is unique to it; (b)
`DeferredRasterizer.cpp:194-200` lists six attachments including "1: Position",
but the code builds five and has no position attachment — `:87-90` in the same
file explains at length that it was removed. Both are small, both are real
duplication/drift.

### C++ Vulkan engine

### Rust WebGPU renderer (`third_party/OxidANT`)

## 2026-08-02 batch — planner (a second vertical mirror, this one in GPU culling; a mirrored world-position reconstruction in the deferred lighting pass; the two comment/duplication items batch XVIII deferred)

Batch XVIII is fully drained (`a3b42dfc`, `21b263e0`, `a1cbbdfd`); every
checkbox left in the file before this batch was `- [b]`. Every `file:line`
below was read out of the tree this pass.

**The headline is that the vertical-mirror bug class fixed in `8b28543c`
(tile-light binning) has two more instances, in two different renderers, and
both are invisible to the tests that cover them because the test scenes are
vertically symmetric.** The two renderers use *opposite* NDC-y conventions and
nothing in the tree writes that down:

| | NDC y | texel/uv v from ndc.y | canonical site |
| --- | --- | --- | --- |
| Vulkan (C++) | +1 = **bottom** | `v = (ndc.y + 1) * 0.5` | `compute/clouds.slang:112-113` |
| WebGPU (Rust) | +1 = **top** | `v = 0.5 - ndc.y * 0.5` | `render/tile_grid.rs:108`, `ssao/ssao.slang:42` |

Task 1 is `gpu_cull/gpu_cull.slang:61` using the **Vulkan** formula
(`float2 uv = ndc * 0.5 + 0.5`) in a shader that is compiled for **WGSL only**
(`compile-slang-shaders.ps1:104`, `Targets = @('wgsl')`) and used only by the
Rust renderer. Every AABB corner therefore lands in the vertically mirrored
half of the depth buffer, so the 8×8 depth-tap rect at `:87` reads occluders
from the wrong place: a primitive occluded at the top of the screen is tested
against whatever is at the bottom. The rewrite in batch VI made the *test*
conservative and correct (`aabbNear > maxSampled`, all eight corners) but
carried this line over unchanged. `tile_grid.rs:108` is the same computation
written the right way, three files away, and was fixed for exactly this reason
in `8b28543c`.

Task 2 is the same class in the C++ deferred lighting pass, and the same shape
as batch XVIII's depth-aspect finding — one rule, several hand-rolled copies,
one of them wrong. `common/fullscreen.slang:25` defines the engine's fullscreen
uv as `float2((x + 1.0) * 0.5, 1.0 - (y + 1.0) * 0.5)`; `ssao/ssao.slang:42`
inverts it correctly (`ndc.y = 1.0 - uv.y * 2.0`); `deferred/deferred.slang`
**re-declares both** — its own `LightingVsOut` (`:87-91`) and
`lighting_vs_main` (`:93-102`) are byte-for-byte `fullscreen.slang`'s, and then
`:110` inverts with `In.uv * 2.0 - 1.0` applied to *both* components. For y that
is not the inverse: substituting the vs mapping gives `clipPos.y = -ndc.y`, so
every reconstructed `worldPos` is mirrored about the horizontal centreline. That
feeds `V` (`:120`) and `calc_cascaded_shadow` (`:132`) — deferred shadows land in
the wrong half of the frame, at `cascaded_shadow_intensity = 0.65`
(`GUISceneSharedVars.ixx:20`).

**Task 2 also has to resolve a contradiction that its own oracle exposes.**
`goldenRenderSuite.cpp:1036` asserts forward-vs-deferred mean absolute channel
difference `< 1.0`, with `:1027-1028` recording `~0.2` measured on this rig. That
number cannot be right for these two shaders even *without* the mirror: forward
(`forward/forward.slang:401-423`) adds IBL or hemisphere ambient, tiled punctual
lights, emissive and occlusion on top of the directional BRDF, while deferred
(`deferred/deferred.slang:126-133`) computes the directional BRDF and the cascade
shadow and nothing else. Two images 0.2/255 apart are the same image. Either the
capture is still not deferred (`:999-1004` records that exact failure happening
once already, fixed by `handleRasterizationModeChange`,
`VulkanRenderer.cpp:277-294`) or the recorded measurement is stale. Task 2 makes
the test say which.

Tasks 3-5 are the small refactors. Task 3 is the consolidation half of task 2 and
must land **after** it (both edit `deferred.slang`). Tasks 4 and 5 are the two
items batch XVIII found, verified and explicitly deferred — both re-confirmed
present this pass, so pick them up rather than re-deriving them.

Numbering used throughout this preamble and inside the entries (the checkboxes
below are split across two sections, so they are not in this order):
**1** = `gpu_cull.slang` mirror (Rust section);
**2** = deferred lighting mirror + parity oracle;
**3** = one fullscreen uv↔NDC definition;
**4** = cloud-output barriers;
**5** = `DeferredRasterizer` attachment comment.

Candidates found but NOT tasked (checked, then rejected — do not re-propose
without new evidence): **extracting the free pipeline/bind-group constructors at
`forward.rs:2591-2943` into a `render/pipelines.rs`** — this is the third time
this file has offered pure code motion and batches XIII and XVII both rejected it
for the same reason; **`GpuCulling`'s readback ring**
(`gpu_occlusion.rs:300-383`) — `free_slot()` scans from `current` and `end_frame`
advances past the slot it just mapped, so the two agree, and the `generation`
guard correctly discards cross-scene results; **`bloom.rs`/`tonemap.rs`/`ssao.rs`
having zero tests** — real, but each is a thin wgpu-object wrapper whose only
CPU logic is `(width / 2).max(1)` (`bloom.rs:112`), which is not worth a suite;
**`DeferredRasterizer.cpp:352`'s missing push-constant range on the lighting
pipeline layout** — `lighting_fs_main` never references the module-scope
`pc_raster` (`deferred.slang:11`), so Slang does not emit it into that entry
point and the layout is correct; verifying it needs the compiled SPIR-V, which
is gitignored.

### C++ Vulkan engine

- [b] **(L) Fix the vertical mirror in the deferred lighting pass's world-position reconstruction, and make the forward/deferred parity oracle able to tell the two paths apart** — deferred shadows are sampled from the mirrored half of the frame, and the test that should have caught it currently claims the two paths agree to 0.2/255.

  **BLOCKED 2026-08-02:** Step 1 requires measuring
  `GoldenRender.DeferredMatchesForwardRoughly`'s current mean-abs-diff on the
  host GPU before touching the shader, and step 5/6 require GPU pixel capture
  to add and validate the new oracle. Host GPU golden verification is
  currently broken in this RDP session (see memory
  `host-gpu-golden-verification`, entries from 2026-08-01): every
  `GoldenRender.*` test fails immediately with `No synchronization frames
  available; skipping draw frame.` (`frameSync.frameSyncCount() == 0`, i.e.
  the swapchain came back with zero images), reproduced here with `quser`
  showing the RDP session as Active (`jonas`, session 2). This is
  environmental, not a code regression — `GoldenRender.RendersNonBlankFrame`
  (untouched) fails identically. Re-attempt once a console/physical session
  restores swapchain image counts; do not chase this further from a headless
  executor run per that memory's guidance ("don't burn more than one retry").

  **Files to read:**
  - `Resources/ShadersSlang/deferred/deferred.slang:87-134` — `LightingVsOut`,
    `lighting_vs_main`, `lighting_fs_main`. `:100` writes the uv, `:110` inverts it.
  - `Resources/ShadersSlang/ssao/ssao.slang:40-46` — `view_pos_at`, the correct
    inverse of the same uv mapping. Copy its y term.
  - `Resources/ShadersSlang/common/fullscreen.slang:11-27` — where the uv
    convention is defined and documented ("UV origin is top-left (y flipped)").
  - `Resources/ShadersSlang/forward/forward.slang:394-423` — the term list the
    forward path actually computes, for the parity discussion below.
  - `Src/GraphicsEngineVulkan/renderer/VulkanRenderer.cpp:169`,`:198-199` —
    `projection[1][1] *= -1` then `inv_projection = inverse(projection)`, i.e.
    `inv_projection` expects the fragment's **true** Vulkan NDC, whatever the
    projection is. This is why feeding `-ndc.y` is wrong independently of the
    projection convention.
  - `Test/commit/VulkanEngine/goldenRenderSuite.cpp:947-1039` —
    `GoldenRender.DeferredMatchesForwardRoughly`, the test to strengthen.
  - `Test/commit/VulkanEngine/goldenRenderSuite.cpp:900-945` — the
    shadow-direction test immediately above it; its "shadowed-pixel union" +
    `moved_fraction` construction is the pattern to follow for the new oracle.

  **Steps:**
  1. **Measure before touching anything.** Build `clangcl-debug`, copy
     `commitTestSuite.exe` to the host, and run
     `--gtest_filter='GoldenRender.DeferredMatchesForwardRoughly'` from the repo
     root on the GPU. Record the `deferred-vs-forward mean abs channel diff`
     line it logs. This number decides step 2.
  2. If the measured diff is still ≈0.2, the deferred capture is **not
     deferred** — find out why before changing the shader (start at
     `VulkanRenderer.cpp:277-294`: confirm `handleRasterizationModeChange` runs
     on the headless harness's frame path at all, and that the post stage's
     input descriptor follows the mode). Fix that first; the rest of this task
     is meaningless until the test compares two different images.
  3. Fix the mirror: replace `deferred.slang:110` with
     `float4 clipPos = float4(In.uv.x * 2.0 - 1.0, 1.0 - In.uv.y * 2.0, depth, 1.0);`
     Add a one-line comment pointing at `ssao.slang:42` as the shared inverse.
     Do **not** touch `lighting_vs_main` in this task — that is task 3.
  4. Recompile shaders (`scripts/windows/compile-slang-shaders.ps1`). `deferred`
     is SPIR-V-only (`compile-slang-shaders.ps1:72-75`), so no WGSL is affected.
  5. Add the oracle, `GoldenRender.DeferredShadowsLandWhereForwardShadowsLand`:
     for each of `Forward` and `Deferred`, capture once with
     `scene_vars.cascaded_shadow_intensity = 0.0F` and once with the default
     `0.65F`, and build the shadowed-pixel mask (per-pixel luminance dropped by
     more than a small threshold between the two captures). Then assert
     `IoU(mask_deferred, mask_forward) > IoU(mask_deferred, vflip(mask_forward))`
     by a clear margin, and that `mask_forward` is neither empty nor the whole
     frame (otherwise the comparison is vacuous). This is direction-of-error
     sensitive and survives driver differences, which a pixel threshold would
     not. `SKIP_WITHOUT_GPU()` and the `supportsFrameCapture()` guard as in the
     neighbouring tests.
  6. Re-run the measurement from step 1 and **update the comment at
     `goldenRenderSuite.cpp:1025-1029` with the new number**. If the honest diff
     is now above `MEAN_ABS_DIFF_LIMIT = 1.0` because deferred genuinely lacks
     forward's ambient/punctual/emissive terms, raise the limit to the measured
     value plus headroom and replace the "the paths no longer shade alike"
     wording with the actual term-by-term gap (list it). Do **not** add the
     missing lighting terms to the deferred shader — that is a renderer-scope
     decision, not this task; record it as a new backlog idea instead.

  **Test:** `GoldenRender.DeferredShadowsLandWhereForwardShadowsLand` (new) plus
  the corrected `GoldenRender.DeferredMatchesForwardRoughly`. Update the golden
  counts in `docs/gpu-golden-testing.md` — `BuildIntegrity.GoldenTestCountsInDocsMatchTheSuite`
  (`buildIntegritySuite.cpp:1841`) will fail otherwise.

  **Build:** `clangcl-debug`. Run:
  `pwsh -ExecutionPolicy Bypass -File .\scripts\windows\Build-Windows-Container.ps1 -Configurations clangcl-debug`
  then `docker cp bb-build-persistent:C:\ws\build-clangcl-debug\bin\commitTestSuite.exe .\`
  and run it from the repo root on the host GPU (containers have no swapchain —
  AGENTS.md § Running on the Host).

  **Context:** Same bug class as `8b28543c` (tile-light binning) and the same
  shape as batch XVIII's depth-aspect finding: a rule with several hand-rolled
  copies, one of which is wrong. The parity test's own comment
  (`goldenRenderSuite.cpp:999-1004`) documents that it passed for weeks while
  comparing forward against forward — treat a suspiciously small diff as
  evidence of that failure mode recurring, not as good news.

- [b] **(S) (refactor) Give the fullscreen uv↔NDC round trip one definition, and pin it** — `deferred.slang` re-declares `fullscreen.slang`'s vertex output and vertex shader verbatim, which is how its inverse drifted.

  **BLOCKED 2026-08-02:** This task's own **Context** says to do it "after task 2
  [the deferred-mirror fix] and not concurrently — both edit `deferred.slang`",
  and its **Test** section requires re-running `GoldenRender.*` on the host GPU
  to confirm the refactor is pixel-neutral. The deferred-mirror fix is itself
  blocked on the same host-GPU verification (see the entry above), so both its
  prerequisite and its own acceptance test are unavailable right now. Re-attempt
  after the deferred-mirror task unblocks.

  **Files to read:**
  - `Resources/ShadersSlang/common/fullscreen.slang:1-27` — `FullscreenVsOut`,
    `fullscreen_vs`, and the usage comment at `:7-9`.
  - `Resources/ShadersSlang/deferred/deferred.slang:87-105` — the duplicate.
  - `Resources/ShadersSlang/ssao/ssao.slang:40-52` — the correct consumer:
    imports `fullscreen`, calls `fullscreen_vs`, and inverts in `view_pos_at`.
  - `Resources/ShadersSlang/ibl/ibl.slang:20-22` — a deliberate exception
    (different triangle winding), documented in place. Must stay allowlisted.
  - `Test/commit/VulkanEngine/buildIntegritySuite.cpp:1445` —
    `NoShaderRedeclaresTheCascadeCount`, the source-scanning gate to copy.

  **Steps:**
  1. Add to `common/fullscreen.slang`, next to `fullscreen_vs`, the inverse:
     `float2 fullscreen_uv_to_ndc(float2 uv)` returning
     `float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0)`, with a comment stating that it
     is the exact inverse of `fullscreen_vs`'s uv and that the two must be edited
     together.
  2. In `deferred.slang`: `import fullscreen;`, delete `LightingVsOut` (`:87-91`)
     and the body of `lighting_vs_main` (`:96-101`) in favour of
     `return fullscreen_vs(vid);` returning `FullscreenVsOut`, change
     `lighting_fs_main`'s parameter type to `FullscreenVsOut`, and build
     `clipPos` from `fullscreen_uv_to_ndc(In.uv)`. Entry-point **names** must not
     change — `compile-slang-shaders.ps1:74-75` and
     `buildIntegritySuite.cpp:944-947` both list them.
  3. In `ssao.slang`, rewrite `view_pos_at`'s first line to use
     `fullscreen_uv_to_ndc(uv)` so there is exactly one copy of the y term.
  4. Recompile shaders; `ssao` emits WGSL, so re-run
     `scripts/windows/compile-slang-shaders.ps1` and commit the regenerated
     `crates/webgpu_renderer/src/shaders/ssao.wgsl` in the submodule
     (`BuildIntegrity.CheckedInWgslIsNotOlderThanItsSlangSource` and
     `CheckedInWgslHasNoHandEdits` enforce this).
  5. Add `BuildIntegrity.NoShaderRedeclaresTheFullscreenUvMapping`: scan every
     `Resources/ShadersSlang/**/*.slang` for the literal uv expression
     (`1.0 - (y + 1.0) * 0.5`) and for a re-declared
     `struct ...VsOut { float4 svPosition : SV_Position; float2 uv : TEXCOORD0; };`,
     and fail on any hit outside `common/fullscreen.slang`, with `ibl/ibl.slang`
     allowlisted by name plus its stated reason. Follow
     `NoShaderRedeclaresTheCascadeCount`'s structure, including anchoring the
     allowlist to a source marker rather than a line number (`e8b1db52`).

  **Test:** `BuildIntegrity.NoShaderRedeclaresTheFullscreenUvMapping` (new, pure
  CPU). Re-run `GoldenRender.*` on the host GPU to confirm deferred and SSAO are
  unchanged — this task must be pixel-neutral, since task 2 already fixed the
  behaviour.

  **Build:** `clangcl-debug`, same invocation as task 2.

  **Context:** Do this **after** task 2 and not concurrently — both edit
  `deferred.slang`. Straight continuation of `a3b42dfc` (one depth-aspect
  definition in `FormatHelper.hpp`) and `f97712f4` (one `ShaderStagePair` instead
  of six copies): the payoff is that the next fullscreen pass cannot get the
  convention wrong, not the lines removed.

- [b] **(S) (refactor) Collapse the two cloud-output image barriers and the rationale comment that is written twice** — deferred from batch XVIII with the finding already verified; ~20 lines of identical field setup and a ~10-line comment repeated almost verbatim.

  **BLOCKED 2026-08-02:** This task's acceptance test is
  `GoldenRender.CloudsAcrossManyFramesDoesNotLoseTheDevice` staying green on
  the host GPU, plus an optional sync-validation rerun. Host GPU golden
  verification is currently unavailable in this RDP session (see the blocked
  deferred-mirror task above for the reproduced failure). This refactor
  touches cross-frame WAR barrier synchronization, so shipping it unverified
  risks a real device-lost/sync-hazard regression rather than a cosmetic one
  — not worth doing blind. Re-attempt once host GPU verification is restored.

  **Files to read:**
  - `Src/GraphicsEngineVulkan/renderer/VulkanRenderer.cpp:871-951` — the whole
    clouds block. `:881-893` and `:913-925` are the two barriers; `:871-880` and
    `:933-941` are the duplicated rationale; `:942-951` is the only part of the
    second comment that is unique (the 2026-08-01 sync-validation measurement).

  **Steps:**
  1. Add a small file-local helper (anonymous namespace or a static lambda above
     the clouds block) that takes the image plus src/dst stage and access masks
     and records an `eGeneral -> eGeneral` colour-aspect
     `vk::ImageMemoryBarrier` — every other field is identical between the two
     call sites.
  2. Replace both barriers with calls to it:
     `eFragmentShader -> eComputeShader`, `{} -> eShaderWrite` before
     `recordComputeCommands`, and `eComputeShader -> eFragmentShader`,
     `eShaderWrite -> eShaderRead` after. Do not change any mask — this is a
     pure de-duplication and the goldens must not move.
  3. Keep the cross-frame-WAR rationale **once**, on the helper or on the first
     call site. Delete the repeat at `:933-941` but **keep `:942-951` intact** —
     the sync-validation measurement is unique and load-bearing evidence.
  4. Preserve the second comment's distinct content too (`:903-912`: why this is
     hand-written rather than `VulkanImage::transitionImageLayout`'s
     `eGeneral->eGeneral` overload, which would cost `eAllCommands ->
     eAllCommands`). That reasoning is not duplicated anywhere.

  **Test:** No new test. Re-run `GoldenRender.CloudsAcrossManyFramesDoesNotLoseTheDevice`
  on the host GPU; it must stay green. If `scripts/windows/Run-SyncValidation.ps1`
  is runnable in this environment, re-run it and confirm no `SYNC-HAZARD`,
  matching the measurement the comment records.

  **Build:** `clangcl-debug`, same invocation as task 2.

  **Context:** Batch XVIII found this, confirmed it, and deferred it explicitly
  — do not re-derive it. `9aac4cb2` (Clouds' duplicated storage-texture and
  compute-pipeline setup) is the pattern; the risk here is deleting the wrong
  half of the comment, so re-read `:942-951` before cutting.

## 2026-08-02 batch II — planner (a partial-face OBJ read that runs off the end of `indices`, a glTF emitter that writes unescaped JSON, a texture-path rule the two renderers disagree on, unpinned histogram constants)

**Every task in this batch is verifiable with no GPU.** The three `- [b]`
entries above are all blocked on host GPU golden verification, which is still
unavailable in this session, so the whole batch was chosen to be provable with
`commitTestSuite.exe --gtest_filter='ObjParseUnit.*'` (device-free by
construction — `objParseSuite.cpp:1-10`) and `cargo test`. Nothing below needs
a swapchain. Every `file:line` was read out of the tree this pass.

**The headline is a memory-safety bug in the OBJ loader that its own fuzz
target cannot find.** `ObjLoader::loadVertices` guards each face *vertex*
against a malformed index and `continue`s past it
(`ObjLoader.cpp:250-252`), but the enclosing face keeps going: the other two
vertices are still emitted. That breaks the "`indices.size()` is a multiple of
3" invariant the rest of the function assumes, and the flat-normal pass at
`:333-344` walks `i += 3` bounded only by `i < indices.size()`, so it reads
`indices[i+1]` / `indices[i+2]` past the end. `GltfLoader.cpp:358` writes the
same loop correctly (`i + 2 < indices.size()`) and adds a degenerate-triangle
guard the OBJ copy lacks — one rule, two hand-rolled copies, one wrong, which
is the same shape as batch XVIII's depth-aspect finding and `a3b42dfc`.

Reachability was traced through the vendored parser rather than assumed:
`tiny_obj_loader.h:6398` (`fixIndex`) returns `idx - 1` for any **positive**
index with no upper-bound check, and the `} else {` branch at `:8155` — which
is the path a triangulating reader takes for a plain 3-vertex `f` — copies the
face's indices into `shape.mesh.indices` verbatim with no validation. (The
`npolys != 3` branch at `:7674` *does* validate, which is why quads are safe
and triangles are not.) So `v 0 0 0 / v 1 0 0 / v 0 1 0 / f 1 2 999999`
parses successfully and hands the loader an out-of-range positive index. Three
lines of OBJ.

`Test/fuzz/obj_parsing_fuzz_test.cpp` claims in its header comment to "prove
malformed model files cannot crash or OOB-read the loader", but its mirror
(`:44-73`) stops at the *attribute* reads — it accumulates a checksum and never
reproduces the index emission or the flat-normal post-pass, i.e. exactly the
two places the bug lives. Task 1 fixes the loader and closes that hole in the
harness. There is a third mirror of the same walk in `Test/perf/perfSuite.cpp:214`.

Tasks 3-5 are independent. Task 3 is a hand-written JSON emitter that
interpolates untrusted strings unescaped. Task 4 is a genuine cross-renderer
divergence: measured against the shipped asset tree, the C++ engine resolves
**23/23** `map_Kd` references correctly and the Rust converter resolves **0/23**.
Task 5 pins the one hand-written WGSL shader in the tree against its Rust
constants — the Slang manifest gates cannot cover it because it is not generated
from Slang (AGENTS.md § Shaders).

Ordering: task 2 must land **after** task 1 (both edit `ObjLoader.cpp`'s
flat-normal loop, and task 1 is the behaviour fix while task 2 is pure
consolidation). Everything else is independent.

Candidates found but NOT tasked (checked, then rejected — do not re-propose
without new evidence): **`common/cascaded_shadow.slang:32` (`proj.xy * 0.5 + 0.5`)
vs `forward/forward.slang:230` (`0.5 - proj.y * 0.5`) looking like a third
vertical mirror** — it is not one; `cascaded_shadow.slang` is SPIR-V/Vulkan
(NDC +1 = bottom) and `forward.slang` is the Rust/WebGPU shader (NDC +1 = top),
so each matches its own renderer's convention per the table in the batch above;
**`MAX_OBJECTS`'s abuse as a byte count in the descriptor pool size**
(`VulkanRenderer.cpp:1421-1423` passes `sizeof(ObjectDescription) * MAX_OBJECTS`
as a *descriptor count*) — wrong units, but it only over-allocates, and the line
above it already says "Historical pool sizing kept as-is (generously
overallocated)"; **`GltfLoader.cpp:296-299`'s "Placeholder for now" comment** —
reads like a TODO but the flat normals it defers to are genuinely computed at
`:352-372`, so the comment is confusing rather than stale; **`bloom.rs` /
`tonemap.rs` / `ssao.rs` / `context.rs` having no unit tests** — re-confirmed,
and re-rejected for the reason the batch above gave.

### C++ Vulkan engine

### Rust WebGPU renderer (`third_party/OxidANT`)

### Cross-renderer

## 2026-08-02 batch III — planner (refactor: a guard whose test pins a copy of it, a full-extent viewport written five times, a per-model count vector built twice on both halves of one invariant)

**Every task in this batch is verifiable with no GPU**, deliberately: the four
`- [b]` entries above are all blocked on host GPU golden verification, which is
still unavailable. All three tasks below are provable with device-free suites
(`SkyBoxUnit.*`, a new `ViewportHelperUnit.*`, `SceneAccessorUnit.*` — the last
constructs a bare `Scene` with no `VulkanDevice`, see
`sceneAccessorSuite.cpp:18-31`). Every `file:line` was read out of the tree this
pass.

**The theme is one rule with more than one hand-rolled copy** — the same shape as
`a3b42dfc` (depth aspect → `FormatHelper.hpp`), `f97712f4` (six shader-stage
blocks → `ShaderStagePair`) and `15e64b10` (flat normals shared between loaders).
Task 1 is the sharpest of the three because the duplicate copy lives *in the
test*: `skyBoxSuite.cpp:11-14` states outright that it re-declares
`cubemapFacesConsistent` rather than calling it, so the suite would stay green
through any regression of the shipped guard. That is a test-coverage gap wearing
a passing test's clothes. Task 3 is the same failure mode one level up: the
"flatten every model's textures in model order" invariant is explicitly
documented as having two halves (`VulkanRenderer.cpp:1536-1541`), and each half
builds its own `textureCountPerModel` loop, so the two can silently disagree
about model count or ordering.

Ordering: all three are independent and touch disjoint files. Task 2 is the
largest edit surface (five files) but the least subtle.

Candidates found but NOT tasked (checked, then rejected — do not re-propose
without new evidence): **`SkyBox::recreateFrameResources` being a one-line
pass-through to `createFramebuffers` (`SkyBox.cpp:500-503`)** — it is a
pass-through, but it is the shape `Rasterizer`/`DeferredRasterizer`/`PostStage`
all present to `VulkanRenderer.cpp:677-688`, so collapsing only SkyBox's would
make the call site *less* uniform, not more; **`Scene::getObjectDescriptions()`
returning by value (`Scene.ixx:111`)** — the one caller
(`VulkanRenderer.cpp:1304`) mutates the result via `assignTextureOffsets`, so the
copy is load-bearing; **`MAX_OBJECTS` used as a byte count in the descriptor pool
size (`VulkanRenderer.cpp:1421-1423`)** — re-confirmed as wrong units and
re-rejected for the reason batch II gave (it only over-allocates, and the line
above already documents the generous sizing).

## 2026-08-02 batch IV — planner (a BLAS that declares one vertex too many, a GPU ranking where the tie-break outweighs the device type, a shadow-map combo that lies at startup, two hard-coded dispatch grids, a KTX2 mip count nothing checks)

**Every task in this batch is verifiable with no GPU**, deliberately: the four
`- [b]` entries above are all blocked on host GPU golden verification, which is
still unavailable in this session. All five tasks below are provable with
device-free gtest suites (the `SwapchainChoices.hpp` / `FormatHelper.hpp`
"extract the pure decision into a header, test it without a device" pattern —
`swapchainChoicesSuite.cpp:1-6`, `formatHelperSuite.cpp:1-16`), a
`BuildIntegrity` source-scan gate, or `cargo test`. Every `file:line` below was
read out of the tree this pass.

**The headline is a Vulkan spec violation in the BLAS build.**
`ASManager::objectToVkGeometryKHR` sets
`acceleration_structure_triangles_data.maxVertex = mesh->getVertexCount()`
(`ASManager.cpp:495`), but `maxVertex` is defined as the **highest index of a
vertex that will be addressed**, i.e. `vertexCount - 1` — and
`Mesh::vertex_count` is `vertices.size()` (`Mesh.cpp:38`). Every BLAS this
engine builds therefore tells the driver it may read one `sizeof(Vertex)`
stride past the end of the vertex buffer. It is invisible today only because
VMA suballocation almost always leaves readable bytes there and because
`robustBufferAccess` is explicitly `VK_FALSE` (`VulkanDevice.cpp:484`), so
there is no bounds check to trip either. This is the same shape as the SBT
handle-stride finding (`74aaee23`): correct on this GPU, undefined by the spec.

Tasks 2-5 are independent. Task 2 is a ranking function whose tie-break term is
numerically larger than the gaps it is meant to break. Task 3 is a startup/GUI
divergence the user sees on every launch. Task 4 pins two dispatch grids against
the `[numthreads(...)]` they must match, the way `ee4abb24` pinned `TILE_SIZE`.
Task 5 is untrusted-asset hardening in the Rust crate, the same class as
`a0cffe7a` / `d25cd1e5`.

Ordering: all five are independent and touch disjoint files.

Candidates found but NOT tasked (checked, then rejected — do not re-propose
without new evidence): **`Texture::createImage` never recording its
`in_mip_levels` into the `mip_levels` member (`Texture.cpp:208-223`), so
`createTextureSampler` builds a `maxLod = 0` sampler for every non-`uploadRgba`
caller** — real, but all three such callers (`Clouds.cpp:33`, `SkyBox.cpp:97`,
`CascadedShadowMap.cpp:54`) pass exactly 1 mip, for which `maxLod = 0` is the
correct value; **the scratch-buffer reuse barrier in
`ASManager::createBLAS:112-121`** — `srcAccessMask = eAccelerationStructureWriteKHR`
/ `dstAccessMask = eAccelerationStructureReadKHR` between consecutive builds is
verbatim the nvpro-core `cmdCreateBlas` idiom, not a missing WAW barrier;
**`asset/hdr.rs`** — swept for the OOB/overflow class the OBJ loader had and
found already hardened at every branch (`checked_add` on the old-style repeat
shift, `get_mut` on every span, a zero-length-literal guard against the infinite
loop), with a test that enumerates twelve malformed inputs; **`ASManager::cleanUp`
leaving `tlas.vulkanAS` non-null and `blas` unshrunk (`ASManager.cpp:381-401`)**
— a double-`cleanUp` would double-destroy, but the only caller
(`VulkanRenderer.cpp:1173`) runs once at shutdown and the destructor is
`= default`, so there is no reachable path.

### C++ Vulkan engine

### Rust WebGPU renderer (`third_party/OxidANT`)

## 2026-08-02 batch V — planner (an SSAO hemisphere pointed into the screen, an exposure shader that snaps on a stalled frame, the last unpinned dispatch grid plus its inert spec constants, two command buffers that record nothing, an LOD merge that is only half order-independent)

**Every task in this batch is verifiable with no GPU**, deliberately: the five
`- [b]` entries above are all blocked on host GPU golden verification, which is
still unavailable in this session. Tasks 1, 2 and 5 are provable with `cargo
test` (unit tests, plus headless tests that already skip themselves when no
adapter is present — `headless.rs:621-624`); task 3 is a `BuildIntegrity`
source-scan gate of exactly the shape `a348bd9f` added for the cloud dispatches;
task 4 is a provable no-op (an empty command buffer executes nothing) that the
compiler and the existing suites confirm. Every `file:line` below was read out of
the tree this pass.

**The headline is that the Rust renderer's SSAO hemisphere points away from the
camera.** `ssao.slang:66` builds the reconstructed normal as
`normalize(cross(px - p, py - p))` where `px` is one texel to the RIGHT and `py`
one texel DOWN. `view_pos_at` (`ssao.slang:40-45`) maps `uv.y` down to `ndc.y`
up, and the camera is `Mat4::perspective_rh` (`scene/camera.rs:52`), so in view
space `px - p ≈ +X` and `py - p ≈ -Y`; `cross(+X, -Y) = -Z`. View space here is
right-handed with visible geometry at `z < 0`, so a surface facing the camera has
a normal of **+Z** — every reconstructed normal is negated. The kernel
(`ssao.slang:17-30`, all twelve entries with `k.z > 0`) is then rotated into a
hemisphere that points *into* the surface, so every sample lands behind the
geometry and the occlusion test `sceneZ >= samplePos.z + bias` is true for
essentially every sample on every surface. Worked through at the shipped tuning
(`forward.rs:1663`, `radius = 0.6, bias = 0.02, intensity = 1.0`) for a
fronto-parallel wall at view `z = -5`: `samplePos.z ≈ -5.18`, `sceneZ = -5`,
`rangeCheck = 1`, so `ao = 1 - 12/12 = 0` — the AO buffer is black wherever there
is geometry, and `tonemap.slang:58-60`'s `lerp(1.0, aoRaw, ssao_strength)`
multiplies the whole image by `1 - ssao_strength` (0.3 at the default 0.7). SSAO
currently contributes a flat global dimming and **no** crease darkening at all.
The existing oracle cannot see it: `ssao_darkens_geometry`
(`tests/headless.rs:620-655`) only asserts that total energy drops, which a
uniform multiply satisfies perfectly — the same "the test that should have caught
it cannot tell the two cases apart" shape as `6a6fa2bf` and `8b28543c`.

Tasks 2-5 are independent. Task 2 is a CPU-oracle/shader divergence on a case
`FrameClock` is documented to produce. Task 3 finishes the dispatch-pinning sweep
`a348bd9f` started and deletes the specialization machinery that the
already-fixed entry at line 2044 proved inert. Tasks 4 and 5 are refactors, the
second of which strengthens an invariant the module already claims.

Ordering: all five are independent and touch disjoint files. Task 1 regenerates a
committed `.wgsl`, so run it before any other change that touches
`Resources/ShadersSlang/`.

Candidates found but NOT tasked (checked, then rejected — do not re-propose
without new evidence): **`Texture::createImage` not recording `in_mip_levels`
into `mip_levels`** — re-confirmed, re-rejected for the reason batch IV gave;
**`PostStage::recreateFrameResources` not destroying its framebuffers before
`createFramebuffer()` re-fills the vector (`PostStage.cpp:149-155`)** — looks
like a per-resize leak, but `VulkanRenderer::recreateSwapChain` calls
`postStage.destroyFramebuffers()` first (`VulkanRenderer.cpp:664`), and the same
holds for `Rasterizer`/`DeferredRasterizer`/`SkyBox`; **`VulkanSwapChain`'s
`std::vector<Texture>` reallocating mid-loop while each element wraps a swapchain
image (`VulkanSwapChain.cpp:155-161`)** — `VulkanImage::setImage` clears
`owns_image` (`VulkanImage.cpp:153-159`) and the move constructor carries the
flag, so a reallocation cannot double-destroy; **`cs_reduce_exposure` having no
`is_finite` recovery for a NaN `exposure_state[0]`, unlike
`adapt_exposure_ev`** — real divergence, but nothing writes NaN into that buffer
(`histogram.wgsl` guards every divide) so there is no reachable path.

### C++ Vulkan engine

### Rust WebGPU renderer (`third_party/OxidANT`)

## 2026-08-02 batch VI — planner (refactor: a RED CI gate that says three suites never run, a source file grep classifies as binary, a descriptor-pool override with one caller that passes bytes as a count)

**Every task in this batch is verifiable with no GPU**, deliberately: the twelve
`- [b]` entries above are all blocked on host GPU golden verification, which is
still unavailable in this session. Task 1 is a YAML edit proven by a test that is
red *right now*; task 2 is a one-character source fix plus a `BuildIntegrity`
source-scan gate of exactly the shape `a348bd9f`/`302faa90` added; task 3 is a
dead-code deletion whose only behavioural edge becomes a pure free function with
its own device-free suite. Every `file:line` below was read out of the tree this
pass; the actionable queue was empty when this batch was written.

**The headline is that `BuildIntegrity.EveryCpuSuiteIsInTheWindowsCiFilter` is
currently RED, and it is red about the thing it exists to catch.** Three
device-free suites are defined under `Test/commit/VulkanEngine` but appear
neither in `Windows.yml`'s `$cpuOnlySuites` array (`.github/workflows/Windows.yml:209-244`)
nor in the test's `gpu_excluded_suites` set (`buildIntegritySuite.cpp:1066`):
`BlasGeometryLimitsUnit` (`blasGeometryLimitsSuite.cpp`, added by `20c242ef`),
`ShadowResolutionUnit` (`guiSceneVarsRoundTripSuite.cpp:193,202,212`, added by
`adb0c183`) and `ViewportHelperUnit` (`viewportHelperSuite.cpp`, added by
`29081658`). Reproduced this pass by running the gate's own two parsers by hand:
`collect_defined_suites`'s start-of-line `TEST(`/`TEST_F(` anchoring
(`buildIntegritySuite.cpp:226-258`) against `parse_ci_filter_suites`'s
`$cpuOnlySuites` scrape (`:355-386`) yields exactly those three names and no dead
filter entries. Because `'BuildIntegrity.*'` *is* in the filter
(`Windows.yml:210`), the Windows CI test step runs this assertion and fails on
it — so the three suites do not run in CI *and* the gate that reports it is
itself failing the job. The comment directly above the array
(`Windows.yml:196`, "If you add a suite, add it to this filter or it will not
run in CI") is the instruction three consecutive executor commits missed.

**Second finding: `SceneConfig.cpp` contains a literal NUL byte, so grep and
ripgrep classify the whole file as binary and silently skip it.**
`Src/GraphicsEngineVulkan/scene/SceneConfig.cpp:116` reads
`if (*override_path != '<NUL>')` — the intended `'\0'` was committed as the raw
0x00 byte itself (confirmed at byte offset 4573; it is the only NUL in any
tracked file under `Src/`, `Test/`, `Resources/ShadersSlang/`, `cmake/` or
`scripts/`). The code is semantically correct and compiles, but every
`grep -rn`/`rg` over `Src/` reports "binary file matches" instead of the line, so
`resolveModelPath`, `getModelFile`, `getAvailableModelPaths`,
`findResourcesBasePath` and `KATAGLYPHIS_MODEL_OVERRIDE` are all invisible to the
search tool that both the agentic loop and a human use to find call sites. Two
searches in this planning pass returned "Binary file
Src/GraphicsEngineVulkan/scene/SceneConfig.cpp matches" where a real hit list was
expected. This is the same failure class as the shader-manifest and CI-filter
gates: a source of truth that tooling silently declines to read.

**Third finding: `DescriptorSetGroup`'s pool-size override exists for exactly one
caller, and that caller passes a byte count where a descriptor count belongs.**
`setPoolSize` (`DescriptorSetGroup.ixx:41-43`, `.cpp:77-89`) is called from one
place in the whole tree — `VulkanRenderer.cpp:1415-1417`, `setPoolSize(eStorageBuffer,
sizeof(ObjectDescription) * MAX_OBJECTS)` = 40 × 40 = **1600** storage-buffer
descriptors, where the derived rule (`DescriptorSetGroup.cpp:108-127`,
`binding.descriptorCount * set_count`) gives 1 × 3. Batches II and III both saw
the wrong units and rejected a fix because "it only over-allocates". **The new
evidence is that the units are not the point**: the override is the sole reason
`pool_size_overrides` exists at all, and it drags a member
(`DescriptorSetGroup.ixx:95`), three move-constructor/assignment clauses
(`.cpp:29,47,55`), a `create()` branch (`.cpp:112-115,127`), a `cleanUp()` line
(`.cpp:270`) and two doc comments (`.ixx:19-20,70-71`) behind it. Delete the one
call and all of it is dead — and the derivation left behind is pure, device-free
logic that has **zero** test coverage today (`grep -rn DescriptorSetGroup Test/`
returns nothing). `MAX_OBJECTS` (`common/Globals.hpp:4`) then has no reader
either. Three never-called public accessors are in the same deletion class,
grep-confirmed at one occurrence each across `Src/` + `Test/`:
`VulkanDevice::getAllocator` (`VulkanDevice.ixx:43`),
`VulkanDevice::getPhysicalDeviceProperties` (`:20`) and
`Mesh::getMaterialIDBuffer` (`Mesh.ixx:42` — the *buffer* is live via
`Mesh.cpp:78`'s device address; only the accessor is dead).

**Ordering matters here, unusually.** Do task 1 first: task 3 adds a new suite
name and will trip the same gate if the array is still stale, and until task 1
lands a green Windows CI run is impossible for any task in this batch.

Candidates found but NOT tasked (checked, then rejected or deferred with a reason
— do not re-propose without new evidence): **`CascadedShadowMap::updateCascades`'s
two per-frame heap allocations** — real (`CascadedShadowMap.cpp:88` move-assigns a
fresh `std::vector<CascadeData>` from `computeCascadeData`, `:105` builds a
`std::vector<glm::mat4>` purely to `memcpy` into the mapped UBO, both on the
per-frame path via `VulkanRenderer.cpp:194`), and the fix is the
`std::array`/`std::span` shape the same module already uses at
`CascadedShadowMapMath.cpp:29` and `CascadedShadowMap.cpp:443` — deferred purely
for the three-task cap, **pick this up next cycle** (it wants a
`computeCascadeDataInto(std::span<CascadeData>, …)` overload so the fifteen
existing `computeCascadeData` call sites in `cascadedShadowMapSuite.cpp` keep
compiling); **18 source files carrying a UTF-8 BOM while the rest do not**
(`GUI.cpp:1`, `Mesh.ixx:1`, `VulkanSwapChain.cpp:1`, …) — cosmetic, no tool in
this project misreads a BOM, and stripping them is pure diff noise;
**`resolveModelPath` (`SceneConfig.cpp:19-41`) and `findResourcesBasePath`
(`:44-67`) running near-identical depth-8 walks for a `Resources/` root** — real
duplication, but they test different predicates (`Resources/<rel>` vs
`Resources/Models`) and collapsing them changes which ancestor wins in trees
where only one predicate holds, so it needs a decision rather than a mechanical
extraction; **`CascadedShadowMap::recordCommands`'s draw loop
(`CascadedShadowMap.cpp:485-517`) as a third copy of
`MeshDrawRecorder::recordSceneMeshDraws`** — the two differ in push-constant
struct *and* in single-frustum vs union-over-cascades visibility, so this is a
generalization, not the verbatim extraction the `MeshDrawRecorder.ixx:24-32`
comment describes; **the docs sweep** — every repo-rooted `` `path` `` reference
in `docs/`, `README.md` and `AGENTS.md` resolves, and the two
`Resources/Shaders/` hits (`shader-build-pipeline.md:53`,
`shader-sharing.md:140-148`) are both inside explicit "Historical note" sections.

### C++ Vulkan engine

## 2026-08-02 batch VII — planner (a uniform block the host and the shader lay out differently, the gate that would have caught it, two glTF loaders that trust index values, the per-frame allocation batch VI deferred)

**The headline is `SceneUBO`: the C++ struct and the compiled SPIR-V disagree
about where every field from `cascadeSplits` onward lives, by exactly 4 bytes.**
This was not inferred — it was read out of the compiled binaries this pass with
`C:\VulkanSDK\1.4.350.0\Bin\spirv-dis.exe`. `Resources/ShadersSlang/build/spirv/rasterizer/rasterizer.fs_main.spv`
declares `%SceneUBO_std140` with `OpMemberDecorate ... Offset` values
`0, 32, 36, 40, 48, 64, 256, 272, 288, 304, 320, 336`. Slang compiles
`ConstantBuffer<T>` as **std140** (the emitted type is literally named
`SceneUBO_std140`; `scripts/windows/compile-slang-shaders.ps1:191` passes no
`-fvk-use-scalar-layout`), so the `uint/float/uint` run at bytes 32/36/40 is
followed by 4 bytes of std140 padding before `cascadeSplits` at 48. The host
struct (`Src/GraphicsEngineVulkan/renderer/SceneUBO.hpp:29-52`) has no such pad,
and this project does **not** define `GLM_FORCE_DEFAULT_ALIGNED_GENTYPES`
(`Src/GraphicsEngineVulkan/CMakeLists.txt:51` sets only
`GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_RADIANS`), so `glm::vec4`/`glm::mat4`
carry alignment 4 and `cascadeSplits` lands at 44. Consequence: the shader's
`cascadeSplits.x` reads the host's `cascadeSplits.y`, all three
`cascadeLightSpaceMatrices` are read 4 bytes off (i.e. garbage light-space
matrices), `view_dir`/`cam_pos`/every cloud vec4 are shifted, and
`cloudParameters` at 336..351 reads past the end of a buffer created and bound
at `sizeof(SceneUBO)` (`VulkanRenderer.cpp:1492-1493`, `:716`, `:723`). Five
shaders bind this block: `rasterizer.fs_main`, `deferred.lighting_fs_main`,
`clouds.clouds_main`, `path_tracing.path_tracing_main`,
`raytrace.rchit.rchit_main`.

**Why nothing caught it: `SceneUBO` is the only shared host/device struct with
no layout test, and it is the only one that mismatches.** An exhaustive sweep of
every laid-out block in `Resources/ShadersSlang/build/spirv/**/*.spv` this pass
found 13 distinct emitted types; every other one agrees with its host twin, and
most are already pinned by `Test/commit/VulkanEngine/pushConstantSuite.cpp`
(`PushConstantRasterizerUnit:36-104`, `PushConstantPathTracingUnit:110`,
`PushConstantPostUnit:125`, `PushConstantRaytracingUnit:137`,
`ObjMaterialLayoutUnit:151`). Verified agreeing this pass:
`ObjMaterial_natural` (0/12/24/36/48/60/64/68/72/76/80/84/92, `ArrayStride 100`
= `sizeof(ObjMaterial)`), `Vertex_natural` (0/12/24/36, `ArrayStride 44`),
`ObjectDescription_std430` (0/8/16/24/32, `ArrayStride 40`),
`PushConstantRasterizer_std430` (0/64/112), `GlobalUBO_std140` and
`CameraUBO_std140` (both 0/64/128/192 — all-`mat4` structs cannot drift),
`ShadowPushConstants_std430`, `DirectionalLightData_std140` (0/16). The rule the
sweep exposes is simple: a shared struct is safe exactly when every member is
16-byte-sized; `SceneUBO` is the only one that interleaves scalars, and it is
the only one that is wrong.

Tasks 1 and 2 are ordered: **do task 1 first**, then task 2 turns its
hand-written expectations into a gate that reads the `.spv` itself. Tasks 3–5
are independent of both. Every task in this batch is verifiable **without a
GPU** — the twelve `- [b]` entries above are still blocked on host GPU golden
verification, and task 1's oracle is deliberately a CPU `offsetof` test rather
than a rendered image. The actionable queue was empty when this batch was
written.

Candidates found but NOT tasked (checked, then rejected — do not re-propose
without new evidence): **`skybox.slang:6-13`'s `CameraUBO` is a verbatim
re-declaration of `common/scene_types.slang:75-82`'s `GlobalUBO`** — real
duplication, but both emit identical offsets (all `mat4`), and deleting it means
editing a `.slang` and regenerating + committing every SPIR-V/WGSL artifact
(`BuildIntegrity.CompiledShadersAreNotOlderThanSharedIncludes`,
`CheckedInWgslIsNotOlderThanItsSlangSource`) for a zero-behaviour change; fold
it into the next shader-touching task instead; **`PushConstantSkyBox`
(`skybox.slang:18-21`) has no host struct at all** — `SkyBox.cpp:334,411` pushes
a bare `sizeof(uint32_t)`, which is correct at offset 0 and needs an allowlist
entry in task 2, not a fix; **`Frustum.cpp`** — read end to end this pass, the
Gribb-Hartmann extraction, the `GLM_FORCE_DEPTH_ZERO_TO_ONE` near-plane
derivation and the degenerate-plane guards are all correct and documented in
place; **18 source files carrying a UTF-8 BOM** — re-checked, still cosmetic,
still diff noise (batch VI rejected this for the same reason).

## 2026-08-02 batch VIII — planner (a key that never stops being held once ImGui takes focus, ten throwing `std::filesystem` calls in a `-fno-exceptions` build, a Rust test suite this repo's CI never runs, a skybox that hands `vkCreatePipelineLayout` a null set layout, four copies of the same "walk up for Resources/" search)

**The headline is the input path: hold `W`, click an ImGui widget, release `W` — the
camera keeps flying forever.** `Window::key_callback`
(`Src/GraphicsEngineVulkan/window/Window.cpp:122`) returns on
`ImGui::GetIO().WantCaptureKeyboard` **before** reaching
`handle_key_callback`, so the `GLFW_RELEASE` never clears
`keys[GLFW_KEY_W]` and `Camera::key_control` keeps translating every frame.
The same shape kills the right-drag look mode:
`handle_mouse_button_callback`
(`Src/shared/frontend/WindowInputCallbacks.ixx:77`) returns on
`WantCaptureMouse` before `glfwSetCursorPosCallback(window, nullptr)`, so
releasing the right button over a panel leaves the cursor callback installed.
`WindowInputUnit.ImGuiCaptureGateSwallowsInput`
(`Test/commit/VulkanEngine/frontendInputSuite.cpp:111`) pins the *swallow*
direction only — the release direction is the half that was never asserted,
and it is the half that leaves the camera moving.

The second finding is systematic rather than local. `cmake/ProjectOptions.cmake`
compiles everything with `-fno-exceptions` / `/EHs-`, and
`Src/shared/util/FileReader.ixx:13` already writes the rule down — "The
`error_code` overload is REQUIRED, not stylistic". A grep for
`std::filesystem::` across `Src/` this pass found **ten calls that use the
throwing overload anyway**, in four files. Under these flags a
`filesystem_error` is not an exception, it is `std::terminate` with no log
line. The `recursive_directory_iterator` one is the subtle member of the set:
passing `ec` to the *constructor* makes construction non-throwing, but the
range-for's `operator++` is still the throwing increment.

Tasks 1–4 are independent. Task 5 depends on task 2 (so the shared resolver is
exception-free by construction) and should land after task 4 (which fixes the
skybox crash; task 5 then folds the skybox's path lookup into the resolver).
Every task is verifiable **without a GPU** except task 4's end-to-end check,
which is a host launch rather than a golden. The actionable queue was empty
when this batch was written.

Candidates found but NOT tasked (checked, then rejected — do not re-propose
without new evidence): **`Resources/ShadersSlang/.../bloom.wgsl`'s blur kernel**
— read end to end, it derives its texel size from `textureDimensions` rather
than a hard-coded constant, so the half-res chain is correct at every extent;
**`asset/hdr.rs`** — the whole RGBE decoder was audited against Radiance's
`freadcolrs`/`oldreadcolrs`, including the `MAX_PIXELS` cap, the `-8` mantissa
shift and every RLE overrun path, and its nine tests already cover them;
**`Clouds::dispatchNoiseGeneration`'s grid** — already pinned by
`CloudDispatch.hpp`'s `static_assert` plus
`BuildIntegrity.CloudDispatchGridsMatchTheShaderWorkgroupSizes`;
**`VulkanDevice::create_pipeline_cache`** — the stale-blob retry and the
"continue without a cache" fallback are both already correct;
**`keyframe_lerp_indices`'s linear keyframe scan**
(`crates/webgpu_renderer/src/render/animation.rs:22`) — genuinely O(n) per
channel per frame where `partition_point` would be O(log n), but the morph
loop already `continue`s before allocating for non-matching primitives, so the
measured cost is a few thousand comparisons per frame on any realistic scene;
not worth a task until something measures it.

## 2026-08-02 batch IX — planner (refactor: a sixth "one rule, five hand-rolled copies" — framebuffer creation; a positional 9-field aggregate spelled out three times; 15 accessors on the two shared scene structs that nothing calls)

**Every task in this batch is verifiable with no GPU**, deliberately: the `- [b]`
entries above are still blocked on host GPU golden verification. Every
`file:line` below was read out of the tree this pass, and every dead-code claim
was confirmed by a whole-tree grep over `Src/` + `Test/`. The actionable queue
was empty when this batch was written (the only remaining `- [ ]`-shaped entries
in this file are `- [b]`).

**The headline is that framebuffer creation is the one remaining member of the
`buildAttachmentDescription` / `fullExtentViewport` family that never got its
helper.** Five files each spell out the same six-field `vk::FramebufferCreateInfo`
block by hand: `Rasterizer.cpp:236-243`, `PostStage.cpp:304-311`,
`DeferredRasterizer.cpp:362-368`, `SkyBox.cpp:321-327` and
`CascadedShadowMap.cpp:210-216`. The five copies have already drifted in three
ways that a shared builder removes by construction: **SkyBox hard-codes
`attachmentCount = 2`** rather than deriving it from `attachments.size()`, so
adding a third attachment to its `std::array` there is silently ignored;
**DeferredRasterizer hoists the create-info out of the loop** and mutates the
`attachments` array it borrows through `pAttachments` on every iteration — correct
today, but it is precisely the borrowed-pointer shape batch XVII found dangling in
swapchain creation; and **PostStage alone uses the two-out-param
`createFramebuffer(&info, nullptr, &framebuffers[i])` overload** where the other
four use the `ResultValue` one. This is the same finding class as `a3b42dfc`
(depth aspect → `FormatHelper.hpp`), `f97712f4` (six shader-stage blocks →
`ShaderStagePair`), `29081658` (`ViewportHelper.hpp`) and `36937517`
(`RenderPassHelper.hpp`).

**Second: `Camera.cpp` constructs the same nine-member `CameraControllerState`
aggregate three times, positionally.** `key_control` (`:63-71`), `mouse_control`
(`:79-87`) and `update` (`:110-118`) each list `position, front, world_up, right,
up, yaw, pitch, movement_speed, turn_speed` in order, with no designated
initializers. Because the four `glm::vec3&` members are mutually interchangeable
to the compiler and so are the two leading `float&` members
(`CameraController.ixx:15-26`), **reordering the struct silently rebinds
`right`↔`up` or `yaw`↔`pitch` at all three call sites with no diagnostic** — the
camera would just start rolling instead of yawing. `apply_keyboard_input`
(`CameraController.ixx:39`) additionally takes a bare `const bool *keys` and
indexes it up to `GLFW_KEY_E` (69) with no length: `Window::get_keys()`
(`Window.ixx:24`) hands out `input_state.keys.data()` off a
`std::array<bool, 1024>` (`WindowInputState.hpp:7,11`) while
`cameraSceneConfigSuite.cpp:108` passes a `std::array<bool, GLFW_KEY_LAST + 1>`,
so two different array sizes already flow into the same unchecked pointer.

**Third, the dead code: 15 of the 19 accessors on the two shared scene structs
have no caller anywhere in `Src/` or `Test/`.** Grep-confirmed at exactly one
occurrence each (their own definition): `ObjMaterial::get_{ambient,diffuse,
specular,transmittance,emission,shininess,ior,dissolve,illum,alphaCutoff,
uv_scale,uv_offset}` (`Src/shared/scene/ObjMaterial.hpp:68-82`) and
`Vertex::get_{normal,color,tex_coors}` (`Src/shared/scene/Vertex.hpp:31-33`).
Only `ObjMaterial::get_textureID` and `Vertex::get_position` are live. Every
field is public, so each accessor is pure redundancy over the direct member
access the rest of the engine already uses.

Ordering: all three are independent and touch disjoint files.

Candidates found but NOT tasked (checked, then rejected — do not re-propose
without new evidence): **the twelve hand-rolled `vk::ImageMemoryBarrier` blocks**
(`FrameCapture.ixx:90,122`, `PathTracing.cpp:62,94,148`, `Raytracing.cpp:92,123`,
`VulkanRenderer.cpp:882,914`, `SkyBox.cpp:147`, `Texture.cpp:288`,
`VulkanImage.cpp:124`) — the same "seven fields a copy-paste drops" shape as task
1, but the `- [b]` cloud-barrier entry above already owns two of those lines, and
a helper task would collide with it; pick this up once that unblocks;
**`ForwardRenderer::new` at 583 lines** (`crates/webgpu_renderer/src/render/forward.rs:364-947`)
— real, and the file already has the `create_*_pipeline` free-function shape to
extract into, but every extracted unit needs a live adapter to exercise, so the
refactor would ship with compile-only verification; **the accessor sweep across
`Src/**/*.ixx`** — re-run this pass over every `get*`/`is*`/`has*`/`set*`
declaration, and apart from the fifteen in task 3 every one has a live caller
(`getComputeQueue`→`Clouds.cpp:241`, `getPresentationQueue`→`VulkanRenderer.cpp:605`,
`getGBuffer{Normal,Albedo,Material}`→`VulkanRenderer.cpp:1587-1589`,
`getMipLevel`→`Model.cpp:72`, `getVertexCount`→`ASManager.cpp:496`,
`isDoubleSided`→`Scene.ixx:120`, …), so batches VI–VIII drained this seam;
**pinning `forward.wgsl`'s binding indices against `forward.rs`'s hand-built
layout** — the `histogram_constants.rs` gate shape would fit, but wgpu validates
the bind-group layout against the shader at pipeline-creation time and errors, so
this drift is loud, not silent; **`Texture::createDefaultTexture`
(`Texture.cpp:195-202`) discarding `uploadRgba`'s `bool`** — the one caller path
already tolerates a texture-less material, and making it propagate is a
behaviour change, not a refactor.

### C++ Vulkan engine

  **Context:** Deleting a member of a `#ifdef __cplusplus` host/device struct is
  only safe because these are member **functions**, not data members — the
  layout the shaders and the BLAS/vertex-input descriptions depend on
  (`ASManager.cpp:495`, `Vertex.cpp:40-49`) is untouched. If any `offsetof` or
  `sizeof` assertion moves, stop: that is a real finding, not a number to
  update.

## 2026-08-02 batch X — planner (a left click that cancels right-button look mode, a `map_Kd` backslash rule only one of the two OBJ loaders applies, a window whose creation failure nothing checks, two more dead accessors, a mip filter that always rounds down)

**Every task in this batch is verifiable with no GPU**, deliberately: the
fifteen `- [b]` entries above are still blocked on host GPU golden verification.
Tasks 1, 2 and 4 land device-free unit tests in suites that already run in
Windows CI (`WindowInputUnit`, `ObjParseUnit`); task 5 is `cargo test` on a pure
function that already has its own `#[cfg(test)]` module; task 3 is a
compile-verified dead-state deletion plus a one-line guard. Every `file:line`
below was read out of the tree this pass, and every dead-code claim was
confirmed by a whole-tree grep over `Src/` + `Test/`. The actionable queue was
empty when this batch was written.

**The headline is that pressing ANY button other than the right one ends
right-button look mode.** `handle_mouse_button_callback`
(`Src/shared/frontend/WindowInputCallbacks.ixx:87-103`) uses one predicate for
two decisions: `should_capture_cursor(button, action)` answers "should I START
look mode", and everything else falls into an `else` branch that does
`mouse_first_moved = true; glfwSetCursorPosCallback(window, nullptr)`. A left
press while the right button is still held is not a right-button press, so it
takes the `else` branch and tears the cursor callback down mid-orbit; the camera
stops turning until the user releases and re-presses the right button. The
existing test even pins the input half of it —
`CursorCaptureDecisionIgnoresImGuiState`
(`Test/commit/VulkanEngine/frontendInputSuite.cpp:178-186`) asserts
`should_capture_cursor(GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS) == false`, which is
correct for "start" and is exactly what the `else` branch then misreads as
"stop". Same shape as the RELEASE-under-ImGui-capture bug `c37394b4` fixed one
callback over: one predicate asked to answer two different questions.

**Second: the two OBJ loaders do not resolve `map_Kd` the same way, and
`docs/model-loading.md:121` claims they do.** The Rust side normalises `\` to
`/` before resolving (`crates/webgpu_renderer/src/asset/obj_to_gltf.rs:132-147`,
`values.last().map(|name| name.replace('\\', "/"))`); the C++ side takes
`mp->diffuse_texname` verbatim (`Src/GraphicsEngineVulkan/scene/ObjLoader.cpp:181-192`)
and tinyobjloader does not translate separators either (grep-checked
`third_party/TINY_OBJ_LOADER/tiny_obj_loader.h` — its only backslash handling is
the Windows UNC prefix at `:1284-1299`). So a Windows-authored
`map_Kd textures\wall.png` resolves in the Rust renderer on every platform and
silently degrades to the white default texture in the C++ one on Linux — i.e.
in the only CI lane that runs on every push. The doc's own bullet
(`docs/model-loading.md:144-146`) already records the normalisation as
"a deliberate deviation", which contradicts the headline sentence six lines
earlier. **No shipped asset under `Resources/Models` uses a backslash**
(grep-confirmed this pass), so nothing is broken today: this is parity plus the
doc fix, not a live defect. The same call site has a second, unrelated gap —
`Kataglyphis::Shared::getBaseDir` (`Src/shared/util/FileReader.ixx:73-79`)
returns `""` for a path with no separator, and `ObjLoader.cpp:191-192` then
builds `"" + "/" + name`, so an OBJ opened by bare filename resolves its
textures against the **filesystem root**.

**Third, the dead state: `Window` keeps two members nothing reads and throws
away the one value that says whether it worked.** `window_width` /
`window_height` (`Window.ixx:34`) are read exactly once, by `glfwCreateWindow`
(`Window.cpp:64-65`); `framebuffer_size_callback` (`Window.cpp:107-113`) keeps
writing the resized values into them and no reader exists anywhere in `Src/` or
`Test/` (grep-confirmed — every consumer calls `glfwGetFramebufferSize`
directly: `VulkanRenderer.cpp:646,648`, `VulkanSwapChain.cpp:52`). Separately,
`initialize()` returns `int` and **both constructors discard it**
(`Window.cpp:33,45`), so a failed `glfwInit`/`glfwCreateWindow` leaves
`main_window == nullptr` and `App::run()` (`App.cpp:35-41`) walks straight into
`glfwCreateWindowSurface(instance, nullptr, …)` (`VulkanRenderer.cpp:1214-1220`)
— GLFW's `assert(window != NULL)` is compiled out in Release, so the release
build crashes with no diagnostic while the message it should have printed
("GLFW Window creation failed!") is already sitting in `initialize()`.

**Fourth, two more dead accessors — which contradicts batch IX's claim that the
`Src/**/*.ixx` accessor sweep was drained.** `GpuTimingSubsystem::passRecordedMask`
(`GpuTimingSubsystem.ixx:239`) and `sliceWasRecorded` (`:249-252`) have exactly
one occurrence each in the whole tree (their own definitions). `passRecordedMask`
additionally indexes `gpu_timing_pass_mask[imageIndex]` with no bounds check
while both of its siblings (`setPassRecordedMask` at `:245`, `sliceWasRecorded`
at `:251`) check — and the vector is empty whenever `create()` took its
unsupported-timestamps early return (`:83-88`), so the one accessor without a
guard is the one that would fire.

Ordering: all five are independent and touch disjoint files. Task 2 edits
`docs/model-loading.md`; nothing else in this batch touches docs.

Candidates found but NOT tasked (checked, then rejected — do not re-propose
without new evidence): **`Texture::createImage` not recording `in_mip_levels`
into `mip_levels`, so `createTextureSampler` builds `maxLod = 0` for Clouds,
SkyBox and CascadedShadowMap** — re-confirmed a third time and re-rejected for
the reason batches IV and V gave, with the extra note that all three of those
images are created with exactly one mip level, so `maxLod = 0` is the correct
value by accident and no sampling behaviour changes; **`BuildIntegrity.EveryCpuSuiteIsInTheWindowsCiFilter`**
— re-ran both of its parsers by hand this pass (45 defined suites vs 43 filter
entries plus `GoldenRender`/`Integration`); it is GREEN, unlike when batch VI
found it red, so do not re-file it; **the shader manifest vs the tree** —
cross-checked `Resources/ShadersSlang/shader-manifest.json` against every
`.slang` on disk: no manifest row names a missing file, and the eleven
unreferenced sources are all `import`-only (`common/*.slang`,
`raytracing/rt_types.slang`) or the `spike/` scratch pair, which is what
`EveryShaderSourceHasCompiledBinary` already encodes; **`sceneConfig::getModelMatrix()`'s
`#if NDEBUG` split** (`SceneConfig.cpp:116-131`) — both branches are
`glm::scale(identity, vec3(1,1,1))`, i.e. the same identity matrix, so the
conditional is dead; real, but a three-line deletion with no behavioural or
test consequence, so it is not worth an executor session on its own — fold it
into the next `SceneConfig` change; **`Scene::reloadModel` not calling
`resolveModelPath`** (`Scene.cpp:166-184`) unlike `loadAdditionalModel` — looks
like the same class as task 2, but its only caller already resolves
(`VulkanRenderer.cpp:390-394`), so the asymmetry is cosmetic;
**`DescriptorSetGroup::writeImageArray` re-calling `findBinding` and
dereferencing without a null check** (`DescriptorSetGroup.cpp:194-195`) —
`beginWrite` at `:192` already returned false if that lookup fails, so the
pointer cannot be null; a redundant lookup, not a defect.

### C++ Vulkan engine

### Rust WebGPU renderer (`third_party/OxidANT`)

## 2026-08-02 batch XI — planner (a frame-sync counter that erases itself one line after it is computed, a glTF loader that keeps the previous parse's mesh ranges, eleven raw-handle log lines, a dead overload, a conditional whose two branches are identical)

**The headline is a live regression that stops the engine rendering, shipped
2026-08-01 in `62e56684`.** `FrameSync::create`
(`Src/GraphicsEngineVulkan/renderer/FrameSync.ixx:33-43`) computes
`frame_sync_count = min(MAX_FRAME_DRAWS, imageCount)` on line 35 and then calls
`cleanUp(logicalDevice)` on line 38 — and `cleanUp` now ends with
`frame_sync_count = 0; current_frame = 0;` (`:117-118`), which `62e56684` added
so the failure paths could reuse it. The ordering was never revisited: the
count is wiped immediately, `image_available.resize(frame_sync_count)` and
`in_flight_fences.resize(frame_sync_count)` resize to **0**, and the
`for (i = 0; i < frame_sync_count; i++)` loop that creates the per-frame
semaphores and fences never executes. `VulkanRenderer::drawFrame` then takes
its `if (frameSync.frameSyncCount() == 0)` early return
(`VulkanRenderer.cpp:443`) on every single frame. `git show 62e56684^` confirms
the `cleanUp` call already sat above the resizes before that commit — it was
harmless only because `cleanUp` did not touch the counter then. **This survived
a full day of executor sessions precisely because host GPU golden verification
is the blocked path** (the fifteen `- [b]` entries above): nothing in the CPU
suites calls `create()`, so `FrameSyncUnit`'s three tests all still pass.

**Second, `GltfLoader::parseCpu` does not reset `meshRanges`, and
`sliceMeshRange` has no bounds guard to survive that.** `parseCpu`
(`GltfLoader.cpp:405-411`) clears `vertices`, `indices`, `materials`,
`materialIndex` and `textureImages` — five of the six arrays `adoptParsed`
moves (`:42-50`). `ObjLoader::parseCpu` clears all six, `meshRanges` included,
under an explicit comment (`ObjLoader.cpp:43-49`). So a second `parseCpu` on
one `GltfLoader` appends the new document's ranges after the previous
document's, and `uploadParsed` (`:99-109`) then feeds every one of them to
`sliceMeshRange` (`MeshRange.ixx:43-63`), which does
`vertices.begin() + range.vertexBase` and `indices[range.indexStart + i]` with
no size check — an out-of-bounds read, not a diagnosable error. Today every
`GltfLoader` is constructed fresh per parse (`AsyncModelParse.ixx:56`,
`Scene.cpp:37`, `:116`), so this is latent rather than firing; `loadModel`
(`:33-40`) is the reachable second-call path the moment anyone reuses an
instance, which is exactly what `ObjLoader`'s comment says it was written to
survive.

**Third, eleven `spdlog::info` lines print raw Vulkan handles.** Grep-confirmed
across exactly three files: `Rasterizer.cpp:113,146,243,348`,
`DeferredRasterizer.cpp:119,166,330,355,377`, `CascadedShadowMap.cpp:227,436`.
They fire once per swapchain image at startup **and again on every window
resize** (`recreateSwapChain` → `destroyFramebuffers` → `recreateFrameResources`),
so an ordinary resize on a triple-buffered swapchain emits a dozen
`0x7f…`-style lines at info level. No other stage does this — `PostStage`,
`SkyBox` and `Clouds` create and destroy the same objects silently — and a
handle address is useful only to a debugger, never to a user reading `logs/`.

**Fourth and fifth, two pieces of confirmed-dead code.**
`VulkanBufferManager` declares `createBufferAndUploadVectorOnDevice` twice
(`VulkanBufferManager.ixx:40-48` const-ref, `:50-57` non-const-ref); the
non-const body (`:111-128`) does nothing but `static_cast` to
`const std::vector<T>&` and call the other one. All five call sites
(`Mesh.ixx:130`, `CascadedShadowMap.cpp:320`, `VulkanRenderer.cpp:1315`,
`:1323`, `ASManager.cpp:273`) bind fine to the const overload. And
`sceneConfig::getModelMatrix` (`SceneConfig.cpp:116-131`) has an `#if NDEBUG`
split whose two branches are byte-identical `glm::scale(identity, vec3(1,1,1))`
— batch X found this, rejected it as too small to stand alone, and asked for it
to be folded into the next `SceneConfig` change; this is that change, paired
with the stale claim in `Camera.cpp:44-48` that "cascade splits are computed as
farPlane * (i / numCascades)", which `computeCascadeDataInto` stopped doing when
`shadowDistance` and the practical split scheme landed
(`CascadedShadowMapMath.cpp:124-161` — `shadowFar = min(shadowDistance, farPlane)`,
so the comment's 4000 → 1333 arithmetic describes code that no longer exists).

**Every task in this batch is verifiable with no GPU**, deliberately, for the
same reason batch X gave. Tasks 1, 2 and 5 land device-free unit tests in
suites that already run in Windows CI (`FrameSyncUnit`, `GltfParseUnit`,
`MeshRangeSlice`, `CameraSceneConfigUnit`); task 3 adds a grep-based
`BuildIntegrity` gate in the shape of the existing
`EngineSourcesUseNonThrowingFilesystemOverloads`; task 4 is a compile-verified
deletion. Every `file:line` below was read out of the tree this pass. The
actionable queue was empty when this batch was written.

Ordering: **task 1 first, and on its own** — it is a live rendering outage and
the other four are cleanups. Tasks 2–5 are independent and touch disjoint
files.

Candidates found but NOT tasked (checked, then rejected — do not re-propose
without new evidence): **`GpuTimingSubsystem::create`** — checked for the same
assign-then-`destroy()` ordering hazard as task 1; it calls `destroy(device)`
FIRST (`GpuTimingSubsystem.ixx:73`) and `destroy` touches no counters, so it is
correct; **`PostStage`/`Rasterizer`/`DeferredRasterizer`/`SkyBox`
`recreateFrameResources` leaking framebuffers** — traced
`VulkanRenderer.cpp:664-689`; all four `destroyFramebuffers()` calls run before
the corresponding `recreateFrameResources()`, so nothing leaks;
**`FrameCapture::invalidateFence` leaving `pending == true` across a swapchain
recreate** — real, but `buffer_size` only ever grows and `width`/`height` are
the *old* (already-fitting) extent, so `take()`'s memcpy stays in bounds; the
class comment already states this is intended; **`Texture::uploadRgba`'s
`mip_levels` vs `createImage`'s `in_mip_levels`** — the third-time-rejected
candidate from batches IV/V/X; `uploadRgba` sets the member before calling
`createImage`, so the sampler's `maxLod` is right on this path, and the three
single-mip callers are correct by accident as batch X recorded;
**`Shared::getBaseDir` returning `""` for a bare filename** (batch X's
second-half finding) — already fixed: `resolveObjTexturePath`
(`ObjLoader.ixx:20-28`) treats an empty base dir as `"."`, and
`ObjParseUnit.EmptyBaseDirStaysRelative` (`objParseSuite.cpp:403-411`) pins it;
**`GUISceneSharedVars::shadow_distance` / `cascade_split_lambda` having no GUI
control** — genuinely unexposed (`GUI.cpp:185-201` offers only resolution,
cascade count, PCF radius and intensity) while both are plumbed through to the
cascade math (`VulkanRenderer.cpp:198-199`) and documented as knobs worth
turning; left untasked because an ImGui panel addition has no device-free
oracle, so it cannot be verified while GPU goldens are blocked — pick it up in
the first batch after that unblocks; **`extractImageBytes` counting a padding
byte at `b64[len-2]` without checking `b64[len-1]`** (`GltfLoader.cpp:228-231`)
— only misreads input that is already invalid base64, and the length guard
above it makes the result harmless.

### C++ Vulkan engine

### Rust WebGPU renderer (`third_party/OxidANT`)

## 2026-08-02 batch XII — planner (refactor: a sixth "one rule, five hand-rolled copies" — render-pass begin; four copies of the extension/layer lookup that already produced one shipped bug; a descriptor write that looks its binding up twice and dereferences the second one unchecked)

**Every task in this batch is verifiable with no GPU**, deliberately: the `- [b]`
entries above are still blocked on host GPU golden verification in this RDP
session. Tasks 1 and 2 land device-free `constexpr`/pure helpers with new gtest
suites that run in the container CPU lane; task 3 is a compile-verified
restructure of private helpers with no behaviour change. Every `file:line`
below was read out of the tree this pass, and every duplication count was
confirmed by a whole-tree grep over `Src/` + `Test/`. The actionable queue was
empty when this batch was written (the only `- [ ]`-shaped entries in this file
are `- [b]`).

**The headline is that `vk::RenderPassBeginInfo` is the sixth member of the
`buildAttachmentDescription` / `fullExtentViewport` / `buildFramebufferCreateInfo`
family, and it has drifted in exactly the way that family exists to prevent.**
Five files each spell out the same six-field block by hand:
`Rasterizer.cpp:73-85`, `PostStage.cpp:68-81`, `DeferredRasterizer.cpp:381-395`,
`SkyBox.cpp:412-423` and `CascadedShadowMap.cpp:461-471`. All five set
`renderArea.offset = {0, 0}` and a full-extent `renderArea.extent`, and all five
are followed immediately by `setFullExtentViewportAndScissor` on the same
extent. **Two of the five hard-code `clearValueCount` instead of deriving it**
— `SkyBox.cpp:422` writes `clearValueCount = 2` next to a two-element
`std::array` and `CascadedShadowMap.cpp:469` writes `clearValueCount = 1` — which
is byte-for-byte the bug `buildFramebufferCreateInfo` was introduced to kill
(`SkyBox`'s `attachmentCount = 2`, pinned today by
`FramebufferHelperUnit.AttachmentCountIsDerivedFromTheSpan`). Two others
(`Rasterizer.cpp:73`, `PostStage.cpp:68`) declare the struct without `{}` while
the other three value-initialize.

**Second: "is this name in the list vulkan-hpp just enumerated" is written four
times, and one of the four already shipped a bug.** `VulkanInstance.cpp:91-102`
(layers), `VulkanInstance.cpp:113-127` (instance extensions),
`VulkanDevice.cpp:422-427` (the `isExtensionSupported` lambda) and
`VulkanDevice.cpp:659-670` (`check_device_extension_support`) are four
independent nested loops around `strcmp(..., props.{extension,layer}Name) == 0`.
The 2026-07-23 deep-review pass fixed `check_instance_extension_support`'s
`strcmp(...) != 0 != 0` — which parsed as `(strcmp != 0) != 0` and made the
function effectively always return true — in **one** of those four copies; the
other three were correct by luck, not by construction. `check_device_extension_support`
also carries an `extensions.empty()` early-return the other three lack, and
`check_instance_extension_support` still takes its input as a mutable
`std::vector<const char *> *` (`VulkanInstance.ixx:27`) where the body only
iterates it.

**Third: `DescriptorSetGroup::writeImageArray` looks its binding up twice and
dereferences the second result without a null check.** `beginWrite`
(`DescriptorSetGroup.cpp:136-149`) already calls `findBinding` and returns false
when it misses, but `writeImageArray` (`:187-207`) throws that away, calls
`findBinding(binding)` again at `:194` and reads `layout_binding->descriptorCount`
at `:195` with no guard. It is unreachable today only because the first lookup
succeeded — the deref is a static-analysis finding waiting to be re-derived, and
the second lookup is a second linear scan. `checkWritePreconditions` (`:127-134`)
has exactly one caller, `beginWrite`. Grep-confirmed: `findBinding`,
`checkWritePreconditions` and `beginWrite` have zero callers outside
`DescriptorSetGroup.cpp`.

Ordering: all three are independent and touch disjoint files.

Candidates found but NOT tasked (checked, then rejected — do not re-propose
without new evidence): **the twelve hand-rolled `vk::ImageMemoryBarrier`
blocks** — still owned by the `- [b]` cloud-barrier entry above, unchanged since
batch IX rejected it; pick it up once host GPU verification unblocks;
**`Src/KomputePlayground`** — `KATAGLYPHIS_BUILD_KOMPUTE_PLAYGROUND` is OFF by
default (`Src/CMakeLists.txt:103`) and its `CMakeLists.txt` has the entire
kompute acquisition block commented out while still linking `kompute::kompute`,
so turning the option ON cannot configure; and `src/main.cpp:40` throws
`std::runtime_error` with no `<stdexcept>` include. Real, but "repair it" vs
"delete it" is an owner decision, not an executor one — raise it, do not task
it; **`FormatHelper.hpp:5`'s unused `#include <stdexcept>`** in a
`-fno-exceptions` build and **the ~25 redundant `= false` assignments on the
value-initialized `vk::PhysicalDevice*Features` structs**
(`VulkanDevice.cpp:331-362`) — both genuine noise, both too small to spend a
task on alone; fold them into whichever of the above touches those files;
**batching the four `updateDescriptorSets(1, &write, ...)` calls in
`DescriptorSetGroup`** — one vkUpdateDescriptorSets per descriptor is real, but
it is initialization-time only and the API change would ripple into every write
call site for no measurable gain; **the Rust `render/{bloom,ssao,tonemap,
overlay,gpu_occlusion}.rs` modules having no tests** — every one of them needs a
live wgpu adapter to exercise, so a coverage task there would ship
compile-only verification (`obj_to_gltf.rs` looked untested by inline
`#[test]` count but has a 650-line integration suite at
`crates/webgpu_renderer/tests/obj_to_gltf.rs`; do not re-flag it).

### C++ Vulkan engine


## 2026-08-02 batch XIII — planner (the Slang port of `ibl.wgsl` dropped an entry point the Rust code still creates a pipeline for, and the GGX importance sampling under it; a frame capture that assumes every swapchain format is 4 bytes; a G-buffer format list written twice)

The actionable queue was empty when this batch was written (0 `- [ ]`, 15
`- [b]` across the whole file). Every `file:line` below was read out of the tree
this pass.

**The headline is a shipped regression in `third_party/OxidANT`,
introduced by `2a4ae68 "Replace hand-written WGSL shaders with Slang-emitted
output"`.** Comparing every generated WGSL against its pre-Slang predecessor
(`git show 2a4ae68^:crates/webgpu_renderer/src/shaders/<name>.wgsl`), nine of
the ten kept exactly their old entry-point count. `ibl.wgsl` went **6 → 5**:

| shader | lines before → after | entry points before → after |
| --- | --- | --- |
| forward | 613 → 692 | 5 → 5 |
| bloom | 60 → 93 | 4 → 4 |
| ssao | 137 → 217 | 3 → 3 |
| occlusion_bbox | 77 → 37 | 2 → 2 |
| **ibl** | **312 → 254** | **6 → 5** |

The one that vanished is `fs_downsample_cube`, and
`crates/webgpu_renderer/src/render/ibl.rs:321` still does
`downsample_cube: make("fs_downsample_cube", CUBE_FORMAT, "ibl_downsample_cube")`
inside `Precompute::new`, against a module built from
`include_str!("../shaders/ibl.wgsl")` (`:236`). `grep -c downsample
src/shaders/ibl.wgsl` = **0**. So constructing `Precompute` — which
`IblEnvironment::bake` (`forward.rs:1172`) does on every environment bake —
names an entry point that does not exist in the module.

Under that, three more things were lost in the same rewrite, all of them
load-bearing and all of them documented as such in the deleted comments:

1. **`fs_prefilter` reads `roughness` and never uses it.** `ibl.slang:93` binds
   `float roughness = params.face_roughness_samples_mip.y;` and lines 100-114
   never mention it again — confirmed in the emitted WGSL, where
   `fs_prefilter` (`ibl.wgsl:169-200`) contains no `.y` read at all. `tangent`
   (`:96`) and `bitangent` (`:97`) are likewise dead: `h` is built in a local
   frame and then used directly against `dir` in world space. And
   `hammersley` is hard-coded to `float2(float(i)/float(sampleCount), 0.0) //
   simplified` (`:102`, `:129`), so `phi` is always 0 and all N samples lie on
   one arc. Net: `ibl.rs:596` computes a per-mip roughness, packs it into the
   uniform, and the shader throws it away — **all five mips of the prefiltered
   cube are the same convolution.**
2. **The equirect source is sampled, not loaded.** `ibl.slang:60` is
   `srcEquirect.Sample(srcSampler, uv)`. The bind-group layout at
   `ibl.rs:245-256` declares binding 1 as
   `TextureSampleType::Float { filterable: false }` with the comment "Not
   filterable, and not sampled: `sample_equirect` uses textureLoad" — the
   function `sample_equirect` no longer exists. The old shader hand-rolled a
   bilinear fetch that **wrapped** longitude, specifically so ClampToEdge would
   not darken a one-texel seam down the -X meridian of every derived map.
3. **`fs_irradiance` went from midpoint to left-endpoint quadrature** and from
   128×64 to 64×32 steps (`ibl.slang:72-79`: `phi = float(phi_i) * phiStep`).
   The deleted comment states the measured cost of exactly this: a constant
   environment convolves to ~0.997 of itself instead of ~0.9997.

**The tests that would have caught (1) already exist and are correct.**
`crates/webgpu_renderer/tests/ibl.rs:184`
`higher_roughness_prefilter_mips_are_strictly_blurrier` asserts variance falls
with every mip and that mip 4 is under 25% of mip 0's variance — it cannot pass
against a roughness-independent prefilter. It never ran, because every test in
that file opens with `let Ok(gpu) = GpuContext::new_headless() else {
eprintln!("SKIP: no GPU adapter available in this environment"); return; };`
and a skip is indistinguishable from a pass in the CI log. That is why tasks 1
and 3 below exist as separate items: one fixes the shader, the other makes the
absence of a GPU stop being silent.

Candidates found but NOT tasked this cycle (re-verify next pass): `Model::cleanUp`
(`Model.cpp:26-37`) still does not `meshes.clear()` while clearing every other
container, and `Scene::cleanUp` (`Scene.cpp:180`) clears nothing at all, so the
destructor re-walks already-released objects — safe only because every leaf
`cleanUp()` happens to be idempotent, an invariant nothing tests; `Model::addSampler`
(`Model.cpp:66-80`) still creates one `vk::Sampler` per texture differing only in
`maxLod`; `VulkanDevice.cpp:586/589/592` take `.value` off three
`getSurface*KHR` calls with no result check, which is the tail of batch III's
unchecked-results finding.

### C++ Vulkan engine


## 2026-08-02 batch XIV — planner (the same Slang port that broke `ibl` also dropped the forward pass's analytic ambient specular and two host-side strength controls; the irradiance quadrature it downgraded; the gate that would have caught all of it; six unchecked surface queries; one sampler per texture)

The actionable queue was empty when this batch was written (0 `- [ ]`, 15
`- [b]` across the whole file). Every `file:line` below was read out of the tree
this pass; the "before" side comes from
`git show 2a4ae68^:crates/webgpu_renderer/src/shaders/<name>.wgsl` inside
`third_party/OxidANT`.

**Batch XIII found three semantic regressions in `ibl.slang` and closed two of
them. It stopped at the entry-point count, and that is why it missed the
bigger one.** Comparing *reachable functions* instead of entry points,
`forward.wgsl` lost two whole functions that its `.slang` source still defines:

| helper | in `forward.slang` | in emitted `forward.wgsl` | called by |
| --- | --- | --- | --- |
| `hemisphere_irradiance` | `:277` | `hemisphere_irradiance_0` (`:575`) | `:420` |
| `sky_radiance` | `:260` | **absent** | **nothing** |
| `env_brdf_approx` | `:283` | **absent** | **nothing** |

Slang dropped them because nothing calls them — they are dead in the source.
`grep -n "sky_radiance\|env_brdf_approx" Resources/ShadersSlang/forward/forward.slang`
returns only the two definitions, no call sites. `occlusion_bbox` was checked
the same way and is faithful (77 → 37 lines is comment loss only), so this is
`ibl` and `forward`, not a systematic emitter fault.

What the old `fs_main` did between the pre-Slang lines 572 and 605, and what
`forward.slang:407-421` does now:

1. **The analytic specular ambient is gone.** Old:
   `env_analytic = mix(sky_radiance(reflected, true), irradiance_analytic, roughness*roughness)`,
   then `specular_analytic = env_analytic * env_brdf_approx(f0, roughness, n_dot_v)`,
   `select`ed against the split-sum path on `env_enabled`. New: the `else`
   branch is `ambient = hemisphere_irradiance(n) * albedo.rgb * (1.0 - metallic)`
   and nothing else. For `metallic = 1` that is **identically zero** — with no
   IBL environment bound, a metal renders black except for direct and punctual
   light.
2. **`enabled_maxmip_intensity.z` (environment intensity) is never read.** The
   host writes it (`crates/webgpu_renderer/src/render/forward.rs:2619`,
   `[1.0, max_mip, intensity, 0.0]`); the shader reads `.x` at `:409` and `.y`
   at `:413` and stops. Old code multiplied both `irradiance_env` and
   `prefiltered` by it. The control is inert.
3. **`light_dir_ambient.w` (ambient strength) is never read.** Old:
   `ibl_strength = frame.light_dir_ambient.w` scaling the whole ambient term.
   `forward.slang:28` still documents the field as "w: ambient" and
   `forward.rs:335` still documents it as "scales both IBL paths alike"; the
   default is `0.35` (`forward.rs:931`). Also inert.
4. **The `(1 - k_s)` Fresnel factor on the diffuse ambient is gone.** Old:
   `diffuse_ibl = (1 - fresnel_schlick(n_dot_v, f0)) * (1 - metallic) * albedo * irradiance`.
   New drops the Fresnel term entirely.

The deleted comment on the old `select`-not-`mix` choice states the reason the
fallback path had to stay bit-exact: "every golden test in the suite depends on
it."

Candidates found but NOT tasked this cycle (re-verify next pass): `Model::cleanUp`
(`Src/GraphicsEngineVulkan/scene/Model.cpp:26-38`) still clears `modelTextures`
and `modelTextureSamplers` but not `meshes`, and `Scene::cleanUp`
(`Scene.cpp:186-189`) clears nothing, so `Scene::reloadModel` (`:166-169`) runs
`cleanUp()` and then destroys the models, re-walking every already-released
`Mesh` — safe only because each leaf `cleanUp()` happens to be idempotent, an
invariant nothing tests; `ssao.slang` and `bloom.slang` were not compared
function-by-function against their pre-Slang originals (both *grew*, so they are
lower risk, but task 3 below would settle them mechanically).

### Rust WebGPU renderer

### Cross-renderer

### C++ Vulkan engine

## 2026-08-03 batch — planner (refactor: the seventh and eighth members of the "one rule, five hand-rolled copies" family — render-pass creation and pipeline-layout creation, both of which SkyBox again hard-codes a count in; 22 generated Sphinx artifacts that `.gitignore` already claims to exclude but git still tracks)

The actionable queue was empty when this batch was written (0 `- [ ]`, 15
`- [b]` across the whole file). Every `file:line` below was read out of the tree
this pass; the git-state claims in task 2 come from
`git ls-files -i -c --exclude-standard` run against this checkout.

**Every task in this batch is verifiable with no GPU**, deliberately: the
fifteen `- [b]` entries above are still blocked. Tasks 1 and 3 land device-free
`constexpr` helpers with new gtest suites that run in the container CPU lane;
task 2 is a git-tracking + `.gitignore` change gated by a new Pester suite.

**The headline is that the `buildAttachmentDescription` /
`fullExtentViewport` / `buildFramebufferCreateInfo` / `buildRenderPassBeginInfo`
family has two more members that never got their helper, and SkyBox hard-codes a
count in both of them.** `SkyBox.cpp` now has *four* places where a literal count
sits next to the container it should have been derived from: the framebuffer
`attachmentCount = 2` (fixed by `4f799788`), the render-pass-begin
`clearValueCount = 2` (fixed by `c041b756`), and the two still open below —
`renderPassInfo.attachmentCount = 2` (`SkyBox.cpp:303`) next to
`std::array attachments = {colorAttachment, depthAttachment}` (`:260`), and
`pipelineLayoutInfo.setLayoutCount = 2` (`SkyBox.cpp:347`) next to
`std::array<vk::DescriptorSetLayout, 2> combinedLayouts` (`:345`). That is the
same defect class the family exists to kill, in the same file, twice more.

**Second, the same two seams carry the same result-handling drift as the
framebuffer one did.** `vk::RenderPassCreateInfo` is spelled out by hand in
`Rasterizer.cpp:206-212`, `PostStage.cpp:246-252`,
`DeferredRasterizer.cpp:276-282`, `SkyBox.cpp:302-308` and
`CascadedShadowMap.cpp:177-184`; **PostStage alone** uses the two-out-param
`createRenderPass(&info, nullptr, &render_pass)` overload (`:254-255`) where the
other four use `ResultValue` — byte-for-byte the deviation batch IX found in the
framebuffer copies. `vk::PipelineLayoutCreateInfo` is worse: nine hand-written
blocks across eight files (`Clouds.cpp:178`, `Raytracing.cpp:253`,
`SkyBox.cpp:346`, `Rasterizer.cpp:318`, `PathTracing.cpp:197`,
`PostStage.cpp:271`, `CascadedShadowMap.cpp:384`, `DeferredRasterizer.cpp:305`
and `:330`) with **four different result-handling shapes** —
`ResultValue` + `ASSERT_VULKAN(VkResult(...))`, `ResultValue` +
`ASSERT_VULKAN(...result)`, the two-out-param overload, and Rasterizer's
hand-rolled `if (result == eSuccess) {...} else { ASSERT_VULKAN(...) }`
(`Rasterizer.cpp:324-329`), which is the only site where the assert is reached
through a branch rather than unconditionally.

**Third: 22 generated files are tracked in git that `.gitignore` already tries
to exclude.** `git ls-files -i -c --exclude-standard` returns 21 paths, every one
of them under `docs/build/` — Sphinx output, last touched 2026-05-16 (`c69e03b8`)
while `docs/source/` moved on 2026-07-31 (`16020e28`), so the committed HTML is
two months stale relative to the sources that generate it. `.gitignore` names the
directory **twice** (`docs/build/**/*` at line 11, `docs/build/*` at line 85), and
neither rule can take effect because git ignores exclude patterns for files
already in the index. A 22nd generated file,
`docs/source/__pycache__/conf.cpython-313.pyc`, is tracked with **no** ignore rule
covering it at all.

Ordering: **task 1 before task 3** — both touch `Rasterizer.cpp`, `PostStage.cpp`,
`DeferredRasterizer.cpp`, `SkyBox.cpp` and `CascadedShadowMap.cpp`, so doing them
in the other order or interleaved invites conflicts. Task 2 is disjoint.

Candidates found but NOT tasked (checked, then rejected — do not re-propose
without new evidence): **the twelve hand-rolled `vk::ImageMemoryBarrier`
blocks** — still owned by the `- [b]` cloud-barrier entry above (the two cloud
barriers are `VulkanRenderer.cpp:882-900` and `:914-932`), unchanged since
batches IX and XII rejected it; **the five copies of the blocking
`map_async` + channel + `poll` + `get_mapped_range` + `unmap` readback in the
Rust crate** (`render/forward.rs:2315`, `render/ibl.rs:898`,
`render/histogram.rs:329` and `:367`, `render/gpu_timing.rs:476`) — a real
five-way duplication, but every extracted unit needs a live wgpu adapter, so the
refactor would ship with compile-only verification; **`render/histogram.rs` and
`render/gpu_occlusion.rs` having zero inline tests** — same adapter problem, and
the CPU-side maths they mirror is already covered by `render/auto_exposure.rs`'s
16 tests; **`CommandBufferManager::beginCommandBuffer` allocating a
`std::vector<vk::CommandBuffer>` for exactly one handle**
(`CommandBufferManager.cpp:29`) — a genuine needless heap allocation, but it is
upload/transition-time only, never on the frame path, and too small to spend a
task on alone; fold it in if something else touches that file.
**`Src/KomputePlayground`** — unchanged since batch XII raised it; still an owner
decision ("repair" vs "delete"), still not an executor task.

### C++ Vulkan engine

### Docs / repo hygiene

## 2026-08-03 batch II — planner (both model loaders upload the same image once per material that references it, straight into a 128-slot cap; a glTF walk that renders every scene in the document at once while the Rust twin renders one; three subsystems that hand-roll the descriptor triad `DescriptorSetGroup` exists to own; a config transform stamped onto model 0 whatever was loaded; a fourth copy of the shadow-resolution table size)

The actionable queue was empty when this batch was written (0 `- [ ]`, 15
`- [b]` across the whole file). Every `file:line` below was read out of the tree
this pass.

**Every task in this batch is verifiable with no GPU except task 3**, which is a
Vulkan-object refactor and says so in its own Test section. The other four are
provable with device-free gtest suites that already exist and already construct
the objects under test without a `VulkanDevice`: `GltfParseUnit.*`
(`gltfParseSuite.cpp` — 28 tests, all on `parseCpu` + the device-free accessors),
`ObjParseUnit.*` (`objParseSuite.cpp` — 18 tests, same shape),
`SceneAccessorUnit.*` (`sceneAccessorSuite.cpp:18-31` builds a bare `Scene` and
`std::make_shared<Model>()` with no device) and `ShadowResolutionUnit.*`
(`guiSceneVarsRoundTripSuite.cpp:193`).

**The headline is that neither model loader deduplicates an image that several
materials share, and the global texture array they feed is capped at 128 slots.**
`GltfLoader::parseCpu` (`GltfLoader.cpp:434-449`) extracts encoded bytes per
*material*: `objMaterial.textureID = textureImages.size(); textureImages.push_back(bytes)`.
Two materials pointing at the same `cgltf_image` therefore produce two copies of
the same PNG in `textureImages`, which `uploadParsed` (`:69-83`) decodes and
uploads as two independent `Texture`s. `ObjLoader::loadTexturesAndMaterials`
(`ObjLoader.cpp:223-225`) does the identical thing keyed on `diffuse_texname`:
`textures.push_back(resolveObjTexturePath(...)); material.textureID = texture_id++`,
so an `.mtl` where five materials share `wood.png` reads, decodes and uploads that
file five times. The cost is not only startup time and VRAM: every duplicate
consumes one entry of the flattened global array whose cap is
`MAX_TEXTURE_COUNT = 128` (`common/host_device_shared_vars.hpp:8`), enforced by
`planFlattenedTextureSlots` (`scene/ObjectDescription.ixx:66-84`) and reported by
`VulkanRenderer.cpp:1537-1544` as "models past the cap will sample the wrong
slots". A 40-material glTF with 8 distinct images currently spends 40 slots
instead of 8. This is the same defect class as `dca11022` (dedup `Model` texture
*samplers* by mip level) one level up the pipeline — that change deduplicated the
samplers while leaving the images they sample duplicated.

**Second, the two glTF loaders disagree about which nodes are in the scene.** The
Rust loader takes `document.default_scene().or_else(|| document.scenes().next())`
and recurses that scene's roots (`asset/gltf_loader.rs:163-170`); the C++ loader
iterates `data->nodes` — every node in the *document*
(`GltfLoader.cpp:457-475`). For a single-scene file the two agree. For a
multi-scene file the C++ renderer merges every scene into one, and a node present
in the document but referenced by no scene (legal glTF, and what most DCC tools
leave behind when a collection is disabled on export) is rendered by C++ and not
by Rust. This is the fifth entry in the cross-renderer-divergence family
(`bd315707` `map_Kd` backslashes, `6aa5eec6` the `Resources/` search,
`d25cd1e5` index validation, `92df2e7f` adapter absence).

Tasks 3, 4 and 5 are cleanups with a real failure mode behind each: three
subsystems hand-rolling the descriptor layout/pool/allocate triad that
`DescriptorSetGroup` was extracted to own — including a *hand-computed* pool size
with the arithmetic left in a comment (`Clouds.cpp:83`, `descriptorCount = 2; // +1 for noise`);
a scene-config transform applied to model index `0` no matter which model was just
appended; and a fourth hand-written copy of "the shadow-resolution table has four
entries".

Ordering: tasks 1 and 2 both edit `GltfLoader.cpp` — **do task 1 first**, since it
changes the material loop task 2 leaves alone, and doing them in the other order
puts task 2's traversal rewrite under task 1's diff. Tasks 3, 4 and 5 are disjoint
from those and from each other.

Candidates found but NOT tasked (checked, then rejected — do not re-propose
without new evidence): **`GUI.cpp:319`'s hand-rolled `vk::DescriptorPoolCreateInfo`**
— it is the pool handed to `ImGui_ImplVulkan_Init`, which allocates its own sets
outside `DescriptorSetGroup`'s model, so it cannot use the helper and must stay;
**`CommandBufferManager::beginCommandBuffer` allocating a `std::vector<vk::CommandBuffer>`
for one handle (`CommandBufferManager.cpp:29`)** — re-confirmed and re-rejected for
the reason the 2026-08-03 batch gave (upload/transition-time only, never on the
frame path); **`MeshDrawRecorder` computing `glm::inverse(glm::transpose(model))`
per model per pass (`MeshDrawRecorder.cpp:35`)** — real redundant work, but
`recordSceneMeshDraws` has exactly two callers (`Rasterizer.cpp:91`,
`DeferredRasterizer.cpp:394`) and only one runs per frame, so it is one 4x4
inverse per model per frame; **the C++ glTF loader reading only `TEXCOORD_0` and
ignoring each texture's `texcoord` index (`GltfLoader.cpp:277-279`)** — a genuine
divergence from `uv_set_bit` in the Rust loader, but `Vertex` carries a single UV
set, so closing it is a vertex-format change, not a loader fix; **the
checked-in generated WGSL having no *content* gate** (`CheckedInWgslIsNotOlderThanItsSlangSource`
compares mtimes only, `buildIntegritySuite.cpp:1220`) — a real hole, but closing it
means running `slangc` inside the test or diffing after a compile, and the
combined WGSL emit is skipped below `minSlangcVersionForWgsl`, which the
`- [b]` ANTfrastructure SDK-bump entry above still blocks.

### C++ Vulkan engine

## 2026-08-03 batch III — planner (the one scene-changing path that forgets to rebind the acceleration structure it just destroyed, plus the gate that would have caught it; a GUI transform that re-applies the 60× scale SceneConfig deleted; a path-tracing history that ignores the light it integrates; an OBJ→glTF converter that drops the vertex colours the C++ loader honours)

The actionable queue was empty when this batch was written (0 `- [ ]`, 15
`- [b]` across the whole file). Every `file:line` below was read out of the tree
this pass.

**The headline is that three code paths change the scene and only two of them
rebind the descriptors afterwards.** `finishModelLoad`
(`VulkanRenderer.cpp:243-253`) and `addModel` (`:727-758`) both run
`createASForScene` → `rebuildObjectDescriptions` → **`updateAllDescriptorSets()`**.
`handleModelReloadRequest` — the GUI model-picker path (`:380-405`) — runs
`createASForScene` → `rebuildObjectDescriptions` →
**`updateTexturesInSharedRenderDescriptorSet()`** and stops there.
`updateRaytracingDescriptorSets()` (`:1346-1362`) is reachable *only* through
`updateAllDescriptorSets()` (`:760-769`), so after picking a different model
from the combo the raytracing descriptor sets still hold the
`vk::AccelerationStructureKHR` that `ASManager::createTLAS` destroyed at
`ASManager.cpp:230-233` — a destroyed handle bound at `TLAS_BINDING` for every
swapchain image, dispatched against on the next frame in which
`guiRendererSharedVars.raytracing || .pathTracing` is set. The same omission
also skips `updatePostDescriptorSets`, `updateGBufferDescriptorSets` and the
`pathTracingAccumulatedFrames = 0` reset that `updateRaytracingDescriptorSets`
performs precisely because "any accumulated history predates that world"
(`:1353-1359`). `goldenRenderSuite.cpp:2280-2341` already pins the equivalent
invariant for `addModel` ("red-proven by removing the addModel AS rebuild"); the
reload path has no such test.

**Second, the GUI model transform re-applies a scale the engine deliberately
removed.** `sceneConfig::getModelMatrix()` returns identity, and its comment
(`SceneConfig.cpp:116-124`) explains why: "the old 60x scale for the tiny
viking_room made the camera start INSIDE the geometry (all backfaces, culled ->
black viewport) and stretched the scene far beyond a cascade's useful
resolution". `handleModelTransformChange` (`VulkanRenderer.cpp:342-343`) still
opens with `glm::scale(modelMatrix, glm::vec3(60.0f, 60.0f, 60.0f)); //
Apply original scale`. `model_position`/`model_rotation` default to all-zero
(`GUISceneSharedVars.ixx:90-91`), so the *first* one-pixel drag of the Position
or Rotation widget (`GUI.cpp:101-106`) jumps the loaded model from 1× to 60×.

**Third, the path tracer's temporal accumulation is invalidated by camera and
quality changes but not by the light it integrates.**
`recordRaytracingOrPathTracing` (`VulkanRenderer.cpp:1102-1110`) resets
`pathTracingAccumulatedFrames` when the view matrix, samples-per-pixel or
max-bounces changed. `path_tracing.slang` reads `sceneUBO.dirLight.direction`
(`:227`) and `sceneUBO.dirLight.color` rgb + w-as-intensity (`:253-255`) inside
the NEE loop and folds the result into the running mean at `:282-292`. Dragging
the directional-light direction, colour or radiance in the GUI therefore leaves
frames lit by the *old* light weighted into the mean, decaying only as `k/N`
with no reset — the same defect the view-matrix check exists to prevent.

**Fourth, the OBJ→glTF converter drops per-vertex colours that the C++ loader
honours.** `ObjLoader::loadVertices` reads `attrib.colors` into `Vertex::color`
(`ObjLoader.cpp:336-342`) and the Rust glTF loader reads `COLOR_0`
(`asset/gltf_loader.rs:492-495`, `:530`), pinned on the C++ side by
`GltfParseUnit.ReadsColor0VertexColours` (`gltfParseSuite.cpp:682`). But
`asset/obj_to_gltf.rs`'s `"v"` arm (`:200-209`) reads exactly three components
and silently ignores the optional `r g b` of `v x y z r g b`, and `to_gltf`
emits `"attributes": { "POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2 }` (`:455`)
with no colour accessor (`:537-541`). A vertex-coloured OBJ renders tinted in
the Vulkan engine and flat-white in the WebGPU one — the sixth entry in the
cross-renderer-divergence family (`bd315707` `map_Kd` backslashes, `6aa5eec6`
the `Resources/` search, `d25cd1e5` index validation, `92df2e7f` adapter
absence, `c7b66fa5` the default-scene walk).

Ordering: **task 1 before task 2** — task 2's gate asserts the helper task 1
introduces. Tasks 3 and 4 also edit `VulkanRenderer.cpp` but in disjoint regions
(`:336-378` and `:1096-1122` versus task 1's `:243-253`/`:380-405`/`:760-769`);
still, do them one at a time. Task 5 is in the Rust submodule and disjoint from
all four.

**GPU verification is currently unavailable** (see the `- [b]` RDP entry above),
so every task below lands a device-free gate or unit test as its primary
evidence, and names the GPU golden only as the secondary check to run once a
console session exists.

Candidates found but NOT tasked (checked, then rejected — do not re-propose
without new evidence): **`ASManager::cleanUp()` is not idempotent**
(`ASManager.cpp:382-402` destroys `tlas.vulkanAS` and every `blas[i].vulkanAS`
without nulling them or clearing `blas`, so a second `cleanUp()` — or a
`createASForScene()` after one — double-destroys) — a real latent double-free
and the odd one out against the "Idempotent: safe to call again" convention
`Clouds::cleanUp`, `CascadedShadowMap::cleanUp` and `DeferredRasterizer::cleanUp`
all state, but no call site reaches either sequence today and the fix has no
device-free test; **`CascadedShadowMap::recordCommands` binding the light-matrices
set at index 0 when the shared span is empty** (`CascadedShadowMap.cpp:421-423`,
a pipeline-layout mismatch where `Clouds::recordComputeCommands` bails instead,
`Clouds.cpp:149-152`) — same unreachability, same lack of a device-free test;
**an absolute `map_Kd` path** (`resolveObjTexturePath` prefixes `base + "/"`
unconditionally, `ObjLoader.cpp:174`; the Rust twin does the same with
`obj_path.with_file_name(uri)`, `obj_to_gltf.rs:635`) — both renderers are wrong
*identically*, so it is a shared limitation, not a divergence, and no shipped
asset uses one; **`assignTextureOffsets` advancing past `MAX_TEXTURE_COUNT`**
(`scene/ObjectDescription.ixx:29-36`) — every shader already clamps
(`int textureId = clamp(int(obj.texture_offset) + material.textureID, 0,
MAX_TEXTURE_COUNT - 1)` in all five of `rasterizer`, `deferred`, `path_tracing`,
`raytrace.rchit`, `shadow_map`), so an over-cap model samples a wrong slot, not
out of bounds, which is exactly what `VulkanRenderer.cpp:1537-1544` already
warns; **`CommandBufferManager::beginCommandBuffer` allocating a
`std::vector<vk::CommandBuffer>` for one handle** — re-confirmed and re-rejected
for the third time (upload-time only, never on the frame path).
**`Src/KomputePlayground`** — unchanged; still an owner decision.

### Rust WebGPU renderer

## 2026-08-03 batch IV — planner (a cloud shader that marches toward the ground instead of the sun; a light-direction slider that can be set to zero and NaN six shaders; a post push-constant block where three of four fields are read by nobody; two OBJ loaders that each mishandle a missing `vn`, in opposite directions)

The actionable queue was empty when this batch was written (0 `- [ ]`, 15
`- [b]` across the whole file). Every `file:line` below was read out of the tree
this pass.

**The headline is that six shaders read `sceneUBO.dirLight.direction` and one of
them disagrees with the other five about which way it points.**
`rasterizer.slang:52`, `deferred.slang:119`, `raytrace.rchit.slang:84` and
`path_tracing.slang:227` all compute `L = normalize(-sceneUBO.dirLight.direction.xyz)`
— the field is the direction the light *travels*, so the vector *toward* the
light is its negation. `compute/clouds.slang:85` names its local
`directionToLight` and assigns it `normalize(scene.dirLight.direction.xyz)`,
**without the negation**, then marches density along it from each sample
(`:91-94`) and feeds the same unnegated vector to the Henyey-Greenstein phase
term (`:169`). With the default light `(-0.55, -1.0, -0.35)`
(`GUISceneSharedVars.ixx:35`) the sun is overhead, so cloud self-shadowing
integrates *downward, away from the sun*, and forward scattering behaves as
back scattering. The host writes this field in exactly one place
(`VulkanRenderer.cpp:179-182`), so there is no second convention to blame.

**Second, that same field can be set to the zero vector from the GUI, and only
one of its consumers survives it.** `GUI.cpp:183` is a
`SliderFloat3("Light Direction", ..., -1.F, 1.0F)` with no length constraint;
`updateUniforms` copies the three floats into `sceneUBO.dirLight.direction`
verbatim. `CascadedShadowMapMath.cpp:183-185` defends itself
(`if (glm::length(light_direction) < 1e-6F) { light_direction = glm::vec3(0.0F, -1.0F, 0.0F); }`)
— the six shader sites above do not, and `normalize(float3(0))` is a NaN. The
cascade fit therefore keeps working against a fallback direction while every
lighting shader goes NaN, which is also a silent host/device disagreement
whenever the vector is merely non-unit.

**Third, `PushConstantPost` ships 16 bytes per post-pass draw and the shader
reads 4 of them.** `post.slang:19-26` declares `aspect_ratio`,
`clouds_enabled`, `shadows_enabled`, `skybox_enabled`; `fs_main` (`:45-65`)
references only `pc_post.clouds_enabled`. The host fills all four
(`PostStage.cpp:83-92`), `recordCommands` takes `shadowsEnabled`/`skyboxEnabled`
parameters to do it (`:62-67`), `VulkanRenderer.cpp:1000` threads two GUI
booleans down to feed them, and two separate gates pin the layout of the dead
fields (`buildIntegritySuite.cpp:749-753`, `pushConstantSuite.cpp:125-131`).
The real skybox toggle is a *different* push constant that the skybox shader
actually reads (`skybox.slang:20`, `:48`), and shadows are switched off by
`sceneUBO.numCascades = 0` (`VulkanRenderer.cpp:213`), so nothing is lost by
deleting these.

**Fourth, both OBJ loaders mishandle a face corner with no normal index, in
opposite directions.** C++ `ObjLoader.cpp:321-330` leaves `normals` at
`glm::vec3(0.0F)` for such a corner, and the flat-normal fallback at `:397-399`
is guarded on `attrib.normals.empty()` — the *whole file* having no `vn`. An OBJ
where only some faces carry `vn` therefore ships zero normals into the vertex
buffer, which every lighting shader turns into `dot(N, L) == 0` or a NaN.
The Rust converter has the mirror-image defect: `obj_to_gltf.rs:283-291`
fabricates `[0.0, 1.0, 0.0]` for a corner with no normal index — the exact
"normal-less asset lit as if every face pointed up" failure that
`GltfLoader.cpp:297-302` documents having fixed on the C++ side — and then
`to_gltf` always emits a NORMAL accessor (`:507`), so
`gltf_loader.rs:560-563`'s `compute_flat_normals` fallback never fires. A `vn`-less
OBJ renders correctly lit in the Vulkan engine and flat-lit-from-above in the
WebGPU one: the seventh entry in the cross-renderer-divergence family
(`bd315707` `map_Kd` backslashes, `6aa5eec6` the `Resources/` search,
`d25cd1e5` index validation, `92df2e7f` adapter absence, `c7b66fa5` the
default-scene walk, `c158dfe6` COLOR_0).

Ordering: **task 1 before task 2** — task 2 normalizes the vector task 1's gate
scans for, and both reason about the same convention. Tasks 3, 4 and 5 are
disjoint from those two and from each other (`PostStage.cpp`, `ObjLoader.cpp`,
and the Rust submodule respectively); still, do them one at a time. Tasks 4 and
5 are the two halves of one divergence — do 4 first so the C++ behaviour task 5
must match is already pinned by a test.

**GPU verification is currently unavailable** (see the `- [b]` RDP entry above),
so every task below lands a device-free gate or unit test as its primary
evidence, and names the GPU golden only as the secondary check to run once a
console session exists.

Candidates found but NOT tasked (checked, then rejected — do not re-propose
without new evidence): **`GltfLoader` attribute accessors shorter than
POSITION** (`GltfLoader.cpp:295-306` indexes `normals`/`uvs`/`colors` with
`i < .size()` guards and falls back to constants) — unreachable, because
`loadDocument` calls `cgltf_validate` (`:447`) *before* walking the document and
cgltf rejects a primitive whose attribute accessors disagree in count;
**`updateTexturesInSharedRenderDescriptorSet` writing fewer descriptors than
`MAX_TEXTURE_COUNT`** (`VulkanRenderer.cpp:1553-1556` feeds `writeImageArray`,
which refuses a short write at `DescriptorSetGroup.cpp:190-196`) — not a bug:
`planFlattenedTextureSlots` pads the plan back up to `maxSlots` with the first
slot (`ObjectDescription.ixx:86-91`); **the per-frame
`update_raytracing_descriptor_set` call in `drawFrame`** (`:541`, guarded on
`.raytracing` only, so the path-tracing branch never gets it and its goldens
still pass — i.e. the rewrite is redundant, since every state change that
invalidates those descriptors already routes through `updateAllDescriptorSets`)
— the deletion is almost certainly right and saves three descriptor writes per
frame, but "almost certainly" plus no host GPU is the wrong trade on the RT
frame path; re-propose once GPU verification is back; **`histogram/histogram.slang`
being a `disabled: true` manifest row whose header claims to mirror
`histogram.wgsl` while covering 1 of its 3 entry points with a different
algorithm** — real documentation drift, but the WGSL↔Rust pair that actually
runs is already pinned by `auto_exposure.rs`'s unit tests and a headless
comparison, so the drift has no runtime reach; **`Src/KomputePlayground`** —
unchanged; still an owner decision.

## 2026-08-03 batch V — planner (refactor: the layout→access/stage mapping that is private, untested, and hand-copied by the one caller it cannot serve; the ninth member of the create-info builder family; a model-loading doc that predates the texture and sampler dedup that just shipped)

The actionable queue was empty when this batch was written (0 `- [ ]`, 15
`- [b]` across the whole file). Every `file:line` below was read out of the tree
this pass.

**Every task in this batch is verifiable with no GPU**, deliberately: the
fifteen `- [b]` entries above are still blocked on host GPU golden verification.
Tasks 1 and 2 land device-free `constexpr`/pure helpers with new gtest suites
that run in the container CPU lane; task 3 is a docs change gated by a new
`BuildIntegrity` test built on the existing marker-parsing pattern.

**The headline is that `VulkanImage` owns the engine's layout→access-mask and
layout→pipeline-stage tables, they are `private` (`VulkanImage.ixx:66-67`) and
therefore have never had a single unit test, and the one call site the shared
transition cannot serve hand-copies both of them.** `transitionImageLayout`
(`VulkanImage.cpp:118-151`) derives `srcAccessMask`/`dstAccessMask` from
`accessFlagsForImageLayout` and the two pipeline stages from
`pipelineStageForLayout` — the whole point being that this logic "lives in
exactly one place" (`VulkanImage.cpp:110-111`). But it hard-codes
`subresourceRange.layerCount = 1` (`:134`), so the cubemap upload cannot use it:
`SkyBox::uploadCubeMapFaces` spells out a nine-field `vk::ImageMemoryBarrier`
(`SkyBox.cpp:149-157`, `layerCount = 6`) and then re-derives both transitions by
hand — `eTopOfPipe → eTransfer` with `{} → eTransferWrite` (`:159-164`) and
`eTransfer → eFragmentShader` with `eTransferWrite → eShaderRead`
(`:179-184`). Both derivations agree with the tables today; the first is
byte-identical to what the helper would produce, and the second differs only in
that the helper maps `eShaderReadOnlyOptimal` to the deliberately-broader
`eAllCommands` (`VulkanImage.cpp:210-213`). That is a copy that is correct
*right now* and has no test on either side of it — the same shape as the
framebuffer/render-pass/pipeline-layout duplications this repo has been
retiring since `36937517`.

**Second, `vk::ImageViewCreateInfo` is the ninth member of the
"one rule, N hand-rolled copies" family and its second copy already drifted.**
`VulkanImageView::create` (`VulkanImageView.cpp:49-67`) writes the full eleven
fields including four explicit `eIdentity` component swizzles;
`CascadedShadowMap::createFramebuffers` (`:196-204`) writes nine of them, omits
the swizzles entirely, and hard-codes `levelCount = 1` next to a `layerCount`
taken from `numCascades`. The omission is harmless only because
`vk::ComponentSwizzle::eIdentity` is zero — exactly the "the copies agree by
luck" state `buildFramebufferCreateInfo`, `buildRenderPassCreateInfo`,
`buildRenderPassBeginInfo`, `buildPipelineLayoutCreateInfo`,
`fullExtentViewport` and `buildAttachmentDescription` were each extracted to
end. `common/` already holds six such headers with six matching suites, so the
pattern, the file layout and the test shape are all settled.

**Third, `docs/model-loading.md` is the architecture reference for the loaders
and it stops before the texture layer that shipped 2026-08-03.** The doc covers
the two loaders, the async split, `MeshRange`/`sliceMeshRange` and `map_Kd`
resolution, and says nothing about: the glTF per-image dedup keyed on
`const cgltf_image *` (`GltfLoader.cpp:464-482`), the OBJ dedup keyed on the
*resolved* texture path (`ObjLoader.cpp:208-215`), the sampler dedup by mip
level (`Model.cpp:68-93` via `findSamplerForMipLevel`,
`SamplerBuilder.cpp:40`), or the reason all three exist — the flat
`MAX_TEXTURE_COUNT = 128` descriptor budget
(`common/host_device_shared_vars.hpp:8`) that `assignTextureOffsets`
(`scene/ObjectDescription.ixx:22`) packs every model into. Three shipped commits
(`1da7c1f3`, `dca11022`, `c158dfe6`) are invisible in the doc that claims to be
their architecture reference, and the 128 is the kind of number
`docs/gpu-golden-testing.md`'s golden counts already had to be corrected twice
by hand (`buildIntegritySuite.cpp:2393-2400`).

Ordering: all three are independent and touch disjoint files (task 1:
`VulkanImage` + `SkyBox`; task 2: `VulkanImageView` + `CascadedShadowMap`;
task 3: `docs/` + `buildIntegritySuite.cpp`). Do them one at a time anyway.

Candidates found but NOT tasked (checked, then rejected — do not re-propose
without new evidence): **the nine hand-written `vk::ImageSubresourceRange`
blocks** (`PathTracing.cpp:57-62`, `Raytracing.cpp:86-91`, `SkyBox.cpp:153-157`,
`Texture.cpp:292-295`, `VulkanImage.cpp:130-134`, `VulkanImageView.cpp:60-67`,
`CascadedShadowMap.cpp:200-204`, plus the two cloud barriers) — the two cloud
copies are still owned by the `- [b]` cloud-barrier entry above, `FrameCapture`
already uses the compact `vk::ImageSubresourceRange{ aspect, 0, 1, 0, 1 }`
constructor that makes a helper redundant, and tasks 1 and 2 below remove three
of the copies as a side effect; re-count after both land; **`Mesh::setModel`
(`Mesh.ixx:57`, `Mesh.cpp:92`) and `GpuTimingSubsystem::timestampMask`
(`GpuTimingSubsystem.ixx:238`) having zero callers** — both genuinely dead (a
full sweep of the 277 member-function names declared across all `.ixx` files
found exactly these two), but `34a1e00f` already deleted `timestampMask`'s
sibling `passRecordedMask` and two batches have now run dead-accessor sweeps;
fold these two into whatever next touches those files rather than spending a
task; **`CascadedShadowMap` managing a raw `vk::ImageView shadowMapArrayView`
(`CascadedShadowMap.ixx:153`) with a hand-written `destroyImageView` in
`cleanUp` (`:226-229`) instead of the move-only `VulkanImageView` RAII wrapper
that exists for exactly this** — a real API-consolidation gap, but converting it
changes live shadow-pass object lifetimes with no device-free test available;
re-propose once GPU verification is back; **`Texture::loadTextureData` returning
a raw owning `unsigned char *` through three out-params
(`Texture.cpp:265-284`)** — a `std::expected`/struct return would be more modern,
but it has exactly one caller (`Texture.cpp:74`) which already wraps the result
in a `unique_ptr` with the right deleter, so the modernization buys nothing;
**`CommandBufferManager::beginCommandBuffer` allocating a
`std::vector<vk::CommandBuffer>` for one handle** — re-confirmed and re-rejected
for the fourth time (upload-time only, never on the frame path).
**`Src/KomputePlayground`** — unchanged; still an owner decision.

### C++ Vulkan engine

### Docs

## 2026-08-03 batch VI — planner (a shared Slang module edited six commits ago whose generated WGSL was never regenerated, the two gates that both failed to notice, a Windows CI allowlist that has silently orphaned four suites, and three `import`s nothing uses)

The actionable queue was empty when this batch was written (0 `- [ ]`, 15
`- [b]` across the whole file). Every `file:line` below was read out of the tree
this pass; the drift in task 1 is sitting in the working tree right now, so it
is directly observable rather than inferred.

**Every task in this batch is verifiable with no GPU**, deliberately: the
fifteen `- [b]` entries above are still blocked on host GPU golden
verification. Tasks 1–3 and 5 are text/manifest gates plus one regenerated
generated file; task 4 is a CI-filter change whose safety is already proven by
the Linux lane running the same suites on every push.

**The headline is that `5805867a` edited `Resources/ShadersSlang/common/brdf.slang`
— a module `forward/forward.slang` imports — and never regenerated the WGSL
that Slang emits from it.** That commit rewired `brdf_direct` to call
`lambert_diffuse` instead of duplicating its math inline (`brdf.slang:78`).
`forward.wgsl` is generated output, checked into the Rust crate via the
manifest's `wgslMap` (`shader-manifest.json`, `src: forward/forward.slang` →
`dst: third_party/OxidANT/crates/webgpu_renderer/src/shaders`),
and the committed copy still inlines the division by π with no
`fn lambert_diffuse_0`. Regenerating with the host's slangc 2026.8 — the exact
version the manifest's `minSlangcVersionForWgsl` names, and which reproduces
the other nine `wgslMap` outputs byte-for-byte — produces a 13-line/8-line diff
that is currently uncommitted in
`third_party/OxidANT`. Six commits shipped on top of it.

**Both gates that should have caught it are structurally unable to.**
`BuildIntegrity.CheckedInWgslIsNotOlderThanItsSlangSource`
(`buildIntegritySuite.cpp:1190-1237`) compares each `wgslMap` destination's
mtime against `mapping.slang_source` **only** — not against the modules that
source imports. Editing `common/brdf.slang` therefore never makes
`forward.wgsl` look stale, even locally. The SPIR-V half of the pipeline
already has the missing check as a separate test
(`CompiledShadersAreNotOlderThanSharedIncludes`, `:832-857`, built on the
`newest_shared_import` helper); the WGSL half never got the twin. And mtimes
are meaningless after a fresh `git clone` — every file lands with roughly the
same timestamp — so even the fixed mtime gate is a coin flip in CI and a
content check is needed alongside it. That the drift survived six commits *and*
the Linux lane (which does run this test) is the proof.

**Third, `Windows.yml`'s `$cpuOnlySuites` is a hand-maintained 50-entry
allowlist and four suites have already fallen out of it**, so
`BuildIntegrity.EveryCpuSuiteIsInTheWindowsCiFilter` (`:1051-1107`) is RED
right now: `ExtensionSupportUnit` (`93d435e2`), `PipelineLayoutHelperUnit`
(`c743d99d`), `ImageViewHelperUnit` (`ed9a1fd2`, yesterday) and `SamplerBuilder`
(the non-`Unit` sibling in `samplerBuilderSuite.cpp:99,108` — the listed
`'SamplerBuilderUnit.*'` glob does not match it, because `.` is a literal in a
gtest filter). The gate turns a silent gap into a loud one but cannot stop it
recurring: it only fires *after* someone runs `BuildIntegrity.*`, which the
last four suite-adding commits did not. **`Linux.yml` already solved this the
other way round** — `--ctest-exclude "^(Integration|GoldenRender)\."` over
gtest-discovered tests (`Linux.yml:114,160,186,337`), a negative filter that
needs no maintenance and keeps the GPU suites out by name just as explicitly.
Windows is the odd one out. The four orphaned suites run green on Linux on
every push, so un-orphaning them on Windows is low risk.

**Fourth, three `import` statements name modules the importing shader never
uses**, and one of them backs a comment that claims the opposite:
`forward/forward.slang:2-3` imports `aces` and `fullscreen` while the file
references neither `aces_tonemap`, `linear_to_srgb` nor `FullscreenVsOut`
(the Rust pipeline tonemaps in a separate pass, `tonemap/tonemap.slang`), yet
`forward.slang:6` states it "Uses the shared BRDF math (import brdf) and ACES
tonemap (import aces)". `sky/sky.slang:1` imports `fullscreen` and hand-rolls
its own `vs_main`. `ibl/ibl.slang:1` is the counter-example that has to keep
working: it never calls `fullscreen_vs` but does use the `FullscreenVsOut`
*type* seven times, so a gate here must look at struct names as well as
function names. This is the same "the source still says it does X, nothing does
X" signature that `EverySlangFunctionIsReachableFromAnEntryPoint` was written
for one commit later.

Ordering: **task 1 first** (it is the drift itself, and tasks 2, 3 and 5 all go
red or noisy against a stale `forward.wgsl`). Tasks 2, 3, 4 and 5 all edit
`Test/commit/VulkanEngine/buildIntegritySuite.cpp`, so do them one at a time
and rebuild between them.

Candidates found but NOT tasked (checked, then rejected — do not re-propose
without new evidence): **the glTF loader's `(0,1,0)` fallback normal when
`NORMAL` is shorter than `POSITION`** (`GltfLoader.cpp:301-302`, which skips
`computeFlatNormals` because `normals` is non-empty) — unreachable, because
`GltfLoader.cpp:447` runs `cgltf_validate`, which rejects a primitive whose
attribute accessors disagree on `count`; **`assignTextureOffsets` advancing
`offset` past `MAX_TEXTURE_COUNT` while `planFlattenedTextureSlots` caps at it**
(`ObjectDescription.ixx:35` vs `:77`) — not an out-of-bounds descriptor read:
all five consumers clamp (`deferred.slang:58`, `rasterizer.slang:59`,
`shadow_map.slang:48`, `raytrace.rchit.slang:72`, `path_tracing.slang:207`);
**`apply_keyboard_input` scaling Q/E yaw by `movement_speed` rather than the
`turn_speed` that sits unused next to it** (`CameraController.ixx:52-53`) — a
real smell, but both are compile-time constants with no GUI slider
(`Camera.cpp:62-63,75-76`), so nothing observable changes and there is no
oracle for the new rate; **the Rust OBJ→glTF converter dropping `Ks`/`Ns`/`Ke`
that `parse_mtl` never reads** (`obj_to_gltf.rs:109-172`) — a documented
deliberate decision (`obj_to_gltf.rs:12`, "dropped rather than guessed into
metallic/roughness"), not drift; **`asset/obj_to_gltf.rs` having zero `#[test]`
blocks** — it has a 650-line integration suite at
`crates/webgpu_renderer/tests/obj_to_gltf.rs`; **the four remaining hand-rolled
`vk::ImageSubresourceRange` field-by-field blocks** (`PathTracing.cpp:57-62`,
`Raytracing.cpp:86-91`, `Texture.cpp:292-295`, `VulkanImage.cpp:133-137`) —
this is the re-count batch V asked for after its tasks 1 and 2 landed:
`SkyBox` and `CascadedShadowMap` are gone, two of the six survivors are the
cloud pair still owned by the `- [b]` cloud-barrier entry, and the rest is a
pure style change on GPU-only code with no device-free test. Batch V's
reasoning stands; do not re-propose until GPU verification is back.
**`Src/KomputePlayground`** — unchanged; still an owner decision.

### Cross-renderer

### CI and release gaps

### Shaders

## 2026-08-03 batch VII — planner (a shader hot-reload button that skips the deferred pipelines entirely, leaks a pipeline layout in four of the five stages that implement it, and leaves the ray-tracing SBT holding a destroyed pipeline's group handles; a look-mode camera that snaps by the cursor's absolute position on the first right-drag of every run; a Slang source that is disabled, compiles to nothing, and diverged wholesale from the WGSL it claims to mirror)

The actionable queue was empty when this batch was written (0 `- [ ]`, 15
`- [b]` across the whole file — batch VI's five tasks all shipped as `f7a5710d`,
`30154355`, `66f36b0f`, `6a7c3b88`, `66dfa78a`). Every `file:line` below was read
out of the tree this pass.

**Tasks 1, 2, 4 and 5 are verifiable with no GPU** (source-scanning
`BuildIntegrity` gates plus one CPU gtest suite that already exists); task 3's
*behaviour* needs the GPU golden suites that the fifteen `- [b]` entries are
still blocked on, but its fix and its gate are device-free, and the defect is a
spec violation readable off the source.

**The headline is that the shader hot-reload path — one GUI button, four
distinct defects.** `VulkanRenderer::shaderHotReload`
(`VulkanRenderer.cpp:403-419`) reloads `rasterizer`, `postStage`,
`raytracingStage` and `pathTracing`, and **never calls
`deferredRasterizer.shaderHotReload`**, even though that method exists, is
declared (`DeferredRasterizer.ixx:33`), is implemented
(`DeferredRasterizer.cpp:49-56`) — and is the *only* one of the five
implementations that gets resource lifetimes right. So it is simultaneously dead
code and a functional hole: press hot reload while `rasterizationMode` is
Deferred and nothing about the deferred G-buffer or lighting shaders is
re-read, silently. (`skyBox`, `dirShadowMap` and `clouds` have no
`shaderHotReload` at all — a wider gap, deliberately out of scope below.)

**Second, four of the five implementations leak a `vk::PipelineLayout` per
press.** Each destroys only the pipeline and then calls a `create...` function
that *also* creates the layout and overwrites the member handle:
`PostStage.cpp:55-60` → `:263-269`, `Rasterizer.cpp:51-54` → `:319-321`,
`Raytracing.cpp:39-43` → `:259-261`, `PathTracing.cpp:40-45` → `:204-206`.
`DeferredRasterizer.cpp:49-56` is the reference — it destroys both pipelines and
both layouts before recreating. Nothing catches this: the layouts are only
destroyed once, in each stage's `cleanUp()`, so the leak is invisible until a
long session with repeated reloads, and ASAN cannot see a Vulkan object leak.

**Third, `Raytracing::shaderHotReload` recreates the ray-tracing pipeline and
does not rebuild the SBT.** `createSBT` (`Raytracing.cpp:291-351`) fills the
three shader-binding-table buffers from
`getRayTracingShaderGroupHandlesKHR(graphicsPipeline, ...)` (`:308`), and
`init` runs it right after `createGraphicsPipeline`. After a hot reload the SBT
still holds handles obtained from a pipeline that has since been destroyed —
group handles are only valid for the pipeline that produced them, so every
`traceRaysKHR` after a reload dispatches through stale handles. `recordCommands`
re-reads the buffers' device addresses each frame (`:301-307`), so the buffers
themselves are picked up; their *contents* are what went stale. Note
`VulkanBuffer::create` (`VulkanBuffer.cpp:51-100`) does **not** release a prior
allocation, so a naive second `createSBT()` call would leak three buffers —
the fix has to clean them up first. This is **not** the `- [b]` SBT miss-region
alignment item above (`BACKLOG.md:3561`); that one is about record strides and
is still blocked. Do not conflate them.

**Fourth, look mode re-seeds its mouse origin on release but never on
entry**, so the first right-drag of every run snaps the camera by the cursor's
absolute screen position. `WindowInputState::mouse_first_moved`
(`WindowInputState.hpp:16`) default-initialises to `false`, and
`handle_mouse_button_callback` (`WindowInputCallbacks.ixx:96-112`) sets it
`true` only on the *release* branch (`:109`). The press branch installs the
cursor callback (`:104`) without re-seeding, so the very first cursor event of
the first look session computes `x_change = x_pos - 0.0` — roughly half the
window width of yaw in one frame. The same omission repeats mid-session: when
ImGui takes mouse capture, `handle_mouse_callback` early-returns (`:62`)
*without* updating `last_x`/`last_y` or setting `mouse_first_moved`, so moving
the cursor across an ImGui panel during a right-drag and back out produces one
delta measured against the position from before the panel. `mouse_first_moved`
exists for exactly this, and `WindowInputUnit.FirstMouseMoveDoesNotJumpTheCamera`
(`frontendInputSuite.cpp:80-101`) tests the mechanism — by setting
`first_moved = true` by hand, which is why the two paths that should set it
were never noticed. `WindowInputUnit.ImGuiCaptureGateSwallowsInput`
(`:113-143`) stops one event short of the jump: it asserts the captured move
yields zero, then never turns capture back off and moves again.

**Fifth, `Resources/ShadersSlang/histogram/histogram.slang` is disabled,
compiles to nothing, and has diverged wholesale from the file its own header
claims it "Mirrors".** Its manifest row (`shader-manifest.json:107-113`) lists
`targets: ["wgsl"]` with `"disabled": true`, so no script emits anything from
it, and it has no `wgslMap` entry — it is the documented WGSL fallback, kept
"for documentation and future SPIR-V use". Except the C++ Vulkan engine has no
auto-exposure pass, so there is no SPIR-V consumer, and as documentation it is
wrong: `histogram.wgsl` has three entry points (`cs_build_histogram`,
`cs_clear_histogram`, `cs_reduce_exposure`) and a per-workgroup privatised
histogram measured at 6.4× the naive version (`histogram.wgsl:43-51`);
`histogram.slang` has one entry point and the naive global-atomic loop it
explicitly notes it fell back to (`histogram.slang:36-39`). No gate can see
this: `EveryShaderSourceHasCompiledBinary` (`buildIntegritySuite.cpp:900-937`)
only walks `manifest->engine_spirv_subdirs`, so a source that is in no *enabled*
row is invisible to it, and `buildIntegritySuite.cpp:115-118` asserts in a
comment that `histogram.wgsl` is "hand-written, with no generating Slang
source" — which is half true and reads as if the file below did not exist.
The same header-comment drift runs through the whole family: all ten `wgslMap`
sources still say "Mirrors `<x>`.wgsl" (`bloom.slang:3`, `forward.slang:3`,
`sky.slang:1`, `ssao.slang:3`, `ibl.slang:3`, `gpu_cull.slang:1`,
`tonemap.slang:4`, `depth_resolve.slang:4`, `occlusion_bbox.slang:1`,
`tex_quad.slang:1`) — true during the migration, backwards now that those
`.wgsl` files are generated *from* the `.slang` and checked in by
`wgslMap`. And `Resources/ShadersSlang/spike/` is the leftover phase-0 scratch
pair whose own header says it is "deleted once the toolchain is proven"
(`spike/trivial.slang:1-3`); the toolchain has been proven for the entire
shader tree, and its presence is the only thing that would force an exclusion
list onto the gate task 5 adds.

Ordering: **tasks 1 and 2 first, in that order** — they touch the same five
`shaderHotReload` bodies, and task 1's gate counts implementations against call
sites while task 2's gate inspects each body, so doing 2 before 1 leaves 1's
gate red for a different reason. **Task 3 after task 2** (both edit
`Raytracing.cpp:39-43`). Tasks 4 and 5 are independent of all of the above.
Every task adds or edits a test in
`Test/commit/VulkanEngine/buildIntegritySuite.cpp` or
`frontendInputSuite.cpp` — rebuild between them.

Candidates found but NOT tasked (checked, then rejected — do not re-propose
without new evidence): **adding `shaderHotReload` to `SkyBox`,
`CascadedShadowMap` and `Clouds`** — a real gap, but it is new behaviour on
three GPU-only subsystems with no device-free oracle, and `CascadedShadowMap`
already has a full destroy/recreate path via `handleShadowResolutionChange`
(`VulkanRenderer.cpp:313-343`) that would need to be factored rather than
duplicated; re-propose once GPU verification is back; **`Texture::uploadRgba`
(`Texture.cpp:109-193`) not calling `cleanUp()` first, so re-uploading into a
live `Texture` would leak the image** — no call site does that (the three
callers are `createFromFile`, `createFromMemory` and `createDefaultTexture`,
all on freshly constructed objects), so it is a latent trap with no reachable
failure; **`GpuTimingSubsystem::readTimings` indexing
`gpu_timing_pass_mask[imageIndex]` (`GpuTimingSubsystem.ixx:144`) after
bounds-checking only `gpu_timing_slice_recorded`** — both vectors are assigned
the same size in `create` (`:105-106`) and cleared together in `destroy`
(`:115-116`), so the check does cover it; **`apply_keyboard_input` scaling Q/E
yaw by `movement_speed` instead of the unused `turn_speed`
(`CameraController.ixx:52-53`)** — re-confirmed and re-rejected for the second
time (both are compile-time constants with no slider, nothing observable
changes, no oracle for the new rate); **the four remaining hand-rolled
`vk::ImageSubresourceRange` blocks** — unchanged since batch VI; two are owned
by the `- [b]` cloud-barrier entry and the rest is a style change on GPU-only
code. **`Src/KomputePlayground`** — unchanged; still an owner decision.

### C++ Vulkan engine

### Shaders

## 2026-08-03 batch VIII — planner (refactor: compute-pipeline creation is the one member of the create-info builder family that never got a shared home; a BuildIntegrity gate whose 19-entry `.spv` list is hand-copied from the eight sources it is supposed to guard; a GPU-timing pass count written out five times, where adding a sixth pass silently yields null name pointers)

The actionable queue was empty again — every remaining checkbox in this file is
`- [b]`. Every `file:line` below was read out of the tree this pass.

**Task 1 is the gap the "one rule, N hand-rolled copies" family left behind.**
Nine helpers now exist for graphics-side create-infos (`PipelineLayoutHelper`,
`RenderPassHelper`, `FramebufferHelper`, `ImageViewHelper`, `ViewportHelper`,
`FormatHelper`, `ImageLayoutHelper`, `MemoryHelper`, `SamplerBuilder`) and
`AGENTS.md` mandates `kataglyphis.vulkan.pipeline_builder` for graphics
pipelines — but **compute** pipelines have no equivalent, so the two subsystems
that create one each spell out the whole chain by hand.
`Clouds::createComputePipeline` (`Clouds.cpp:71-95`) already extracted that
chain — as a **private method**, which is exactly the shape batch V flagged for
`ImageLayoutHelper` ("private, untested, and hand-copied by the one caller it
cannot serve"): `PathTracing::createPipeline` (`PathTracing.cpp:198-242`) cannot
call it and re-writes the same eight assignments, including the `pName = "main"`
that only holds because Slang renames every entry point (`AGENTS.md`, "Code
Conventions"). `ShaderStagePair` (`ShaderHelper.ixx:35-55`) is the precedent for
where this belongs and how to test it without a device.

**Task 2 is the hand-maintained-allowlist antipattern this repo just retired
twice.** `BuildIntegrity.ActivePipelineShadersHaveCompiledBinaries`
(`buildIntegritySuite.cpp:949-993`) opens with the comment "Exactly the paths
built from the `slang_spv_dir` constants under `Src/`" and then hard-codes 19
of them. Nothing enforces the "exactly". Retarget a shader in
`shader-manifest.json`, rename an entry point, or add a pass, and the list keeps
asserting about paths the engine no longer loads while saying nothing about the
one it now does — the test goes green *because* it drifted. `30154355` replaced
Windows CI's `cpuOnlySuites` allowlist with a derived negative filter and
`3467`'s batch replaced a fuzz-target allowlist "that cannot say no" for exactly
this reason; this is the same fix applied to the third and last such list.

**Task 3 is a five-place count with a plausible-looking failure mode.**
`GPU_TIMED_PASS_COUNT = 5` (`GUIRendererSharedVars.ixx:12`) is hand-maintained
against `enum class GpuTimedPass` (`:13`, five enumerators),
`GPU_TIMED_PASS_NAMES` (`:14-20`), `GPU_TIMED_PASS_EXPORT_NAMES` (`:26-32`) and
the `pass_ms` initializer (`:40`, five literal `-1.0f`s). Bump the count without
touching the rest and C++ zero-fills: the two name arrays gain `nullptr` entries
that `ImGui::Text("%-18s", …)` (`CommonGuiPanels.ixx:70,72`) reads as a string
and `dump["passes"][…]` (`GpuTimingSubsystem.ixx:211`) uses as a JSON key, while
`pass_ms` gains `0.0F` — which the GUI renders as a measured **0.000 ms** rather
than `n/a`, because `-1.0F` is the sentinel (`:69`). This is the same class as
`c7a55c2e` (shadow-resolution table size derived from `kShadowMapResolutions`)
and `a0759d88` (`MAX_TEXTURE_COUNT` pinned against the header), and the existing
`gpuTimingSuite.cpp` covers only `GpuPassAverage`, never the tables.

Candidates found but NOT tasked this cycle (checked, then rejected or deferred
with a reason — do not re-propose without new evidence): **an
`ImageMemoryBarrier` builder for the eleven hand-rolled barriers**
(`FrameCapture.ixx:92,124`, `PathTracing.cpp:71,103,157`,
`Raytracing.cpp:101,132`, `VulkanRenderer.cpp:874,906`, `Texture.cpp:288`,
`VulkanImage.cpp:127`) — this is the tenth member of the builder family and the
obvious next one, but two of those sites are owned by the `- [b]` cloud-barrier
entry and the whole set is cross-frame synchronization whose acceptance test is
a host-GPU golden run plus `Run-SyncValidation.ps1`, both unavailable in this
RDP session; re-propose once host GPU verification is restored, not before.
**`Clouds` has no `shaderHotReload`** (grepped: `Clouds.cpp` has none, unlike
the five stages `EveryShaderHotReloadImplementationIsCalledByTheRenderer`
covers), so `clouds.slang` and `noise.slang` cannot be hot-reloaded — a real
gap, but it is a feature with a GPU-only acceptance test, not a refactor.
**The four remaining hand-rolled `vk::ImageSubresourceRange` blocks** —
unchanged since batch VI and re-rejected for the third time; stop re-checking
them. **`Src/KomputePlayground`** — unchanged; still an owner decision.

### C++ Vulkan engine

## 2026-08-03 batch IX — planner (three of the eight subsystems that load SPIR-V have no hot reload, so the GUI button silently skips the shadow, sky and cloud shaders; a command-buffer factory documented to return null that eight of its nine call sites record into anyway; a `Texture` that forgets how many mip levels it has; a near-plane derivation its own test file says it cannot check; a keyboard turn rate measured in metres per second)

The actionable queue was empty again — every remaining checkbox in this file was
`- [b]`. Every `file:line` below was read out of the tree this pass.

**Task 1 re-opens a candidate batch VIII rejected, with a different acceptance
test.** Batch VIII noted "`Clouds` has no `shaderHotReload`" and dropped it as "a
feature with a GPU-only acceptance test, not a refactor". Grepping the whole tree
this pass shows the gap is three times larger and has a CPU-only oracle:
**eight** files load a `Resources/ShadersSlang/build/spirv/…` path
(`DeferredRasterizer.cpp`, `PathTracing.cpp`, `PostStage.cpp`, `Rasterizer.cpp`,
`Raytracing.cpp`, `Clouds.cpp`, `CascadedShadowMap.cpp`, `SkyBox.cpp`) and only
**five** implement `shaderHotReload`. `VulkanRenderer.cpp:403-406` states the
omission is deliberate — but the button in the GUI is unlabelled about it, so
editing `shadows/*.slang`, `skybox.slang`, `clouds.slang` or `compute/noise.slang`
and pressing reload appears to work and changes nothing, which is exactly the
"instruments disagree" failure this file opens with. The existing gate
`EveryShaderHotReloadImplementationIsCalledByTheRenderer`
(`buildIntegritySuite.cpp:3561`) checks implementation → call site; inverting it
to creator → implementation is the same grep in the other direction and needs no
device.

**Task 2 is the third member of the "a documented failure return that nobody
reads" family.** `CommandBufferManager::beginCommandBuffer` returns
`vk::CommandBuffer{}` on three failure paths (`CommandBufferManager.cpp:16,34,48`)
and its partner `endAndSubmitCommandBuffer` guards against exactly that
(`:59-62`) — so the null return is the contract, not an accident. Nine call sites
exist; **one** checks it (`Texture.cpp:160`). The other eight record commands into
`VK_NULL_HANDLE` — `vkCmdPipelineBarrier(VK_NULL_HANDLE, …)`, not a validation
error the layers can catch on a dispatchable-handle argument. Same shape as
`VulkanCreationResultsAreChecked` (`buildIntegritySuite.cpp:2800`), and the same
gate technique applies.

**Task 3 is a member field that is only ever written on one of the two paths that
should write it.** `Texture::createImage` (`Texture.cpp:208-223`) takes
`in_mip_levels`, forwards it to `VulkanImage::create`, and never assigns
`this->mip_levels`. `mip_levels` is what `createTextureSampler` passes as the
sampler's `maxLod` (`Texture.cpp:241`) and what the public `getMipLevel()` returns
(`Texture.ixx:38`). Only `uploadRgba` sets it. Every texture built the other way —
`Clouds::createStorageTexture`, `SkyBox::uploadCubeMapFaces`,
`CascadedShadowMap`'s array — therefore gets `maxLod = 0`. All three happen to use
one mip level today, so this is latent, not currently visible; it is also exactly
the trap the next multi-mip storage/cubemap texture walks into, and it will
present as "my mips are ignored", not as an error.

**Task 4 is a coverage gap the test file itself declares.**
`frustumSuite.cpp:107-109` says in a comment that its tests do NOT distinguish the
`[0,1]`-depth near plane (`row2`) from the OpenGL `row3 + row2` form, and
`Frustum.cpp:40-51` spends twelve lines justifying the unusual choice. A
derivation that is deliberate, non-textbook, and unpinned is one "cleanup" commit
from silently reverting.

**Task 5 is a unit mismatch in shared frontend code.**
`apply_keyboard_input` (`CameraController.ixx:52-53`) adds
`movement_speed * delta_time` — a distance in world units — to `yaw`, a value in
degrees, while `turn_speed` (the field that exists for exactly this) is read only
by `apply_mouse_input`. It is also the only one of the two input helpers that does
not refresh the basis it invalidated.

Candidates found but NOT tasked this cycle (checked, then rejected or deferred
with a reason — do not re-propose without new evidence): **an
`ImageMemoryBarrier` builder** — unchanged since batch VIII, still gated on host
GPU verification being restored. **The four remaining hand-rolled
`vk::ImageSubresourceRange` blocks** — re-rejected for the fourth time; stop
re-checking them. **`Src/KomputePlayground`** — still an owner decision.
**`histogram.wgsl` is the only file in the Rust crate's `src/shaders/` with no
`wgslMap` row** — that is the intended outcome of `b1016f25` retiring
`histogram.slang`, not drift. **`GltfLoader::adoptParsed`** — checked against
`GltfLoader.ixx`'s member list; it moves all six parse-result members, no
repeat of the batch XI mesh-range bug. **`docs/cpp-renderer-improvements.md` has
not been touched since 2026-08-02** — it is a curated campaign log, not a
per-commit changelog; drift is arguable and no gate claims otherwise.

## 2026-08-03 batch X — planner (a shadow-map stabilization that snaps to a grid one texel coarser than the box it snaps inside, in both renderers; a Rust cascade whose texel size changes continuously as you dolly; two `beginCommandBuffer` failure gates whose callers then index an empty vector and dereference a null texture; a device feature enabled without ever asking whether the device has it; the destruction half of the pipeline-layout builder family)

The actionable queue was empty again — every remaining checkbox in this file was
`- [b]`. Every `file:line` below was read out of the tree this pass.

**Tasks 1 and 2 are two independent defects in the same feature**, cascade
stabilization, which both renderers implement and both got subtly wrong in ways
their own tests are constructed not to see. Task 1's arithmetic is a two-line
fix in each renderer; task 2 is Rust-only. They are separate tasks because task
1's oracle is "does the probe move by an integer over a LONG camera travel" and
task 2's is "does the box size stay put when the camera dollies" — different
assertions, different files, and task 1 must land first (task 2's test would
otherwise fail for task 1's reason).

**Task 3 is the other half of `0c4d2faa`.** That commit added the
`if (!command_buffer) { … return; }` gate at every `beginCommandBuffer` call
site and `EveryBeginCommandBufferResultIsChecked`
(`buildIntegritySuite.cpp:2936`) to keep it that way — correct, and it stopped
the recording-into-`VK_NULL_HANDLE` class. What no gate covers is what the
CALLER of the gated function then does. Two of those early returns hand back a
half-built object that the next line uses unconditionally: `ASManager::createBLAS`
returns with `blas` empty and `createASForScene` immediately calls `createTLAS`,
which reads `blas[model_index]` (`ASManager.cpp:265`); `Clouds::createStorageTexture`
returns `nullptr` and six sites dereference it. A gate that proves the return
value is checked is not a gate that proves the failure is survivable.

**Task 4 is the one feature in `VulkanDevice::createLogicalDevice` that is
asserted rather than queried.** Every other feature in that function is
`features2.features.X = available_features2.features.X` — copied from a
`getFeatures2` result, and the comment at `:372-380` explains at length why the
chain matters. `computeDerivativeFeatures.computeDerivativeGroupQuads = VK_TRUE`
(`:497`) is set unconditionally, gated only on the EXTENSION being present. The
extension exposes two independent bits (`computeDerivativeGroupQuads`,
`computeDerivativeGroupLinear`) and a device may advertise the extension while
supporting only linear — on such a device `vkCreateDevice` fails outright with
`VK_ERROR_FEATURE_NOT_PRESENT` and the engine does not start at all.

**Task 5 is the destruction half of `PipelineLayoutHelper.hpp`.** The creation
side was collapsed into `buildPipelineLayoutCreateInfo` across nine sites; the
teardown side is still hand-rolled `if (h) { destroy(h); h = nullptr; }` pairs
in **20** places (10 owners × `shaderHotReload` + `cleanUp`). This is not the
deferred `ImageMemoryBarrier` builder — no barrier, no synchronization, no GPU
oracle: `destroyPipelineLayout`/`destroyPipeline` calls only, verifiable by a
source gate in the exact shape of `ComputePipelinesAreCreatedThroughTheSharedHelper`
(`buildIntegritySuite.cpp:3980`).

Candidates found but NOT tasked this cycle (checked, then rejected or deferred
with a reason — do not re-propose without new evidence): **an
`ImageMemoryBarrier` builder** — unchanged since batch VIII, still gated on host
GPU verification being restored; task 5 above is the adjacent, GPU-free member
of the same family, and landing it does not unblock this one. **The four
remaining hand-rolled `vk::ImageSubresourceRange` blocks** — re-rejected for the
fifth time; stop re-checking them. **`Src/KomputePlayground`** — still an owner
decision. **`ASManager::createBLAS`'s single shared scratch buffer with a
Write→Read barrier between builds** — the write-after-write on the scratch
region is arguably under-synchronized, but the barrier matches the upstream
nvpro reference verbatim and the only instrument that can settle it is
`Run-SyncValidation.ps1`, which needs the host GPU; re-propose with a
`SYNC-HAZARD` line, not from reading. **`apply_mouse_input`/`apply_keyboard_input`
never wrap `yaw`** (`CameraController.ixx:58-59,69`) — real unbounded growth,
but the measured cost is ~0.03° of angular quantization after an hour of
continuous turning, below what the existing `cameraControllerSuite` tolerances
could even assert; not worth a task. **`chooseSwapchainImageCount` is the one
swapchain choice still inline** (`VulkanSwapChain.cpp:58-64`) rather than in the
tested `SwapchainChoices.hpp` — checked the logic for a defect and found none
(`minImageCount + 1` clamped down to `maxImageCount` is always `>= minImageCount`),
so this is a pure symmetry itch with no bug behind it. **`ASManager::cleanUp`
leaves destroyed handles in `blas`/`tlas`** — not idempotent unlike its
siblings, but it is called exactly once, from a destructor path; folded into
task 3's reading list rather than tasked on its own.

## 2026-08-03 batch XI — planner (refactor: `Scene` writes the same "is this index in range" rule eleven times and the two per-frame record loops pay for it four times per mesh; the one pipeline builder `AGENTS.md` mandates has zero CPU coverage and carries a member nothing reads; the per-frame GUI→UBO marshalling is 76 untestable lines with a `vec4` capacity nothing pins)

The actionable queue was empty again — 0 `- [ ]`, 15 `- [b]` across the whole
file. Every `file:line` below was read out of the tree this pass.

**Every task in this batch is verifiable with no GPU**, deliberately: the
fifteen `- [b]` entries above are still blocked (host GPU goldens remain
unusable over RDP). All three land device-free code with gtest suites that run
in the container CPU lane. `Test/commit/VulkanEngine/CMakeLists.txt` globs
`*.cpp` with `CONFIGURE_DEPENDS` and Windows CI's suite filter is derived
(`30154355`), so a new suite file needs no registration anywhere.

**The headline is a twelfth member of the "one rule, N hand-rolled copies"
family, and this one is on the frame path.** `Scene` spells out
`if (model_index >= model_list.size()) { return <fallback>; }` in **ten**
accessors (`Scene.ixx:34, 42, 50, 57, 64, 92, 100, 108, 118, 128`) plus an
eleventh spelling in `Scene.cpp:161` (`model_id >= getModelCount()`), and five
of those ten then repeat
`Mesh *mesh = model_list[…]->getMesh(…)` + a null test (`:95, 103, 111, 119,
129`). The eleven copies do not even agree on what failure means: out of range
returns a `static` empty vector (`:35, 43`), `0` (`:51, 66`), an identity matrix
(`:58`), a `vk::Buffer{}` (`:93`), `false` (`:118`), an inverted "unknown" AABB
(`:128`) — and, in the one mutator, an `spdlog::error` (`Scene.cpp:162`). That
is the same "four different result-handling shapes" spread batch `2026-08-03`
found across the pipeline-layout copies. The cost is not only duplication:
`MeshDrawRecorder.cpp:49-69` and `CascadedShadowMap.cpp:444-468` each redo the
whole bounds-check-plus-`getMesh` walk **four times per mesh per frame**
(`getMeshBounds`, `isMeshDoubleSided`/none, `getVertexBuffer`, `getIndexBuffer`,
`getIndexCount`) for a lookup whose answer cannot change between the calls.

**Second, `kataglyphis.vulkan.pipeline_builder` has no test file at all.**
`AGENTS.md` ("Code Conventions") mandates it for every graphics pipeline and six
call sites obey (`Rasterizer.cpp:316`, `DeferredRasterizer.cpp:294` and `:315`,
`PostStage.cpp:264`, `CascadedShadowMap.cpp:341`, `SkyBox.cpp:310`) — grep
confirms no stage hand-rolls `createGraphicsPipelines`. But `grep -rl
pipeline_builder Test/` returns nothing, because `PipelineBuilder::build`
(`PipelineBuilder.cpp:85-182`) assembles ~65 lines of create-infos and then
calls `device.createGraphicsPipelines` in the same function, so there is no
seam a device-free test can reach. Nine sibling helpers
(`RenderPassHelper`, `FramebufferHelper`, `PipelineLayoutHelper`, …) all took
the opposite shape — return the create-info, let the caller make the device call
— and each one has a suite pinning every call site field-by-field. This is that
split, applied to the builder the conventions single out. It also removes
`int32_t base_pipeline_index = 0;` (`PipelineBuilder.ixx:70`): grep finds
exactly one occurrence in the whole tree, and `build()` hard-codes
`basePipelineIndex = -1` (`:175`).

**Third, the per-frame GUI→UBO marshalling has never been tested, and one of
its two array capacities is a magic `4`.** `VulkanRenderer::updateUniforms`
(`VulkanRenderer.cpp:164-239`) is 76 lines that are almost entirely pure host
maths — aspect ratio with a divide-by-zero guard (`:169`), the Vulkan
projection with its `[1][1] *= -1` flip (`:172-176`), the cascade split/matrix
copy clamped to `MAX_CASCADES` and zeroed when shadows are off (`:208-214`),
and ~40 lines of flat GUI field packing. Nothing reaches it on CPU because it
takes a `Scene*`, a `Camera*` and reads `vulkanSwapChain.getSwapChainExtent()`.
The precedent for fixing exactly this is in the tree: `13773702` pulled
`handleModelTransformChange`'s maths into `common/GuiModelTransform.hpp` — a
plain header taking `std::span<const float, 3>`, with `guiModelTransformSuite.cpp`
pinning it — and `511cc2cb` did the same for the light-direction normalization
(`common/LightDirection.hpp`). The capacity detail: `sceneUBO.cascadeSplits` is
a single `vec4` (`SceneUBO.hpp:48`, comment "up to 4 cascades") while
`cascadeLightSpaceMatrices` is `[MAX_CASCADES]` (`:49`, `MAX_CASCADES = 3` in
`host_device_shared_vars.hpp:9`). Raising `MAX_CASCADES` to 5 grows the matrix
array correctly and writes `cascadeSplits[4]` past the end of a `vec4`. No
`static_assert` covers it, and the one test that gestures at it hard-codes the
literal — `guiSceneVarsRoundTripSuite.cpp:173` reads
`EXPECT_LE(defaults.num_shadow_cascades, 4) << "must stay <= MAX_CASCADES
(SceneUBO array size)"`, a `4` whose message names a constant that is `3`.

Ordering: **task 1 and task 3 both touch `VulkanRenderer.cpp`** (task 1 only via
`MeshDrawRecorder.cpp`/`CascadedShadowMap.cpp`, task 3 only in `updateUniforms`)
— they do not overlap line-wise, but land task 1 first if both are in flight.
Task 2 is disjoint.

Candidates found but NOT tasked this cycle (checked, then rejected or deferred
with a reason — do not re-propose without new evidence): **dead accessors** —
swept exhaustively this pass (every lowercase member-function name in every
`.ixx`/`.hpp` under `Src/`, counted against `Src/` + `Test/`); the only zero-caller
name left is `Mesh::setModel` (`Mesh.ixx:57`, `Mesh.cpp:92`), one two-line
function, too small to task alone — fold it into whatever next touches `Mesh`.
**An `ImageMemoryBarrier` builder** — unchanged since batch VIII, still gated on
host GPU verification. **The four remaining hand-rolled
`vk::ImageSubresourceRange` blocks** — re-rejected for the sixth time; stop
re-checking them. **A `GraphicsPipelinesAreCreatedThroughTheBuilder` gate** in
the shape of `ComputePipelinesAreCreatedThroughTheSharedHelper`
(`buildIntegritySuite.cpp:4036`) — all six sites already comply and no site has
ever drifted, so it would gate a violation that has not happened; revisit if one
appears. **The five copies of the blocking `map_async` readback in the Rust
crate** — unchanged since the `2026-08-03` batch rejected it; every extracted
unit still needs a live wgpu adapter. **`ForwardRenderer::new` (583 lines),
`upload_scene` (379) and `render_tonemapped` (616)** in
`crates/webgpu_renderer/src/render/forward.rs` — genuinely monolithic, but a
method extraction there ships with compile-only verification for the same
adapter reason, and `render_tonemapped` is the whole frame graph. **`Scene::
getObjectDescriptions()` returning by value** (`Scene.ixx:132`) — looks like a
needless copy but its one caller (`VulkanRenderer.cpp:1312`) mutates the result
via `assignTextureOffsets`; the copy is load-bearing. **`Src/KomputePlayground`**
— still an owner decision.

### C++ Vulkan engine

## 2026-08-03 batch XII — planner (the path tracer's two image barriers name the vertex shader on both edges where its ray-tracing twin names the right stages; a window that loses focus mid-drag keeps look mode engaged forever; a PCF kernel that walks off the shadow map into a ClampToEdge sampler, identically in both renderers; a bloom threshold measured in pre-exposure units; the tenth member of the create-info builder family)

The actionable queue was empty again — 0 `- [ ]`, 15 `- [b]` across the whole
file. Every `file:line` below was read out of the tree this pass.

**Every task in this batch is verifiable with no GPU.** The fifteen `- [b]`
entries are still blocked on host GPU golden verification, so nothing here
depends on it: tasks 1, 2 and 5 land device-free code plus gtest suites that
run in the container CPU lane, and tasks 3 and 4 are shader edits whose
acceptance is the regenerated-WGSL gate set plus a source-scanning
`BuildIntegrity` test. `Test/commit/VulkanEngine/CMakeLists.txt` globs `*.cpp`
with `CONFIGURE_DEPENDS` and Windows CI's suite filter is derived (`30154355`),
so a new suite file needs no registration anywhere.

**The headline is that `PathTracing::recordCommands` names `eVertexShader` on
both of its image barriers, and its ray-tracing twin — same image, same
consumer, thirty lines away — names the correct stages.** `Raytracing.cpp`
transitions `renderImage` into the compute-equivalent stage with
`eColorAttachmentOutput -> eRayTracingShaderKHR` (`:108-114`) and back out with
`eRayTracingShaderKHR -> eFragmentShader` (`:135-141`) — `eFragmentShader`
because the consumer is `post.slang`'s `fs_main`, sampling the offscreen image
in the post pass. `PathTracing.cpp` writes the same two edges as
`eVertexShader -> eComputeShader` (`:83-88`) and `eComputeShader ->
eVertexShader` (`:161-166`). Both halves are wrong in the same direction. The
first barrier's *source* scope is supposed to cover whatever last wrote that
image — the raster/skybox colour-attachment writes, or the previous frame's
post-pass fragment read — and `eVertexShader` covers neither, so the PT
dispatch's `eShaderWrite` is unordered against them. The second barrier's
*destination* scope is supposed to be the stage that reads the image, and the
only reader is a fragment shader; as written, the layout transition and the
visibility operation are only guaranteed complete before vertex-shader
execution. This is precisely the class `Run-SyncValidation.ps1` exists for (it
found 10 real WRITE-AFTER-WRITE hazards in July 2026), and it is the same
reasoning the cloud-output barriers already carry in full at
`VulkanRenderer.cpp:906-915` ("PostStage's only subpass dependency is
`eColorAttachmentOutput -> eColorAttachmentOutput` and cannot order a
compute-shader write"). Path tracing is behind a GUI toggle, which is why
nothing has tripped over it.

**Second, look mode survives a focus loss.** `handle_mouse_button_callback`
installs the cursor-position callback on right-press and uninstalls it on
right-release (`WindowInputCallbacks.ixx:111-124`). If the window loses focus
while the button is held — alt-tab, Win+D, a system dialog — GLFW never
delivers the release, so the callback stays installed and `mouse_first_moved`
stays `false`. `Window::window_focus_callback` (`Window.cpp:110-118`) already
knows about this shape for the keyboard: it calls `reset_window_keys` on focus
loss for exactly the reason `handle_key_callback:35-37` documents ("otherwise a
key held when a widget grabs focus is never released and the camera keeps
moving forever"). It does nothing for the mouse. The consequence is the same
defect `58dcda35` fixed for look-mode entry and `CursorCrossingAnImGuiPanel...`
fixed for panel hand-off: `last_x`/`last_y` go stale at the pre-alt-tab
position and the first cursor event after refocus differences against it,
snapping the camera by the distance the cursor travelled elsewhere.

**Third, the PCF kernel samples outside the region it just bounds-checked, in
both renderers, against a `ClampToEdge` comparison sampler.**
`cascaded_shadow.slang:34-36` rejects `shadowUV` outside `[0,1]` and returns
"fully lit", then `:45-53` samples `shadowUV + float2(x,y) * texelSize` for
`x,y` in `[-pcfRadius, +pcfRadius]` — `pcfRadius` is a live GUI value
(`sceneUBO.pcfRadius`, `:39`), so the kernel reaches arbitrarily far past the
edge. `forward.slang` has the identical shape: bounds check at `:232`, a fixed
3x3 at `:244-253`. Both shadow samplers are `ClampToEdge`
(`CascadedShadowMap.cpp:72-73`; `forward.rs:747-755`), so an off-map tap
compares `Dref` against the *replicated edge texel's* depth. Where that edge
texel holds a near occluder — routine, since the cascade box edge cuts through
geometry — a fully lit fragment reads as partly shadowed, producing a dark
fringe along every cascade boundary whose width scales with the slider. Note
that the C++ sampler cannot simply be switched to `ClampToBorder`:
`Texture::createTextureSampler` hard-codes `vk::BorderColor::eIntOpaqueBlack`
(`Texture.cpp:249`), which for a depth format reads as depth 0 — *everything*
shadowed — and an int border colour on a float-format image is itself invalid;
the wgpu half would additionally need the `ADDRESS_MODE_CLAMP_TO_BORDER`
feature, which is not universally available on the web target the wasm demo
ships to. The portable fix is in the shader and identical on both sides.

**Fourth, bloom's bright pass thresholds in pre-exposure units, so
auto-exposure switches bloom on and off wholesale.** `bloom.slang:11` fixes
`THRESHOLD = 1.0` and `:43-48` subtracts it from the raw HDR sample;
`tonemap.slang:61` then composites `aces_tonemap((hdr * ao + bloom *
params.x) * exposure)`, i.e. exposure is applied *after* bloom is added. The
adapted exposure is real and can span decades — `auto_exposure.rs` bins
luminance over `MIN_LOG_LUMINANCE = -10.0` to `MAX_LOG_LUMINANCE = 4.0` and
maps the geometric mean onto `EXPOSURE_KEY = 0.18`. In a scene averaging 0.05
linear the exposure is ~3.6 and nothing in raw HDR exceeds 1.0, so bloom is
identically zero however bright the frame looks; in a scene averaging 2.0 the
exposure is ~0.09 and essentially every pixel exceeds the threshold, so the
whole frame blooms. Bloom should key off what is blown out *after* exposure,
which is the same "ONE source of truth" rule `tonemap.slang:28-30` already
states for the exposure value itself. A corroborating dead write sits next to
it: `tonemap.rs:167` still packs `exposure_ev.exp2()` into `params.z`, which
`tonemap.slang:22` documents as `z: unused` because the shader reads
`exposureState[0]` instead.

**Fifth, `vk::SubpassDescription` and its `vk::AttachmentReference` operands are
hand-rolled in all five render passes** — the tenth member of the family
`RenderPassHelper.hpp` already names by file in its own header comment. The
copies are `Rasterizer.cpp:163-174`, `PostStage.cpp:207-219`,
`DeferredRasterizer.cpp:199-227`, `SkyBox.cpp:216-227` and
`CascadedShadowMap.cpp:144-150`, and they carry the same
count-typed-out-by-hand defect the previous nine did: `colorAttachmentCount = 1`
is written as a literal next to a single reference in three of them, while
`DeferredRasterizer` correctly derives both its `3` and its `4` from
`.size()`. `CascadedShadowMap` is the depth-only case (no colour attachment at
all) and `DeferredRasterizer`'s lighting subpass is the only one with input
attachments, so the helper needs all three arms.

Ordering: **tasks 3 and 4 both regenerate WGSL and bump the
`third_party/OxidANT` submodule pin** — land them one
at a time, pushing the submodule before the superproject each time (AGENTS.md,
"Shipping a change that spans both repos"). Tasks 1, 2 and 5 are disjoint from
everything, though **task 5 touches the same five files as several shipped
refactors**, so do not interleave it with anything else that edits a render
pass. Tasks 1 and 4 both add a `BuildIntegrity` gate, so rebuild between them.

Candidates found but NOT tasked this cycle (checked, then rejected — do not
re-propose without new evidence): **the twelve hand-rolled
`vk::ImageMemoryBarrier` blocks and the four `vk::ImageSubresourceRange`
blocks** — unchanged; still owned by the `- [b]` cloud-barrier entry and still
gated on host GPU verification, and task 1 deliberately edits only the stage
arguments so it does not collide when that unblocks. **`Texture::generateMipMaps`
ending every mip in `eShaderReadOnlyOptimal` with `dstStageMask =
eFragmentShader` while the RT closest-hit and PT compute kernels also sample
those textures** (`Texture.cpp:349-354, 366-371`) — looks like task 1's defect
but is not reachable: every caller goes through
`CommandBufferManager::endAndSubmitCommandBuffer`, which fence-waits the whole
submission before returning (`CommandBufferManager.cpp:101-112`), so no later
submission races it. **`App::run` returning `EXIT_SUCCESS` after a device-lost
abort** (`App.cpp:70-82`, `hasDeviceLost()` is checked three times and never
reaches the return) — real, and it makes a crashed run read as green to
`run-clangcl-*.ps1`, but there is no device-free way to exercise it and it is
two lines; fold it into whatever next touches `App.cpp`. **`apply_mouse_input`
never wrapping `yaw`** (`CameraController.ixx:69`) — unbounded growth is real
but float precision stays adequate for any plausible session length and there
is no oracle for "wrapped correctly" that the existing pitch-clamp tests do not
already cover. **`depth_resolve.slang:30` computing `int2(In.uv * dims)` with no
clamp** where `ssao.slang:37` clamps — the fullscreen triangle only produces
pixel-centre uvs strictly inside `[0,1)`, so the out-of-range index is
unreachable. **`Src/KomputePlayground`** — unchanged; still an owner decision.

### Shaders

## 2026-08-03 batch XIII — planner (a cloud ray march that multiplies density by the distance already travelled instead of the step length, and shadows the volume from the camera rather than from the sample; four Cloud Settings controls that reach no shader, one of them a whole `vec4` uploaded every frame; the cascade light matrices, single-buffered while the matrices the lighting pass samples with are per-image; three post-acquire early returns that leave the acquire semaphore signaled forever; a noise volume written by a queue family that does not own it)

The actionable queue was empty again — 0 `- [ ]`, 15 `- [b]` across the whole
file. Every `file:line` below was read out of the tree this pass.

**Three of the five findings are in the clouds subsystem, and that is not a
coincidence.** Clouds is the one stage with no pixel oracle: the two goldens
that touch it (`GoldenRender.CloudsAcrossManyFramesDoesNotLoseTheDevice`,
`GuiInputSweepNeverCrashesOrLosesTheDevice`, `goldenRenderSuite.cpp:2785` and
`:2620-2700`) assert only that the device survives, so anything short of a
crash reads as green. Every other subsystem in this engine has had a
correctness pass driven by something that could see it being wrong.

**Every task in this batch is verifiable with no GPU.** The fifteen `- [b]`
entries are still blocked on host GPU golden verification, so nothing here
depends on it: each task lands device-free code plus a `BuildIntegrity`
source-scanning gate that runs in the container CPU lane.
`Test/commit/VulkanEngine/CMakeLists.txt` globs `*.cpp` with
`CONFIGURE_DEPENDS` and Windows CI's suite filter is derived (`30154355`), so
a new suite file needs no registration anywhere.

**The headline is that `clouds.slang`'s ray march uses the distance already
travelled where it needs the step length.** `clouds_main:163-176` computes
`stepSize = (float(i) / float(N)) * (oT.y - oT.x)` — the offset of sample `i`
from the box entry point — and then uses that same value as the segment
length in *both* integration terms: `lightEnergy += density * stepSize * …`
and `transmittance *= exp(-density * stepSize)`. Summed over the loop that is
`L * (N-1)/2` of optical depth where Beer-Lambert wants `L`, so with the GUI's
default 128 march steps (`GUI.cpp:213`, slider range 1–128) the volume
integrates roughly 63× too dense, and moving the *quality* slider changes the
*density* proportionally. Sample 0 also contributes nothing (`stepSize == 0`)
and the last sample never reaches `oT.y`. `light_march:82-102` is wrong twice
over: it takes its box span from `box_intersect(eyePosition, …)` — the ray
leaving the **camera** toward the light, not the one leaving `samplePos`,
which is the point being shadowed — and then averages raw density
(`totalDensity /= float(M)`) and feeds that straight to `exp(-totalDensity)`,
so the self-shadow term carries no length units at all and does not change
when the volume is scaled. Both are pure shader edits; `clouds.slang` is
SPIR-V-only (`shader-manifest.json`), so no WGSL regeneration and no submodule
bump.

**Second, four of the eleven Cloud Settings controls reach no shader.**
`sceneUBO.cloudMovementDirection` is packed every frame from
`cloud_movement_direction` and `cloud_speed`
(`VulkanRenderer.cpp:221-225`) — and grepping every `.slang` in the tree for
that name returns exactly one hit, its declaration in
`scene_types.slang:94`. No shader has ever read it: a whole `vec4` of the
per-image uniform upload, plus two sliders (`GUI.cpp:212,215`), that do
nothing. `cloud_num_march_steps_to_light` (`GUI.cpp:214`, slider 1–128) is the
same story from the other end — it exists in `GUISceneSharedVars`, is set by
both cloud goldens, is round-tripped by `guiSceneVarsRoundTripSuite.cpp:64`,
and never enters the UBO at all, because `clouds.slang:127` hard-codes
`num_march_steps_to_light = 4`. Two more controls are merely mislabelled:
"Illumination intensity" writes `cloud_scale` → `cloudMeshScale.w` →
`cloud.scale`, the density multiplier, and "Density" writes `cloud_density` →
`cloudMeshOffset.w` → `cloud.threshold`, the noise cut-off. The durable win
here is the gate, not the wiring: there is no test anywhere that a `SceneUBO`
member the host fills is read by anything.

**Third, the cascade light matrices are single-buffered while the matrices
the lighting pass samples with are per-image.** `CascadedShadowMap` owns one
`VulkanBuffer lightMatricesBuffer`, host-visible and persistently mapped, and
`updateCascades:129-134` memcpys the freshly computed
`cascadeData[i].viewProjMatrix` into it every frame from `App::run`'s loop —
i.e. from `VulkanRenderer::updateUniforms`, which runs *before* `drawFrame`
and therefore before any fence wait. `MAX_FRAME_DRAWS == 3`
(`common/Globals.hpp`), and the fence `drawFrame` waits on only guarantees the
submission three frames prior has completed, which is exactly the reasoning
`VulkanRenderer.cpp:874-883` spells out for the cloud-output image. So the CPU
overwrites the uniform buffer that up to two in-flight shadow passes are still
reading. The same matrices reach the lighting shader by a second route —
`fillSceneUboCascades` into `sceneUBO.cascadeLightSpaceMatrices`
(`VulkanRenderer.cpp:216-219`), which *is* duplicated per swapchain image and
uploaded inside `drawFrame` (`update_uniform_buffers`, `:720-729`). Two halves
of one piece of data with two different buffering schemes: the depth map can
be rendered from frame N's cascades and sampled with frame N+2's. The fix is
to copy the pattern that is already correct twenty lines away — compute into a
CPU struct in `updateUniforms`, upload per-image in `update_uniform_buffers`.

**Fourth, three of `drawFrame`'s early returns run after
`acquireNextImageKHR` has already signaled `imageAvailableSemaphore` and never
retire it.** `VulkanRenderer.cpp:509-513` (image index out of range),
`:515-519` (missing render-finished semaphore) and `:562-565`
(`record_commands` returned false) all `return` without submitting anything
that waits on that semaphore, and `frameSync.advanceFrame()` is the last
statement of the function (`:626`), so `currentFrame` does not move and the
**same** semaphore is handed to the next `acquireNextImageKHR`. That is
VUID-vkAcquireNextImageKHR-semaphore-01286: the semaphore must have no pending
signal operation and must be unsignaled. These are defensive paths, which is
why nothing has tripped over them — the same category as `0c4d2faa` and
`c75d2c7e`, both of which were worth closing. Two smaller defects sit in the
same function: `checkChangedFramebufferSize():461-465` consumes the resize
flag and then recreates only *conditionally*, so a resize that arrives while
the guard is false is lost permanently; and `update_uniform_buffers:720-729`
logs an out-of-range index and returns while `drawFrame` carries on and
renders the frame against never-written uniforms.

**Fifth, the cloud noise volume is written by a queue family that does not own
it.** `Clouds::createStorageTexture:39-47` creates `cloudNoiseTexture` with
`sharingMode = eExclusive` (`VulkanImage.cpp:78`) and transitions it on the
**graphics** queue; `dispatchNoiseGeneration:97-136` then builds its own
command pool on `compute_family` and submits the noise dispatch on
`device->getComputeQueue():133`. There is no release/acquire barrier pair, so
per spec the contents written by the compute family are undefined to the
graphics family that reads them in `recordComputeCommands`. And the two
families genuinely differ here: `VulkanDevice::getQueueFamilies:576-586` walks
every family and assigns without breaking, so `compute_family` ends up as the
**last** compute-capable family — on this project's RX 9070 XT that is a
dedicated async-compute family, not family 0. `getComputeQueue()` has exactly
one caller in the whole tree (that line), and the per-frame cloud dispatch is
already recorded into the graphics command buffer, so moving the one-off noise
dispatch onto the graphics queue removes the hazard and the accessor together.

Ordering: **land task 1 before task 2** — both edit `clouds.slang`, and task 1
is a pure shader change while task 2 moves a UBO field underneath it. Tasks 3,
4 and 5 are disjoint from those and from each other. All five add a
`BuildIntegrity` gate, so rebuild between them rather than batching.

Candidates found but NOT tasked this cycle (checked, then rejected — do not
re-propose without new evidence): **the twelve hand-rolled
`vk::ImageMemoryBarrier` blocks and the four `vk::ImageSubresourceRange`
blocks** — unchanged; still owned by the `- [b]` cloud-barrier entry and still
gated on host GPU verification. **`PostStage::recordCommands:69-71` building a
colour clear value its render pass can never use** (`loadOp = eLoad`,
`PostStage.cpp:190-195`) — real dead code, two lines; fold it into whatever
next touches `PostStage.cpp`. **`rasterizer.slang:76`'s
`lerp(float3(0.04), ambient, 0.0)`** — the metallic term is hard-wired to 0, so
the second operand and the lerp are both dead; same disposition, fold it in.
**`histogram.wgsl`'s `cs_reduce_exposure` having no counterpart to
`adapt_exposure_ev`'s `!current_ev.is_finite()` recovery**
(`auto_exposure.rs:130-132`, pinned by a test at `:350-355`, while the shader's
own header claims to mirror that function) — a genuine divergence, but every
value feeding `exposure_state[0]` is provably finite (`bin_luminance` is
bounded and positive over `1..62`), so it is unreachable rather than latent.
**`PostStage`'s depth attachment declaring `initialLayout =
eDepthStencilAttachmentOptimal` for an image nothing transitions out of
`eUndefined`** (`PostStage.cpp:141-158, 200-205`) — works only because
`SkyBox`'s pass, which shares the view and declares `eUndefined`, always runs
first (`VulkanRenderer.cpp:1002-1008`); fragile, but correct as recorded.
**`Src/KomputePlayground`** — unchanged; still an owner decision.

### C++ Vulkan engine

## 2026-08-03 batch XIV — planner (refactor: framebuffer teardown spelled out nine times across five stages, where four stages re-implement their own `destroyFramebuffers()` inside `cleanUp()` and only `cleanUp()` guards the null device; a golden-suite count in prose that the gate written to stop exactly this drift does not reach, already stale by one test; the two model loaders' `uploadParsed` tails, where only one of the two forwards `MeshRange::doubleSided`)

The actionable queue was empty again — 0 `- [ ]`, 15 `- [b]` across the whole
file. Every `file:line` below was read out of the tree this pass.

**Every task in this batch is verifiable with no GPU**, deliberately: the
fifteen `- [b]` entries are all blocked on host GPU golden verification, so
nothing here depends on it. Each task lands device-free code plus a
`BuildIntegrity` source-scanning gate that runs in the container CPU lane.
`Test/commit/VulkanEngine/CMakeLists.txt` globs `*.cpp` with
`CONFIGURE_DEPENDS` and `kataglyphis_collect_module_interfaces`
(`Src/GraphicsEngineVulkan/CMakeLists.txt:30`) globs `*.ixx`, so neither a new
suite file nor a new module interface needs registering anywhere.

**Re-confirming the standing rejection first so it is not re-derived:** an
`ImageMemoryBarrier` builder was checked again this pass and is **still not
tasked** — twelve hand-rolled blocks, still owned by the `- [b]` cloud-barrier
entry, still gated on host GPU verification. Same for the four remaining
`vk::ImageSubresourceRange` blocks. The three tasks below are the create-info
family members whose acceptance test is *not* a rendered pixel.

**The headline is that framebuffer teardown is written nine times and the four
`destroyFramebuffers()` methods that exist are each duplicated by the
`cleanUp()` of the very class that owns them.** `Rasterizer::cleanUp:109` and
`Rasterizer::destroyFramebuffers:133-136` are the same loop over the same
member; so are `DeferredRasterizer.cpp:126-129`/`:149-152`,
`PostStage.cpp:109`/`:129`, and `SkyBox.cpp:398-400`/`:424`. Only the
`cleanUp()` half of each pair sits behind an `if (!device) { return; }`, so
calling `destroyFramebuffers()` after `cleanUp()` has reset the `shared_ptr`
dereferences null — an ordering the four stages happen to satisfy today
(`VulkanRenderer.cpp:664-689`) and nothing enforces.
`CascadedShadowMap.cpp:246-249` is the ninth copy, the single-handle variant,
and it is the only one that nulls what it destroyed. This is the exact
destruction-half counterpart to `buildFramebufferCreateInfo`, and
`destroyPipelineAndLayout` in `common/PipelineLayoutHelper.hpp` — with its
`BuildIntegrity.PipelineTeardownGoesThroughTheSharedHelper` gate at
`buildIntegritySuite.cpp:4622` — is the template for both the helper and the
gate.

**Second, `docs/gpu-golden-testing.md` has drifted again, in the one place the
gate cannot see.** `BuildIntegrity.GoldenTestCountsInDocsMatchTheSuite`
(`buildIntegritySuite.cpp:3313`) pins the four integers in the
`<!-- golden-counts: defined=31 runnable=30 integration=2 total=32 -->` marker
at `:84`, and those are correct. But `:178-179`, 94 lines below the marker,
still says "runs the rest of the suite (**28** tests) clean … '28 tests from 2
test suites ran', 'PASSED 28 tests'" — a number the same document derives as
`total − 3 known-excluded`, which is `32 − 3 = 29` today. Batch XV introduced
the marker *because* these counts had already been corrected by hand twice;
the prose sentence was left ungated and has now drifted within two days of the
marker being made correct. The durable fix is to derive that number too.

**Third, `ObjLoader::uploadParsed` and `GltfLoader::uploadParsed` are the same
function with two different texture sources.** `ObjLoader.cpp:97-158` and
`GltfLoader.cpp:53-112` each spell out: the device/empty-vertices guard, the
per-texture "if creation failed, occupy the slot with the default texture
anyway" fallback (with a ~6-line comment repeated nearly verbatim in both), the
"no textures at all → reserve slot 0" fallback, and the mesh-range loop over
`sliceMeshRange`. `MeshRange.ixx:41-44` already states the design intent —
"Shared by both loaders' `uploadParsed` so the re-basing arithmetic lives in one
place" — and the arithmetic *is* shared; the loop around it is not. The drift
that intent was meant to prevent has already happened once: `GltfLoader.cpp:109`
passes `range.doubleSided` to `add_new_mesh` and `ObjLoader.cpp:153-154` does
not, relying on the default argument. That is inert today (`MeshRange.ixx:26-28`
records `doubleSided` as "always false for OBJ"), which is exactly why nothing
has caught it.

Ordering: **land these one at a time, rebuilding between them** — all three add
a test to `buildIntegritySuite.cpp` and would otherwise collide there. They are
otherwise disjoint. Task 3 adds a module interface and therefore needs
`-FreshContainer`; tasks 1 and 2 do not.

Candidates found but NOT tasked this cycle (checked, then rejected — do not
re-propose without new evidence): **an `ImageMemoryBarrier` builder and the
four `vk::ImageSubresourceRange` blocks** — re-checked, unchanged, still owned
by the `- [b]` cloud-barrier entry and still gated on host GPU verification;
this is the seventh rejection, stop re-checking them. **Dead accessors** —
swept again this pass (every `get*`/`is*`/`has*` name declared in any
`.ixx`/`.hpp` under `Src/`, counted against `Src/` + `Test/` + the Rust crate);
zero zero-caller names remain, so batch XI's sweep is still complete apart from
`Mesh::setModel`, which that batch already recorded as fold-it-in. **The five
pipeline-creation functions at `forward.rs:2707-2947`** — `create_shadow_pipeline`
and `create_masked_shadow_pipeline` do repeat the same
`DepthBiasState { constant: 2, slope_scale: 2.0 }` and the two MSAA-enabled
pipelines repeat `MultisampleState { count: MSAA_SAMPLE_COUNT, .. }`, but every
extracted unit is a `wgpu::RenderPipeline` that only a live adapter can
observe, so this would ship with compile-only verification — the same reason
`ForwardRenderer::new`/`upload_scene`/`render_tonemapped` were rejected in the
2026-08-03 batch XII prose. **`src/render/{bloom,ssao,overlay,tonemap}.rs` and
`gpu_occlusion.rs` having no `mod tests`** — real gaps, but each module's whole
surface is GPU-resident; there is no device-free unit to assert, which is why
their coverage lives in `tests/headless.rs` instead. **`Src/KomputePlayground`**
— unchanged; still an owner decision.

### C++ Vulkan engine

## 2026-08-03 batch XV — planner (a shadow-map array that carries two byte-identical `VkImageView`s; a descriptor fallback that binds the light matrices at the set index the shared layout owns; three single-descriptor writers that ignore the declared `descriptorCount` on the two 128-entry array bindings; two `create()`s that leak their previous allocation; an animation keyframe lookup that rescans from index 0 every channel every frame)

The actionable queue was empty again — 0 `- [ ]`, 15 `- [b]` across the whole
file. Every `file:line` below was read out of the tree this pass.

**Every task in this batch is verifiable with no GPU**, deliberately, for the
same reason as batch XIV: all fifteen `- [b]` entries are blocked on host GPU
golden verification, so nothing here may depend on it. Tasks 2–5 land device-free
C++ plus a CPU gtest (a pure-function suite or a `BuildIntegrity` source scan);
task 1 is entirely in the Rust crate and runs under `cargo test`.
`Test/commit/VulkanEngine/CMakeLists.txt` globs `*.cpp` with `CONFIGURE_DEPENDS`
and `kataglyphis_collect_module_interfaces` (`Src/GraphicsEngineVulkan/CMakeLists.txt:30`)
globs `*.ixx`, so neither a new suite file nor a new module interface needs
registering anywhere.

**The headline is that `CascadedShadowMap` builds the same image view twice.**
`init()` creates the sampled view through the `Texture` it owns
(`CascadedShadowMap.cpp:65`: `depthFormat`, `eDepth`, 1 mip, `e2DArray`,
`numCascades` layers) and `createFramebuffers()` then creates a *second* view
over the same image with byte-identical parameters
(`:218-223`, via `buildImageViewCreateInfo`), stores it in the private
`shadowMapArrayView` (`CascadedShadowMap.ixx:162`), and destroys it separately
in `cleanUp()` (`:241-244`). The first view is what the renderer binds as the
sampled shadow map (`VulkanRenderer.cpp:1562`, via `getShadowMapArray()`); the
second is used for exactly one thing — the single-attachment framebuffer at
`:225-230`. That is one redundant `VkImageView` per shadow-map creation, and the
shadow map is recreated on **every** shadow-resolution change
(`VulkanRenderer.cpp:331-353`), not just at startup.

**Second, the shadow pass's "no shared descriptor set" fallback binds the wrong
set index.** `recordCommands` (`CascadedShadowMap.cpp:452-460`) builds
`shadowDescriptorSets = {lightMatricesSet, lightMatricesSet}` and, when the
incoming span is empty, binds **one** set starting at index 0 — but the pipeline
layout declares set 0 = `sharedRenderDescriptorSetLayout` and set 1 =
`lightMatricesDescriptors.getLayout()` (`:358`). So on that path the light
matrices go to a slot whose layout does not describe them and set 1, which the
vertex shader actually reads, is never bound at all. It is unreachable from
`VulkanRenderer` today (`:1002-1003` always passes a one-element span), which is
exactly why it has never been noticed.

**Third, three of the four `DescriptorSetGroup` writers cannot express the two
array bindings they share a set with.** `beginWrite` hard-codes
`out.descriptorCount = 1` (`DescriptorSetGroup.cpp:141`) and `writeBuffer` /
`writeImage` / `writeAccelerationStructure` never revisit it. `writeImageArray`
is the only one that checks the declared count (`:190-196`). The shared render
set declares `TEXTURES_BINDING` and `SAMPLER_BINDING` with
`static_cast<uint32_t>(MAX_TEXTURE_COUNT)` descriptors each
(`VulkanRenderer.cpp:1459-1462`), so a `writeImage` aimed at either one writes
element 0 and silently leaves the other 127 descriptors unwritten — the class of
mistake `writeImageArray`'s own guard exists to catch, on the same object.

**Fourth, `VulkanBuffer::create()` and `VulkanImage::create()` overwrite their
handles without releasing what was there.** `VulkanBuffer::create`
(`VulkanBuffer.cpp:51-121`) assigns `buffer`, `allocation`, `mappedData` and
`created = true` unconditionally; `VulkanImage::create`
(`VulkanImage.cpp:49-90`) does the same with `image`/`allocation`/`owns_image`.
Both classes' move-assignment operators *do* call `cleanUp()` first
(`VulkanBuffer.cpp:33`, `VulkanImage.cpp:33`) and `cleanUp()` is already
idempotent, so the asymmetry is the whole finding. Every call site currently
carries the obligation instead, and two of them document it in prose —
`FrameCapture.ixx:76-83` and `VulkanBufferManager.cpp:106-113` both write
`cleanUp(); create(...)` with a comment explaining why. Nothing is leaking
today; the point is that nothing stops the next call site from leaking.

**Fifth, in the Rust crate, the animation keyframe lookup is a linear rescan.**
`keyframe_lerp_indices` (`render/animation.rs:22-25`) walks `times` from index 0
on every call, and it is called once per channel per animation per frame — twice
over, since the morph-weight pass repeats the whole loop
(`render/forward.rs:2466` and `:2514`). `times` is sorted by construction (glTF
requires it), so this is a `partition_point` away from `O(log n)`. The same two
call sites also each spell out the segment duration by hand
(`forward.rs:2467-2470` and `:2515-2518`, byte-identical), re-deriving from
`i0`/`i1` a quantity the helper already computes internally as `span`
(`animation.rs:26`) — and the two disagree on the degenerate case (`span` clamps
with `.max(1e-6)`, the callers do not).

Ordering: **tasks 2 and 3 both edit `CascadedShadowMap.cpp` and tasks 2 and 5
both add to `buildIntegritySuite.cpp` — land those three one at a time,
rebuilding between them.** Task 1 (Rust) and task 4 (`DescriptorSetGroup` +
`descriptorPoolSizesSuite.cpp`) are disjoint from everything else. No task in
this batch adds a module interface, so none needs `-FreshContainer`.

Candidates found but NOT tasked this cycle (checked, then rejected — do not
re-propose without new evidence): **an `ImageMemoryBarrier` builder and the four
`vk::ImageSubresourceRange` blocks** — re-checked again, unchanged, still owned
by the `- [b]` cloud-barrier entry and still gated on host GPU verification;
this is the eighth rejection, stop re-checking them.
**`CascadedShadowMap::createDescriptorSetAndPipeline` creating and destroying its
own throwaway `VkCommandPool`** (`:277-283`, `:300`) on the graphics family that
`VulkanRenderer::graphics_command_pool` (`VulkanRenderer.cpp:1481-1492`) already
owns and hands to every other subsystem's `init()` — real, but it is a third
edit to the same file this batch already touches twice; fold it in whenever
`CascadedShadowMap.cpp` is next opened. **The per-image
`dirShadowMap.uploadLightMatrices(i)` seeding loop at `VulkanRenderer.cpp:148`**
— it looks redundant against `update_uniform_buffers`
(`VulkanRenderer.cpp:765`), which uploads the acquired image's matrices before
`record_commands` every frame, but the comment at `:141-147` claims a first-frame
ordering reason and disproving it needs a GPU run; left alone.
**`Window.cpp:102-103`'s comment citing `VulkanRenderer.cpp:646`** — the
`glfwGetFramebufferSize` call it points at is now at `:690-692`; a stale
line-number reference in a comment, too small to task, fix it in passing.
**`pack_punctual_lights` returning a 16 KB array by value**
(`render/lights.rs:23-25`) — checked the caller: it runs from `upload_scene`
(`forward.rs:1243`), not the frame path, so it is not the per-frame cost it
looks like. **`src/render/{bloom,ssao,overlay,tonemap}.rs` and `gpu_occlusion.rs`
having no `mod tests`** — unchanged; every unit is GPU-resident, coverage lives
in `tests/headless.rs`. **`Src/KomputePlayground`** — unchanged; still an owner
decision.

### Rust WebGPU renderer (`third_party/OxidANT`)

### C++ Vulkan engine

## 2026-08-03 batch XVI — planner (the third member of the `create()`-releases-previous family, and the image/view destruction order the first two just made reachable; a shadow-map setup that builds a command pool, a buffer manager and a staging round trip to write a host-visible buffer that is overwritten a frame later; a descriptor group that will happily declare the same binding twice; a PCF radius bounded by the literal `20` in three files and clamped nowhere; a GUI model-transform control that has been dead since `viking_room` left the tree)

The actionable queue was empty again — 0 `- [ ]`, 15 `- [b]` across the whole
file. Every `file:line` below was read out of the tree this pass.

**Every task in this batch is verifiable with no GPU**, deliberately, for the
same reason as batches XIV and XV: all fifteen `- [b]` entries are blocked on
host GPU golden verification, so nothing here may depend on it. Every task
lands device-free C++ plus either a pure-function gtest or a `BuildIntegrity`
source scan. `Test/commit/VulkanEngine/CMakeLists.txt` globs `*.cpp` with
`CONFIGURE_DEPENDS`, so no new suite file needs registering.

**The headline is that `VulkanImageView::create()` is the third member of the
family batch XV only half-finished.** `VulkanBuffer::create()` and
`VulkanImage::create()` now open with `cleanUp()` (`VulkanBuffer.cpp:51`,
`VulkanImage.cpp:62`), gated by
`BuildIntegrity.ResourceCreateReleasesThePreviousAllocation`
(`buildIntegritySuite.cpp:966+`). `VulkanImageView::create()`
(`VulkanImageView.cpp:40-59`) still assigns `device` and `imageView`
unconditionally while its own `operator=(&&)` calls `cleanUp()` first
(`:26`) — the identical asymmetry, in the identical class shape, left behind.
Worse, the two shipped fixes made a *new* ordering hazard reachable:
`Texture::uploadRgba` calls `createImage()` (`Texture.cpp:146`) — which now
**destroys** the previous `VkImage` — while `vulkanImageView` still holds a
view of that image, and only then calls `createImageView()` (`:190`).
Destroying a `VkImage` that still has live `VkImageView`s is
VUID-vkDestroyImage-image-01000. `SkyBox::uploadCubeMapFaces` has the same
shape (`SkyBox.cpp:135` then `:188`). Nothing re-uploads into a live `Texture`
today, which is why validation has not fired; the point is that the guarantee
now advertised on `create()` is only half true.

**Second, `CascadedShadowMap::createDescriptorSetAndPipeline` builds three
throwaway things to write a buffer that is overwritten a frame later.** It
creates its own `vk::CommandPool` on the graphics family (`:270-276`) and
destroys it 20 lines on (`:293`) — the family
`VulkanRenderer::graphics_command_pool` (`VulkanRenderer.cpp:1481-1492`)
already owns and hands to `rasterizer.init` (`:108`), `clouds.init` (`:111`)
and `skyBox.init` (`:150`). It then constructs a function-local
`VulkanBufferManager vbm;` (`:282`) — the only local one in the tree; `Mesh`,
`VulkanRenderer` and `ASManager` all hold it as a member precisely so its
staging buffer is reused (`VulkanBufferManager.ixx:59-64`) — so the reuse is
defeated and a fresh 64 KB staging allocation is created and destroyed. And
the destination it stages into is **host-visible and persistently mapped**
(`:286`, `eHostVisible | eHostCoherent`), which is exactly what
`uploadLightMatrices` relies on to `memcpy` straight into it
(`:149-154`). So the staging buffer, the copy, the command pool and the
fence-synchronised submit all exist to seed matrices that
`VulkanRenderer::init`'s per-image loop (`VulkanRenderer.cpp:148`) rewrites
before the first frame. All of it runs again on **every** shadow-resolution or
cascade-count change (`VulkanRenderer.cpp:327-357`), not just at startup.

**Third, `DescriptorSetGroup` cannot notice that the same binding number was
declared twice.** `addBinding` (`DescriptorSetGroup.cpp:58-71`) pushes
unconditionally; `create` (`:73-116`) hands the whole vector to
`createDescriptorSetLayout` (`:84-88`), where a duplicate `binding` is
VUID-VkDescriptorSetLayoutCreateInfo-binding-00279; and `findBinding`
(`:118-125`) silently returns the first of the pair, so every write would land
on one of the two. `create` also overwrites `layout`, `pool` and
`descriptor_sets` without releasing what was there — the same asymmetry as the
headline, on a class whose `operator=(&&)` again calls `cleanUp()` first
(`:41`). It cannot simply call `cleanUp()`, because `cleanUp()` clears
`bindings` and those are populated *before* `create()`.

**Fourth, the PCF radius is bounded by a literal `20` in three files and
clamped in none.** `GUI.cpp:199` slides `pcf_radius` over `1..20`;
`goldenRenderSuite.cpp:2630` and `:2675` re-spell the same `20`;
`VulkanRenderer.cpp:202` does a bare
`static_cast<unsigned int>(guiSceneSharedVars.pcf_radius)` into
`SceneUBO::pcfRadius` (`SceneUBO.hpp:34`, `scene_types.slang:87`); and
`cascaded_shadow.slang:39` does `int radius = int(sceneUBO.pcfRadius)` and
loops `(2r+1)^2` taps with no bound of its own. A negative `pcf_radius` from
any non-GUI writer (a test, a config load, future scripting — `pcf_radius` is
a plain `int` at `GUISceneSharedVars.ixx:44`, and `goldenRenderSuite.cpp:540`
already writes it directly) casts to `4294967295`, back to `-1` in the shader,
runs zero loop iterations, and returns `1.0` — **the entire scene fully
shadowed**. This is the same unpinned-constant class as the shadow-resolution
table (`kShadowMapResolutionCount`) and `MAX_CASCADES`, both of which already
have one home and a gate.

**Fifth, the GUI's model Position/Rotation controls have been dead for as long
as `viking_room` has been gone.** `GUI.cpp:69` hard-codes
`kDefaultSelectedModelPath = "Models/VikingRoom/viking_room.obj"` and only
sets `selected_model_index` when that exact path appears in
`sceneConfig::getAvailableModelPaths()` (`:70-78`). There is no
`Resources/Models/VikingRoom` in the tree — `SceneConfig.cpp:117-120`'s own
comment describes the 60x viking_room scale as history — so the index stays
`-1`, the combo reads "Select a model...", and although the `DragFloat3`
widgets still render and set `model_transform_changed` (`:101-106`),
`VulkanRenderer::handleModelTransformChange` gates the whole body on
`selected_model_index >= 0` (`VulkanRenderer.cpp:374`) and drops it.

Ordering: **tasks 1, 2, 3 and 4 all add to `buildIntegritySuite.cpp` — land
them one at a time, rebuilding between them.** Tasks 2, 3 and 5 change a
module interface (`CascadedShadowMap.ixx`, `DescriptorSetGroup.ixx`,
`SceneConfig.ixx`), so each needs `-FreshContainer` per the module rule in
`docs/gpu-golden-testing.md`. Task 1 and task 4 do not.

Candidates found but NOT tasked this cycle (checked, then rejected — do not
re-propose without new evidence): **an `ImageMemoryBarrier` builder and the
four `vk::ImageSubresourceRange` blocks** — ninth rejection, still owned by the
`- [b]` cloud-barrier entry and still gated on host GPU verification; stop
re-checking them. **`SkyBox::recreateFrameResources` not destroying its own
framebuffers** — looked like a leak, is not: `VulkanRenderer::recreateSwapChain`
destroys all four stages' framebuffers at `:708-711` *before*
`vulkanSwapChain.recreate()`, and it must, because the framebuffers reference
the old swapchain image views. Moving the teardown into the stages would
invert a required order; leave it. **`MeshDrawRecorder`'s `unknownBounds`
sentinel surviving `transformAABB`** — checked, `transformAABB` returns an
invalid box unchanged (`Frustum.cpp:97`), so the "unknown bounds are visible"
rule holds. **`Clouds` giving `cloudOutputTexture` a sampler** — checked the
consumer: the post pass binds it as a combined image sampler
(`VulkanRenderer.cpp:1290-1292`), so the sampler is used. **`Window.cpp:102-103`'s
comment citing `VulkanRenderer.cpp:646`** — still stale (the call is at
`:690-692`); still too small to task, fix it in passing. **The unclamped
`num_shadow_cascades` write-back** (`VulkanRenderer.cpp:343-350` clamps the
cascade count but never tells the GUI) — real but latent: `MAX_CASCADES` is 3
and no desktop `maxMultiviewViewCount` is below it, and the write-back itself
is not unit-testable without a device. **`Src/KomputePlayground`** — unchanged;
still an owner decision.

### C++ Vulkan engine

## 2026-08-03 batch XVII — planner (refactor: nine headers still carry a dual-compile shim for a GLSL consumer deleted months ago, in two mutually incompatible spellings; the gate written to catch exactly this drift greps for one literal path and misses eight stale comments; two of four render stages derive their depth format twice where the other two cache it and document why)

The actionable queue was empty when this batch was written (0 `- [ ]`, 15
`- [b]` across the whole file). Every `file:line` below was read out of the tree
this pass.

**Every task in this batch is verifiable with no GPU**, deliberately: the
fifteen `- [b]` entries above are still blocked. All three land in the
container CPU lane — tasks 1 and 2 are header/comment edits gated by new
`BuildIntegrity` source scans, task 3 is a two-member change gated the same way.

**The headline is that nine headers still `#ifdef __cplusplus` themselves for a
shader compiler that no longer exists.** `Resources/Shaders/` (the GLSL tree)
was deleted when the engine moved to Slang — `TEST(BuildIntegrity,
SlangSourcesDoNotReferenceTheDeletedGlslTree)` (`buildIntegritySuite.cpp:1684`)
exists to keep it deleted, and `find Resources -name '*.glsl' -o -name '*.frag'
-o -name '*.vert' -o -name '*.comp'` returns nothing. The Slang shaders do not
`#include` these headers at all: they **redeclare** the structs
(`common/scene_types.slang:52` `ObjectDescription`, `:77` `GlobalUBO`, `:85`
`SceneUBO`; `common/push_constants.slang:14` `PushConstantRasterizer`), which is
why `TEST(BuildIntegrity, SharedStructOffsetsMatchTheCompiledSpirv)`
(`:3980`) and `HostAndShaderSharedConstantsAgree` (`:1257`) exist. No script
under `scripts/` feeds any of these headers to `slangc`. So every `#ifdef
__cplusplus` in them is a branch that is always taken and every `#else` branch
is unreachable text.

The nine, in **two incompatible spellings** of the same dead idea:
`renderer/pushConstants/PushConstant{PathTracing,Post,Rasterizer,RayTracing}.hpp`,
`renderer/GlobalUBO.hpp` and `renderer/SceneUBO.hpp` open with the same
`#ifdef __cplusplus` / `#pragma once` / five `using vec2 = glm::vec2; … using
uint = unsigned int;` aliases / `namespace Kataglyphis::VulkanRendererInternals {`
block — **six byte-comparable copies**, already drifted in their includes
(`GlobalUBO.hpp:7-10` takes four `glm/vecN.hpp` headers, the four push-constant
headers take `glm/glm.hpp`). `Src/shared/scene/Vertex.hpp:4-14` and
`Src/shared/scene/ObjMaterial.hpp:4-11` instead define `KTG_VEC2`/`KTG_VEC3`
macros with a live `glm::vecN` branch and a dead `#else` branch expanding to
bare `vec2`/`vec3`. `Src/GraphicsEngineVulkan/ObjectDescription.hpp:4-6` is a
third variant, guarding `#include <cstdint>`, with a comment still calling
itself a "shared C++/GLSL header" (`:18`).

**Second, the gate that was supposed to stop this drift only ever looks for one
literal string.** `SlangSourcesDoNotReferenceTheDeletedGlslTree` greps `.slang`
files for `"Resources/Shaders"` (`buildIntegritySuite.cpp:1692`) — so a comment
naming a dead GLSL file by its *bare* name sails through, and C++ sources are
not scanned at all. Eight such comments are live in the tree today (listed in
task 2), including `common/aces.slang:12` asserting "The C++ post.frag currently
uses pow(x, 1/2.2); migrating to this shared function fixes that discrepancy"
when `post.slang` has imported `aces` and used `aces_tonemap` since the port
(`post/post.slang:1,56`, and its own header comment at `:5-6` says so).

**Third, `PostStage` and `CascadedShadowMap` cache the chosen depth format in a
member and say why; `Rasterizer` and `DeferredRasterizer` re-derive it.**
`PostStage.cpp:195-197`: "depth_format was already resolved by
createDepthbufferImage(), which init() always runs first — reuse it rather than
querying again, so the attachment and the image it is paired with cannot
diverge." `Rasterizer` calls `chooseDepthFormat` at `:154` (inside the
`buildAttachmentDescription` argument list, with no named variable) and again at
`:256`; `DeferredRasterizer` calls it at `:98` and `:178`. In both, `init()`
runs `createTextures()` before `createRenderPass()`
(`Rasterizer.cpp:44-45`, `DeferredRasterizer.cpp` `init`), so the PostStage
shape applies verbatim.

Ordering: the three tasks are disjoint — task 1 touches only the nine headers
(plus whatever they force), task 2 only comments plus one gate, task 3 only
`Rasterizer`/`DeferredRasterizer`. Tasks 2 and 3 both add a `BuildIntegrity`
test, so if they are done in one session, add both before rebuilding.

Candidates found but NOT tasked (checked, then rejected — do not re-propose
without new evidence): **`Mesh::setModel`** (`Mesh.ixx:57`, `Mesh.cpp:92`) — a
genuinely uncalled setter and the only one a whole-tree sweep of 263 declared
functions turned up (every other accessor flagged at ≤2 references has a real
caller, including the `getCastersDrawn`/`getGBuffer*`/`getShadowMapArray`
family earlier batches suspected); one three-line deletion is not a task, fold
it in if something else touches `Mesh`. **`common/noise.slang`'s `snoise`/`fbm`
having no production consumer** — only `tests/noise_test.slang` imports it, but
the manifest row calls that out as a deliberate cross-target CI guard
("verify the shared noise math emits to BOTH targets"), so deleting it would
remove a gate, not dead weight. **The five blocking `map_async` readbacks in
the Rust crate** and **`render/histogram.rs`/`render/gpu_occlusion.rs` having no
inline tests** — unchanged since the 2026-08-03 batch rejected both; every
extracted unit still needs a live wgpu adapter. **The two `vk::AttachmentReference`
colour/depth pairs** (`Rasterizer.cpp:159-165`, `PostStage.cpp:205-211`) — two
copies of four lines, and the other three render passes
(`DeferredRasterizer.cpp:193-210`, `SkyBox.cpp:221-225`,
`CascadedShadowMap.cpp:165`) each need a different shape, so a helper would
serve two of five callers. **`forward.slang`'s `iblParams` and
`shadowCascadeIndex` both sitting at `[vk::binding(0, 1)]`** (`:66`, `:78`) —
looks like the batch X over-subscription bug returning, is not: they belong to
different pipelines with different bind-group layouts. It deserves a comment
saying so, not a task; fold it into task 2 if convenient.
**`Src/KomputePlayground`** — unchanged; still an owner decision.

### C++ Vulkan engine

## 2026-08-03 batch XVIII — planner (the one per-swapchain-image subsystem `recreateSwapChain()` forgets, whose two symptoms are an error log and a shadow pass that silently stops rendering; a model reload issued during the 2.8 s startup parse that leaves the scene holding both models; the eleventh member of the create-info builder family; a renderer change log whose "queued" list still asks for two things that shipped in July)

The actionable queue was empty when this batch was written (0 `- [ ]`, 15
`- [b]` across the whole file). Every `file:line` below was read out of the tree
this pass.

**Every task in this batch is verifiable with no GPU**, deliberately: host
golden verification is still blocked over RDP (see the `- [b]` entry at the end
of this file), so all four land in the container CPU lane — three are gated by
`BuildIntegrity` source scans, one by a new device-free unit suite.

**The headline is that `VulkanRenderer::reprovisionPerImageResources()`
(`VulkanRenderer.cpp:673-685`) re-provisions the UBOs, the shared/post/G-buffer
descriptors and the raytracing descriptors — and nothing else.** But
`CascadedShadowMap` is *also* sized per swapchain image: `init()` stores
`swapChainImageCount` (`CascadedShadowMap.cpp:48`), and
`createDescriptorSetAndPipeline()` allocates exactly that many descriptor sets
(`:262`) and that many light-matrix buffers (`:273`). The only two calls to
`dirShadowMap.init(...)` are the constructor (`VulkanRenderer.cpp:122`) and
`handleShadowResolutionChange` (`:350`); `recreateSwapChain()` never re-inits it,
so after a recreate that changes the image count the shadow map keeps the OLD
count. Both consumers then bail by index, and both bails are silent to a pixel
oracle: `uploadLightMatrices` error-logs and returns
(`CascadedShadowMap.cpp:140-144`, reached every frame from
`update_uniform_buffers` at `VulkanRenderer.cpp:765`), and `recordCommands`
returns before recording anything (`CascadedShadowMap.cpp:411-413`) — so for the
added images the shadow pass renders nothing and the lighting pass samples
whatever depth the array held last. `record_commands` bounds-checks
`sharedRenderDescriptors` and `postDescriptors` (`VulkanRenderer.cpp:870-871`)
but not the shadow map's own set vector, which is the asymmetry that let this
sit. Every other per-image subsystem is handled: `Rasterizer` resizes
`offscreenTextures`/`framebuffer` from `getNumberSwapChainImages()`
(`Rasterizer.cpp:210,238`), `PostStage` likewise (`PostStage.cpp:269`),
`GpuTimingSubsystem` is recreated at `VulkanRenderer.cpp:717`, `FrameSync` and
the command buffers at `:748-749`.

**Second, `Scene::reloadModel` wipes the scene without cancelling the
asynchronous startup parse that is still running.** `beginModelLoadAsync()`
(`Scene.cpp:91-97`) sets `modelLoadPending = true` and starts a ~2.8 s worker
(the figure is `AsyncModelParse.ixx:18`'s own measurement on the bundled model);
the GUI is live from frame one, so its "reload model" button
(`VulkanRenderer::handleModelReloadRequest`, `VulkanRenderer.cpp:386-410`) can
fire inside that window. `reloadModel` clears `model_list` and loads
synchronously (`Scene.cpp:170-188`) but leaves `modelLoadPending` set, so the
next `pollModelLoad` (`:101`) uploads the startup model and `add_model`s it on
top — two models in a scene the user asked to replace, with
`sceneConfig::getModelMatrix()` applied to whichever landed at index 0.
`AsyncModelParse` already has everything needed to discard a parse
(`waitForCompletion()`, `takeResult()`, `takeGltfResult()`), so this is a
missing call, not missing machinery.

**Third, seven hand-written `vk::ImageMemoryBarrier` field lists, in two
mutually incompatible spellings.** `Raytracing.cpp:94,125`,
`PathTracing.cpp:63,101,155` and `FrameCapture.ixx:95,127` each set the same six
boilerplate fields (both queue-family indices to ignored, and a
colour/mip-0/layer-0/count-1 subresource range) around the four that actually
differ. The drift is already visible: Raytracing/PathTracing write
`VK_QUEUE_FAMILY_IGNORED` and build the range field by field, FrameCapture
writes `vk::QueueFamilyIgnored` and uses the braced aggregate
`vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 }`. This
is the same shape as `buildAttachmentDescription`,
`buildFramebufferCreateInfo`, `buildRenderPassCreateInfo`,
`buildPipelineLayoutCreateInfo` and `buildSubpassDescription` before it.

**Fourth, `docs/cpp-renderer-improvements.md` still asks for two units that
shipped in July.** `AGENTS.md`'s routing table sends every renderer/device-path
refactor there, but its last "Shipped" row is `buildAttachmentDescription`
(commit `36937517`, 2026-08-02) and ~90 engine commits have landed since. Two
statements are now actively wrong rather than merely incomplete: "In progress
(nothing — remaining queue: stage/renderer-level RAII, sync-validated barrier
removal)" names sync-validated barrier removal and stage-level RAII, both of
which this file's own **Completed** section records as done on 2026-07-19; and
queued design note 5 says the redundant same-layout swapchain barrier should be
"remove[d] only after a sync-validation ... run confirms the post render pass's
external dependency covers it", when `VulkanRenderer.cpp:1048-1051` says exactly
that run happened and the barrier is gone.

Ordering: the four tasks are disjoint. Tasks 1, 3 and 4 each add a
`BuildIntegrity` test, so if several are done in one session, add all of them
before rebuilding.

Candidates found but NOT tasked (checked, then rejected — do not re-propose
without new evidence): **the shared depth image in `Rasterizer`** (one
`depthBufferImage` behind N per-image framebuffers, `Rasterizer.cpp:213-214`) —
looks like a cross-frame WAW between frames in flight, is not: the render pass's
external dependency already carries `eLateFragmentTests` +
`eDepthStencilAttachmentWrite` in its source scope for precisely this reason,
with a comment saying so (`Rasterizer.cpp:176-185`). **The stale
`vk::DescriptorSetLayout` handles that `reprovisionPerImageResources()` leaves in
`rasterizer`/`deferredRasterizer`/`clouds`/`skyBox`/`dirShadowMap`** — destroying
a set layout after the pipeline layout was created from it is legal, and sets
allocated from an identically-defined replacement stay compatible, so this is
spec-clean; task 1 removes the shadow-map instance of it anyway as a side
effect. **The two cloud-output barriers** (`VulkanRenderer.cpp:922,954`) — task 3
deliberately leaves them alone; they belong to the `- [b]` entry in the
2026-08-02 batch, which is blocked on a GPU golden. **`VulkanImage.cpp:134` and
`Texture.cpp:302`** — also `vk::ImageMemoryBarrier`, but their aspect/mip/layer
values are parameters of a general transition helper, not boilerplate, so they
are exempt from task 3. **Every sampler site** — all three already go through
`buildSamplerCreateInfo` (`PostStage.cpp:163`, `Model.cpp:80`,
`Texture.cpp:250`); no fourth copy exists. **`App::run()` skipping
`window->cleanUp()` on the `!window->is_valid()` early return**
(`App.cpp:38-41`) — a leak that lasts microseconds before process exit; fold it
in if something else touches `App.cpp`. **`Src/KomputePlayground`** — unchanged;
still an owner decision.

### C++ Vulkan engine

### Docs

## 2026-08-03 batch XIX — planner (a deferred geometry pass that invents a 0.1 alpha cutoff for every OPAQUE material, contradicting the header comment that defines the rule; the texture-slot clamp written out five times; a Rust frame graph that omits four of the passes it is documented to mirror, validated by a `debug_assert` that can only compare a literal to itself; a full-screen histogram compute pass recorded every frame in the mode that discards its output; the last hand-maintained list in the repo with no gate)

The actionable queue was empty when this batch was written (0 `- [ ]`, 15
`- [b]` across the whole file). Every `file:line` below was read out of the tree
this pass.

**Every task in this batch is verifiable with no GPU**, deliberately: host
golden verification is still blocked over RDP (see the `- [b]` entry below).
Tasks 1 and 2 are Slang edits whose evidence is a recompile plus new
`BuildIntegrity` source scans; tasks 3 and 4 land in the Rust crate's own
`cargo test` lane (which `Linux.yml` runs on `ubuntu-24.04`); task 5 is a new
`BuildIntegrity` gate over two checked-in files.

**The headline is that the deferred geometry pass applies an alpha cutoff of
0.1 to materials that the engine's own contract says must never discard.**
`Src/shared/scene/ObjMaterial.hpp:20-26` states the rule in prose: "A negative
value means 'not a MASK material' (OPAQUE/BLEND) — the raster shaders discard a
fragment only when `alphaCutoff >= 0` and the sampled base-colour alpha falls
below it, so OPAQUE materials (the default, and every OBJ material) never
discard and are bit-unchanged." `GltfLoader.cpp:128` implements the producer
side (`alpha_mode == cgltf_alpha_mode_mask ? material.alpha_cutoff : -1.0F`),
and `ObjMaterial`'s default ctor sets `alphaCutoff(-1.0F)`, so *every* OBJ
material and every glTF OPAQUE/BLEND material arrives with a negative cutoff.
Two of the three raster consumers honour that: `rasterizer.slang:61-62`
(`material.alphaCutoff >= 0.0 && baseSample.a < material.alphaCutoff`) and
`shadow_map.slang:46-50` (`alphaCutoff >= 0.0 && textureID >= 0`). The third
does not — `deferred.slang:60-61` reads

    float alphaCull = (material.alphaCutoff >= 0.0) ? material.alphaCutoff : 0.1;
    if (texColor.a < alphaCull) discard;

so the sentinel that means "never discard" is turned into "discard below 0.1".
Any textured surface whose base-colour texture carries alpha below 0.1 anywhere
— a diffuse PNG with an unused/zeroed alpha channel is the common case — is
solid in forward mode and full of holes in deferred mode, in the same scene,
from the same material. This is a spec deviation as well as an internal
divergence: glTF says an OPAQUE material's alpha "is ignored and the rendered
output is fully opaque".

**Second, the texture-slot clamp that batch III cited as the reason
`assignTextureOffsets` needs no bounds check is written out five times.**
`clamp(int(obj.texture_offset) + material.textureID, 0, MAX_TEXTURE_COUNT - 1)`
appears byte-identically at `rasterizer.slang:59`, `deferred.slang:58`,
`shadow_map.slang:48`, `path_tracing.slang:207` and `raytrace.rchit.slang:72`.
All five already `import scene_types`, which is where `MAX_TEXTURE_COUNT` is
defined (`common/scene_types.slang:8`), so unlike the `material_fetch` helpers
this one has a home every consumer can reach — including the two ray-tracing
shaders, which cannot import `material_fetch` (its header comment explains why:
`discard` conflicts with RT capabilities). The cap is the same one
`VulkanRenderer.cpp:1537-1544` warns about and `planFlattenedTextureSlots`
enforces; five copies is how the host and device halves of that rule drift.

**Third, the Rust crate's frame graph declares five passes where the renderer
records nine, and the assertion that was supposed to keep them in step cannot
fail.** `render/graph.rs:102-103` says the graph is "the frame graph the forward
renderer records, as data. Kept next to the recording code so the two stay in
step; `validate` proves the wiring." `forward_frame_graph()` lists shadow,
forward+sky, bloom, ssao, tonemap. `ForwardRenderer::render` additionally
records a `depth_resolve` fullscreen pass (`render/forward.rs:2100-2118`), the
occlusion/GPU-cull pass (`:2131-2167`, `TimedPass::OcclusionCull`), the
histogram build (`:2188-2193`, `TimedPass::Histogram`) and the exposure
reduction (`:2194-2198`, `TimedPass::ExposureReduce`) — so three of the eight
variants of `TimedPass::ALL` (`render/gpu_timing.rs:71-80`) have no row in the
graph at all, plus the untimed depth resolve. The `debug_assert` at
`forward.rs:2126-2129` validates `forward_frame_graph()` against `&[]` — a
literal against itself, with the identical call already asserted by
`graph::tests::forward_graph_is_valid` — so it can only ever pass, while
allocating a `Vec<PassDesc>` and a `HashSet` on every debug frame.

**Fourth, the histogram build pass runs every frame in the mode that throws its
result away.** `forward.rs:2188-2198` calls `self.histogram.encode(...)` (a
clear plus a full-screen build over the HDR target) and then
`encode_reduce(...)` unconditionally. `auto_exposure` defaults to `false`
(`forward.rs:921`), and in that mode the reduce shader takes the manual branch
immediately — `src/shaders/histogram.wgsl:146-149` reads `auto_enabled` and
returns `manual_ev` without touching a single bin. So in the default
configuration the renderer pays one full-resolution compute pass per frame
whose only consumer is a branch that is not taken. The fix is the one the code
twelve lines above already applies to bloom and SSAO, with its rationale
spelled out ("Turning the overlay slider to 0 used to cost exactly as much as
leaving it on", `forward.rs:2169-2177`): skip the pass, keep the consumer.

**Fifth, `Test/perf/baselines/win-9070xt-32core.json` is the last
hand-maintained list in this repo with nothing gating it.** The fuzz-target
array in `Windows.yml` got its gate at `buildIntegritySuite.cpp:1351-1395`
precisely because "a fuzz target declared in `Test/fuzz/CMakeLists.txt` but
never added to Windows.yml's array does not run and nothing says so"; the
Windows CPU-suite allowlist was replaced by "run everything except the two GPU
suites". The perf baseline has the same shape and no gate:
`Compare-PerfBaseline.ps1:29-32` documents that "benchmarks present in only one
file are reported but never fatal", and `Compare-PerfBaseline.Tests.ps1:70-94`
pins that behaviour in both directions. That policy is correct for the
comparator — but it means a benchmark added to `perfSuite.cpp` without a
baseline row is silently never compared, forever. The two lists happen to agree
today (17 registered instantiations, 17 baseline rows); nothing keeps them that
way.

Ordering: **task 1 before task 2** — both edit `deferred.slang`,
`rasterizer.slang` and `shadow_map.slang`, and task 2's gate is easier to write
once task 1 has already moved the alpha rule into a named helper. **Task 3
before task 4** — task 4 makes the histogram pass conditional, which task 3's
graph must then describe accurately (say so in the graph's doc comment rather
than adding a conditional row). Task 5 is disjoint from all four.

Note for tasks 1 and 2: `common/scene_types.slang` and
`common/material_fetch.slang` are imported **only** by SPIR-V-target shaders
(`rasterizer`, `deferred`, `shadow_map`, `path_tracing`, `raytrace.rchit`,
`clouds`, `cascaded_shadow`) — no `wgslMap` entry reaches them, so neither task
regenerates any checked-in WGSL and neither is blocked by the `- [b]` slangc
version floor.

Candidates found but NOT tasked (checked, then rejected — do not re-propose
without new evidence): **`gpu_cull.slang:83-84`'s comment** ("take the maximum
(nearest-to-camera) sampled depth") — backwards for this renderer's standard
0=near/1=far depth, where the maximum is the *farthest* sample; the code itself
is correct (`aabbNear > maxSampled` is the conservative test) and a one-line
comment fix is not a task — fold it in if something else touches that shader.
**The C++ engine rendering glTF `BLEND` materials as opaque** (`GltfLoader.cpp:128`
maps BLEND to the same -1 sentinel as OPAQUE, while the Rust renderer has a
sorted blend pass) — a real seventh cross-renderer divergence, but closing it
means a whole transparent pass with its own sort and pipeline state, which is an
L-sized feature and not an executor task. **`Scene::getObjectDescriptions()`
returning the vector by value** (`Scene.ixx:142`, copied at
`VulkanRenderer.cpp:1371`) — a genuine needless copy, but its only caller runs
on scene change, never per frame; fold it in if something else touches `Scene`.
**The five blocking `map_async` readbacks in the Rust crate** and
**`render/histogram.rs`/`render/gpu_occlusion.rs` having no inline tests** —
unchanged since the 2026-08-03 batch and batch XVII rejected both; every
extracted unit still needs a live wgpu adapter. **`CommandBufferManager::beginCommandBuffer`
allocating a `std::vector<vk::CommandBuffer>` for one handle** — re-confirmed and
re-rejected for the fourth time (upload-time only, never on the frame path).
**`Src/KomputePlayground`** — unchanged; still an owner decision.

### Shaders

### Rust WebGPU renderer (`third_party/OxidANT`)

### Performance testing


  **Context:** A `BuildIntegrity` gtest, not a Pester test, on purpose: the
  Pester suites only run on `[build-win]` commits, while the commit suite runs
  on every Linux push. The baseline numbers themselves stay machine-specific and
  out of CI — this gates only the *membership* of the two lists, which is
  machine-independent.

## 2026-08-03 batch XX — planner (refactor: three headers whose free functions still have internal linkage, one of which an `inline` neighbour calls across ten translation units; 47 hand-written `wgpu::BindGroupLayoutEntry` literals in the Rust crate, reducible to five named shapes; the cascade-fit signature — eleven positional parameters, four of them adjacent floats with defaults, spelled out six times)

The actionable queue was empty when this batch was written (0 `- [ ]`, 15
`- [b]` across the whole file). Every `file:line` below was read out of the tree
this pass.

**Every task in this batch is verifiable with no GPU**, deliberately: host
golden verification is still blocked over RDP (see the `- [b]` entry near the
end of this file), so nothing here may depend on it. Task 1 is a header-linkage
change gated by a new `BuildIntegrity` source scan; task 2 lands entirely in the
Rust crate's `cargo test` lane (which `Linux.yml` runs on `ubuntu-24.04`); task
3 is a signature change covered by the 19 existing `CascadedShadowMapUnit`
tests plus one new one. `Test/commit/VulkanEngine/CMakeLists.txt` globs `*.cpp`
with `CONFIGURE_DEPENDS`, so no new suite file needs registering.

**The headline is that three headers still define their free functions
`static`, and one of them is called from an `inline` function in the same
header.** Fifteen helper headers under `Src/GraphicsEngineVulkan/` use `inline`
or `constexpr` — `ExtensionSupport.hpp`, `ImageBarrierHelper.hpp`,
`RenderPassHelper.hpp`, `PipelineLayoutHelper.hpp`, `ViewportHelper.hpp`,
`FramebufferHelper.hpp`, `ImageViewHelper.hpp`, `ImageLayoutHelper.hpp`,
`MemoryHelper.hpp`, `ComputePipelineHelper.hpp`, `SceneUboMarshal.hpp`,
`LightDirection.hpp`, `GuiModelTransform.hpp`, plus `Src/shared/util/`'s two
`.ixx` — and three do not:

- `common/FormatHelper.hpp:11` `choose_supported_format`
- `vulkan_base/PhysicalDeviceChoices.hpp:23,38,51,73`
  `parseGpuSelectionMode`, `gpuSelectionModeToString`, `matchesSelectionMode`,
  `scorePhysicalDevice`
- `vulkan_base/SwapchainChoices.hpp:13,38,52`
  `chooseBestSurfaceFormat`, `chooseBestPresentationMode`, `clampSwapExtent`

All eight sit at namespace scope in column 0 (every `static` in a `.ixx` is an
indented class member, so the two cases are trivially separable). `static` at
namespace scope in a header means **internal linkage**: each including TU gets
its own private copy of the function body.

`FormatHelper.hpp` is inconsistent with itself, and that is the part that is a
real defect rather than a style nit. `chooseDepthFormat` (`:43`) is `inline` —
external linkage, one entity, defined identically in every TU — and its body
calls `choose_supported_format` (`:45`), which is a **different entity in every
TU**. [basic.def.odr] requires that in each definition of an inline function,
each name refer to the same entity; naming an internal-linkage function from an
inline one violates that, and it is IFNDR — no diagnostic required, so a green
build proves nothing. `FormatHelper.hpp` is included by nine TUs
(`DeferredRasterizer.cpp`, `FrameCapture.ixx`, `PostStage.cpp`,
`Rasterizer.cpp`, `CloudDispatch.hpp`, `Clouds.cpp`, `CascadedShadowMap.cpp`,
`Texture.cpp`) plus `formatHelperSuite.cpp`, so there are ten copies of
`choose_supported_format` and ten `chooseDepthFormat` definitions that
disagree about which one they call. Nothing is misbehaving today — the ten
copies are byte-identical, so whichever the linker keeps behaves the same — but
this is precisely the ODR class that cost this repo three months of red Linux
CI, and the fix is one keyword per function.

**Second, the Rust crate writes out `wgpu::BindGroupLayoutEntry` longhand 47
times, and there are only five shapes.** Counted across
`render/{bloom,forward,gpu_occlusion,histogram,ibl,occlusion,ssao,tonemap}.rs`
(22 of them in `forward.rs` alone), every entry is 6–9 lines of nested struct
literal, and the taxonomy is tiny:

| shape | count |
| --- | --- |
| `Buffer { Uniform, has_dynamic_offset: false, min_binding_size: None }` | 13 |
| `Buffer { Storage { read_only: true }, false, None }` | 8 |
| `Buffer { Storage { read_only: false }, false, None }` | 3 |
| `Texture { Float { filterable: true }, D2, multisampled: false }` | 7 |
| `Texture { Float { filterable: false }, D2, false }` | 2 |
| `Texture { Depth, D2, false }` / `Depth, D2Array` / `Depth, D2, ms: true` | 4 |
| `Texture { Float { filterable: true }, Cube, false }` | 1 |
| `Sampler(Filtering)` / `Sampler(Comparison)` | 7 |

`has_dynamic_offset: false` and `min_binding_size: None` are written 24 times
without exception — they carry no information at any call site. This is the
Rust twin of the C++ create-info builder family (`buildAttachmentDescription`,
`buildFramebufferCreateInfo`, `buildRenderPassCreateInfo`,
`buildPipelineLayoutCreateInfo`, `buildSubpassDescription`,
`buildImageMemoryBarrier`, `buildComputePipelineCreateInfo`, …), which this
repo has now consolidated eleven times over for exactly this reason: the
boilerplate is where the one field that differs gets lost. The crate already
has device-free unit tests living next to the code they cover
(`forward.rs:2984-3002`, `graph.rs`'s `mod tests`), so the helpers and their
guard are testable without an adapter.

**Third, the cascade-fit signature is eleven positional parameters, and it is
spelled out six times.** `computeCascadeData` and `computeCascadeDataInto` each
take `(uint32_t numCascades, const glm::mat4 &cameraView, float cameraFov,
float aspect, float nearPlane, float farPlane, const glm::vec3 &lightDir, float
shadowDistance = 0.0F, float splitLambda = 0.5F, uint32_t shadowMapResolution =
0)` — declared at `CascadedShadowMap.ixx:54` and `:73`, defined at
`CascadedShadowMapMath.cpp:82` and `:111`, and the five-line tail
`float cameraFov, / float aspect, / float nearPlane, / float farPlane, /
const glm::vec3 &lightDir,` appears six times across those two files plus
`CascadedShadowMap.cpp:87`. Four consecutive `float`s, three of them with
defaults, mean every call site is a silent transposition away from a wrong
answer that still type-checks — and the calls do look like that:
`computeCascadeData(kCascades, default_view(), kFov, kAspect, kNear, kFar,
default_light(), 0.0F, 0.5F)` (`cascadedShadowMapSuite.cpp:178`),
`computeCascadeData(numCascades, view, kFov, kAspect, kNear, kFar, lightDir,
kShadowDistance, 0.5F, kShadowMapResolution)` (`perfSuite.cpp:123-124`). The
repo already prefers named aggregates for exactly this — `PathTracingHistoryKey`
is built with designated initializers at `VulkanRenderer.cpp:1161-1167`, and
`ShadowSetBinding` / `PhysicalDeviceScore` exist to give small tuples names.

Ordering: the three tasks are disjoint — task 1 touches three headers plus
`buildIntegritySuite.cpp`, task 2 only the Rust submodule, task 3 only the
`CascadedShadowMap` module and its two test callers. Task 3 edits a module
interface (`CascadedShadowMap.ixx`) and therefore needs `-FreshContainer`; tasks
1 and 2 do not.

Candidates found but NOT tasked (checked, then rejected — do not re-propose
without new evidence): **`Mesh::setModel`** (`Mesh.ixx:57`, `Mesh.cpp:92`) —
re-confirmed dead by a fresh whole-tree sweep of every declared function
(`setModel` is still the only name whose sole two references are its own
declaration and definition); unchanged from batch XVII, still a three-line
deletion rather than a task, fold it in if something else touches `Mesh`.
**The two cloud-output barriers and their twice-written WAR comment**
(`VulkanRenderer.cpp:938-956`, `:970-1008`) — still owned by the `- [b]` entry
in the 2026-08-02 batch and still gated on a host GPU golden; this is the ninth
rejection, stop re-checking them. **The two shadow-bias formulas** —
`cascaded_shadow.slang:38` uses `max(0.002 * (1 - N·L), 0.0005)` while
`forward.slang:242` uses `clamp(0.002 * (1 - N·L) + 0.0005, 0.0005, 0.004)`, so
the two renderers disagree at grazing angles. A real divergence, but closing it
means editing `forward.slang`, which regenerates checked-in WGSL and therefore
runs straight into the `- [b]` slangc version floor at the end of this file — it
cannot be an executor task until that unblocks. **`create_shadow_pipeline` and
`create_masked_shadow_pipeline`** (`forward.rs:2836`, `:2882`) — share a
`DepthStencilState` verbatim, but they are two copies of six lines and their
`cull_mode`/`fragment` differences are documented on the spot; not worth a
helper. **`Scene::getObjectDescriptions()` returning by value**
(`Scene.ixx:142`) — re-confirmed scene-change-only, never per frame; unchanged
from batch XIX. **`VulkanRenderer`'s mixed `snake_case`/`camelCase` method
names** (`record_commands` and `drawFrame` on the same class) — real
inconsistency, but `.clang-tidy` enables `readability-*` yet clang-tidy never
runs in the container build (`docs/code-quality.md`), so a rename would be a
large untested diff with no gate behind it. **`Src/KomputePlayground`** —
unchanged; still an owner decision.

### C++ Vulkan engine

### Rust WebGPU renderer (`third_party/OxidANT`)

## 2026-08-03 batch XXI — planner (the two PowerShell modules that genuinely live in this repo have no tests, and both are broken: one reports success for a test binary that never started, the other's clang-tidy skip matches 1 of 33 module TUs; the always-on Linux lane runs 4 of 6 fuzz targets and skips exactly the one that has caught a real bug; the fourth member of the `create()`-releases-previous family; the module bootstrap AGENTS.md says cannot move upstream)

The actionable queue was empty when this batch was written (0 `- [ ]`, 15
`- [b]` across the whole file). Every `file:line` below was read out of the tree
this pass.

**Every task in this batch is verifiable with no GPU**, deliberately: host
golden verification is still blocked over RDP (see the `- [b]` entry near the
end of this file), so nothing here may depend on it. Tasks 1, 2 and 5 land
entirely in `scripts/windows/tests/` and run under the existing `pester-tests`
CI job and locally with pinned Pester 3.4.0 — no build at all. Task 3 adds a
filesystem gate to `buildIntegritySuite.cpp` plus a workflow edit. Task 4 is a
five-line source change plus an extension of an existing text-order gate.
`Test/commit/VulkanEngine/CMakeLists.txt` globs `*.cpp` with
`CONFIGURE_DEPENDS`, so no new suite file needs registering.

**The headline is that `scripts/windows/modules/` holds exactly two modules —
the two AGENTS.md says are genuinely project-specific and must stay here
(`WindowsClang.Common`, `WindowsTesting.Common`) — and they are the only two
with no Pester suite at all.** `scripts/windows/tests/` carries twelve suites;
five of them (`WindowsCMake.Common`, `WindowsConfig.Common`,
`WindowsMsix.Common`, `WindowsMsix.Signing`, `WindowsWebDav.Common`) cover
modules that were **upstreamed to ANTfrastructure on 2026-08-02** and no longer
exist under `scripts/windows/modules/`. Those suites are not redundant —
ANTfrastructure's own `windows/scripts/tests/` has no equivalents, so this repo is
their only coverage, do not delete them — but the coverage map is exactly
inverted from where the risk is. Reading the two untested modules found a real
defect in each, and neither is subtle once you look:

- `WindowsTesting.Common.psm1:228-241` — `Invoke-ManualTestExecutable` wraps the
  run in `Invoke-WithRuntimePath -Script { ... }` and puts `return $false`
  **inside the scriptblock** (`:236`). In PowerShell that returns from the
  *scriptblock*, emitting `$false` into the pipeline; `Invoke-WithAsanOptions`'s
  `& $Script` propagates it, `Invoke-WithRuntimePath` returns it, and
  `Invoke-ManualTestExecutable` never captures it — then falls through to
  `return $true` (`:241`). The function's output is therefore `@($false, $true)`,
  a two-element array. `run-clangcl-debug.ps1:203-206` does
  `$ranFuzzExecutable = Invoke-ManualTestExecutable ...; if (-not
  $ranFuzzExecutable) { throw ... }` — and `-not` on a non-empty array is
  `$false`, so **the "did not start successfully" throw is unreachable on
  exactly the path it was written for** (the Windows loader/runtime mismatch
  exit codes `-1073741511` / `-1073741515`). A fuzz executable that cannot start
  reads as a clean run.
- `WindowsClang.Common.psm1:59` — the module-TU skip is
  `if ($content -match '^import\s+kataglyphis')` over `Get-Content -Raw`.
  PowerShell's `-match` is .NET `Regex` with default options, so `^` anchors at
  the **start of the whole string**, not at each line. Every module
  implementation unit in this repo opens with `module;` and reaches
  `import kataglyphis...` only after the global-fragment includes and the
  `module kataglyphis...;` declaration. Measured this pass: 33 of the 45 `.cpp`
  files under `Src/` contain `import kataglyphis` at line start, and exactly
  **one** (`Src/GraphicsEngineVulkan/Main.cpp`) has it on line 1. So the skip
  fires for 1 file and hands the other 32 module TUs to clang-tidy — the thing
  AGENTS.md describes this module as existing to prevent
  ("hard-codes the `Src` root and the `import kataglyphis` module-TU skip").

**Second, the always-on Linux lane runs four of the six registered fuzz
targets, and the two it skips are the two that touch real engine surface.**
`Test/fuzz/CMakeLists.txt` registers six via `kataglyphis_add_fuzz_test`:
`first_fuzz_test` (`:106`), `example_fuzz_test` (`:107`),
`obj_parsing_fuzz_test` (`:109`), `gltf_parsing_fuzz_test` (`:116`),
`scene_config_fuzz_test` (`:133`), `shader_file_reader_fuzz_test` (`:160`),
`texture_loading_fuzz_test` (`:166`). Both CI lanes hand-list a subset, and the
two lists disagree with each other and with the CMake file:

| target | `Linux.yml:139-146` | `Windows.yml:240` |
| --- | --- | --- |
| `first_fuzz_test` | yes | no |
| `example_fuzz_test` | no | no |
| `obj_parsing_fuzz_test` | yes | yes |
| `gltf_parsing_fuzz_test` | yes | yes |
| `scene_config_fuzz_test` | yes | yes |
| `shader_file_reader_fuzz_test` | **no** | yes |
| `texture_loading_fuzz_test` | **no** | yes |

The asymmetry matters because the lanes are not equal: `Linux_x86.yml` runs on
every push/PR, while `Windows.yml` is opt-in per commit via `[build-win]`. So
the two targets that only Windows runs get a signal on a minority of commits —
and `Windows.yml:224-226` records that *"the shader-file reader target found a
real terminate-on-throw bug from a seed on its first run"*. `example_fuzz_test`
runs on neither lane. Windows at least fails loudly on a missing binary
(`Windows.yml:242`); the Linux step has no such check and no gate ties either
list to `Test/fuzz/CMakeLists.txt`, so target #7 will be born unrun and nothing
will say so. This repo has already single-sourced two hand-maintained lists for
exactly this reason (the Slang shader manifest, the perf-baseline/benchmark gate
in `14faba9e`); the fuzz lists are the next one.

**Third, `Texture::createTextureSampler` is the fourth member of the
`create()`-releases-previous family and the only one that still leaks.**
`Texture.cpp:242-262` overwrites `textureSampler` with a freshly created
`vk::Sampler` and never destroys the previous one, while `VulkanBuffer::create`,
`VulkanImage::create` (`ef9a8a4d`), `VulkanImageView::create` (`e9ecb576`) and
`DescriptorSetGroup::create` (`fe384d1a`) were all given release-previous
semantics in the last two days, each pinned by a text-order gate in
`buildIntegritySuite.cpp` (`ResourceCreateReleasesThePreviousAllocation:1048`,
`DescriptorSetGroupCreateReleasesThePreviousAllocation:1087`). Be honest about
the impact: this is **latent, not live**. All three current callers
(`Clouds.cpp:37`, `CascadedShadowMap.cpp:73`, `SkyBox.cpp:194`) reach it through
a freshly `make_unique<Texture>()`, including on the re-provisioning paths
(`Clouds::recreateFrameResources:164` replaces the `unique_ptr`;
`CascadedShadowMap::init` rebuilds `shadowMapArray`). The value is closing the
family — `Texture` already has the matching `releaseImageView()` (`:264`), so
the asymmetry is one missing three-line helper — and the gate that stops it
being reintroduced.

Ordering: the five tasks are disjoint. Tasks 1, 2 and 5 each touch one
PowerShell file plus one new Pester suite; task 3 touches `Linux.yml`,
`Windows.yml` and `buildIntegritySuite.cpp`; task 4 touches `Texture.cpp`,
`Texture.ixx` and `buildIntegritySuite.cpp`. Task 4 edits a module interface
(`Texture.ixx`) and therefore needs `-FreshContainer`; nothing else here does.

Candidates found but NOT tasked (checked, then rejected — do not re-propose
without new evidence): **a whole-tree sweep for dead public accessors** — ran it
this pass over every `get*`/`is*`/`has*`/`supports*` declared in a `.ixx` and it
came back clean; the ten with only two references each have exactly one real
call site (`graphicsFamilySupportsCompute` → `Clouds.cpp:102`, `getCascadeData`
→ `VulkanRenderer.cpp:217`, `getVertexCount` → `ASManager.cpp:530`, and so on).
Stop re-checking this. **`VulkanDevice` creates a device queue on
`compute_family` that it never retrieves** (`VulkanDevice.cpp:287-307` builds
the queue set from three families; `:557-558` fetches only graphics and
presentation) — real, but harmless: the Vulkan spec guarantees a
graphics+compute family exists, so `compute_family` always equals
`graphics_family` on any device that gets this far, and removing it changes
`vkCreateDevice` on a path no CPU test reaches while host GPU verification is
blocked. Revisit together with the RDP blocker. **`Scene`'s destructor undoes
`App::run()`'s device-lost guard** — `App.cpp:69-75` deliberately skips
`scene->cleanUp()` when the device is lost, but `scene` is declared *before*
`vulkan_renderer` (`App.cpp:37-45`), so `~Scene()` (`Scene.cpp:210`) runs its
`cleanUp()` after `VulkanRenderer::cleanUp()` has already destroyed the VMA
allocator and the logical device. Only benign because the leaf `cleanUp()`s are
idempotent and the normal path cleans first. Not tasked because it is owned by
the `- [b]` **Renderer-level RAII cleanup consolidation** entry at the top of
this file and blocked on the same thing: device loss cannot be induced here, so
the fix is untestable. Record it there if that entry is ever unblocked.

## 2026-08-03 batch XXII — planner (a build-directory name that three Windows presets share and two more get wrong, which CI papers over by uploading installers from both trees; a shader-sharing doc whose "compiled to both targets" list is wrong for every entry in it; the cascade-count half of a double lock whose PCF-radius twin shipped last week; a framebuffer teardown that lives in the caller and is enforced by nothing; a light slider labelled after the wrong term)

The actionable queue was empty when this batch was written (0 `- [ ]`, 15
`- [b]` across the whole file). Every `file:line` below was read out of the tree
this pass.

**Every task in this batch is verifiable with no GPU**, deliberately: host
golden verification is still blocked over RDP (see the `- [b]` entry near the
end of this file), so nothing here may depend on it. Task 1 is PowerShell +
JSON + a Pester suite and needs no build at all. Tasks 2 and 5 add
`buildIntegritySuite.cpp` gates that read files off disk. Tasks 3 and 4 are
small source edits plus CPU unit tests.
`Test/commit/VulkanEngine/CMakeLists.txt` globs `*.cpp` with
`CONFIGURE_DEPENDS`, so no new suite file needs registering.

**The headline is that `CMakePresets.json`'s `binaryDir` and
`scripts/windows/Build-Windows.config.psd1`'s `BuildDir` are two independent
answers to "where does this configuration build", and they have drifted apart
far enough that CI hedges.** `Build-Windows.ps1:94-95` reads `BuildDir`/`Preset`
out of the `.psd1` and passes the directory to the upstream driver as
`-BuildPath` (`:241`, `:263`, `:270`, `:288`, `:320`), which overrides the
preset's own `binaryDir`. So the preset field is dead on the script path and
authoritative everywhere else — `ctest --preset`, a bare `cmake --preset`, and
anyone reading the file. Measured this pass:

| configuration | `.psd1` `BuildDir` | preset | preset `binaryDir` |
| --- | --- | --- | --- |
| `clangcl-debug` | `build-clangcl-debug` | `x64-ClangCL-Windows-Debug` | `build-clangcl-debug/` ✔ |
| `clangcl-profile` | `build-clangcl-profile` | `x64-ClangCL-Windows-Profile` | **`build_release/`** |
| `clangcl-release` | `build-clangcl-release` | `x64-ClangCL-Windows-Release` | **`build_release/`** |
| `msvc-debug` | `build-msvc-debug` | `x64-MSVC-Windows-Debug` | **`build/`** |
| `msvc-release` | **`build-msvc-debug`** | `x64-MSVC-Windows-Release` | **`build/`** |

Three consequences, all real:

- `x64-ClangCL-Windows-Profile`, `x64-ClangCL-Windows-Release` and
  `x64-ClangCL-Windows-RelWithDebInfo` all inherit `build_release/`
  (`CMakePresets.json:362-371`, `:373-382`, `:384-393`, `:416-418`), so
  configuring any two of them in sequence without the script reconfigures the
  same directory with a different `CMAKE_BUILD_TYPE`. The MSVC pair does the
  same in `build/` (`:283-332`) — which is also the Linux presets' directory.
- `test-x64-ClangCL-Windows-Profile` (`:606-617`) points at
  `x64-ClangCL-Windows-Profile`, i.e. at `build_release/` — a directory the
  container build never creates. The one preset AGENTS.md advertises for
  benchmarking cannot find the tree that holds `perfTestSuite.exe`.
- `Windows.yml:293-308` uploads installers from **both** `build_release/**` and
  `build-clangcl-release/**`, commented "legacy/expected CMake preset output"
  and "also include the build directory used by the PowerShell build script".
  That is the drift, written down, in CI.

Separately, `msvc-release`'s `BuildDir` is literally `build-msvc-debug` with
`BuildDirEnv = 'BUILD_DIR_MSVC'` (`.psd1:13-18`): a Release MSVC build writes
into the Debug MSVC tree. AGENTS.md's table (`:107`) records the collision
rather than flagging it. `CMakePresets.Integrity.Tests.ps1` already guards this
file against dangling `configurePreset` and `inherits` references — it just has
no assertion tying it to the `.psd1`, which is the only reason all of the above
survived.

**Second, `docs/shader-sharing.md`'s central claim is wrong for every shader it
names.** `:90-97` lists "**Entry points compiled to both targets** (Rust/WebGPU
+ C++/Vulkan share the pass)": forward, sky, bloom, SSAO, IBL, GPU occlusion
culling, tonemap, depth resolve, occlusion bbox, tex quad. Parsing
`shader-manifest.json` this pass, the rows carrying a `spirv` target are exactly
21, and **not one of those ten files is among them**: post, the two test guards,
the four raytracing sources, path tracing, skybox, compute noise, clouds,
rasterizer, deferred and shadow_map. Every shader in the "both targets" list is
WGSL-only; only `tonemap` is annotated as such in the prose. The paragraph below
it ("**Vulkan-only**", `:99-104`) is correct, which is what makes the first one
easy to believe. Cross-checked against the engine: `grep -rn '\.spv' Src/` loads
17 distinct names, all from that Vulkan-only set — nothing loads
`gpu_cull`/`depth_resolve`/`occlusion_bbox`/`tex_quad` SPIR-V. This repo already
gates prose against data in four places (golden-suite counts,
`max-texture-count`, the perf baseline, the renderer change log); this list is
the next one.

**Third, `numCascades` never got the second lock `pcfRadius` did.**
`cascaded_shadow.slang:43` clamps the PCF radius in the shader with a comment
explaining that it is "the second lock, not the first, so a stray unclamped
write can never turn the loop below into zero iterations". Twelve lines above
it, `:16` does `int cascadeCount = int(sceneUBO.numCascades);` with no clamp,
and `:30` indexes `sceneUBO.cascadeLightSpaceMatrices[cascadeIndex]` — an array
of `MAX_CASCADES` (3) elements (`scene_types.slang:78`, `:102`) — with
`cascadeIndex` derived from that unclamped count. The host side is the only
guard, and its own bound is one-sided: `fillSceneUboCascades`
(`SceneUboMarshal.hpp:58-74`) computes `activeCascades` from `splitDepths.size()`
alone and pairs it with `viewProjMatrices[i]` under a bare `assert` (`:63`) that
compiles out in Release. Be honest about the impact: **both are latent, not
live.** The one caller (`VulkanRenderer.cpp:216-228`) already clamps to
`MAX_CASCADES` and passes two spans of identical length, and the GUI slider is
capped at `MAX_CASCADES` (`GUI.cpp:191`). The value is symmetry with a lock that
shipped four days ago and a gate that keeps both.

**Fourth, framebuffer teardown for four render stages lives in the caller.**
`VulkanRenderer::recreateSwapChain()` destroys them at `:724-727`, *before*
`vulkanSwapChain.recreate()` at `:729`, because they reference the outgoing
swapchain image views; the stages then rebuild them at `:738-748`. But
`PostStage::recreateFrameResources` (`PostStage.cpp:131-137`),
`Rasterizer::recreateFrameResources` (`Rasterizer.cpp:135-144`),
`DeferredRasterizer::recreateFrameResources` (`DeferredRasterizer.cpp:149-164`)
and `SkyBox::recreateFrameResources` (`SkyBox.cpp:429-432`) all call a
`createFramebuffer(s)` that does `resize()` + overwrite
(`PostStage.cpp:267-284`, `Rasterizer.cpp:208-226`,
`DeferredRasterizer.cpp:317-337`, `SkyBox.cpp:268-281`) and none of them
destroys anything. **Today that is correct and leak-free** — this is not a bug
report. It is that the invariant is load-bearing (dropping one line at `:724-727`
leaks N framebuffers per resize, forever, and surfaces only as an object-lifetime
error at `vkDestroyDevice`), stated in exactly one comment, and enforced by
nothing. A fifth stage gets it wrong by default. The ordering constraint is why
the destroy cannot simply move into the callee.

Ordering: the five tasks are disjoint. Task 1 touches `CMakePresets.json`, the
`.psd1`, `Windows.yml`, `AGENTS.md` and one Pester suite; task 2 touches
`docs/shader-sharing.md` and `buildIntegritySuite.cpp`; task 3 touches
`GUISceneSharedVars.ixx` (a module interface — needs `-FreshContainer`) plus its
readers; task 4 touches one Slang source, one header and two test files; task 5
touches `buildIntegritySuite.cpp` plus four comments. Nothing here needs a GPU
and nothing here needs the RDP blocker resolved.

Candidates found but NOT tasked (checked, then rejected — do not re-propose
without new evidence): **`BuildIntegrity.CheckedInWgslIsNotOlderThanItsSlangSource`
is mtime-based and therefore inert on a fresh clone** (`buildIntegritySuite.cpp:1691-1747`)
— already known, already recorded twice in this file (see the batch VI and batch
XIII prose), and already answered by the content gate
`EveryReachableSlangFunctionSurvivesIntoItsCheckedInWgsl` (`:2984`). Stop
re-finding it. **The deferred lighting pass hard-codes `metallic = 0.0` and
folds `f0 = lerp(float3(0.04), ambient, 0.0)`** (`deferred/deferred.slang:126-127`),
and the G-buffer material target spends three of four channels on constants
(`:72`) — real dead generality, but it sits inside the shader the `- [b]`
world-position-mirror entry owns, and any edit there wants the forward/deferred
parity oracle that entry is blocked on. **`FrameCapture` has no CPU suite**
where `FrameSync` and `GpuTimingSubsystem` do (`FrameCapture.ixx:34-238`) — read
it end to end this pass and found nothing wrong; its only device-free surface is
a four-flag state machine, and the two predicates worth pinning
(`isCapturableSwapchainFormat`, `capturedFormatIsBgra`) already live in
`FormatHelper.hpp` under `formatHelperSuite`. Not worth a suite until it grows
logic. **`transformAABB` rebuilds eight corners per mesh per frame, twice**
(`Frustum.cpp:95-118`, once for the camera and once for the shadow casters)
where the model matrix is per-model — measurable only under `BM_FrustumCull`,
which already covers the hot loop and shows it is not the bottleneck.

## 2026-08-04 batch — planner (refactor: eleven fullscreen render pipelines in the Rust crate that differ in four fields and repeat twenty; the one compute dispatch grid in that crate still spelled as a bare `+ 63) / 64`, next to two siblings that are named and pinned; the twelfth member of the C++ create/destroy helper family — render-pass teardown, hand-rolled in the same five stages that already route framebuffers and pipelines through a shared helper)

The actionable queue was empty when this batch was written (0 `- [ ]`, 15
`- [b]` across the whole file). Every `file:line` below was read out of the tree
this pass.

**Every task in this batch is verifiable with no GPU**, deliberately: host
golden verification is still blocked over RDP (see the `- [b]` entry near the
end of this file), so nothing here may depend on it. Tasks 1 and 2 are Rust and
verify with `cargo test` (the crate's unit tests and its `tests/*.rs`
CPU-only integration tests are all device-free; the ones that need an adapter
skip themselves). Task 3 is a header, five call sites, one CPU suite and one
`buildIntegritySuite.cpp` gate that reads files off disk.
`Test/commit/VulkanEngine/CMakeLists.txt` globs `*.cpp` with
`CONFIGURE_DEPENDS`, so no new suite file needs registering.

**The headline is that the Rust crate solved "one rule, N hand-rolled copies"
for bind-group layout entries (`render/bind_layout.rs`, five named
constructors, 47 call sites) and then never did it for the thing above it —
the pipeline descriptor.** Eleven render pipelines across four modules are the
*same* fullscreen pass, and they agree on every field but four. Measured this
pass:

| module | pipelines | vertex entry | color format | blend |
| --- | --- | --- | --- | --- |
| `render/bloom.rs:55-79` | 3 (`fs_brightpass`, `fs_blur_h`, `fs_blur_v`) | `vs_main` | `HDR_FORMAT` | `None` |
| `render/ssao.rs:57-81` | 2 (`fs_ssao`, `fs_blur`) | `vs_main` | `AO_FORMAT` | `None` |
| `render/tonemap.rs:58-81` | 1 (`fs_main`) | `vs_main` | `output_format` (runtime) | `None` |
| `render/ibl.rs:270-294` | 5 (`fs_equirect_to_cube`, `fs_downsample_cube`, `fs_irradiance`, `fs_prefilter`, `fs_brdf_lut`) | `vs_fullscreen` | `CUBE_FORMAT` ×4, `LUT_FORMAT` | `Some(BlendState::REPLACE)` |

Everything else is byte-identical in all eleven: `buffers: &[]`,
`write_mask: ColorWrites::ALL`, `primitive: PrimitiveState::default()`,
`depth_stencil: None`, `multisample: MultisampleState::default()`,
`multiview_mask: None`, `cache: None`, and `compilation_options: Default::default()`
twice. That is ~20 repeated lines per site against 4 that carry meaning. The
divergence it has already produced is in the last column: `ibl.rs` writes
`Some(BlendState::REPLACE)` where the other three write `None`. Those are the
same thing — `REPLACE` is `src * 1 + dst * 0` — so nothing renders differently
today; it is two spellings of "no blending" that arrived because nobody was
looking at the same line twice. The same file also holds nine copies of the
single-bind-group-layout pipeline layout (`bind_group_layouts: &[Some(&x)],
immediate_size: 0`): `bloom.rs:49-53`, `forward.rs:622-626`,
`forward.rs:2696-2700`, `gpu_occlusion.rs:105-109`, `histogram.rs:73-77`,
`ibl.rs:264-268`, `occlusion.rs:153-157`, `ssao.rs:51-55`, `tonemap.rs:52-56`.
Three sites in `forward.rs` (`:520`, `:570`, `:600`) take multiple layouts and
are correctly out of scope.

**Second, `gpu_occlusion.rs` is the one compute dispatch in the crate whose
workgroup size is a bare literal.** `:250-251` reads

```rust
// round up to 64 (workgroup_size)
let wg_count = (count as u32 + 63) / 64;
```

Twelve lines of `histogram.rs` do the identical thing through
`auto_exposure::BUILD_WORKGROUP` / `CLEAR_WORKGROUP` (`:208`, `:225-226`),
using `div_ceil`, against constants whose doc comments name the shader entry
point they mirror (`auto_exposure.rs:29-33`) — and
`tests/histogram_constants.rs` *pins both of them* against
`src/shaders/histogram.wgsl` by parsing `@workgroup_size(...)` out of the
source. `gpu_cull.wgsl:29-31` declares `@workgroup_size(64, 1, 1)` on
`cs_main`; the Rust side has no constant, no `div_ceil`, and no gate. Be
honest about the impact: **this is latent, not live** — 64 is correct in both
places today. `gpu_cull.wgsl` is Slang-generated, so the generated-shader
gates do cover WGSL-vs-Slang; what none of them cover is WGSL-vs-Rust, which
is exactly the edge `tests/histogram_constants.rs` was written for. The value
is that changing the Slang workgroup size would currently produce a silently
wrong dispatch count with no test saying so.

**Third, render-pass teardown is the twelfth member of the create/destroy
helper family, and the only one still hand-rolled.** The engine has
`Kataglyphis::destroyFramebuffers` / `destroyFramebuffer`
(`common/FramebufferHelper.hpp:55-70`) and `destroyPipelineAndLayout`
(`common/PipelineLayoutHelper.hpp:49-60`), both taking their handles by
reference, both no-op'ing on a null device, and both enforced by a
`buildIntegritySuite.cpp` gate that greps `Src/GraphicsEngineVulkan/` for the
raw call (`FramebufferTeardownGoesThroughTheSharedHelper` at `:5612-5657`,
`PipelineTeardownGoesThroughTheSharedHelper` at `:5504-5555`). The render pass
sitting between them in every one of those `cleanUp()` bodies is still written
out longhand, five times, in the same five stages:

| file | lines | member |
| --- | --- | --- |
| `renderer/Rasterizer.cpp` | `:120-123` | `render_pass` |
| `renderer/DeferredRasterizer.cpp` | `:121-124` | `renderPass` |
| `renderer/PostStage.cpp` | `:115-118` | `render_pass` |
| `scene/sky_box/SkyBox.cpp` | `:406-409` | `renderPass` |
| `scene/light/directional_light/CascadedShadowMap.cpp` | `:244-247` | `renderPass` |

All five are the identical three-line `if (handle) { device.destroyRenderPass(handle); handle = nullptr; }`,
and three of the five sit on the line directly after a
`destroyPipelineAndLayout(...)` call — i.e. the shared helper and its
hand-rolled neighbour are adjacent in the same function. Two of the five omit
the null-device guard that `Rasterizer.cpp:107` provides earlier in the body;
`CascadedShadowMap.cpp:244` and `SkyBox.cpp:406` reach
`device->getLogicalDevice()` through a `shared_ptr` that `cleanUp()`'s own
idempotence contract says may already be reset. Nothing is broken today — the
early returns cover it — but that is one more invariant held by reading, in
the family the repo has spent eleven tasks making unnecessary to read.

Ordering: the three tasks are disjoint and touch no shared file. Tasks 1 and 2
are both in `crates/webgpu_renderer` but in different modules (task 1:
`bloom`/`ssao`/`tonemap`/`ibl` + a new `pipeline_desc.rs`; task 2:
`gpu_occlusion.rs` + a new `tests/cull_constants.rs`) — the one overlap is
`gpu_occlusion.rs:105-109`, which task 1 rewrites and task 2 does not read, so
do task 1 first if both land in one session. Task 3 is C++ only.

Candidates found but NOT tasked (checked, then rejected — do not re-propose
without new evidence): **`ForwardRenderer::animation_duration`
(`forward.rs:2375`) and `disable_gpu_timing` (`:981`) have no caller anywhere
in the workspace** — real dead API, but two accessors is thinner than the two
dead-accessor sweeps that already shipped (batches VIII and X), and
`animation_duration` is the natural read-side pair of `set_animation_time`,
which a viewer UI would want. **`GpuTimingSubsystem::timestampMask()`
(`GpuTimingSubsystem.ixx:238`) is dead** — one reference in the tree, its own
definition; same reasoning, and it is the accessor for a field the class
genuinely uses internally. Fold all three into the next dead-API sweep rather
than spending a task on them. **`VulkanRenderer` mixes ~50 `snake_case` member
functions (`record_commands`, `update_uniform_buffers`, `create_surface`,
`create_command_pool`, …) with camelCase ones (`drawFrame`,
`recreateSwapChain`, `recordRasterPass`)** — a real convention violation, but
renaming across module interface units means `-FreshContainer` rebuilds for
pure churn with no assertion that can distinguish success from failure. Not
worth an executor session. **`render_tonemapped` is 647 lines
(`forward.rs:1511-2158`) and `ForwardRenderer::new` is 479 (`:368-847`)** —
the extraction is real and wanted, but it is the one change in this crate that
can silently reorder passes, and the oracles that would catch that
(`tests/headless.rs`, `tests/forward_ambient.rs`) need an adapter, i.e. the
RDP-blocked host run. Task it when that blocker clears. **The C++
`vk::WriteDescriptorSet` sites are fully consolidated** — all seven live inside
`DescriptorSetGroup.cpp`; nothing outside it hand-rolls a descriptor write any
more. Stop re-checking.

## 2026-08-04 batch II — planner (a cloud volume whose half-extents are the density slider, so either of two sliders at 0 divides by zero in the inverse model matrix; a mouse delta that is assigned instead of accumulated, so every cursor event but the last one in a frame is thrown away; the path-tracing doc's "open work" still asking for the white furnace it shipped; six `cleanUp()` bodies that release raw handles yet are neither idempotent nor called from a destructor, against 17 siblings that are; the submit half of the command-buffer pair that cannot report failure, and the one caller that destroys its originals anyway)

The actionable queue was empty when this batch was written (0 `- [ ]`, 15
`- [b]` across the whole file). Every `file:line` below was read out of the
tree this pass.

**Every task in this batch is verifiable with no GPU**, deliberately: host
golden verification is still blocked over RDP (see the `- [b]` entry near the
end of this file). Tasks 1, 2 and 5 land pure helpers plus unit suites that
already exist (`sceneUboMarshalSuite.cpp`, `frontendInputSuite.cpp`,
`blasGeometryLimitsSuite.cpp`); tasks 3 and 4 are `buildIntegritySuite.cpp`
gates that read files off disk. `Test/commit/VulkanEngine/CMakeLists.txt`
globs `*.cpp` with `CONFIGURE_DEPENDS`, so no new suite file needs
registering. Nothing here is Rust, so the broken host MSVC linker is not in
the way.

**The headline is that `compute/clouds.slang` uses one field for two
unrelated jobs.** `:131` reads `cloud.scale = scene.cloudMeshScale.w` — the
density multiplier every `sample_density` term is scaled by (`:40-52`) — and
then `:137` reads

```hlsl
cloud.radius = scene.cloudMeshScale.xyz * cloud.scale * 10.0;
```

so the same value is also a multiplier on the volume's half-extents. Three
lines later `:150-155` builds the inverse model matrix by dividing by those
half-extents (`1.0 / cloud.radius.x`, `-cloud.offset.x / cloud.radius.x`, and
the y/z twins). `cloudMeshScale.w` is `guiSceneSharedVars.cloud_scale`
(`VulkanRenderer.cpp:237`), whose GUI slider is `0.F..1.0F`
(`GUI.cpp:210`), and `cloudMeshScale.xyz` is `cloud_mesh_scale`, slider
`0.F..1000.0F` (`GUI.cpp:216`). **Both minima are exactly zero**, and ImGui
lets a slider sit on its minimum, so either one dragged to the left end makes
a `cloud.radius` component zero and the inverse matrix `±inf`; `box_intersect`
(`:64-81`) then compares NaNs, and whatever `outputImage[tid.xy]` receives
(`:191`) flows into the offscreen HDR target and through post. Clouds default
to off (`GUISceneSharedVars.ixx:73`), so this needs the user to tick "Enable
Clouds" first — it is a reachable hang-your-frame bug, not a startup one.

Sitting on top of that, the two host variables are named for the opposite of
what they feed:

| GUI variable | GUI label | UBO slot | shader field | what it does |
| --- | --- | --- | --- | --- |
| `cloud_scale` | "Density" | `cloudMeshScale.w` | `cloud.scale` | density multiplier (+ the radius coupling above) |
| `cloud_density` | "Coverage threshold" | `cloudMeshOffset.w` | `cloud.threshold` | noise cut-off below which a sample is clear sky |

`SceneUBO.hpp:46-47` even documents the slots as `w = cloudScale` /
`w = cloudDensity`, propagating the inversion one layer further. This is the
same class of defect as the shipped `direcional_light_radiance` rename
(`02d7aa38`), one step worse because here the two names are swapped rather
than merely misspelled.

**Second, `handle_mouse_callback` assigns where it must accumulate.**
`Src/shared/frontend/WindowInputCallbacks.ixx:88-89` ends with

```cpp
    x_change = static_cast<float>(x_pos) - last_x;
    y_change = last_y - static_cast<float>(y_pos);
```

and the reader is `consume_axis_delta` (`:28-33`), which returns the value
**and zeroes it**. A field that is zeroed on read is an accumulator; this one
is overwritten by every event. `glfwPollEvents()` dispatches every queued
cursor-position event before the frame runs, and `process_camera_input`
(`FrameInput.ixx:35-39`) consumes exactly once per frame, so with a 1000 Hz
mouse at 60 FPS roughly sixteen events arrive per frame and fifteen of their
deltas are discarded — the camera turns by the last event's step instead of
the frame's total travel. The symptom is a look that under-rotates and
stutters in proportion to how fast you move the mouse, which reads as "mouse
sensitivity is inconsistent" rather than as a dropped-input bug.

**Third, `docs/path-tracing.md` asks for work its own sibling doc records as
shipped.** Its "Open work" section (`:108-113`) still lists "Furnace golden:
wants a uniform-environment toggle (the sky is a gradient)". That toggle
shipped: `PathTracing.cpp:106-117` reads `KATAGLYPHIS_PT_FURNACE` and sets
`clearColor.w = 1.0F`, `path_tracing.slang:60-65` branches on it
(`bool furnace = pc_ray.clearColor.w > 0.5`) for both the albedo (`:198-201`)
and the environment (`:122-125`), `GoldenRender.PathTracingPassesTheWhiteFurnaceTest`
exists at `goldenRenderSuite.cpp:2486`, and
`docs/cpp-renderer-improvements.md:27` logs it under commit `44e93e52`. The
same doc's "Verification" section (`:80-96`) says "**Four** PT-facing
goldens" and names four; the suite now holds six —
`PathTracingAccumulatesAndConverges` (`:1338`),
`PathTracingRespondsToTheDirectionalLight` (`:1476`),
`PathTracingHonorsTheQualityControls` (`:1568`),
`RaytracedWorldFollowsTheModelTransform` (`:1694`),
`PathTracingPassesTheWhiteFurnaceTest` (`:2486`) and
`RaytracedLargeMeshDoesNotLoseTheDevice` (`:2829`). Unlike
`gpu-golden-testing.md` (gated by `GoldenTestCountsInDocsMatchTheSuite`,
`buildIntegritySuite.cpp:3974`), `model-loading.md` (`:4072`),
`shader-sharing.md` (`:4149`) and `cpp-renderer-improvements.md` (`:6322`),
**`path-tracing.md` has no gate at all** — which is why it drifted.

**Fourth, `cleanUp()` idempotence is a convention 17 classes keep and 6
break.** `AGENTS.md`'s Code Conventions state that `cleanUp()` "remains for
explicit early teardown and is idempotent". Measured this pass across the 24
classes under `Src/GraphicsEngineVulkan/` that declare `void cleanUp()`:

| class | destructor | `cleanUp()` releases | guard |
| --- | --- | --- | --- |
| `GUI` | `GUI.cpp:344` `= default` | ImGui context + backends + `vk::DescriptorPool` (`:268-275`) | none |
| `Window` | `Window.cpp:171` `= default` | `glfwDestroyWindow` + `glfwTerminate` (`:74-78`) | none |
| `VulkanSwapChain` | `VulkanSwapChain.cpp:180` `= default` | image views + `vk::SwapchainKHR` (`:173-178`) | none; `device` deref unguarded |
| `VulkanInstance` | `VulkanInstance.cpp:127` `= default` | `instance.destroy()` (`:125`) | none |
| `VulkanDevice` | `VulkanDevice.cpp:196` `= default` | pipeline cache + VMA allocator + `logical_device.destroy()` (`:93-102`) | none |
| `ASManager` | `ASManager.cpp:438` `= default` | TLAS + every BLAS handle (`:416-436`) | `if (!vulkanDevice)` only; handles are never nulled |

The other seventeen (`Allocator`, `DeferredRasterizer`, `PathTracing`,
`PostStage`, `Rasterizer`, `Raytracing`, `VulkanRenderer`, `Clouds`,
`CascadedShadowMap`, `Model`, `Scene`, `SkyBox`, `Texture`,
`DescriptorSetGroup`, `VulkanBuffer`, `VulkanImage`, `VulkanImageView`) all
spell `~X() { cleanUp(); }`. `Mesh` and `VulkanBufferManager` are the two
legitimate `= default`s — every member is itself RAII — and must be left
alone. The live consequence is in `App.cpp:70-81`: on the device-lost path
`gui->cleanUp()` is deliberately skipped, and because `~GUI()` is `= default`
the ImGui context and both backends are never shut down at all, even though
`ImGui_ImplGlfw_Shutdown()`/`ImGui::DestroyContext()` need no device. The rest
is latent: nothing calls any of these twice today, which is exactly what makes
it an invariant held by reading.

**Fifth, the command-buffer pair is only half checked.**
`beginCommandBuffer` returns a null handle on failure and every call site
checks it — `EveryBeginCommandBufferResultIsChecked`
(`buildIntegritySuite.cpp:3761`) enforces that. Its partner
`endAndSubmitCommandBuffer` (`CommandBufferManager.ixx:9-12`,
`CommandBufferManager.cpp:56-126`) returns `void`, yet it has two failure
exits that leave the work unperformed: `queue.submit` failing (`:89-99`) and
the fence-wait fallback (`:101-112`). Eleven call sites cannot tell.
`ASManager::compactBLAS` is the one where that matters:
`ASManager.cpp:230-231` submits the compaction copies, and `:236-240` then
destroys every original BLAS and installs `compacted` in its place —
unconditionally. If that submit failed, the copies never ran, and the TLAS
built moments later (`createTLAS`, `:246`) is built over acceleration
structures whose contents are undefined. The same function has a second gap:
`:205-229` builds a buffer and a `vk::AccelerationStructureKHR` per
`compacted_sizes[i]` with no check that the value is non-zero, and
`VkBufferCreateInfo::size` must be greater than zero
(VUID-VkBufferCreateInfo-size-00912) — a BLAS the driver reports as
compacting to 0 bytes turns into an invalid create call.

Ordering: the five tasks are disjoint. Task 1 and task 4 both touch
`gui/GUI.cpp`, but at different ends (task 1 the cloud sliders at `:204-221`,
task 4 `cleanUp()` at `:268-275`) — do them in either order. Task 5 touches
`ASManager.cpp` teardown-adjacent code; if it lands in the same session as
task 4, do task 4's `ASManager` idempotence first so task 5 edits the final
shape.

### C++ Vulkan engine

### Cross-renderer / shared frontend

### Docs

Candidates found but NOT tasked (checked, then rejected — do not re-propose
without new evidence): **`compute/clouds.slang`'s `phase_HG` (`:57-61`)
spells its denominator `1 + g² + 2g·cosθ` where the canonical
Henyey-Greenstein form for this `cosTheta` convention is `1 + g² − 2g·cosθ`,
which would make the volume back-scattering at `g = 0.5` instead of
forward-scattering**, and **the powder term at `:182-186` ADDS to
transmittance (`transmittance = saturate(transmittance + powderness)`) after
the `< 0.01` early-out, so denser samples come out more transmissive**. Both
look wrong on the derivation, both change how clouds look, and neither can be
confirmed without the host GPU run that is blocked over RDP — task them
together the moment that blocker clears, with a golden that measures cloud
luminance against sun azimuth. **Decoupling `cloud.radius` from
`cloud.scale` (`clouds.slang:137`)** is the natural follow-up to task 1 and is
deliberately excluded from it for the same reason: it resizes the rendered
volume. **`GpuTimingSubsystem::timestampMask()`
(`GpuTimingSubsystem.ixx:238`), `ForwardRenderer::animation_duration`
(`forward.rs:2373`) and `disable_gpu_timing` (`forward.rs:979`) are still the
only dead public API in either renderer** — re-measured this pass by scanning
every zero-arg and every argument-taking member declaration under `Src/` and
every `pub fn` in `crates/webgpu_renderer/src/`; three symbols is still too
thin for a task of its own, so keep folding them into the next sweep that has
other reasons to exist. **`Scene::reloadModel` (`Scene.cpp:184`) does not call
`resolveModelPath` while `loadAdditionalModel` (`:76`) does** — checked, and
it is correct: the only caller (`VulkanRenderer.cpp:404-408`) resolves the
path first. Stop re-checking that one.

## Completed (kept for the reasoning, not the status)

- **Stage-level RAII** (2026-07-19) — leaf types (`VulkanBuffer`/`VulkanImage`)
  are move-only with destructor release; extended through the render stages.
- **Sync-validated barrier removal** (2026-07-19) — sync validation first
  exposed 10 real depth-sync hazards, fixed in the same unit.
- **GPU timestamps + debug labels per pass** (2026-07-19).
- **`DescriptorSetGroup` extraction** (2026-07-19, VulkanRenderer -617 lines).
- **Clouds compute cost** (2026-07-19) — per-pixel `inverse()` hoisted into
  the UBO, A/B verified. Half-res dispatch still open as a quality tradeoff.
- **CMake preset diet** (2026-07-19: 26/24/1 → 23/22/6) — the real problem
  was one test preset, not preset count.
- **CI sanitizers** (2026-07-19) — ASan+UBSan and TSan steps on Linux CI plus
  a `linux-debug-asan-clang` preset.
- **C++ golden rendering tests** (2026-07-19) — headless capture
  (`requestFrameCapture`/`takeCapturedFrame`, fence-synced) plus structural
  assertions. Works while the desktop is locked, unlike screenshots.
- **Perf suite that measures the engine** (2026-07-19) — camera / projection /
  scene-config / OBJ-parse benchmarks; baseline table above.
- **SceneConfig fuzzing** (2026-07-19) — also fixed fuzz targets never
  enabling `CXX_SCAN_FOR_MODULES`, which had made engine modules unfuzzable.
- **Shadow casters were culled by the shadow pass** (2026-07-20, `f429634f`)
  — the camera projection is Y-flipped for Vulkan, reversing triangle winding;
  the cascade matrices come from `glm::ortho` with no such flip, so back-face
  culling removed exactly the faces the camera keeps. The depth map sat at its
  clear value for ~99.8% of sampled texels. Culling is now off for that
  pipeline. Note the earlier "1.4% occlusion" figure never reproduced (it
  measured 0.031% on re-run) — see the open instrument item above.
- **Cascade fitting** (2026-07-20) — shadows fit a `shadow_distance` (60)
  rather than the camera far plane: 3.80 -> 3.04 cm/texel over the subject.
  A practical/logarithmic split blend exists but defaults OFF (lambda 0)
  because measurement did not support enabling it.
- **Perf suite registered with CTest** (2026-07-20) — gates on "the
  benchmarks execute", not on a time budget; see the commit for why.
- **GUI -> Scene round-trip tests** (2026-07-20) — five CPU-only tests over
  the two-copy split that is a suspect in the instrument disagreement.
- **Shader-file reader fuzzing** (2026-07-20) — found `fileExists` throwing
  on permission-denied paths, which is a terminate with exceptions disabled.
- **CSM caster transform** (2026-07-20, `bf6fa37e`) — the shadow pass used a
  hard-coded identity model matrix while the forward pass used the scene's
  (a scale of 60), so casters rendered at 1/60 size and the depth map never
  left its clear value. Five earlier defects were found and fixed while
  chasing this one; the CPU unit tests in `cascadedShadowMapSuite.cpp` and the
  `ShadowPushCarriesTheSceneModelMatrix` regression guard came out of it.
### sccache solved, and the abseil incompatibility that hid behind everything (2026-07-20)

**sccache: root cause was the volume mount.** Ran the server by hand with
trace logging: every `DiskCache::put_raw` died with os error 3 on the wcifs
volume at `C:\sccache`, while PowerShell could write the same paths - the
failure is specific to how the server writes (tempfile + rename) on wcifs.
`SCCACHE_DIR` now lives in the container FS (`C:\sccache-local`), which is
fine because builds run in the persistent container - the cache lives exactly
as long as the thing using it, and a volume with 100% write errors persisted
nothing anyway. Verified after the fix: **757 cache writes, 0 errors**.

Two traps found while fixing it, both mine:

- `SCCACHE_ERROR_LOG` must not live under `SCCACHE_DIR`: the server opens the
  log BEFORE the disk cache creates its directory, dies if the parent is
  missing, and then every wrapped tool fails with "Timed out waiting for
  server startup". With `RUSTC_WRAPPER=sccache` that poisons `cargo tree`,
  which corrosion reports as "Failed to find a dependency on cxxbridge-cmd" -
  three indirections from the cause. The log now sits at `C:\sccache-error.log`.
- A fresh container exposed that the "working" Windows fuzz build was stale
  objects: FUZZTEST at main does not compile against the abseil LTS it itself
  pins.

**The abseil incompatibility (both platforms, one bug):** `fuzzing_bit_gen.h`
friend-declares `absl::random_internal::{DistributionCaller, MockHelpers}`
without including their headers, and in abseil LTS 20260526 `bit_gen_ref.h`
no longer provides them transitively. Upstream's Bazel CI layers includes
differently, so they do not see it. Fixed in two places with the reason for
the asymmetry recorded: the `fuzztest_*` library targets get a force-include
flag, but OUR fuzz targets cannot - a force-include flows into the
synthesized C++20 module BMI compiles of imported engine modules, which have
no abseil include path (tomlplusplus BMI failed with exactly that) - so the
sources include the two headers before `fuzztest.h` instead, which module
synthesis never sees. Full Windows container build: 3/3 steps, 66 tests pass.

The module-TU bypass still caps sccache's payoff; that part of the sccache
item stays open.

### Incremental container builds can ship ODR-broken binaries (2026-07-20, SEVERE)

Found while landing the GPU-timing JSON export, and it upgrades the recorded
"touching a source file usually does NOT cause the container to recompile it"
papercut from annoyance to correctness bug.

Adding members to `VulkanRenderer` (a C++23 module interface, `.ixx`) and
rebuilding incrementally produced a binary where `commitSuite.cpp` allocated
the OLD sizeof while the constructor wrote the NEW layout - an instant ASan
heap-buffer-overflow at construction. The "rebuild" ran **19 ninja edges**;
consumers of the changed module were never recompiled. Ruled out sccache
first: clearing the cache and rebuilding reproduced the crash (an 83% hit rate
on the post-change build looked damning and was innocent). Only deleting the
build tree on BOTH host and container and cold-building produced a sound
binary - 67 tests pass.

Consequence: **after any module-interface change, an incremental container
build is not trustworthy until the module dependency tracking survives the tar
transport.** Until the mtime/dyndep interaction is fixed, treat "ASan crash at
object construction after touching an .ixx" as build skew, not as a code bug -
and cold-build before debugging anything.

### GPU timings are now a comparable artifact (2026-07-20)

`KATAGLYPHIS_GPU_TIMING_JSON=<path>` makes the renderer accumulate raw (not
GUI-smoothed) per-pass times and write averages on cleanUp. Measured on this
machine over 94 frames: ShadowCascades 0.066 ms, Main 0.042 ms, Post 0.037 ms,
Sky 0.024 ms. Unsupported timestamps still write the file with
`"timestamps_supported": false`, so "cannot measure" and "never ran" are
distinguishable. The Rust renderer exposes the same numbers via
`gpu_timings_ms()`, so the side-by-side harness can now compare timings.

### #74 The Linux fuzzer lane — ROOT CAUSE FOUND AND FIXED (2026-07-20)

Red since 2026-05-17. **The cause was an ODR violation, not FUZZTEST and not
the toolchain**, and both of my earlier hypotheses were wrong.

`Test/fuzz/CMakeLists.txt` applied `-fsanitize=address` **per fuzz target**,
while the abseil that FuzzTest links was built without it. Abseil's
`raw_hash_set` layout depends on whether ASan is active, so the two disagreed
about container internals and the binary died during startup — before reaching
a single test, which is why it crashed even while merely *listing* tests.

Reproduced locally in the CI image (Rancher Desktop — see ANTfrastructure
`docs/rancher-desktop-linux-containers.md`), same source, same compiler, one
variable changed:

| ASan applied to | Result |
|---|---|
| the fuzz target only (what CI did) | `raw_hash_set.h:1016` assertion, *"Try enabling sanitizers."* / SEGV |
| **every TU** | **2 tests PASSED** |

The local build also gave a legible assertion where CI only ever showed
`SEGV on unknown address 0x000000000000`. Three months of that bare SEGV cost
far more than the twenty minutes the container took.

**Windows was never affected**, and the reason is the whole story: on Windows
`third_party/CMakeLists.txt` builds a `kataglyphis_fuzztest_windows_asan`
interface library and propagates the flags to abseil, re2 and every
`fuzztest_*` target by hand. Linux got the per-target flag and none of that
propagation. The bug is that asymmetry.

**The fix:** fuzz targets now *require* `myproject_ENABLE_SANITIZER_ADDRESS`
project-wide on Linux and refuse to build otherwise, the per-target flag is
gone, and CI runs them from `build-asan-clang` instead of the plain Debug tree.

**Two other things this turned up:**

- `:latest` had not been rebuilt since 2026-04-16 while `:latest-cross` is
  refreshed routinely. CI now builds against `:latest-cross`.
- That switch exposed a hardcoded `--gcc-toolchain=/opt/gcc-15.2.0` in 32
  places in `Linux.yml`; the cross image ships **gcc-16.1.0**, so linking
  failed with `cannot find crtbeginS.o`. Updated. Worth deriving rather than
  hardcoding if it moves again.

**The second failure arrived on schedule, plus a third problem that explains
the lane's whole history of lying.** The ODR-fix run failed differently:
`build-asan-clang/first_fuzz_test: No such file or directory`. The ASan build
step had reported success in ~30 seconds — because it failed at configure and
`cmd 2>&1 | tee log` reports tee's exit code, and the runner's default shell
has no pipefail. **Every build step in `Linux.yml` was masked this way**; only
the fuzzer step, which has no `tee`, could ever surface failure. That is why
the lane's failures always landed on the fuzzer step regardless of what was
actually broken. Fixed with an explicit `shell: bash` default (`-eo pipefail`).

The underlying configure failure: the `:latest-cross` image runs as uid 1001
(`kataglyphis`) with `CARGO_HOME=/usr/local/cargo` owned by root, so
Corrosion's cargo dies with "failed to create directory .../registry".
Verified in the image locally; `cmake-configure-build.sh` now falls back to a
writable `${TMPDIR:-/tmp}/cargo-home` when the configured one is unwritable
(fallback itself verified in the image: cargo 1.93.1 runs).

**Lesson recorded on my own process:** I ruled out FUZZTEST pin drift against a
breakage date I had not verified, then spent three CI round trips on a control
that moved two variables at once. The local reproduction took one container run
and answered it outright. Get the failing thing into a shell before theorising.

## 2026-08-04 batch III — planner (four shading paths that drop glTF's `baseColorFactor` the moment a texture exists, where the Rust twin multiplies it; a texture upload that returns `true` after the submit it just ignored failed; an app that exits 0 after a device loss closed the window; the exposure reduction whose CPU twin has a NaN recovery it lacks, next to two of its constants no gate pins; the Rust crate's `clippy`/`rustfmt`, which this repo compiles twice and lints zero times)

The actionable queue was empty when this batch was written (0 `- [ ]`, 15
`- [b]` across the whole file). Every `file:line` below was read out of the
tree this pass.

**Host GPU golden verification is still blocked over RDP** (see the `- [b]`
entry near the end of this file), so every task here is written to be
*accepted* without an adapter: tasks 2 and 3 land pure helpers plus
`buildIntegritySuite.cpp` grep gates, task 4 is CI wiring verified by running
the linters, task 5's acceptance is a CPU parse test plus a gate. Task 1 is
Rust, where the host MSVC linker is broken (`cargo test` cannot link — see
`docs`/memory), so its acceptance is `cargo clippy --all-targets` compiling
the new test plus the always-on Linux lane running it on push. Task 5's
*pixels* still want a golden re-run when RDP clears; that is called out in the
entry rather than hidden.

**The headline is that the C++ engine drops glTF's `baseColorFactor`, in four
places, whenever the material also has a base-colour texture.** The pattern is
byte-identical in all four:

| shader | line | textured branch | untextured branch |
| --- | --- | --- | --- |
| `rasterizer/rasterizer.slang` | `:57-68` | `ambient = baseSample.xyz` | `ambient = material.diffuse` |
| `deferred/deferred.slang` | `:57-73` | `texColor = textures[..].Sample(..)` | `texColor = float4(material.diffuse, 1.0)` |
| `raytracing/raytrace.rchit.slang` | `:70-82` | `ambient += textures[..].SampleLevel(..)` | `ambient += material.diffuse` |
| `path_tracing/path_tracing.slang` | `:204-213` | `hitColor = textures[..].Sample(..)` | `hitColor = material.diffuse` |

`GltfLoader.cpp`'s `fromGltfMaterial` (`:111-155`) *does* carry the factor —
`baseColor = pbr.base_color_factor.rgb` at `:117`, stored as `diffuse` at
`:143` — so the value reaches the GPU in `ObjMaterial.diffuse` and is then
thrown away by every path that finds a texture. The glTF 2.0 spec defines base
colour as factor **×** texture, and the Rust renderer already implements it
that way: `forward/forward.slang:393` is
`float4 albedo = prim.base_color * baseSample * In.vertexColor`, and the
checked-in `forward.wgsl:721` shows the same product. So this is simultaneously
a spec-conformance bug in the C++ engine and a cross-renderer divergence that
`docs/shader-sharing.md` does not record. The symptom is silent: a glTF that
tints a shared texture per material (the standard atlas/variant pattern)
renders every variant identically in the C++ engine and correctly in the Rust
one. OBJ is unaffected in practice — `ObjLoader.cpp:199` fills `diffuse` from
`Kd`, which is `1 1 1` for the textured materials in the tree — which is also
why no golden has ever moved on it.

**Second, `Texture::uploadRgba` reports success it did not verify.** Commit
`8001c7b6` gave `endAndSubmitCommandBuffer` a `bool` return with two real
failure exits (`CommandBufferManager.cpp:89-99` — `queue.submit` failed, the
buffer is freed and nulled — and the fence-wait fallback at `:101-112`).
`Texture.cpp:190-191` discards it with `static_cast<void>` and then returns
`true` at `:197`. `createFromFile`/`createFromMemory` forward that `true`, and
`addTextureOrDefault` (`ModelAssembly.ixx:28-38`) reads it as "keep this
texture" — so a failed upload leaves an image whose device memory was never
written bound into the descriptor array and sampled as undefined content,
instead of taking the white-default fallback that already exists one branch
away. `createDefaultTexture` (`:200-207`) returns `void`, so the fallback
itself cannot report failure either. The `static_cast<void>` is *allowed* by
`buildIntegritySuite.cpp:3864-3917` (the gate demands the result be assigned,
tested or explicitly discarded), so this is not gate drift — it is a call site
that took the discard when it had a `bool` return of its own to put it in.
Same file, `:165-169`: the null-command-buffer path returns `false` *after*
`vulkanImageView.cleanUp()` (`:149`) and `createImage()` (`:151`) already ran,
leaving the `Texture` holding a fresh image, no view, and a non-zero
`mip_levels`.

**Third, the app exits 0 after a fatal frame.** `drawFrame`'s
`abort_frame_with_fatal_error` (`VulkanRenderer.cpp:460-467`) logs, sets
`device_lost_detected` when the result is `eErrorDeviceLost`, and calls
`glfwSetWindowShouldClose(..., GLFW_TRUE)`. The loop in `App.cpp:49-68` then
exits normally and `:82` returns `EXIT_SUCCESS` unconditionally;
`Main.cpp:183-185` hands that straight to the process. Two consequences: a
device-lost run (the exact failure mode `path-tracing device lost` is about)
is indistinguishable from a clean quit at the shell, and
`Run-SyncValidation.ps1`, which exits non-zero *only* on `SYNC-HAZARD` in the
log, passes a run that lost the device before recording anything. Note the
window is also closed by two paths that never set `device_lost_detected`
(`:505-512`, invalid sync handles) and by every non-device-lost
`abort_frame_with_fatal_error` (a failed `queue.submit`, a failed
`waitForFences`), so `hasDeviceLost()` alone is not the predicate — the
renderer needs a "the loop aborted" flag distinct from "the device is gone",
because `App.cpp:70-77` deliberately keys Vulkan teardown on the latter.

**Fourth, `histogram.wgsl`'s reduction is missing the one guard its CPU twin
documents as load-bearing.** The shader's own header says it "mirrors
`render::auto_exposure::{average_luminance, exposure_ev_for_luminance,
adapt_exposure_ev}`". `adapt_exposure_ev` opens with
`if !current_ev.is_finite() { return target_ev }`
(`auto_exposure.rs:130-132`), and `a_non_finite_current_value_recovers_instead_of_propagating`
(`:350-355`) pins it with the reason: "If exposure ever becomes NaN the frame
is lost; adaptation must be able to climb out rather than staying NaN
forever." `cs_reduce_exposure` has no equivalent: `current_ev` is read from the
persistent buffer at `:168` and flows into `current_ev + (target_ev -
current_ev) * blend` at `:194` and into the hold path at `:185`, both of which
propagate a non-finite value for the rest of the process. Alongside it,
`tests/histogram_constants.rs` pins four of the shader's six hand-duplicated
constants — `HISTOGRAM_BINS`, `MIN_LOG_LUMINANCE`, `MAX_LOG_LUMINANCE` and the
two workgroup sizes — but **not** `EXPOSURE_KEY` (`histogram.wgsl:112` vs
`auto_exposure.rs:37`) or `BLACK_THRESHOLD` (`histogram.wgsl:17` vs the bare
`1e-6` literals at `auto_exposure.rs:46` and `:102`). That file exists
precisely because this shader is the one Slang cannot generate and no
generated-shader gate covers it; a drifted `EXPOSURE_KEY` would make the GPU
expose to a different grey than the CPU oracle in `tests/histogram.rs`
asserts, silently.

**Fifth, this repo compiles the Rust crate twice and lints it zero times.**
`Linux.yml:277-286` runs `scripts/linux/run-cargo-tests.sh` on the
`ubuntu-24.04` leg, and its comment states the case exactly: the crate is
compiled here by the Rust bridge and the wasm demo, but its tests only ran in
`Kataglyphis-RustProjectTemplate`'s own workflow, "so edits made to
`crates/webgpu_renderer` from this working tree got no test signal until the
submodule was pushed separately". The identical argument applies to
`clippy`/`rustfmt` and has not been acted on: `grep -rn "clippy\|rustfmt"
.github/workflows/` returns nothing in this repo, while the submodule's own
`rust_ubuntu24_04.yml:123` runs
`third_party/ANTfrastructure/linux/scripts/02-toolchain/rust/cargo_fmt_clippy.sh`.
The driver already exists upstream (`cargo fmt --all -- --check` then
`cargo clippy --all-targets --all-features -- -D warnings`), so per AGENTS.md
§ "Rule: Reusable Work Belongs in ANTfrastructure" this repo owes only a thin
wrapper and a workflow step.

Ordering: the five are disjoint. Tasks 2 and 3 both add
`buildIntegritySuite.cpp` gates; do them in either order but rerun the whole
suite after the second. Task 5 touches four `.slang` files and nothing else
here does.

### Rust WebGPU renderer (`third_party/OxidANT`)

### CI

- [b] **(M) Run the Rust crate's `rustfmt`/`clippy` on this repo's always-on Linux lane** (**blocked on owner decision**) — the crate is compiled twice here and linted zero times, so edits to `crates/webgpu_renderer` from this working tree get no lint signal until the submodule is pushed separately.

  **Blocker (found while doing step 1, "run the linters locally to learn
  whether the pinned commit is clean"):** it is not clean. `cargo fmt --all
  -- --check` against the pinned commit
  (`bf26e12353ee686c1581b5c25b8b6edd32ba9148`) reports diffs in ~30 files,
  all under `crates/webgpu_renderer` itself (`src/asset/gltf_loader.rs`,
  `src/render/{animation,bounds,cascades,texture,tile_grid}.rs`,
  `src/scene/mod.rs`, several files under `tests/`, etc.) — not in some
  other crate, so step 5's crate-scoped `-p kataglyphis_webgpu_renderer`
  fallback does not help; the dirt is already inside the one crate that
  fallback would scope to.

  This contradicts the task's premise ("the submodule pin is at a commit
  whose own workflow runs this script green"): `rust_ubuntu24_04.yml:112`'s
  "Check formatting" step has `continue-on-error: true`, so that workflow
  reports green regardless of the fmt/clippy outcome — it was never actually
  gating. `clippy` could not be verified locally either: this host's MSVC
  linker is broken (see `AGENTS.md`/memory `rust-crate-msvc-linker-broken`),
  and `cargo clippy` still links proc-macro/build-script crates
  (`serde_core`, `cubecl-common`, `time-macros`, ...) even though it skips
  linking the crate under lint, so the task's assumption "clippy does not
  link" does not hold here.

  Wiring step 3 as specified — an unconditional hard gate mirroring the
  existing "Run Rust renderer tests" step, no `continue-on-error` — would
  turn this repo's always-on Linux lane red starting now, not just on future
  `crates/webgpu_renderer` edits. That's a call for the owner: either (a)
  fix formatting in the `Kataglyphis-RustProjectTemplate` submodule itself
  and bump this repo's pin (a cross-repo change outside this task's scope),
  or (b) accept the new step running non-blocking, which weakens the gate
  this task exists to add.

  `scripts/linux/run-cargo-lints.sh` has been added (mirrors
  `run-cargo-tests.sh`, delegates to
  `linux/scripts/02-toolchain/rust/cargo_fmt_clippy.sh` workspace-wide, no
  `-p` filter) and confirmed to correctly delegate and correctly fail (exit
  1) on the diffs above — it is not wired into `Linux.yml` yet. Once the pin
  is clean, step 3 (add the CI step right after `:286`) and step 4 (AGENTS.md
  wrapper-map + "What CI runs" updates) are a small follow-up.

  **Files to read:**
  - `.github/workflows/Linux.yml` — `:277-286`, the "Run Rust renderer tests" step, whose comment already makes this exact argument for tests
  - `scripts/linux/run-cargo-tests.sh` — the wrapper to copy verbatim (`CARGO_HOME` fallback, `RUST_PROJECT_DIR` resolution, the "delegate upstream" comment)
  - `third_party/ANTfrastructure/linux/scripts/02-toolchain/rust/cargo_fmt_clippy.sh` — the upstream driver: `cargo fmt --all "$@" -- --check` then `cargo clippy --all-targets --all-features "$@" -- -D warnings`
  - `third_party/OxidANT/.github/workflows/rust_ubuntu24_04.yml` — `:123`, where the submodule runs the same script workspace-wide and green
  - `AGENTS.md` § "Rule: Reusable Work Belongs in ANTfrastructure" and the wrapper map

  **Steps:**
  1. Before writing anything, run the linters locally from `third_party/OxidANT` to learn whether the pinned commit is clean: `cargo fmt --all -- --check` and `cargo clippy --all-targets --all-features -- -D warnings`. Clippy does not link, so the broken host MSVC linker is not in the way. Record the result in the commit message.
  2. Add `scripts/linux/run-cargo-lints.sh`, a near-copy of `run-cargo-tests.sh`: source `lib/common.sh`, resolve `REPO_ROOT`/`RUST_PROJECT_DIR`, assert the ANTfrastructure script exists, export the same `CARGO_TARGET_DIR`/`CARGO_HOME` fallbacks, then `( cd "${RUST_PROJECT_DIR}" && bash "${CARGO_FMT_CLIPPY_SH}" )`. Run it **workspace-wide, with no `-p`** — `cargo fmt --all -p <crate>` is a conflicting-arguments error, and workspace-wide is exactly what the submodule's own green CI runs.
  3. Add a "Lint Rust renderer crate" step to `.github/workflows/Linux.yml` immediately after the existing Rust test step (`:286`), same `if: ${{ inputs.runner == 'ubuntu-24.04' }}` gate, same `run-in-linux-container@main` action, `script: bash ./scripts/linux/run-cargo-lints.sh`. ARM must not pay for it, for the same reason the comment at `:277-281` gives for tests.
  4. Add the new wrapper to `AGENTS.md`'s wrapper map table (next to the `run-cargo-tests.sh` row) and to `AGENTS.md` § "What CI runs" where the Rust test step is described. Keeping that table complete is a stated invariant.
  5. If step 1 surfaced findings in crates **other than** `webgpu_renderer`, do not fix them here and do not silence them with `#[allow]`: the submodule's own CI owns those crates. Fall back to a crate-scoped wrapper instead — `cargo fmt -p kataglyphis_webgpu_renderer -- --check` and `cargo clippy -p kataglyphis_webgpu_renderer --all-targets --all-features -- -D warnings` invoked directly rather than via the upstream script — and say in the script's header comment why the upstream delegation was not usable.

  **Test:** Run `bash ./scripts/linux/run-cargo-lints.sh` (Git Bash on the host, or in the Linux container per `AGENTS.md` § "Running the Linux build locally") and confirm it exits 0. There is no unit test for a CI step; the acceptance is a clean local run plus the workflow YAML parsing (`gh workflow view` or a `yq`/`python -c "import yaml"` parse of `Linux.yml`).

  **Build:** none (CI/scripts). The Linux lane runs on every push, so no `[build-win]`/`[build-arm]` marker is needed to get the signal.

  **Context:** This closes the last gap in the argument `Linux.yml:277-281` already makes. The submodule pin is at a commit whose own workflow runs this script green, so a red first run means the pin drifted or this repo's working tree has uncommitted crate edits — both worth knowing, and both invisible today.

## 2026-08-04 batch IV — planner (refactor: a formatting-drift figure quoted three times, all three wrong and two of them contradicting each other, behind a build check that reports and never fails; the cloud half of the GUI→UBO marshalling, where one of four `vec4`s goes through the shared header and three are packed inline against a shader nothing pins them to; the depth attachment, created by the same seven-argument chain in three raster stages)

## 2026-08-04 batch V — planner (the one local runner that reports a broken run as a clean one and skips five of seven fuzz targets; the alpha half of the `baseColorFactor` fix that shipped yesterday, where a factor-only MASK material never discards; a base-colour texture that declares `TEXCOORD_1` and is silently sampled with UV0; the model×mesh draw walk written twice with the same flat-index invariant; the camera/light half of the GUI→SceneUBO marshalling, where three of four `.w` slots carry values no shader reads)

The actionable queue was empty when this batch was written (0 `- [ ]`, 15
`- [b]` across the whole file). Every `file:line` below was read out of the
tree this pass.

**Host GPU golden verification is still blocked over RDP** (see the `- [b]`
entry near the end of this file), so every task here is accepted without an
adapter: tasks 1 and 3 are script/loader changes with a `buildIntegritySuite`
grep gate, task 2's acceptance is a CPU parse assertion plus a gate (its
*pixels* want a golden re-run once RDP clears — called out in the entry, not
hidden), tasks 4 and 5 are pure refactors whose acceptance is the existing
CPU suites staying green plus a new gate.

  **Steps:**
  1. In `MeshDrawRecorder.ixx`, export a `SceneMeshVisit` aggregate
     (`uint32_t modelIndex, meshIndex, flatMeshIndex; Mesh *mesh; glm::mat4
     modelMatrix; AABB worldBounds;`) and a
     `forEachSceneMesh(Scene *scene, auto &&visit)` that owns the walk: the
     model loop, `getModelMatrix`, the mesh loop, `findMesh`, the
     `unknownBounds` fallback, `transformAABB`, and the flat-index increment
     that happens for **every** mesh. The callback returns `bool drawn` so the
     caller keeps ownership of the cull decision and the counters stay in one
     place.
  2. Rewrite `recordSceneMeshDraws` over it: the callback keeps the
     inverse-transpose computation (hoist it so it still runs once per model,
     not once per mesh — use `visit.meshIndex == 0` or cache the last
     `modelIndex`), the `isVisible` test, the push, `setCullMode`, the binds
     and the draw. `MeshDrawStats` is filled from the callback's return value.
  3. Rewrite `CascadedShadowMap::recordCommands`'s inner loop over the same
     helper, keeping the cascade-union `isVisibleAsShadowCaster` test and
     `castersDrawn`/`castersConsidered`. Delete the duplicated
     `unknownBounds`/`findMesh` comment there and link to the helper instead.
  4. Confirm behaviour is unchanged by reading, not by guessing: the flat index
     must still advance for culled meshes in both callers, and the shadow pass
     must still push before binding.

  **Test:** Add `BuildIntegrity.OnlyOneWalkFlattensSceneMeshesForDrawing` to
  `buildIntegritySuite.cpp`: grep `Src/GraphicsEngineVulkan/` for
  `getMeshCount(` inside a loop over `getModelCount(` and assert exactly one
  source file (`MeshDrawRecorder.cpp`) contains that pair — same
  "one rule, one definition" gate the depth-attachment, framebuffer and
  render-pass helpers already carry. CPU only.

  **Build:** `clangcl-debug`, with `-FreshContainer` because
  `MeshDrawRecorder.ixx` is a module interface:
  `pwsh -ExecutionPolicy Bypass -File .\scripts\windows\Build-Windows-Container.ps1 -Configurations clangcl-debug -FreshContainer`
  Then `.\build-clangcl-debug\commitTestSuite.exe` from the repo root. Once the
  RDP blocker below clears, `GoldenRender.FrustumCull*` and the shadow goldens
  are the behavioural check.

  **Context:** This is the same family as the eleven create/destroy helpers
  already extracted (see `docs/cpp-renderer-improvements.md`) — the payoff here
  is that the flat-index invariant stops being a comment repeated in two files
  and becomes a single loop. Do not try to also unify the push constants or the
  cull predicate; those are genuinely different and merging them is how a
  helper becomes a config struct nobody can read.

## 2026-08-04 batch VI — planner (the geometry half of the "upload reports success it never verified" family — vertex/index buffers, the TLAS instance buffer and the skybox cubemap all discard the submit result the texture path just learned to read; a masked-shadow pass in the Rust renderer that alpha-tests with the wrong UV set and without vertex-colour alpha; the analytic sky written out three times across two languages; a Rust orbit controller that never ends a drag on focus loss and snaps when the cursor comes back off an egui panel)

The actionable queue was empty when this batch was written (0 `- [ ]`, 15
`- [b]` across the whole file). Every `file:line` below was read out of the
tree this pass.

**Host GPU golden verification is still blocked over RDP** (see the `- [b]`
entry near the end of this file), so every task here is accepted without an
adapter: task 1's acceptance is the CPU suites plus a `buildIntegritySuite`
gate, task 3's is the existing CPU suites plus a new constants-pinning test,
and tasks 2 and 4 are Rust, where the host MSVC linker is broken (`cargo test`
cannot link — see memory), so their acceptance is `cargo clippy --all-targets`
compiling the new tests plus the always-on Linux lane running them on push.
Task 2's *pixels* want a golden/visual re-run when RDP clears; that is called
out in the entry rather than hidden.

**The headline is that the submit-failure fix that shipped yesterday
(`8da7d054`) covered exactly one of the four upload paths.** `8001c7b6` gave
`CommandBufferManager::endAndSubmitCommandBuffer` a `bool` return with two
real failure exits (`CommandBufferManager.cpp:89-99`, `:101-112`).
`Texture::uploadRgba` now reads it. The other three still do not:

| path | discard | what is then treated as valid |
| --- | --- | --- |
| `VulkanBufferManager.cpp:34-35` (`copy_buffer_impl`) | `static_cast<void>` | every vertex, index, material and material-index buffer (`Mesh.ixx:130-131`), the TLAS instance buffer (`ASManager.cpp:323-330`), the object-description SSBO (`VulkanRenderer.cpp:1373`, `:1381`) |
| `VulkanBufferManager.cpp:72-73` (device overload of `copyImageBuffer`) | `static_cast<void>` | nothing — the overload has zero callers (see task 1 step 4) |
| `SkyBox.cpp:189` (`uploadCubeMapFaces`) | `static_cast<void>` | `:193-194` builds the image view and sampler, `:114`/`:127` writes the descriptor — a never-written cubemap becomes the sky |

The same three functions also *log and return* on `beginCommandBuffer`
returning null (`VulkanBufferManager.cpp:22-25`, `:63-66`, `SkyBox.cpp:153-157`)
without telling the caller, so `createBufferAndUploadVectorOnDevice`
(`VulkanBufferManager.ixx:69-100`) hands back a freshly created, entirely
unwritten device-local buffer that the ray-tracing BLAS build then reads
vertex positions out of. None of this is gate drift: `buildIntegritySuite.cpp:4122`
(`EveryEndAndSubmitCommandBufferResultIsChecked`) deliberately *permits* an
explicit discard. These are call sites that took the discard when they had a
return value of their own to put it in — the same sentence that was written
about `Texture.cpp` one batch ago.

**Second, the Rust renderer's alpha-masked shadow pass tests the wrong
texels.** `forward.slang`'s `fs_main` selects between `In.uv` and `In.uv1` per
texture slot from `prim.material_flags.y` (`:376-381`) and multiplies vertex
colour into the alpha it discards on (`:393-394`,
`albedo = prim.base_color * baseSample * In.vertexColor`). Its shadow twin does
neither: `vs_shadow_masked` (`:181-192`) forwards `o.uv = In.uv`
unconditionally and drops `In.color` on the floor, and `fs_shadow_masked`
(`:194-202`) tests `prim.base_color.a * baseColorTex.Sample(...).a`. The
generated WGSL shows the same shape (`crates/webgpu_renderer/src/shaders/forward.wgsl:279`,
`o_1.uv_4 = _S23.uv_5;`, and `:293`). `uv_set_mask` bit 0 *is* the base-colour
slot and *is* populated from the glTF (`asset/gltf_loader.rs:666-671`,
`uv_set_bit("base color", i.tex_coord(), 0)`) and packed into
`material_flags[1]` (`render/forward.rs:1729-1734`), so this is reachable
data, not a hypothetical: a MASK material whose base-colour texture declares
`TEXCOORD_1` — cut-out foliage with a second UV set is the standard case —
alpha-tests its shadow against UV0 and casts a silhouette that does not match
the geometry the forward pass draws. The C++ engine does not have this
divergence (it supports only TEXCOORD_0 and warns, `GltfLoader.cpp:161-168`),
so `docs/shader-sharing.md` does not record it either.

**Third, the analytic sky exists three times, in two languages.** The three
constants and the horizon/zenith/ground gradient are written verbatim in
`sky/sky.slang:36-44`, in `forward/forward.slang:271-281` (`SKY_ZENITH`,
`SKY_HORIZON`, `SKY_GROUND`, `sky_radiance`), and again on the CPU in
`render/ibl.rs:143-158` (`EnvironmentImage::sky`, whose own doc says "The
analytic sky of `sky.wgsl`, panoramised"). The sun-disk term is duplicated
between the first two (`sky.slang:46-49` vs `forward.slang:282-288`), down to
the `pow(cosSun, 1200.0) * 24.0 + pow(cosSun, 48.0) * 0.5` magic numbers. They
must agree — `forward.slang:442` reflects `sky_radiance(reflected, true)` into
the analytic ambient of the very surfaces the sky pass draws behind, and
`ibl.rs`'s panorama is what gets convolved into the irradiance map that
*replaces* it — and nothing checks that they do. `tests/ibl.rs:933-945` only
asserts an ordering (horizon > zenith > ground) that all three would still
satisfy after any of them drifted.

**Fourth, the Rust orbit controller has neither of the two input fixes the C++
frontend already shipped.** `OrbitController::handle_event`
(`scene/controller.rs:45-88`) has no `WindowEvent::Focused` arm — it falls into
`_ => false` — so a left-drag that ends after the window loses focus (alt-tab,
a browser tab switch) never sees `ElementState::Released` and leaves
`dragging == true` forever; every later cursor move orbits with no button held.
`Src/shared/frontend/WindowInputCallbacks.ixx:22-26` (`handle_focus_lost`) is
the fix for exactly this on the C++ side. Separately, both dispatch sites skip
the controller entirely when egui consumes the event
(`examples/viewer.rs:296-303`, `src/wasm_demo.rs:234-241`,
`if !consumed { self.controller.handle_event(...) }`), so `last_cursor` goes
stale at the pre-panel position while the cursor crosses the overlay and the
first event after it leaves diffs against that stale position — the camera
snaps by the distance travelled over the panel. That is the same defect, and
the same reasoning, as the comment at `WindowInputCallbacks.ixx:75-84` ("Keep
tracking the raw cursor position while ImGui holds capture … snapping the
camera by the distance crossed while hovering the panel").

Ordering: the four tasks are disjoint. Task 1 touches `VulkanBufferManager.ixx`
(a module interface — needs `-FreshContainer`), `VulkanBufferManager.cpp`,
`Mesh.ixx`, `ASManager.cpp`, `VulkanRenderer.cpp`, `SkyBox.cpp` and
`buildIntegritySuite.cpp`. Tasks 2 and 3 both regenerate `forward.wgsl`; if
they land in the same session, do task 2 first so task 3's regeneration is the
last one and its checked-in WGSL is final. Task 4 touches only
`scene/controller.rs` plus the two dispatch sites.

### C++ Vulkan engine

### Rust WebGPU renderer (`third_party/OxidANT`)

## 2026-08-02 — reuse-sweep residuals (the sweep itself shipped)

The 2026-08-02 reuse sweep landed: WindowsCMake/Config/Formatting/WebDav/
MSIX modules + download script upstreamed to ANTfrastructure; uv-venv logic
consolidated (4 copies -> upstream python_uv.sh / WindowsUv.Common); Slang
manifest single-sourced as `Resources/ShadersSlang/shader-manifest.json`
with the four PS/sh drifts fixed; app runners consolidated over ANTfrastructure
`app-runner.sh` + `Resolve-AppExecutablePath`; CI docker-run boilerplate
moved into ANTfrastructure composite actions; Pester wired into CI
(`pester-tests` job); cargo-retry upstreamed; LICENSES-README rewritten;
CHANGELOG.md deleted (git history + this file are the record). What remains:

- [b] **ANTfrastructure has no LICENSE file** (S, **blocked on owner
  decision**) — surfaced by the license audit; consumers cannot state its
  terms. Pick a license (sibling Kataglyphis repos use MIT) and add the
  file in ANTfrastructure. Also queued in ANTfrastructure's
  docs/refactoring-backlog.md.

## 2026-08-02 batch II — findings from the Stevedore + Rancher verification pass

- [b] **Host GPU golden verification is unusable over RDP** (M, **blocked on
  a console/physical login**) — re-confirmed 2026-08-02: 28 of 30
  `GoldenRender.*` fail from the repo root on the RX 9070 XT, every frame
  logging "No synchronization frames available; skipping draw frame"
  (`VulkanRenderer.cpp:443`) because `createSynchronization()` (`:1470`)
  sizes FrameSync from `vulkanSwapChain.getNumberSwapChainImages()` and the
  swapchain reports **0 images** under this session. `query session` shows
  the work running in `rdp-tcp#0` while `console` (ID 1) is a separate
  session — the same environmental failure first recorded 2026-08-01, in
  both active and disconnected RDP states.
  **This is NOT the FrameSync extraction (`e7e7579d`) misbehaving** — that
  guard only reports the empty swapchain — and it is not the 2026-08-02
  dedup/shader work either (which touched no engine source and whose shader
  outputs are byte-identical). Do not "fix" it by weakening the guard or
  rewriting FrameSync.
  What is actually needed: run the suite from a console/physical login and
  confirm 30/30, then decide whether host GPU verification needs a documented
  console-session prerequisite (docs/gpu-golden-testing.md) and/or whether
  the offscreen golden path should stop depending on a real swapchain at all.
  Until then treat host GPU goldens as unavailable and do not burn executor
  retries on them.

- [b] **Bump the ANTfrastructure Linux image's Vulkan SDK past slangc 2026.8**
  (S) — `third_party/ANTfrastructure/linux/Dockerfile.base` already
  carries `ARG VULKAN_VERSION=1.4.350.0` (landed in ANTfrastructure `709756e`,
  "bump Vulkan SDK to 1.4.350.0"), and this repo's submodule pin
  (`6aeb0f6`) is already past that commit — the Dockerfile edit itself is
  done, nothing left to change there.
  BLOCKER: what remains is rebuilding and pushing the multi-arch
  `:latest-cross` image to `ghcr.io/kataglyphis/kataglyphis_beschleuniger`.
  There is no CI job that does this automatically (ANTfrastructure's
  `ubuntu24.04.yml` only runs `preflight` + `build-docs`); it's a manual,
  local run of `linux/scripts/build-cross-chain.sh`, which `preflight.sh`
  itself documents as taking **hours under QEMU** across 3 architectures,
  requires `GHCR_PAT` push credentials, and mutates a shared registry tag
  that CI (`Linux.yml`) pulls for every build. That's an hours-long,
  hard-to-reverse, shared-infrastructure action outside a one-shot headless
  executor turn — needs to be run by the owner (or a long-lived session)
  with registry credentials, not attempted unattended.
  Once pushed, confirm a container run of `compile-slang-shaders.sh` emits
  10 combined WGSL files and leaves
  `git -C third_party/OxidANT status` clean.
  Build preset: none (container tooling).

## 2026-08-04 batch VII — planner (refactor: 83 declaration lines that take the engine's one `shared_ptr<VulkanDevice>` by value, against five newer helpers that take it by reference, with exactly three real sinks among them; the colour twin of the depth-attachment chain, hand-rolled in the same three raster stages the depth helper was extracted from; eleven `wgpu::TextureDescriptor` literals in the Rust crate that differ in four fields and repeat four)

The actionable queue was empty when this batch was written (0 `- [ ]`, 16
`- [b]` across the whole file). Every `file:line` below was read out of the
tree this pass.

**Host GPU golden verification is still blocked over RDP** (see the `- [b]`
entry above), so none of these three is accepted on pixels. All three are
signature/plumbing refactors with no intended behaviour change, so acceptance
is the existing CPU suites staying green plus a new single-definition gate per
task — the same shape the depth-attachment, framebuffer and render-pass
helpers already carry. Task 3 is Rust, where the host MSVC linker is broken
(`cargo test` cannot link — see memory), so its acceptance is
`cargo clippy --all-targets` compiling the new test plus the always-on Linux
lane running it on push. **Do not use `cargo fmt --check` as a gate on the
Rust crate**: the pinned submodule is known not fmt-clean (~30 files), which
is why `f88dd634` left the CI wiring blocked.

**First, the engine passes its single most-passed object by value 83 times.**
`std::shared_ptr<VulkanDevice>` is threaded through nearly every creation
path, and 83 declaration/definition lines across 45 files take it *by value* —
one atomic increment and one atomic decrement per call, for a pointer none of
them consume. The regex that finds them is
`std::shared_ptr<VulkanDevice> ?<identifier>` followed by `,` or `)`; the
heaviest files are `scene/Texture.{ixx,cpp}` (7 each),
`renderer/accelerationStructures/ASManager.{ixx,cpp}` (7 each) and
`scene/Scene.{ixx,cpp}` (5 + 4). This is not a style preference the codebase
is undecided about — the five newest helpers already take it by const
reference and read correctly:
`renderer/DepthAttachment.ixx:26`, `renderer/FrameCapture.ixx:61` and `:178`,
`scene/ModelAssembly.ixx:31`/`:46`/`:65`,
`vulkan_base/ShaderHelper.ixx:74` (`createComputePipeline`) and
`vulkan_base/VulkanBufferManager.ixx:58`. Ten lines do it right, 83 do it the
old way, and nothing records which is intended.

Exactly **three** of the 83 are genuine sink parameters — they consume the
argument with `std::move` into a member, so by-value is the correct signature
and must stay:

| sink | declaration | definition |
| --- | --- | --- |
| `DescriptorSetGroup::create` | `vulkan_base/DescriptorSetGroup.ixx:68` | `DescriptorSetGroup.cpp:78` (`device = std::move(vulkan_device)`) |
| `GltfLoader::GltfLoader` | `scene/GltfLoader.ixx:60` | `GltfLoader.cpp:32` (`: device(std::move(device))`) |
| `ShaderStagePair::ShaderStagePair` | `vulkan_base/ShaderHelper.ixx:40` | `ShaderHelper.cpp:72` (`: device_(std::move(device))`) |

Everything else copies and never moves. Note `loadSpirvShaderModule`
(`ShaderHelper.ixx:27`, `.cpp:55`) sits next to a sink but is *not* one — it
only reads the device — while its neighbour `createComputePipeline` in the
same file already takes a const reference. That file alone has all three
spellings.

**Second, the colour attachment is the depth attachment's untouched twin.**
`createDepthAttachment` (`renderer/DepthAttachment.ixx:25-45`) was extracted
because three raster stages spelled out `chooseDepthFormat -> createImage ->
createImageView` by hand. The colour/storage half of that same chain —
`createImage(..., eOptimal, <usage>, eDeviceLocal) -> createImageView(...,
eColor, 1)` — is still written out longhand in the *same three stages*:
`renderer/DeferredRasterizer.cpp:76-83` (a local `createAttachment` lambda,
called four times at `:86` and `:91-93`), `renderer/Rasterizer.cpp:242-255`,
and `renderer/VulkanRenderer.cpp:1325-1334` (`pathTracingAccumulation`,
storage-only). They already agree on tiling and memory property by
coincidence, which is exactly the accident `NoStageHandRollsTheDepthAttachment
Chain` (`buildIntegritySuite.cpp:6293`) was written to prevent on the depth
side.

**Third, the Rust crate writes `wgpu::TextureDescriptor` eleven times for one
shape.** Every one of these is a single-layer 2D texture — `dimension: D2`,
`depth_or_array_layers: 1`, `view_formats: &[]` — differing only in label,
size, format, usage, and (in two cases) mip count:
`render/bloom.rs:107`, `render/ssao.rs:100`, `render/forward.rs:2188`,
`render/ibl.rs:324` / `:382` / `:427` / `:767`, `render/texture.rs:158` /
`:234` / `:270` / `:289`. Three sites are legitimately a different shape and
must stay: `render/ibl.rs:343` (`dummy_cube`) and `render/ibl.rs:649`
(`create_cube`) are six-layer cube textures, and `render/forward.rs:679`
(`shadow_map_array`) is a depth array. The crate already has the right home
for this — `render/bind_layout.rs` and `render/pipeline_desc.rs` are the two
descriptor-shape modules extracted for exactly this reason in the 2026-08-04
batch — and `render/texture.rs` already owns two of the eleven.

Ordering: **do task 2 before task 1** if both land in one session, so the new
`ColorAttachment.ixx` exists before task 1's sweep and its signature is
covered by task 1's gate rather than added after it. (Task 2 must write the
helper with a `const std::shared_ptr<VulkanDevice> &` parameter from the
start, copying `createDepthAttachment`'s signature.) Task 3 is Rust and
disjoint from both.

## 2026-08-04 batch VIII — planner (the C++ vertex has no alpha channel, so `COLOR_0` is dropped and the MASK test the Rust twin performs with it cannot be performed at all; two of six render passes whose external subpass dependency is read-only on the source side, both of them sharing one depth image with a pass that writes it; a shadow-map re-init that leaves every swapchain image but one holding unseeded light matrices, next to a startup path that reads the cascade count from `MAX_CASCADES` instead of the GUI default one line below it; ~62 lines of command-line parser in `Main.cpp` that nothing calls; 26 `wgpu::BufferDescriptor` literals in the Rust crate that collapse to five shapes)

The actionable queue was empty when this batch was written (0 `- [ ]`, 16
`- [b]` across the whole file). Every `file:line` below was read out of the
tree this pass.

**Host GPU golden verification is still blocked over RDP** (see the `- [b]`
entry near the end of this file), so nothing here is accepted on pixels.
Tasks 1 and 2 change rendering; their acceptance is the CPU suites plus a new
gate each, and both entries name the goldens that should re-run once RDP
clears rather than pretending CPU coverage is enough. Tasks 3 and 4 are
plumbing/dead-code with source gates. Task 5 is Rust, where the host MSVC
linker is broken (`cargo test` cannot link — see memory), so its acceptance is
`cargo clippy --all-targets` compiling the new test plus the always-on Linux
lane running it on push. **Do not use `cargo fmt --check` as a gate on the
Rust crate**: the pinned submodule is known not fmt-clean (~30 files).

**First, the C++ engine cannot perform the glTF alpha test the spec defines,
because its vertex has nowhere to put `COLOR_0.a`.** glTF's alpha value is
`baseColorFactor.a * baseColorTexture.a * COLOR_0.a`. The first two terms
shipped in `bdbec99a`/`1a839cad`; the third is structurally absent.
`Src/shared/scene/Vertex.hpp:13` declares `glm::vec3 color`,
`common/scene_types.slang:28` mirrors it as `float3 color`,
`scene/Vertex.cpp:62` describes it as `eR32G32B32Sfloat`, and
`scene/GltfLoader.cpp:293` reads the attribute with `readAttribute<3>` — so
the alpha is discarded at parse time and there is no channel to carry it in.
The Rust renderer does all of this: `asset/gltf_loader.rs:492-495` widens
`COLOR_0` to `[f32; 4]` ("vec3 or vec4 in the file, always vec4 here"), and
`forward/forward.slang:194` (`o.alpha = In.color.a`) feeds `:204`
(`prim.base_color.a * baseColorTex...a * In.alpha`). The two renderers
therefore disagree on which texels a MASK material discards whenever a file
ships a vertex-alpha channel — and `d6709c55` fixed exactly this on the Rust
shadow side six commits ago, so the divergence is fresh, not historical.

**Second, two render passes declare an external dependency that cannot order
a depth write, and both share their depth image with a pass that writes it.**
Six render passes exist. Four get the source scope right:
`Rasterizer.cpp:177-193` (with a comment naming the
`SYNC-HAZARD-WRITE-AFTER-WRITE` it fixed and why
`eEarlyFragmentTests|eLateFragmentTests` + `eDepthStencilAttachmentWrite` are
both required), `SkyBox.cpp:253-262` ("Cover the depth attachment's
transition + load as well (sync hazard...)"), and both of
`CascadedShadowMap.cpp:189-199`. The two that do not:

| pass | external dependency | source scope |
| --- | --- | --- |
| `DeferredRasterizer.cpp:223-229` | `VK_SUBPASS_EXTERNAL -> 0` | `srcStageMask = eBottomOfPipe`, `srcAccessMask = eMemoryRead` |
| `PostStage.cpp:210-217` | `VK_SUBPASS_EXTERNAL -> 0` | `srcStageMask = eColorAttachmentOutput`, `srcAccessMask = eColorAttachmentWrite` |

`eBottomOfPipe` is not a meaningful *source* stage and `eMemoryRead` is not a
write, so `DeferredRasterizer`'s dependency has an empty write source scope —
yet its `depthBufferImage` is a **single** image (`DeferredRasterizer.cpp:98`,
one `Texture`, not one per swapchain image) whose `loadOp` is `eClear`
(`:191`, via `buildAttachmentDescription`'s default), so every frame's clear
races the previous frame's `eLateFragmentTests` store. `PostStage`'s depth is
likewise a single image (`PostStage.cpp:143`) cleared at `:193` — and it is
the *same* image the skybox pass renders into: `VulkanRenderer.cpp:153` and
`:741` hand `postStage.getDepthBufferImageView()` to
`skyBox.createFramebuffers`, and `record_commands` runs sky (`:1049-1055`)
immediately before post (`:1062-1068`) in one command buffer. Two passes
clearing and writing one depth image back to back, with no dependency that
covers depth on either side. (Worth noting while there: because post clears
the depth the skybox pass just wrote, the skybox's depth writes are discarded
outright — flag it, do not "fix" it in this task.)

**Third, a shadow-map re-init leaves N-1 swapchain images holding unseeded
light matrices, and the startup path reads its cascade count from the wrong
place.** `VulkanRenderer`'s constructor ends with a deliberate loop
(`VulkanRenderer.cpp:141-148`) whose comment spells out the hazard: "without
this only the image the first drawFrame() happens to acquire gets real
matrices, and the rest keep ... default-constructed matrices". The two paths
that tear the buffers down and rebuild them —
`reinitShadowMapForCurrentSettings()` (`:315-342`, reached from
`handleShadowResolutionChange` at `:344-357` and from
`reprovisionPerImageResources()` at `:688`) — do **not** re-run it.
`CascadedShadowMap::cleanUp` clears `lightMatricesBuffers`
(`CascadedShadowMap.cpp:263-264`) and `createDescriptorSetAndPipeline`
reallocates them from a freshly `resize`d `cascadeData` (`:52`), so after any
shadow-resolution or cascade-count change every swapchain image except the
next acquired one renders its shadow pass from default matrices until
`drawFrame` cycles back to it. Separately, the same function is where the
cascade count is *supposed* to come from the GUI: `:331-332` reads
`guiSceneSharedVars.num_shadow_cascades`, while the constructor at `:112-113`
passes `MAX_CASCADES` as the requested count — three lines above the comment
at `:118-121` stating "The GUI is the single source of truth for the startup
shadow-map resolution, so it can no longer disagree with what the combo
shows". The resolution half was fixed; the cascade half was not.
`GUISceneSharedVars.ixx:43` defaults `num_shadow_cascades = 3` and
`host_device_shared_vars.hpp:9` sets `MAX_CASCADES = 3`, so the two agree
**today** — this is latent drift, not a live defect, and it is cheap to close
while fixing the seeding because both live in the same function.

**Fourth, `Main.cpp` carries a complete command-line parser that nothing
calls.** `CommandLineParseResultKind` (`:35-39`), `CommandLineParseResult`
(`:41-44`), `print_usage` (`:96-99`) and `parse_command_line` (`:101-146`)
have zero call sites anywhere in the repo — `main` uses
`absl::ParseCommandLine` (`:154`) and `absl::GetFlag(FLAGS_gpu)` (`:156`)
instead. `normalize_gpu_mode` (`:46-54`) is **live** (called at `:158`) and
must stay. That is ~62 lines of anonymous-namespace code describing a
`--help`/`--gpu` contract abseil now owns, sitting in the file a reader opens
first to learn how the engine starts.

**Fifth, the Rust crate writes `wgpu::BufferDescriptor` 26 times for five
shapes.** Every one sets `mapped_at_creation: false` and differs only in
label, size and usage. Twenty-four of them fall into five usage sets:

| shape | usage | sites |
| --- | --- | --- |
| uniform | `UNIFORM \| COPY_DST` | `render/forward.rs:433`, `:626`, `:1192`; `render/tonemap.rs:76`; `render/ssao.rs:71`; `render/histogram.rs:128`; `render/occlusion.rs:195` |
| storage (host-written) | `STORAGE \| COPY_DST` | `render/forward.rs:459`, `:486`, `:492`, `:1660`, `:1671`; `render/gpu_occlusion.rs:124` |
| storage (GPU-written) | `STORAGE \| COPY_SRC` | `render/gpu_occlusion.rs:131`; `render/histogram.rs:105` |
| readback | `COPY_DST \| MAP_READ` | `render/forward.rs:2206`; `render/ibl.rs:810`; `render/gpu_timing.rs:361`; `render/gpu_occlusion.rs:367`; `render/histogram.rs:112`, `:135`; `render/occlusion.rs:498` |
| query resolve | `QUERY_RESOLVE \| COPY_SRC` | `render/gpu_timing.rs:355`; `render/occlusion.rs:492` |

Two sites are genuinely their own shape and must stay literal:
`render/histogram.rs:119` (`STORAGE | COPY_SRC | COPY_DST` — the exposure
state buffer is read back *and* seeded) and `render/occlusion.rs:472`
(`VERTEX | COPY_DST`). The crate already has the right home pattern:
`render/bind_layout.rs` and `render/pipeline_desc.rs`, with
`tests/texture_desc_single_definition.rs` (shipped `dc7b4e65`) as the gate
shape to copy.

Ordering: tasks 3 and 4 touch `VulkanRenderer.cpp`/`Main.cpp` and are
disjoint from everything else. Task 1 and task 2 both touch
`buildIntegritySuite.cpp` and both regenerate SPIR-V — if they land in one
session, do **task 2 first**, because task 1's shader edits then recompile on
top of an already-fixed render pass rather than the other way round. Task 5 is
Rust and disjoint from all four.

## 2026-08-04 batch IX — planner (a depth attachment that two render passes allocate, clear and declare dependencies for while neither of them tests or writes depth; glTF `emissive_factor`, parsed into `ObjMaterial`, uploaded per material, mirrored in `scene_types.slang` and read by zero shaders; a `KHR_texture_transform` rotation the Rust twin applies per spec and the C++ loader warns about and drops; a compute kernel that samples a base-colour texture with implicit LOD, in the one file whose ray-tracing sibling documents why that is illegal; the SPIR-V gate that would have caught it)

The actionable queue was empty when this batch was written (0 `- [ ]`, 16
`- [b]` across the whole file). Every `file:line` below was read out of the
tree this pass, at `3b141b04`.

**Host GPU golden verification is still blocked over RDP** (see the `- [b]`
entry near the end of this file), so nothing here is accepted on pixels.
Tasks 1, 2, 3 and 4 change rendering; their acceptance is the CPU suites plus
a source or SPIR-V gate each, and each entry names the goldens that should
re-run once RDP clears rather than pretending CPU coverage is enough.

**First, `PostStage` allocates a full-resolution depth image that no pass in
the engine tests or writes.** `PostStage::createDepthbufferImage()`
(`renderer/PostStage.cpp:140-148`) builds a swapchain-extent depth `Texture`
through `createDepthAttachment`. Exactly two consumers exist, and both are
inert:

| consumer | what it does with the depth image |
| --- | --- |
| `PostStage` itself | attaches it (`:192-197`, `loadOp = eClear`, `storeOp = eDontCare`), clears it every frame (`:71`), and draws a fullscreen quad with the `PipelineBuilder` depth defaults — `depth_test = true`, `depth_write = true`, `eLessOrEqual` (`:253`, `PipelineBuilder.ixx:108-110`) — against a buffer just cleared to 1.0, so every fragment passes and every write lands in a `eDontCare` attachment |
| `SkyBox` | receives the same view (`VulkanRenderer.cpp:136-139`, `:733-735`), attaches it (`SkyBox.cpp:233-236`), clears it (`:387-389`) — and its pipeline sets `setDepthTest(false).setDepthWrite(false).setDepthCompareOp(eAlways)` (`SkyBox.cpp:334-336`) |

So per frame the engine pays for one swapchain-extent depth allocation (re-made
on every `recreateFrameResources`, `PostStage.cpp:130-138`), two full-screen
depth clears, two framebuffer attachment slots, and two subpass dependencies
whose depth halves exist only to order those clears
(`PostStage.cpp:210-215`'s `buildExternalColorDepthDependency`,
`SkyBox.cpp:253-264`'s hand-rolled `eEarlyFragmentTests|eLateFragmentTests` +
`eDepthStencilAttachmentWrite`). `PostStage.cpp:190-191` already says out loud
that the clear "discards the depth values the skybox pass just wrote" — and
the skybox writes none, because its pipeline has depth writes off. Nothing
samples this image: `gbufferDescriptors.writeImage(i, 3, ...)`
(`VulkanRenderer.cpp:1655`) binds `deferredRasterizer`'s depth, a different
image. Note that ImGui draws **inside** the post render pass
(`PostStage.cpp:95`) and its pipeline is created against
`postStage.getRenderPass()` (`VulkanRenderer.cpp:154`), so the render pass may
lose its depth attachment without a compatibility break — but that wiring is
what makes this a "read both call sites first" change rather than a delete.

**Second, glTF emissive materials render black in the C++ engine.**
`GltfLoader.cpp:140` reads `material.emissive_factor` into a `glm::vec3` and
`:180` passes it to `ObjMaterial::emission`; `ObjLoader.cpp:198` fills the
same field from `.mtl`; `ObjMaterial.hpp` carries it as the fifth `vec3`;
`scene_types.slang:39` mirrors it as `float3 emission`;
`buildIntegritySuite.cpp:983` pins its offset against the compiled SPIR-V. And
then:

```
$ grep -rn "emission" Resources/ShadersSlang --include=*.slang
Resources/ShadersSlang/common/scene_types.slang:39:    float3 emission;
```

One hit — the declaration. Not one of the five shading paths
(`rasterizer.slang`, `deferred.slang`, `raytrace.rchit.slang`,
`path_tracing.slang`, `shadow_map.slang`) reads it. Twelve bytes per material
travel host→device every load to be ignored, and every emissive glTF surface
in the Vulkan renderer is lit only by the directional light. The Rust twin
does apply it: `forward/forward.slang:388`
(`float3 emissive = prim.emissive_factor.rgb * emissiveSample.rgb`) feeding
`:436` (`color = directLight + punctual + ambient + emissive`), and
`asset/gltf_loader.rs:636-640` even folds `KHR_materials_emissive_strength`
into the factor first. Same file pair, same glTF, two different pictures.

**Third, the C++ loader drops `KHR_texture_transform`'s rotation and says so
in a log line; the Rust loader applies it per spec.** `GltfLoader.cpp:154-172`
reads `transform.scale` and `transform.offset` into `ObjMaterial::uv_scale` /
`uv_offset`, and at `:167-171` warns that a non-zero `transform.rotation` "is
not applied (only scale/offset are)". `material_fetch.slang:27-31` implements
exactly that subset: `return uv * material.uv_scale + material.uv_offset;`.
`asset/gltf_loader.rs:611-626` builds the full spec matrix —
`Mat3::from_translation(offset) * Mat3::from_angle(-t.rotation()) *
Mat3::from_scale(scale)` — packs it as a 2×3 (`scene/mod.rs:173`,
`render/forward.rs:129`), and `forward.rs:1707-1715` uploads it per primitive.
Any rotated atlas/decal material therefore samples at different texels in the
two renderers, and the C++ side tells you so once per material and then
renders wrong anyway.

**Fourth, the path-tracing compute kernel samples with implicit LOD.**
`path_tracing.slang:47` is `[shader("compute")] [numthreads(8, 8, 1)]`, and
`:209` calls `textures[textureId].Sample(textureSamplers[textureId],
texCoords)`. `OpImageSampleImplicitLod` requires the `Fragment` execution
model — there are no derivatives in a compute invocation. The sibling file
knows this: `raytrace.rchit.slang:75-77` carries the comment "fragment
derivatives, so an implicit-LOD `.Sample()` fails SPIR-V" and uses
`SampleLevel(..., 0.0)`. The path-tracing port did not copy that. It is latent
rather than live on the shipped scenes — `dinosaurs.obj` is untextured, so the
branch never executes there, and it is **not** the root cause of the
device-lost tracked in the `- [b]` entry above (that reproducer never reaches
this line; do not reopen that entry on the strength of this one). It fires the
moment a textured model is path-traced.

The same two ray shaders are also the only base-colour sample sites in the
repo that skip `transform_uv`. Three of five apply it —
`rasterizer.slang:60`, `deferred.slang:60`, `shadow_map.slang:55` — while
`raytrace.rchit.slang:77` and `path_tracing.slang:209` pass raw interpolated
`texCoords`. So a `KHR_texture_transform` material already renders differently
between this engine's raster and ray modes, before task 3 widens the
transform.

**Fifth, nothing validates the emitted SPIR-V.** `grep -rn "spirv-val"` over
`scripts/`, `Test/` and the CMake files returns nothing: the Slang output is
compiled, copied and loaded, never checked. `buildIntegritySuite.cpp` already
walks `.spv` at the word level for a different purpose —
`spirv_literal_string` (`:857`) and `parse_spirv_member_offsets` (`:878`),
driven from the `spirv_root` recursive walk at `:1124-1135` — so the shape for
a structural gate is already in the file and needs no new tool, no
SPIRV-Tools dependency and no GPU.

Ordering: **task 4 before task 5**, or task 5's gate lands RED on
`path_tracing.path_tracing_main.spv`. Task 1 is C++-only and disjoint from
everything else. Tasks 2, 3 and 4 all regenerate SPIR-V; task 3 is the only
one that changes `ObjMaterial`'s field set, so land it before or after the
other two but not interleaved, to keep the `buildIntegritySuite.cpp:978-991`
offset-pin churn in a single commit.

## 2026-08-04 batch X — planner (refactor: the duplication has moved out of `Src/` and into `Test/` — a byte-identical repo-root walk in three suites and 54 hand-rolled file slurps beside a helper that already exists; a README whose three CI badges report a *different repository's* pipelines and a Sphinx site still named after the old project; 26 copies of one skip block and 20 of one three-line mode setup in the golden suite)

The actionable queue was empty when this batch was written (0 `- [ ]`, 16
`- [b]` across the whole file). Every `file:line` below was read out of the
tree this pass, at `dc1fb9a6`.

**The headline finding is a negative one, and it should change where the next
refactor batches look.** An 8-line/3-copy clone sweep over every `.cpp`,
`.ixx` and `.hpp` under `Src/` returns **nothing** — the "one rule, N
hand-rolled copies" family that has driven a dozen batches is exhausted on the
engine side. The same sweep over `Resources/ShadersSlang/**/*.slang` also
returns nothing. Over `Test/` it returns five groups, four of them 7-copy. A
sweep for `pub fn`s in the Rust crate with zero references anywhere in
`crates/` or `Src/` returns exactly two (`forward.rs:975 disable_gpu_timing`,
`forward.rs:2353 animation_duration` — folded into task 3 below as a delete,
not worth their own entry), and a sweep for zero-argument accessors in `Src/`
with no call site returns one (`timestampMask`). **The test tree is now the
duplicated part of this repository, and it is where tasks 1 and 3 point.**

**First, three suites carry a byte-identical `find_repo_root()`.**
`buildIntegritySuite.cpp:54-63`, `renderPassCreateHelperSuite.cpp:110-119` and
`sceneAsyncLoadSuite.cpp:38-47` are the same ten lines, under the same
two-line comment ("Tests run with the repo root as working directory
(`gtest_discover_tests` sets `WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}`), but be
forgiving if that changes"), down to the depth-6 loop bound. Two of the three
also carry a whole-file read: `renderPassCreateHelperSuite.cpp:121-126`
(`read_file`, returns `std::string`) and `sceneAsyncLoadSuite.cpp:62` (inline).
`buildIntegritySuite.cpp:1110-1121` has a third spelling, `read_file_text`,
returning `std::optional<std::string>` — and it is called **9 times in a file
that contains 54 hand-rolled `istreambuf_iterator<char>` slurps**. The drift
this invites has already happened once: five of the six `spirv_root`
derivations read `slang_root / "build" / "spirv"` (`:1207`, `:1261`, `:1296`,
`:1492`, `:1588`) and the sixth (`:5566`) spells the whole path out from
`repo_root` instead.

**Second, the README's three build badges point at
`github.com/Kataglyphis/Kataglyphis-Renderer`.** That is a different
repository. `git remote -v` here is
`git@github.com:Kataglyphis/Kataglyphis-BeschleunigerBallett.git`, and the
CodeQL and dependency-submission badges three lines below (`README.md:24-25`)
already use the correct slug — so the top of the README shows two repositories'
CI status side by side with nothing to tell them apart. `docs/source/conf.py`
has the same half-finished rename: `:86` already overrides
`html_theme_options["repository_url"]` to the BeschleunigerBallett URL, while
`:78` still sets `project = "Kataglyphis-Renderer"` and `:115-116` still name
the Breathe project that. The rendered site therefore titles every page
"Kataglyphis-Renderer 1.5.0 documentation" under an `index.rst` whose own H1
reads "Kataglyphis-BeschleunigerBallett Documentation". Adjacent and in the
same file: `docs/source/getting_started.md:11` lists "an OpenGL 4.6 capable
driver/runtime **for the OpenGL renderer**" as a build prerequisite, and
`docs/LICENSES-README.md:142` is the record of that renderer's removal ("kein
`third_party/glad`-Submodul mehr vorhanden, keine glad-/OpenGL-Loader-Referenzen
unter `Src/`"). Confirmed independently: the only `OpenGL` hits under `Src/`
are three comments about clip-space and winding conventions
(`SceneUboMarshal.hpp:27`, `Frustum.cpp:40,46`, `GltfLoader.cpp:364`).

**Third, `goldenRenderSuite.cpp` writes the same prologue 26 times.** 31
`TEST(GoldenRender, ...)` bodies; 26 of them open with the identical
three-line block

```
if (!harness.renderer->supportsFrameCapture()) {
    GTEST_SKIP() << "Surface does not support eTransferSrc; frame capture unavailable.";
}
```

and 20 of them then write the identical triple `renderer_vars.raytracing =
false; renderer_vars.pathTracing = false; renderer_vars.rasterizationMode =
RasterizationMode::Forward;`. The file already has the right shape for the
fix — `SKIP_WITHOUT_GPU()` at `:325` is exactly this pattern solved once — and
`EngineHarness` (`:90-145`) is the natural home for the rest.

Ordering: all three are independent. Task 2 is the only one that touches no
C++ beyond a new gate, so it is the cheapest to land first. Tasks 1 and 3 both
edit `Test/commit/VulkanEngine/`, but disjoint files — task 1 does not touch
`goldenRenderSuite.cpp` and task 3 does not touch any file task 1 rewrites.

## 2026-08-04 batch XI — planner (colour management: the swapchain is a UNORM format and *nothing* sRGB-encodes into it, which a golden test's own measured constant proves; the last survivor of the "hard-coded roughness 0.9" fix that shipped 2026-07-22; glTF `metallic_factor`, read by the loader and dropped on the floor, next to three `lerp(0.04, albedo, 0.0)` identities; three samplers that ask for 16x anisotropy without consulting the device limit, on a device the engine refuses to run on if it lacks the feature at all; five `ObjMaterial` members uploaded per material and read by zero shaders)

The actionable queue was empty when this batch was written (0 `- [ ]`, 16
`- [b]`). Batch X's three tasks all shipped (`bacdcd3f`, `75961b50`,
`3d19b5b5`). Every `file:line` below was read out of the tree this pass, at
`3d19b5b5`.

**The headline finding is that the C++ engine never sRGB-encodes its final
image, and one of its own tests measured the proof.** `post.slang:57-59` states
"The C++ engine renders to an sRGB target, so the hardware encodes and we emit
linear values." It does not. `chooseBestSurfaceFormat`
(`SwapchainChoices.hpp:13-36`) searches for `eR8G8B8A8Unorm` /
`eB8G8R8A8Unorm` — **UNORM**, never the `_SRGB` variants — paired with
`eSrgbNonlinear`, and `PostStage.cpp:161` builds the post render pass from
exactly that format. A UNORM attachment performs no transfer-function encode
on write, so the ACES output lands raw in the buffer and the presentation
engine displays it as though it were sRGB-encoded. The corroboration is in
`goldenRenderSuite.cpp:2402-2413`, whose comment says "the C++ engine's sRGB
render target does NOT re-apply `linear_to_srgb` … the ACES output is written
directly. So tonemap(1.0) = `aces_tonemap(1.0)` * 255 = 0.80380 * 255 =
204.97, matching the measured mean below almost exactly" — and the test then
asserts a mean of 205 ±6 (`:2482-2489`). **205 is the un-encoded value.** Had
the target actually been sRGB, the hardware encode would have put that pixel
at `linear_to_srgb(0.80380)` * 255 ≈ 231.6. The test did not catch the bug; it
calibrated itself to it. This is not a hypothetical: the Rust renderer had the
identical defect on non-sRGB web canvases, fixed 2026-07-20, and
`docs/webgpu-srgb-audit.md` quantifies it as "177.17 vs 127.77, a 49-level
gap — which is the 'slightly dark on web' symptom, quantified". The two halves
of the chain that *were* fixed make it worse, not better: base-colour textures
moved to `eR8G8B8A8Srgb` on 2026-07-22 (completed item 7 above) and the
skybox cubemap is `eR8G8B8A8Srgb` (`SkyBox.cpp:150`), so every input now
decodes to linear correctly and only the output encode is missing — the two
errors no longer cancel.

**Second, the `roughness = 0.9` that completed item 8 removed is still in the
ray-tracing hit shader.** That entry ("Forward shading ignores material
diffuse and roughness", DONE 2026-07-22) records replacing "the hard-coded 0.9
that DEFERRED also wrote into its own G-buffer". `rasterizer.slang:73` and
`deferred.slang:79` both derive roughness from `material.shininess` today;
`raytracing/raytrace.rchit.slang:111` is still `float roughness = 0.9;`, so
the RT path shades every surface in the scene as if it were near-fully rough
regardless of its material.

**Third, `metallic_factor` never leaves the glTF loader.**
`GltfLoader.cpp:125-129` opens `pbr_metallic_roughness` and reads
`base_color_factor` and `roughness_factor` — and not `metallic_factor`.
`ObjMaterial` has no slot for it (`ObjMaterial.hpp:6-36`), so all three C++
shading paths pass a literal `0.0` for the `brdf_direct` metallic parameter
and compute `float3 f0 = lerp(float3(0.04), ambient, 0.0)` — an identity whose
`ambient` operand is dead and whose shape deliberately mimics
`brdf.slang:56`'s documented `mix(0.04, albedo, metallic)` while pinning the
mix to zero. Three copies (`rasterizer.slang:77`, `deferred.slang:133`,
`raytrace.rchit.slang:120`) plus the CI-guard shader
(`tests/brdf_test.slang:18`). The Rust twin does the real thing
(`forward.slang:385`, `:400`): `metallic = clamp(material_factors.x *
mrSample.b, 0, 1)` and `f0 = lerp(float3(0.04), albedo.rgb, metallic)`. So
every metal in a glTF scene renders as a dielectric in the Vulkan renderer and
as a metal in the WebGPU one. `gltfParseSuite.cpp` has zero assertions on
metallic, roughness or shininess.

**Fourth, three sampler sites hard-code `maxAnisotropy = 16.0F`** without
reading `VkPhysicalDeviceLimits::maxSamplerAnisotropy`
(`PostStage.cpp:135-140`, `Model.cpp:80-85`; `Texture.cpp:260-267` passes
`VK_FALSE`/`1.0F` and is fine). VUID-VkSamplerCreateInfo-anisotropyEnable-01071
requires `maxAnisotropy` to lie in `[1.0, limits.maxSamplerAnisotropy]`, so on
any device reporting less than 16 this is a validation error. The two sites
also answer "is anisotropy available" differently — `PostStage.cpp:133` issues
its own `getPhysicalDevice().getFeatures()` call while `Model.cpp:78` uses the
cached `device->supportsSamplerAnisotropy()`. Underneath both:
`VulkanDevice.cpp:669` makes `device_features.samplerAnisotropy` a hard
device-**suitability** requirement, so the engine refuses to run at all on a
device without it — which is exactly the class of software Vulkan device
(lavapipe) that could give the golden suites a CI home, and which also makes
the `aniso ? 16.0F : 1.0F` fallbacks unreachable dead branches today.

**Fifth, five `ObjMaterial` members are uploaded per material and read by no
shader.** A grep for `material.ambient`, `.specular`, `.transmittance`, `.ior`
and `.illum` across every `.slang` returns **zero** hits (against 6 for
`.diffuse` and 2 for `.dissolve`). They are mirrored into the GPU-side struct
(`scene_types.slang:33-48`), which must match the host layout byte-for-byte
because the shaders cast a buffer device address to `Materials*` — so they
cost 44 bytes in every material record for nothing. On the host they are
written only by `ObjLoader.cpp:195-203` from the `.mtl` and read by nobody.
`SceneUBO` already has this gate (`BuildIntegrity.EverySceneUboFieldIsReadByAShader`,
`buildIntegritySuite.cpp:5728`); the material struct has none.

Ordering: task 1 and task 4 are independent of everything else and of each
other. Task 2 must land before task 3 (both edit `raytrace.rchit.slang`, and
task 3 assumes task 2 already replaced the literal). Task 3 must land before
task 5 (both edit `ObjMaterial.hpp` + `scene_types.slang` + the
`SharedStructOffsetsMatchTheCompiledSpirv` fixture; doing them in the other
order means writing that fixture twice). Task 1 shifts absolute pixel values
in the golden suite, so land it before running any golden-based verification
for tasks 2/3.

### C++ Vulkan engine

## 2026-08-04 batch XII — planner (the last lossy glTF PBR factor: `roughness_factor` round-trips through a Phong shininess approximation whose inverse is not its inverse, and the loader's own comment says so; a node whose world transform has a negative determinant, whose triangles the spec says to re-wind and which this engine instead back-face-culls away; a default `ObjMaterial` that emits blue 0.1, harmless until emissive shipped three commits ago and now glowing on a model the golden suite renders; `Ke`, read by the C++ OBJ loader and dropped by the Rust converter that feeds the renderer which does support emissive; eleven `file:line` references in one doc, seven of them wrong)

The actionable queue was empty when this batch was written (0 `- [ ]`, 16
`- [b]`). Batch XI's five tasks all shipped (`99ff68a5`, `01761e08`,
`9deae53c`, `08ba468a`, `2cbf2719`). Every `file:line` below was read out of
the tree this pass, at `3caefb7c`.

**First, `roughness_factor` is the one glTF PBR factor still going through a
lossy approximation, and `GltfLoader.cpp:110-115` documents its own defect:**
"metallic_factor carries through losslessly to `ObjMaterial::metallic`.
roughness_factor is still lossy: it round-trips through a Phong shininess
approximation … and `material_roughness()`'s `sqrt(2/(shininess+2))` does not
invert `mix(128,1,roughness)`". Both halves are in the tree.
`GltfLoader.cpp:128` reads `pbr.roughness_factor` into a local, `:144` burns it
into `shininess = mix(128, 1, clamp(roughness,0,1))`, and `ObjMaterial` has no
roughness slot (`ObjMaterial.hpp:6-58`) — so `material_fetch.slang:30-33` is
the only way back out, and it is a different curve. Concretely: glTF
`roughnessFactor` 0.5 becomes shininess 64.5, which comes back as
`sqrt(2/66.5)` = **0.173**, not 0.5; `roughnessFactor` 0.0 becomes shininess
128 → 0.124, and `roughnessFactor` 1.0 becomes shininess 1 → 0.816. The error
is largest exactly where it is most visible (the whole smooth half of the
range collapses into 0.12–0.17). All three C++ shading paths read it —
`rasterizer.slang:73`, `deferred.slang:82` (which is what lands in the
G-buffer's `.r`), `raytrace.rchit.slang:110` — and the Rust twin does the
exact thing instead: `forward.slang:386`, `roughness = clamp(material_factors.y
* mrSample.g, 0.045, 1.0)` fed from `gltf_loader.rs:633`,
`roughness_factor: pbr.roughness_factor()`. This is the same shape as the
`metallic_factor` task that shipped yesterday (`9deae53c`), and the same fix
applies. `gltfParseSuite.cpp` has assertions on metallic (`:651`, `:668`) and
none on roughness.

**Second, a glTF node with a negative-determinant transform renders
inside-out.** The spec (3.7.4, Meshes): "When a mesh primitive uses any
triangle-based topology and the determinant of the node's global transform is
negative, the winding order of the triangle faces MUST be reversed." Both
loaders bake the world matrix into vertex positions — `GltfLoader.cpp:310`,
`world * vec4(positions[i],1)` — and neither ever computes that determinant
(`grep -rn determinant Src/GraphicsEngineVulkan/scene/` returns nothing).
Mirroring a node (a `scale` with an odd number of negative components, the
standard way an exporter emits a left/right symmetric pair) therefore flips
the geometric facing of every triangle it owns while leaving the index order
alone. That is not cosmetic here: `MeshDrawRecorder.cpp:50-51` sets
`eCullMode` per draw and single-sided meshes get `vk::CullModeFlagBits::eBack`,
so a mirrored single-sided mesh has its *front* faces discarded and renders as
the inside of itself (or vanishes). The normals do not paper over it — a
provided `NORMAL` goes through `glm::inverseTranspose` (`:432`) and comes out
correctly flipped, so shading and geometry disagree; and a primitive with no
`NORMAL` gets `computeFlatNormals` (`:391`) derived from the *unreversed*
winding, so its flat normals point the wrong way too. `emitTri`
(`:349-357`) is the single choke point every topology already funnels through
(`:358-377`), which makes this a two-line behaviour change.

**Third, the default `ObjMaterial` emits blue.** `ObjMaterial.hpp:39` has
`emission(0.0F, 0.0F, 0.10F)` in the default constructor — an arbitrary
non-black value that was inert for as long as nothing shaded emission, and
stopped being inert at `59eca71c` ("shade glTF `emissive_factor` in the forward
and deferred raster paths"), which added `color += material.emission`
(`rasterizer.slang:86`) and `g.outMaterial = float4(..., material.emission)`
(`deferred.slang:82`). `ObjLoader.cpp:229` — `if (tol_materials.empty()) {
materials.emplace_back(); }` — is the reachable path: an `.obj` with no
`mtllib` gets exactly that default. Four bundled models take it
(`Resources/Models/{buddha/buddha,bunny/bunny,StanfordDragon/dragon,ShadowTest/shadow_rig}.obj`),
and **`shadow_rig.obj` is the golden suite's own fixture**
(`goldenRenderSuite.cpp:265`, `SHADOW_RIG_MODEL`, added at `:1166`), so the
constant is currently baked into rendered goldens. Everything that fills the
struct from a file overwrites the field (`ObjLoader.cpp:196` from `mp->emission`,
`GltfLoader.cpp:142` from `emissive_factor`, `neutralMaterial()` at `:102`
passes an explicit zero), which is why nothing caught it: the default is
reachable from exactly one line. The goldens are structural metrics, not
stored images (`GoldenMetrics.hpp`), so no golden needs regenerating.

**Fourth, `Ke` never survives the Rust OBJ→glTF conversion.** The C++ OBJ
loader reads it (`ObjLoader.cpp:196`, `material.emission = mp->emission`) and
now shades it. The Rust path converts `.obj` to glTF first, and that
converter's material struct has three fields — `name`, `base_color`,
`base_color_texture` (`obj_to_gltf.rs:29-35`) — with no emissive slot;
`parse_mtl` (`:109-175`) matches `newmtl`/`Kd`/`d`/`map_Kd`/`Tr` and drops
everything else through `_ => {}`; and the emitted material JSON
(`:619-625`) writes only `baseColorFactor`, an optional `baseColorTexture`,
`metallicFactor` and `roughnessFactor`. The receiving end is fully wired:
`gltf_loader.rs:638-641` reads `emissive_factor` (with
`KHR_materials_emissive_strength`), `forward.rs:1438-1441` packs it into the
material uniform, `forward.slang:41` declares it. So an emissive `.mtl` glows
in the Vulkan renderer and is black in the WebGPU one, for the sake of one
missing `match` arm. `docs/model-loading.md` claims the two OBJ paths agree
and does not record this.

**Fifth, `docs/model-loading.md` is the only doc in the tree that cites
`file:line`, and most of its citations are stale.** Eleven such references
(`grep -rEon '[A-Za-z_/.-]+\.(cpp|ixx|hpp|rs):[0-9]+'` finds 11 there and 1
everywhere else combined). Verified this pass: `GltfLoader.cpp:282` for
`COLOR_0` is now `:294-301`; `GltfLoader.cpp:377` for the `computeFlatNormals`
fallback is now `:391`; `GltfLoader.cpp:464-483` for the `imageSlot` map is now
`:478-`; `ObjLoader.cpp:210-240` for `pathSlot` is now `:189-214`;
`ObjLoader.cpp:333-340` for `attrib.colors` is now `:313-320`; and
`ObjLoader.cpp:399-404` for its `computeFlatNormals` fallback points at
nothing — that loader has no such call, only the comments at `:375`/`:380`.
The doc also has no material-field mapping table at all, which is now a real
gap: `ObjMaterial` grew five fields in eight days (`alphaCutoff`,
`uv_transform_row0/1`, `metallic`, and `roughness` in task 1 of this batch) and
the only place their `.mtl`/glTF provenance is written down is the header's own
comments. This doc already carries a machine-checked marker
(`<!-- max-texture-count: N -->`, `buildIntegritySuite.cpp:4957-5022`), so the
enforcement pattern is in place.

Ordering: task 2 and task 4 are independent of everything and of each other.
Task 1 and task 3 both edit `Src/shared/scene/ObjMaterial.hpp` — do not run
them concurrently; task 1 first (it also touches `scene_types.slang` and the
`ObjMaterial_natural` offset fixture, which task 3 does not). Both are
module-interface changes (`ObjMaterial.ixx` includes that header in its global
module fragment), so both need `-FreshContainer`. Task 5 documents what tasks 1
and 3 change, so land it last.

### C++ Vulkan engine

### Rust WebGPU renderer (`third_party/OxidANT`)

## 2026-08-04 batch XIII — planner (refactor: the ray-tracing pipeline is the one stage that never adopted the shared shader-stage builder — six hand-assigned `pName = "main"` blocks survive project-wide, four of them here, next to four five-line shader-group blocks that differ in two fields; a whole C++23 module whose entire job is to add one log line to three free functions, and whose `read()` has zero production callers; and 50 copies of one open-and-getline loop in the gate suite, beside a helper header that says it is "the one place those helpers live now")

The actionable queue was empty when this batch was written (0 `- [ ]`, 15
`- [b]`). Batch XII's five tasks all shipped (`4156455e`, `86d18037`,
`90d62581`, `2f68854c`, `11705447`). Everything below was read out of the tree
this pass, at `11705447`.

**First, `Raytracing.cpp` is the last stage that hand-assigns
`vk::PipelineShaderStageCreateInfo`.** `common/ComputePipelineHelper.hpp:13`
already owns `buildComputeShaderStageCreateInfo(module)`, and its comment
states the rule that makes the builder safe: "Slang always emits `main` as the
entry-point symbol regardless of the Slang-side function name (AGENTS.md), so
no call site has a reason to pass its own entry-point name here — a builder
that took one would just be giving every future call site a way to get it
wrong." That builder is compute-only, so the two non-compute construction sites
never got it. `Raytracing.cpp:171-189` writes the same three-line
`.stage`/`.module`/`.pName` block four times (raygen, miss, shadow-miss,
closest-hit), and `ShaderHelper.cpp:75-81` writes it twice more for
`ShaderStagePair`'s vertex/fragment pair. Six hand-written `pName = "main"`
literals, in two files, against one builder that exists precisely to stop that.
`grep -rn 'pName' Src/` finds no other spelling.

The shader-group block immediately below is the same shape one level down.
`Raytracing.cpp:198-230` declares a C array `vk::RayTracingShaderGroupCreateInfoKHR
shader_group_create_infos[4]`, fills each element with five assignments and
`push_back`s it — 33 lines in which only two fields ever vary: three groups are
`eGeneral` with a `generalShader` index and `VK_SHADER_UNUSED_KHR` in the other
three slots, and one is `eTrianglesHitGroup` with `closestHitShader` set and
`generalShader` unused. Two constexpr builders (`buildGeneralShaderGroup`,
`buildTrianglesHitGroup`) collapse it to four lines, and — the reason this is
worth doing rather than cosmetic — they put the four `VK_SHADER_UNUSED_KHR`
sentinels in one place instead of twelve. A missed sentinel here is not a
compile error; it is a garbage shader index in the SBT.

Scope note: **do not change the group count, the group order, or the
`StageIndices` enum.** The known SBT defect (the miss region declaring one
record while the shadow ray reads record 1, and the handle-offset stride
assumption) is tracked separately as the blocked entry at
`BACKLOG.md`'s "(M) Fix the ray-tracing SBT" — this task is a pure
construction-site refactor and must leave `shader_groups`' contents
byte-identical.

**Second, `Kataglyphis::File` is a C++23 module whose whole job is to add one
`spdlog` line to three free functions, and one of its three methods is dead.**
`util/File.ixx` exports a class privately inheriting
`Shared::FileLocationHolder` (a 15-line header that exists only for this one
class — `grep -rn FileLocationHolder Src Test` finds no other user), and
`util/File.cpp:18-43` implements all three methods as
`if (!Shared::fileExists(loc)) { log; return {}; } return Shared::<the free
function>(loc);`. The free functions are `Src/shared/util/FileReader.ixx`'s
`readTextFile`, `readBinaryFile` and `getBaseDir`, all already directly unit
tested by `Test/commit/VulkanEngine/fileReaderSuite.cpp` (including the two
error paths the `fileExists` guard duplicates: missing path and directory path,
`:90`/`:99`/`:110`/`:119`).

Production call sites, in full:

- `File::readCharSequence()` — one, `ShaderHelper.cpp:58`,
  `File(spvPath).readCharSequence()`: a temporary constructed and destroyed on
  the same line, which is the tell that the object was never carrying state.
  Its "does not exist" log is redundant there — the very next statement is
  `if (!validateSpirvBlob(code))`, which logs *critical* with the far better
  message ("Invalid or missing SPIR-V shader blob at '…' — run
  compile-slang-shaders.ps1") and then `std::abort()`s. The wrapper's `err`
  line only ever prints immediately before that.
- `File::getBaseDir()` — one, `ObjLoader.cpp:183-184`, which constructs
  `File model_file(modelFile)` on one line purely to call `.getBaseDir()` on
  the next. `Shared::getBaseDir(modelFile)` is the same call with the object
  removed.
- `File::read()` — **zero.** The only caller in the tree is
  `Test/fuzz/shader_file_reader_fuzz_test.cpp:56`.

That fuzz test is the one this repo cares most about (its header comment
records that the byte-exactness property is what protects shader loading, and
per memory it is also the target the Linux lane skips), so it must keep
fuzzing the same functions — pointed at `readBinaryFile`/`readTextFile`/
`getBaseDir` directly, its two properties are unchanged and it stops testing a
wrapper on the way to them.

**Third, `buildIntegritySuite.cpp` hand-rolls the same open-and-getline loop 50
times, next to a helper header that claims to have ended exactly that.**
`Test/commit/VulkanEngine/RepoFiles.hpp` was added to stop this — its comment
says "Three suites each grew their own copy of both … This header is the one
place those helpers live now" — and it does own `repoRoot()`, `slangRoot()`,
`spirvRoot()` and `readFileText()`. But `readFileText` returns the whole file
as one string, and the overwhelmingly common need in this suite is *lines*. So
the line-reading twin was never written, and `grep -c 'std::ifstream'
buildIntegritySuite.cpp` is **60**, of which 50 are immediately followed by a
`while (std::getline(file, line))` loop within six lines. Each is the identical
four-line preamble: construct, `if (!file) { return <nullopt|empty>; }`,
declare `std::string line`, loop. (The remaining 10 are legitimately different:
two hand `nlohmann::json::parse` the stream, and the SPIR-V readers already go
through `readFileText`.)

There is a portability wrinkle that makes this more than tidying, and the
helper must handle it explicitly. All 50 copies open in **text** mode
(`std::ifstream file(path)`, no `std::ios::binary`), which on Windows strips
`\r` and on Linux does not — while `readFileText` opens binary. `.gitattributes`
pins `*.ps1`/`*.psm1`/`*.cmd` to CRLF, and `.github/workflows/Windows.yml` is
CRLF on this checkout (`file` reports "with CRLF line terminators"). Several of
these parsers read exactly those files —
`parse_local_runner_fuzz_targets` (a `.ps1`), `parse_ci_fuzz_targets` and
`parse_ci_gpu_excluded_suites` (that workflow). They survive today only because
every one of them matches with `find`/`substr` rather than comparing a whole
line, so a trailing `\r` falls outside the match. That is luck, not design, and
it is exactly the trap a naive `readFileText` + split-on-`\n` conversion would
walk into. The helper must strip one trailing `\r` per line, which makes the
converted sites behave the same on both platforms — strictly better than the
status quo, where they behave differently.

Ordering: run these **sequentially**, not concurrently. Tasks 1 and 2 both edit
`Src/GraphicsEngineVulkan/vulkan_base/ShaderHelper.cpp` (task 1 at `:75-81`,
task 2 at `:58`), and tasks 1 and 3 both edit
`Test/commit/VulkanEngine/buildIntegritySuite.cpp` (task 1 adds a gate, task 3
rewrites the helper block). Task 1 first, then task 2, then task 3. Tasks 1 and
2 both need `-FreshContainer`: task 1 adds a header pulled into global module
fragments, task 2 **deletes** `File.ixx`/`File.cpp`/`FileLocationHolder.hpp`,
and a reused container keeps building deleted files (AGENTS.md § Container
reuse). Task 3 is test-only and does not.

### C++ Vulkan engine

### Test suites

## 2026-08-04 batch XIV — planner (the emissive chain, end to end: the one glTF extension the Rust twin folds into `emissive_factor` and the C++ loader drops, so every HDR emitter loads four times too dim; the two shading paths still carrying a "deliberately not yet integrated" comment pointing at a backlog task that shipped this morning, which also drop `COLOR_0` entirely; the G-buffer channel that stores emissive in 8 UNORM bits and can never carry a strength above 1; a path tracer that samples the exact pixel centre every sample of every frame, so no amount of accumulation ever anti-aliases an edge; and the fact that the only thing guarding any of this is a `grep` for `.emission`)

The actionable queue was empty when this batch was written (0 `- [ ]`, 16
`- [b]`). Batch XIII's three tasks all shipped (`96d8c5f7`, `04bd2368`,
`2cf18361`). Everything below was read out of the tree this pass, at
`2cf18361`.

**Emissive shipped this morning and stopped one link short of working.**
`59eca71c` wired `ObjMaterial::emission` into `rasterizer.slang` and
`deferred.slang`; `4156455e` made the default emission black; `2f68854c`
carried `.mtl` `Ke` through the Rust OBJ→glTF converter. What none of them
touched is the four places the value is *attenuated or dropped* on the way to
a pixel, and they compound: the loader scales it by 1 instead of the authored
strength, the G-buffer clamps it to 1, and the two ray paths ignore it
outright. An asset authored with `KHR_materials_emissive_strength: 4` — the
standard Blender/Sketchfab export for anything meant to glow — renders four
times too dim in forward, four times too dim *and* clipped in deferred, and
black in both ray modes.

**First, `KHR_materials_emissive_strength`.** `GltfLoader.cpp`'s
`fromGltfMaterial` reads `material.emissive_factor[0..2]` raw. cgltf parses
the extension (`third_party/cgltf/cgltf.h:506-509` declares
`cgltf_emissive_strength`, `:552` the `has_emissive_strength` flag, `:4900`
the `KHR_materials_emissive_strength` case), and the Rust loader already folds
it in — `asset/gltf_loader.rs:638-642`, with the comment that says exactly
why: "fold it into the factor so the shader path stays unchanged; default 1.0
when the extension is absent". The C++ side never asks. The same file already
does per-material extension handling for `KHR_texture_transform` two blocks
down, so there is a shape to copy.

Note also that `gltfParseSuite.cpp` has **no emissive test at all** — 43
`GltfParseUnit` cases and `grep -in emissive` finds nothing. The plain
`emissiveFactor` path shipped untested on the parse side; the fixture this
task adds covers both.

**Second, `raytrace.rchit.slang` and `path_tracing.slang` both carry a comment
deferring emissive to a backlog task that no longer exists.**
`raytrace.rchit.slang:68-71` and `path_tracing.slang:206-210` each say
"material.emission is deliberately not yet integrated here … (see BACKLOG.md's
emissive_factor task)". That task is `59eca71c`, shipped, and its entry was
pruned from this file — so both comments now point at nothing. The reasoning
in them is still correct and is the specification for the fix: the rchit is a
primary-ray shading term and takes the rasterizer's treatment (add after
shadowing), while the path tracer must do `radiance += throughput * emission`
*before* the `throughput *= hitColor` on `:226`, or the mean the accumulation
buffer converges toward is wrong rather than merely different.

The same two shaders have a second, independent hole: **neither reads
`Vertex.color`.** `scene_types.slang:24-30` declares it, `GltfLoader.cpp`
fills it from `COLOR_0` (`readAttribute<4>`, with the vec3→alpha-1.0
pre-fill), and `rasterizer.slang:71` / `deferred.slang:70` both multiply it
into albedo. The rchit interpolates position, normal and UV barycentrically
(`:54-64`) and simply never interpolates colour; the path tracer likewise
(`:181-196`). So `Models/GltfTest/vertex_colored_quad.gltf` renders with its
vertex colours in forward and deferred and white in both ray modes. The
interpolation is per-shader (it needs the barycentrics that are already in
hand), so this does **not** want a helper in `common/` — inline it, and keep
`common/` untouched so the conservative shader-staleness rebuild and the
`CheckedInWgslIsNotOlderThanItsSlangSource` mtime trap stay out of the way.

**Third, the G-buffer's material attachment is 8-bit UNORM.**
`DeferredRasterizer.ixx:74-76`: normals are `eR16G16B16A16Sfloat`, albedo and
*material* are `eR8G8B8A8Unorm`. `deferred.slang:78-82` packs
`float4(roughness, emission)` into that attachment and its comment states the
consequence outright — "emissive is quantized to 8 bits and clamped to [0,1] —
which matches glTF's emissive_factor range **absent
KHR_materials_emissive_strength**". The moment the first task lands, that
caveat becomes a live defect: forward carries the strength (the offscreen
target has been `rgba16f` since the HDR unit), deferred clips it at 1.0, and
the two raster paths disagree on any emitter. `GBUFFER_NORMAL_FORMAT` proves
the half-float attachment works as an input attachment on this rig.

**Fourth, the path tracer never jitters its primary ray.**
`path_tracing.slang:69` computes `float2 pixelCenter = float2(tid.xy) +
float2(0.5)` *inside* the sample loop but from nothing that varies — every one
of `samples_per_pixel` samples, every frame, traces the identical primary ray
through the exact pixel centre. The bounce RNG decorrelates the *indirect*
estimate, so the image still converges, but it converges to the point-sampled
image: geometric edges stay hard-aliased no matter how long the accumulation
runs. This is not a known limitation — `docs/path-tracing.md`'s "Open work"
section lists only RNG decorrelation, and its estimator section describes the
primary ray without mentioning pixel-area sampling. The fix is one line
(`+ float2(rng, rng)` instead of `+ 0.5`), it is what makes
`samples_per_pixel` mean what its name says, and it needs no extra state: the
RNG is already seeded per pixel and folded with the frame index.

**Fifth, none of the above has a pixel oracle.** The only guard on emissive
today is `BuildIntegrity.EmissiveIsConsumedByTheRasterShadingPaths`
(`buildIntegritySuite.cpp:2619`), which greps four substrings out of two
`.slang` files. It cannot tell a shader that adds emission from one that adds
zero, and it would pass unchanged through every defect described above.
`goldenRenderSuite.cpp` has the instruments to close that: `ScopedModelOverride`
(`:240`) forces the scene via `KATAGLYPHIS_MODEL_OVERRIDE`, and
`DeferredMatchesForwardRoughly` (`:988-1073`) already establishes the
per-pixel mean-abs-channel-diff instrument, with the measured numbers that
justify its threshold (~0.2 with both paths healthy, >2 for a single
deliberate shading defect, limit 1.0).

Ordering: two independent chains. **Chain A is 1 → 3 → 5** (the loader
produces the value, the format carries it, the golden proves it) — task 5's
strength assertion is red until both 1 and 3 have landed. **Chain B is 2 → 4**
(both edit `path_tracing.slang`; task 2 rewrites the `hitColor` block at
`:198-226`, task 4 the `pixelCenter` line at `:69`). The chains do not touch
each other's files. Tasks 2 and 4 are shader-only and need **no C++ rebuild** —
recompile with `compile-slang-shaders.ps1` and run the goldens. Tasks 1, 3 and
5 do need a container build; none of them changes a module interface or
deletes a file, so `-FreshContainer` is not required.

### C++ Vulkan engine

### Shaders

### Test suites

## 2026-08-04 batch XV — planner (a gate that is RED right now because the commit that shipped this morning's goldens never bumped the count marker, next to a prose sentence carrying a third, differently-wrong copy; a ray-traced closest hit that seeds every pixel with the full unlit albedo, so an RT shadow is barely darker than lit ground and neither raster path does anything of the kind; MASK cut-outs that are solid quads in ray-traced mode because every BLAS geometry declares itself `eOpaque` and there is no any-hit shader at all; a Rust OBJ→glTF converter that clamps `Ke` to 1 where the C++ OBJ loader keeps it; and a pixel oracle that reads one column past its crop, which every caller already works around by hand)

The actionable queue was empty when this batch was written (0 `- [ ]`, 16
`- [b]`). Batch XIV's five tasks all shipped (`6109f2c8`, `e5de9629`,
`a9911a5a`, `5adbe178`, `846b50d8`). Everything below was read out of the tree
this pass, at `846b50d8`.

**First, a commit test is RED on `develop` as of this writing.**
`BuildIntegrity.GoldenTestCountsInDocsMatchTheSuite`
(`buildIntegritySuite.cpp:4840`) pins `docs/gpu-golden-testing.md`'s
`<!-- golden-counts: ... -->` marker against a file-I/O count of
`TEST(GoldenRender, ...)` / `TEST(Integration, ...)` definitions under
`Test/commit/VulkanEngine/`. The marker (`:84`) says `defined=32 runnable=31
integration=2 total=33`; the tree holds **34** `TEST(GoldenRender, ...)`
definitions (one of them `DISABLED_DumpsFrameToPng`) and 2 `TEST(Integration,
...)`, i.e. `defined=34 runnable=33 integration=2 total=35`. `846b50d8` added
the two emissive goldens and did not touch the marker. This is a CPU test — it
runs in the container and in the always-on Linux lane — so it is not a
host-only latency; it fails every build right now.

The prose three lines above the marker (`:77-79`) is a *separate* copy of the
same numbers and is wrong in a third way: "the baseline is 30 runnable
`GoldenRender` tests (31 defined … ) + 2 `Integration` tests = 32 total". The
gate reads only the HTML marker, so prose drift is exactly the failure mode
the gate was written for and exactly the failure mode it cannot see. Two
copies of one fact, one of them unguarded, is the thing to remove — not to
re-synchronise for the third time.

**Second, `raytrace.rchit.slang` starts from the unlit albedo and the two
raster paths do not.** `:73-86` accumulates the base colour into a local
called `ambient` (it is not an ambient term — it is the sampled/factor base
colour times `COLOR_0`), and `:115` does `payload.hit_value = ambient;` before
the shadow test. The direct-lighting term is then *added* on top only when the
ray reaches the light (`:118-124`). So a ray-traced surface in full shadow
renders at its complete albedo, and a lit one renders at albedo + BRDF.

Neither raster path has any such term. `rasterizer.slang:78-82` is
`color = brdf_direct(...)` then `color *= 1.0 - shadow * intensity`;
`deferred.slang`'s `lighting_fs_main` is the same two lines. There is no
ambient, no IBL and no sky contribution anywhere in the C++ raster shading —
`grep -rn ambient Resources/ShadersSlang/*/*.slang` finds it only in the
Rust-side `forward.slang` and in this one rchit local. The consequence is not
subtle: on `shadow_rig.obj` the ray-traced ground under the box is nearly as
bright as the ground beside it, so RT mode barely renders a shadow at all,
while the same scene in forward and deferred shows a clean one. Three shading
paths over one `ObjMaterial`, and one of them silently adds a full extra
albedo bounce.

Nothing measures this. RT mode does have goldens —
`RaytracedWorldFollowsTheModelTransform` (`goldenRenderSuite.cpp:1952`), the
uv-transform card golden at `:2034`, and `RaytracedLargeMeshDoesNotLoseTheDevice`
(`:3038`) — but all three assert on *presence* (a transform moved, a frame is
not blank, the device survived). None asserts a brightness relationship, so
the divergence has been invisible to the suite since the RT path was written.

**Third, glTF `alphaMode: MASK` is honoured in all three raster shaders and in
neither ray mode.** `material_fetch.slang:50-53` owns the one predicate,
`BuildIntegrity.RasterShadersShareOneAlphaCutoffRule`
(`buildIntegritySuite.cpp:2366`) pins its three *raster* callers, and
`GoldenRender.MaskCardDiscardsCutoutTexelsVisually` (`:2181`) proves the
forward discard in real pixels with `Models/GltfTest/mask_card.gltf`. In RT
and PT the same card is a solid quad: `raytrace.rchit.slang` never calls
`alpha_masked_out` (a closest-hit shader cannot discard — that needs an
any-hit), the shadow ray is traced with `RAY_FLAG_FORCE_OPAQUE` (`:104`), and
`Raytracing.cpp:186` builds its hit group with
`buildTrianglesHitGroup(eClosestHit)`, whose header comment states the
situation plainly: "any-hit/intersection left unused (the engine has no
any-hit or procedural-intersection shaders today)".

The load-bearing detail is one line away from all of that:
`ASManager.cpp:565` sets `acceleration_structure_geometry.flags =
vk::GeometryFlagBitsKHR::eOpaque` on **every** BLAS geometry. That flag makes
the implementation skip any-hit invocation entirely, so adding an any-hit
shader without touching it produces a pipeline that compiles, links, and
changes nothing — the most expensive possible way to not fix this.

**Fourth, `Ke` survives the C++ OBJ loader and is clamped by the Rust
converter.** `ObjLoader.cpp:195` takes tinyobj's `emission` verbatim, so
`Ke 4 4 4` reaches `ObjMaterial::emission` as 4. `obj_to_gltf.rs:143-153`
clamps the same input to `[0,1]`, and its own comment says why — "values above
1 belong in `KHR_materials_emissive_strength`, which this converter does not
emit". Every OBJ the Rust renderer draws goes through that converter, so an
HDR emitter is four times too dim there and correct in the C++ engine. The
receiving end already works: `gltf_loader.rs:638-642` reads
`material.emissive_strength()` and folds it into `emissive_factor` (and
`6109f2c8` taught the C++ loader the same), so the only missing link is the
emitter. It writes no `extensions` block and no `extensionsUsed` array today.

**Fifth, `detail_fraction` reads one pixel past the last column of its crop.**
`GoldenMetrics.hpp:90-99` compares each pixel with `base + 1U` — its
right-hand neighbour — for `x` up to `crop.x1 - 1`, so the read at the last
column lands on `crop.x1`, outside the crop. For a crop whose `x1` is the
frame width that is the first pixel of the *next row* (a wrapped, meaningless
"edge"), and on the bottom row it is one pixel past the end of the vector — an
out-of-bounds read the ASAN debug build would flag if a caller ever hit it.
Every caller already works around it by hand, which is the tell:
`goldenMetricsSuite.cpp:59` had to invent a `detail_safe_crop()` whose comment
spells the hazard out, and `goldenRenderSuite.cpp:2530` passes
`Crop{minx, maxx, miny, maxy + 1U}` — inclusive in `y`, exclusive in `x`, with
nothing saying the asymmetry is deliberate. The two sibling helpers in the
same header (`mean_luminance_in_crop`, `swung_fraction`) have no such footgun.

Ordering: task 1 first (it is the red gate), and **tasks 2 and 3 each add a
`TEST(GoldenRender, ...)` and must bump the same marker in the same change** —
the gate will tell them so. Tasks 2 and 3 both edit `raytrace.rchit.slang`
(task 2 the `:73-128` shading block, task 3 the `:104` shadow-ray flags), so
run 2 before 3. Task 2 is shader-only: recompile with
`compile-slang-shaders.ps1` and run one golden, no C++ rebuild. Task 3 adds a
shader **and** changes C++; it does not delete a file or change a module
interface, so `-FreshContainer` is not required. Tasks 4 and 5 are independent
of everything else.

Deliberately **not** queued this pass, so it does not get half-done: the
path-tracing half of task 3. `path_tracing.slang` uses inline `RayQuery` with
`RAY_FLAG_FORCE_OPAQUE` on both its primary and shadow queries, so honouring
MASK there means handling candidate hits in the `Proceed()` loop rather than
adding a shader — a different change, in the one file that carries the `- [b]`
device-lost blocker. Pick it up once task 3 has established that the BLAS
opacity flags can be relaxed without a perf or stability surprise.

### Test suites

### Shaders

### C++ Vulkan engine

### Rust WebGPU renderer (`third_party/OxidANT`)

  **Build:** No C++ build. `cargo check` / `clippy` / `fmt` from
  `third_party/OxidANT` are the local signal — `cargo
  test` and `cargo build` do **not** link on this host (the VC++ Build Tools
  install is incomplete and Git Bash's `link.exe` shadows MSVC's). The crate's
  test suite runs in this repo's always-on Linux lane via
  `scripts/linux/run-cargo-tests.sh`, so push and read CI for the test result; do
  not report the tests as passing locally when they were never run.

  **Context:** The glTF spec caps `emissiveFactor` at `[0,1]` per component,
  which is exactly why the extension exists — factoring the magnitude out into
  `emissiveStrength` is the standard encoding, and the one Blender and Sketchfab
  produce. Both loaders on the receiving side already handle it
  (`gltf_loader.rs:640` and, since `6109f2c8`, `GltfLoader.cpp:149`), so this
  closes the loop rather than opening a new one. Keep the "unchanged documents
  stay byte-identical" property — the converter's existing tests depend on it,
  and it is the reason the `emissiveFactor` field itself is emitted
  conditionally.

## 2026-08-04 batch VII — planner (refactor: three consumer files exempted from the barrier gate whole-file when each has a bounded, known count, plus a fourth entry that matches nothing; `ASSERT_VULKAN` as a bare unbraced `if` whose argument four VMA call sites rely on being evaluated exactly once; an identity predicate in the Rust crate threaded through a per-frame field, both existing only to carry a parameter the body discards)

The actionable queue was empty when this batch was written (0 `- [ ]`, 16
`- [b]` across the whole file). Every `file:line` below was read out of the
tree this pass.

**Host GPU golden verification is still blocked over RDP** (see the `- [b]`
entry near the end of this file), so all three tasks here are accepted
without an adapter, by construction: task 1 changes only
`buildIntegritySuite.cpp`, task 2 is a preprocessor/call-site normalisation
whose acceptance is that the whole engine still compiles and the CPU suites
stay green, and task 3 deletes Rust code whose behavioural coverage
(`tests/histogram.rs`) runs in this repo's always-on Linux lane.

None of the three overlaps the blocked entry **"Collapse the two cloud-output
image barriers and the rationale comment that is written twice"** — that one
rewrites the two barriers and needs a golden; task 1 only stops the gate from
pretending it cannot see them. Task 1 should in fact land *first*, because it
gives that blocked task a mechanical tell when it eventually ships (the budget
drops from 2 to 0 and the gate fails until the map is updated).

Ordering: tasks 1, 2 and 3 are independent and touch disjoint files. Task 2 is
the only one that recompiles the whole engine.

### Test suites

### C++ Vulkan engine

### Rust WebGPU renderer (`third_party/OxidANT`)

## 2026-08-04 batch XVI — planner (the glTF `sampler` object, which the Rust loader reads in full and the C++ loader never looks at once, next to a per-texture sampler array that already has a slot for it; base-colour images referenced by an external file URI, which this repo's own OBJ→glTF converter is the thing that emits and the C++ loader is the thing that silently drops; the `eOpaque` follow-up `ASManager.cpp` asks for by name, now that every geometry in the scene invokes an any-hit shader; the path tracer, which forces every ray opaque and so is the last shading path where a MASK cut-out is a solid quad; and a G-buffer albedo attachment left at 8-bit UNORM three commits after its sibling was widened for exactly this reason)

The actionable queue was empty when this batch was written (0 `- [ ]`, 16
`- [b]` across the whole file). Batch VII's three tasks all shipped
(`6a70d193`, `c59d2f82`, `46c63fbf`). Everything below was read out of the
tree this pass, at `46c63fbf`.

**The duplication families are drained.** A mechanical 8-line duplicate-block
scan over all 149 files in `Src/` this pass returns nothing but shared
`#include`/`import` preambles and the two rasterizers' parallel public
interfaces — no remaining "one rule, N hand-rolled copies". That is why this
batch has no `(refactor)` entry: the twelve create/destroy helper extractions
have taken the C++ tree as far as that pattern goes. What is left is
behavioural, and four of the five items below are the same shape — a glTF
feature the Rust twin implements and the C++ loader does not read at all.

**First, `GltfLoader` never looks at a glTF `sampler`.** `parseCpu`
(`scene/GltfLoader.cpp:508-528`) records only `textureImages` — a
`std::vector<std::vector<unsigned char>>` of encoded bytes — and
`uploadParsed` (`:73-77`) uploads each one. `texture->sampler` is never
dereferenced anywhere in the file. Every texture the engine ever binds
therefore gets the one sampler `Model::addSampler` (`scene/Model.cpp:68-93`)
builds: `vk::Filter::eLinear`, `vk::SamplerAddressMode::eRepeat`, max
anisotropy. The Rust twin reads the whole object —
`asset/gltf_loader.rs:339-362`'s `to_cpu_sampler` maps `wrap_s`/`wrap_t` and
all five `MinFilter`/`MagFilter` combinations into `CpuSampler`, and
`render/texture.rs:7-30` turns that into the `wgpu::Sampler`.

This is not a "would need a descriptor-layout change" gap. The layout is
already there: `scene_types.slang:14-15` declares `TEXTURES_BINDING = 3` and
`SAMPLER_BINDING = 4` as **two parallel arrays of `MAX_TEXTURE_COUNT`**
(`VulkanRenderer.cpp:1478-1480`), and every shading path indexes both with the
same `resolve_texture_slot()` result. `VulkanRenderer.cpp:1621-1630` already
writes one `vk::Sampler` per slot. Nothing in the shaders has to change; the
per-texture sampler descriptor exists and is being filled with N copies of one
handle.

The visible failure is the ordinary one: a `CLAMP_TO_EDGE` base-colour texture
(the standard authoring for a decal, a UI atlas or any texture whose UVs run
slightly outside `[0,1]`) tiles instead of clamping, so the opposite edge of
the image bleeds in along every border. A `NEAREST`-filtered texture — the one
case where the author's choice *is* the look — is bilinear-filtered into mush.

**Second, an image referenced by an external file URI is dropped.**
`extractImageBytes` (`GltfLoader.cpp:224-266`) handles exactly two forms, and
says so at `:219-223`: a `buffer_view` (the `.glb` case) and a `base64,` data
URI. A plain `image->uri` naming a file next to the document falls through
every branch and returns `{}`, so `parseCpu:521-527` never assigns a
`textureID` and the material loads untextured. Geometry is unaffected —
`cgltf_load_buffers` (`:497`) *does* resolve an external `.bin`, so the mesh
arrives intact and only the textures are missing, which is the failure mode
hardest to attribute to the loader.

The comment's stated reason ("no asset in the tree uses one, and it would need
the document path to resolve") is no longer either true or a blocker. It is
not a blocker because `extractImageBytes` is called from `parseCpu`, whose
`modelFile` parameter is the document path — the same string already passed to
`cgltf_load_buffers` one line earlier. And it is not true because **this
repo's own converter emits exactly that form**:
`asset/obj_to_gltf.rs:600-604` writes `{ "uri": "<bare filename>" }` per
image and `:845-849` copies the texture next to the converted document. So
`obj2gltf` (`examples/obj2gltf.rs`) produces documents that the Rust renderer
displays textured and the C++ renderer displays grey — and
`docs/model-loading.md:225-233` documents that emission as deliberate, which
means the two halves of this repo have a written, agreed-on interchange format
that one of them cannot read.

**Third, `ASManager.cpp` asks for this one by name.** `objectToVkGeometryKHR`
(`:564-571`) sets `acceleration_structure_geometry.flags = {}` for every BLAS
geometry, with the comment "Correctness before speed - a follow-up should set
`eOpaque` per geometry (off only for meshes that actually contain a MASK
material) once RT frame time with every geometry now invoking any-hit is
measured." That follow-up is now worth doing: `raytrace.rahit.slang` shipped
(`Raytracing.cpp:164-192` wires it as `eAnyHit` into the triangles hit group),
so every hit on every triangle in the scene now enters an any-hit invocation,
and its first act (`rahit_main`, `:33`) is to load the object description and
the per-triangle material through two buffer-device-address indirections just
to discover `alphaCutoff < 0.0` and return. On a scene like `crytek-sponza`
that is two dependent memory loads per hit, scene-wide, to answer a question
the BLAS flag answers for free.

The predicate is already available where it is needed: `Mesh`'s constructor
(`scene/Mesh.ixx:22-27`) receives the full `std::vector<ObjMaterial>`, so
"does any material in this mesh carry `alphaCutoff >= 0`" is computable at
construction and needs no new plumbing from either loader.

**Fourth, the path tracer forces every ray opaque.** `path_tracing.slang:104`
and `:110` declare and trace `RayQuery<RAY_FLAG_FORCE_OPAQUE>`, and `:256-266`
does the same for the NEE shadow query. `RAY_FLAG_FORCE_OPAQUE` makes the
implementation commit every candidate triangle without ever surfacing it as
`CANDIDATE_NON_OPAQUE_TRIANGLE` — and a ray query has no any-hit shader stage
at all, so the flag is the *only* thing standing between the kernel and the
alpha test. `raytrace.rahit.slang` does nothing for this path.

So PT is now the one remaining shading path where a glTF MASK cut-out is a
solid quad. Forward, deferred and the shadow pass discard through
`material_fetch.slang`'s `alpha_masked_out`; RT mode discards through the
any-hit shader as of this week; PT does not, and additionally casts a solid
NEE shadow from geometry the other four paths see through. The kernel already
has every ingredient — `:215-225` fetches the material and samples the
base-colour texture with `SampleLevel` for the committed hit.

**Fifth, `GBUFFER_ALBEDO_FORMAT` is still `eR8G8B8A8Unorm`**
(`DeferredRasterizer.ixx:75`), while its two siblings on the same G-buffer are
`eR16G16B16A16Sfloat` (`:74`, `:76`). `deferred.slang:83`'s comment records
why the material attachment was widened three commits ago (`a9911a5a`):
"roughness is no longer quantized to 256 steps". The albedo attachment was not
revisited, and it holds the one value in the G-buffer that is stored in the
*wrong space* for 8 bits: `g.outAlbedo = texColor` (`:76`) is **linear** base
colour — `Texture.cpp:123` uploads every texture as `eR8G8B8A8Srgb` so the
hardware decodes to linear at sample time, and `base_color()` then multiplies
the linear factor. 256 uniform steps in linear space put nearly all of the
precision where the eye has least, and the forward path quantizes nothing.

The fix is not a widening — it is one enum. `eR8G8B8A8Srgb` costs the same
bandwidth, encodes on colour-attachment write and decodes on
`SubpassLoad`, and gives the stored value a perceptual distribution. Albedo is
bounded in `[0,1]` by construction (`baseColorFactor`, the texture, and
`COLOR_0` are all `[0,1]`), so nothing clips; alpha is not sRGB-encoded by the
format and the lighting pass reads only `.rgb` anyway
(`deferred.slang:126-138`).

**GPU verification is still blocked over RDP** (see the `- [b]` entry near the
end of this file), and `path_tracing` mode additionally device-losts on the
host RX 9070 XT on unmodified `develop` (the `- [b]` at line ~2030). Every
task below therefore states a CPU-only acceptance criterion that the container
build and the always-on Linux lane can actually deliver, and treats the golden
run as "if you have an adapter, also do this". Task 4 in particular is a
compile-and-gate acceptance by construction — do not claim a PT visual result
you cannot obtain.

**Ordering:** task 3 must land before task 4. Task 3 restores `eOpaque` on
non-MASK geometry, which is what keeps task 4's candidate-commit loop off the
hot path for the 99% of the scene that has no cutoff; landing 4 first is
correct but makes every triangle in the scene take the slow path in the
meantime. Tasks 1, 2 and 5 are independent of everything else and of each
other.

### Test suites

### C++ Vulkan engine

### Rust WebGPU renderer (`third_party/OxidANT`)

## 2026-08-04 batch XVII — planner (a `doubleSided` rule both renderers implement in the pipeline and neither implements in the shader, so every back face is lit by a normal pointing away from it; two ray-tracing normal transforms that column-multiply `WorldToObject` where their own comment says row-multiply, reachable from the GUI's Rotation slider; the emissive *texture*, the one lit glTF texture slot the Rust twin samples and the C++ engine has no field for; the tangent attribute that is the single prerequisite for closing the normal-map gap; and two mip-chain barriers that publish to `eFragmentShader` in an engine where three of five shading paths read those textures from compute and ray-tracing stages)

The actionable queue was empty when this batch was written (0 `- [ ]`, 16
`- [b]` across the whole file). Batch XVI's five tasks all shipped
(`94567de7`, `5f5b2e77`, `59c563fd`, `5dfb1dcf`, `53eef705`). Everything below
was read out of the tree this pass, at `53eef705`.

**First, neither renderer flips the shading normal on a back face.** glTF 2.0
§3.9.4 (Double Sided): when a material is double-sided, the normal vector MUST
be flipped for back-facing fragments. Both renderers already do the *pipeline*
half of `doubleSided` and stop there. C++: `MeshDrawRecorder.cpp:44-52` sets
`eCullMode` dynamically per draw — `eNone` for a double-sided mesh, `eBack`
otherwise. Rust: `forward.rs:1934` / `:2630` select
`pipeline_double_sided` (cull `None`) for exactly those primitives. Then all
three raster fragment shaders use the interpolated normal unmodified:
`rasterizer.slang:54` (`float3 N = normalize(In.shadingNormal)`),
`deferred.slang:76` (`g.outNormal = float4(normalize(In.shadingNormal), …)`)
and `forward.slang:390` (`float3 nGeom = normalize(In.worldNormal)`, which then
seeds the whole TBN at `:391-394`). `SV_IsFrontFace` / `@builtin(front_facing)`
appears nowhere in `Resources/ShadersSlang/` — a grep for it returns nothing.

The visible failure is the one double-sided authoring exists for: a leaf card,
a curtain, a sheet of cloth, a flag. Seen from behind, `dot(N, L)` has the
wrong sign for every fragment, so `brdf_direct` returns ~0 and the surface goes
black — while the exact same geometry seen from the front lights correctly.
In deferred the wrong normal is *stored*, so it also corrupts the specular term
and the shadow bias in the lighting pass.

The fix needs no new material plumbing, and this is the part worth getting
right: a back-facing fragment can only reach the shader if culling was off for
that draw, and culling is off only for double-sided meshes. `!isFrontFace`
therefore already *means* "double-sided back face" in both renderers. Do not
add a `doubleSided` field to `ObjMaterial` for this.

**Second, the two ray-tracing normal transforms are transposed.**
`raytrace.rchit.slang:58-59` carries the comment "Normal at hit position
(inverse-transpose = row-multiply WorldToObject)" and then writes
`mul((float3x3)WorldToObject(), normalHit)` — a **column**-multiply. Slang
follows HLSL: `mul(M, v)` is `M·v`, `mul(v, M)` is `Mᵀ·v`. The correct normal
transform is `(M⁻¹)ᵀ·n` = `WorldToObjectᵀ·n` = `mul(normalHit,
(float3x3)WorldToObject())`, which is the row form the comment names.
`path_tracing.slang:205` has the same shape:
`mul(worldToObject, float4(normalHit, 0.0))`, where `worldToObject` is
`CommittedWorldToObject3x4()`. The sibling line for *position* two lines up
(`:200-201`) is correct — an affine point transform genuinely is a column
multiply — which is likely how the normal line acquired the same spelling.

For a model instance whose transform is a rotation `R`, the correct world
normal is `R·n` and both shaders compute `Rᵀ·n`: the normal is rotated by −θ
instead of +θ, a 2θ error. This is not hypothetical or dev-only.
`GUI.cpp:100` exposes a `DragFloat3("Rotation", …)` slider;
`VulkanRenderer.cpp:351-375` turns it into a model matrix and rebuilds the
TLAS so the traced world follows the raster world; `ASManager.cpp:291` writes
that matrix straight into the instance transform. So: rotate the model in the
GUI, switch to RT or PT, and the lighting rotates the wrong way while the
rasterizer's lighting rotates correctly. With an identity or
translation-only model matrix the linear part is `I` and the two spellings
agree — which is why every golden passes.

**Third, `raytrace.rchit.slang` never face-forwards its normal, and its own
sibling does.** `path_tracing.slang:206-210` flips `hitWorldNormal` against
the ray direction and cites the reason ("GLSL `faceforward(N, I, N)`"). The
closest-hit shader has no equivalent: `:88-96` computes `N = worldNormal` and
immediately gates the shadow ray on `dot(worldNormal, L) > 0`. Neither RT nor
PT culls back faces, so a hit on the far side of any surface — every
double-sided card, and the interior of any single-sided shell — is shaded with
a normal pointing away from the ray and comes out black. This is the same
defect as finding one, in the path where it is not conditional on a cull mode.
It is folded into task 2 rather than task 1 because it edits the same four
lines as the transpose fix.

**Fourth, the emissive texture has no C++ representation at all.** The Rust
loader reads five texture slots per material (`gltf_loader.rs:649-665`:
base colour, metallic-roughness, normal, emissive, occlusion) and
`forward.slang:378/388` samples `emissiveTex` and multiplies it into
`prim.emissive_factor.rgb`. The C++ `ObjMaterial`
(`Src/shared/scene/ObjMaterial.hpp`) has exactly one `textureID`, and all four
C++ shading paths add the *factor* alone: `rasterizer.slang:86`,
`deferred.slang:89`, `raytrace.rchit.slang:132`, `path_tracing.slang:253` each
read `material.emission` with no texture lookup. So a material that authors a
dim `emissiveFactor` and puts the actual pattern in an `emissiveTexture` —
the standard way to author a lit sign, a screen, a glowing panel or a strip of
windows — renders as a uniform wash of the factor colour over the whole
surface in the C++ renderer and correctly in the Rust one.

Emissive is the right slot to close first, and the cheapest: it needs no
tangent frame (unlike normal), it is sRGB like base colour so
`Texture::uploadRgba`'s hard-coded `eR8G8B8A8Srgb` (`Texture.cpp:123`) is
already correct for it (metallic-roughness and occlusion are *not* — those
need a UNORM upload path first), and the descriptor side is already built: the
128-slot `TEXTURES_BINDING`/`SAMPLER_BINDING` arrays and
`resolve_texture_slot()` do not care how many slots one material claims.

**Fifth, `Vertex` has no tangent, and that is the whole reason the C++ engine
has no normal mapping.** `Src/shared/scene/Vertex.hpp` is
position/normal/color/texture_coords, mirrored device-side in
`scene_types.slang:24-30` (which the RT and PT shaders read *by buffer device
address*, so the two must change together or every hit reads garbage) and
pinned by the `Vertex_natural` layout gate at
`buildIntegritySuite.cpp:1053-1057`. The Rust twin reads `TANGENT`, and when
the file ships none it generates them (`gltf_loader.rs:564`, `compute_tangents`
at `:714`, Lengyel's method with an accumulated bitangent for the handedness
sign) with an optional MikkTSpace pass behind a flag. Normal mapping is the
largest remaining visual gap between the two renderers and it is gated on this
one attribute; task 4 adds it and its generation as its own verifiable
increment, and deliberately changes no shading.

**Sixth, `Texture::generateMipMaps` publishes its mip chain to the fragment
stage only.** `Texture.cpp:371-376` and `:388-393` transition each level to
`eShaderReadOnlyOptimal` with `dstStageMask = eFragmentShader`. The engine has
exactly one answer to "what stage does this layout imply" —
`pipelineStageForLayout` in `common/ImageLayoutHelper.hpp`, which returns
`eAllCommands` for `eShaderReadOnlyOptimal` and says why in a comment ("this is
what lets a transition be recorded on a queue other than graphics"), and which
`VulkanImage.cpp:146` routes through. These are model textures: three of the
five shading paths sample them from a stage `eFragmentShader` does not cover —
`raytrace.rchit.slang` (`eRayTracingShaderKHR`) and `path_tracing.slang`, a
compute kernel (`eComputeShader`). `SkyBox.cpp:189-191` already carries the
note for exactly this widening ("Destination stage widens from
`eFragmentShader` to `eAllCommands` … not a behavioural regression"), so
`Texture.cpp` is the last hand-rolled narrow copy. Note that
`ImageBarrierHelper.hpp:25-27` exempts `Texture.cpp` from conversion — that
exemption is about the *subresource range* (a mip chain cannot use the helper's
one-mip default) and says nothing about the stage mask, so it does not cover
this.

**GPU verification is still blocked over RDP** (the `- [b]` entry near the end
of this file), and `path_tracing` mode additionally device-losts on the host
RX 9070 XT on unmodified `develop` (the `- [b]` at line ~2030). Every task
below therefore states a CPU-only acceptance criterion the container build and
the always-on Linux lane can actually deliver — a source-level `BuildIntegrity`
gate, a CPU parse/unit test, or both — and treats a golden run as "if you have
an adapter, also do this". Do not claim a rendered result you cannot obtain.

**Ordering:** tasks 1, 2, 3 and 5 are independent and touch disjoint files.
Task 4 must land *last* of the shader-touching set: it changes
`scene_types.slang`'s `Vertex`, which every RT/PT shader reads by address, so
landing it in the middle would rebase tasks 1–3's diffs for no reason. Task 5
is the only one that touches no shader at all and can go first or last.

### Rust WebGPU renderer (`third_party/OxidANT`)

> The back-face normal flip above is cross-renderer: its third step edits
> `forward.slang`, whose WGSL output is checked in under
> `crates/webgpu_renderer/src/shaders/forward.wgsl`. Verify the Rust half with
> `cargo check`/`cargo clippy`, not `cargo build`/`cargo test` — see the
> `- [b]` entry on this host's incomplete MSVC linker install.

## 2026-08-05 batch — planner (refactor: a model matrix `Mesh` stores, initialises to identity and never reads, whose setter has zero callers and whose real owner is `Model` one level up; two `buildSamplerCreateInfo` overloads whose last ten field assignments are byte-identical and whose first five differ only in where the filters come from; and `ObjMaterial`'s twelve-parameter positional constructor, which has grown one argument per glTF factor for four commits running and now needs a trailing `// comment` on every argument at all four call sites)

The actionable queue was empty when this batch was written (0 `- [ ]`, 16
`- [b]` across the whole file). Batch XVII's five tasks all shipped
(`923011db`, `70796fa0`, `d0a25d21`, `bb7092ae`, `f6741266`), plus the normal-
mapping follow-up task 4 filed (`2030c374`). Everything below was read out of
the tree this pass, at `2030c374`.

**First, `Mesh` carries a model matrix nothing reads.** `Mesh.ixx:84` declares
`glm::mat4 model{}`, `Mesh.cpp:94` assigns it `glm::mat4(1.0F)` as the last
statement of the constructor, `Mesh.cpp:97` defines
`Mesh::setModel(glm::mat4)` — and a repo-wide search for callers of either
`Mesh::setModel` or `Mesh::getModel` (`Mesh.ixx:38`) returns nothing but the
declarations themselves. The live twin is one level up and fully wired:
`Model::set_model` (`Model.ixx:43`, `Model.cpp:59`) and `Model::getModel`
(`Model.ixx:41`), reached through `Scene::update_model_matrix`
(`Scene.cpp:159-168`, the only writer, called from `VulkanRenderer.cpp:366`
when the GUI Rotation/Scale sliders move) and `Scene::getModelMatrix`
(`Scene.ixx:81-84`, which `MeshDrawRecorder.ixx:67` reads once per model per
frame). So the per-mesh copy is not a stale mirror of the per-model one — it
is a second, unrelated identity matrix that was never connected to anything.

This matters beyond the four lines: `Mesh::getModel()` is a public accessor
that compiles, returns a plausible value, and would silently hand a future
caller identity for a rotated model. The class already documents the opposite
rule for `double_sided` vs `has_masked_material` (`Mesh.ixx:50-63`) — derive
state where it cannot go stale, or do not hold it. Deleting is the cheap half
of that rule.

**Second, the two `buildSamplerCreateInfo` overloads share ten identical
lines.** `SamplerBuilder.cpp:11-38` (scalar `filter` + `addressMode`) and
`:40-66` (`GltfSamplerDesc`) differ in exactly five assignments —
`magFilter`/`minFilter` (one `filter` vs `desc.mag`/`desc.min`),
`addressModeU`/`addressModeV`/`addressModeW` (one `addressMode` vs
`desc.addressModeU`/`desc.addressModeV`/`desc.addressModeU`) and `mipmapMode`
(hard-coded `eLinear` vs `desc.mipmapMode`). The remaining ten
(`borderColor`, `unnormalizedCoordinates`, `mipLodBias`, `minLod`, `maxLod`,
`anisotropyEnable`, `maxAnisotropy`, `compareEnable`, `compareOp`, plus the
zero-initialised struct) are copied verbatim, in the same order, with the same
values.

The scalar overload is expressible as the desc overload with no behaviour
change at all: `GltfSamplerDesc`'s defaults (`SamplerBuilder.ixx:32-36`) are
`eRepeat`/`eRepeat`/`eLinear`/`eLinear`/`eLinear`, and the desc overload's
`addressModeW = desc.addressModeU` collapses to the scalar overload's
`addressModeW = addressMode` whenever U and V are the same value — which is
the only thing a single `addressMode` parameter can produce. This is the same
consolidation `ImageBarrierHelper.hpp:7-17` and `RenderPassHelper.hpp` already
performed on their own duplicated field blocks, and the reason is the same one
that header states: the two copies drift. `samplerBuilderSuite.cpp` already
pins all four production call sites field-by-field, so the refactor is
verifiable without a device.

**Third, `ObjMaterial`'s positional constructor has become the drift risk it
was meant to prevent.** `ObjMaterial.hpp:72-88` takes twelve parameters, eight
of them defaulted, in an order that must match the twelve default-member
values in the sibling default constructor at `:66-70`. Four of those
parameters (`metallic`, `roughness`, `emissiveTextureID`, `normalTextureID`)
were appended in the last four days, one per commit — `9611f22d`, `d0a25d21`,
`2030c374` and the roughness work — and each append had to touch both
constructors, both call sites in `GltfLoader.cpp` and both in
`blasGeometryLimitsSuite.cpp`. The call sites show what that costs:
`GltfLoader.cpp:219-230` is a twelve-line argument list where every single
line carries a trailing `// name` comment restating the parameter it is
positionally bound to, and `blasGeometryLimitsSuite.cpp:39` and `:44` are
`ObjMaterial({...}, {...}, 0.0F, 1.0F, -1, -1.0F)` with no comments at all —
six positional values whose meaning is only recoverable by counting.

C++20 designated initializers remove the counting entirely, and this struct is
the ideal candidate: it is standard-layout by construction (both layout gates
depend on that), it has no bases, no virtuals and no private data, and its
only member function (`get_textureID`, `:90`) does not affect aggregate-ness.
Replacing both constructors with per-member default initializers keeps every
offset, `sizeof`, and default value bit-identical — which
`ObjMaterialLayoutUnit.MatchesTheSlangTwinScalarLayout`
(`pushConstantSuite.cpp:148-164`, thirteen exact offsets plus
`sizeof == 80`) and the `ObjMaterial_natural` entry in
`buildIntegritySuite.cpp:1042-1054` both prove mechanically, with no GPU.

The same file has a smaller instance of the same pattern: `GltfLoader.cpp`
warns "only TEXCOORD_0 is supported" in three near-identical blocks — for
base colour (`:147-152`), emissive (`:200-207`) and normal (`:210-217`) —
that differ only in the texture view, the slot name in the message, and
whether the guard is `has_pbr_metallic_roughness` or a null `texture`. Folding
them into one helper is the natural companion edit, and it is what a fourth
texture slot (metallic-roughness, occlusion) will otherwise copy a fourth
time.

**Verification context.** Host GPU goldens are still blocked over RDP (the
`- [b]` entry near the end of this file), and `path_tracing` mode additionally
device-losts on the host RX 9070 XT on unmodified `develop` (the `- [b]` at
line ~2030). All three tasks below are accepted CPU-only by construction:
none changes a shader, a barrier, a render pass, or any value that reaches the
GPU. Task 1 deletes unreachable code, task 2 is a byte-identical struct
factoring pinned by an existing field-by-field suite, and task 3 is a
declaration-syntax change pinned by two existing offset gates. Do not claim a
rendered result you cannot obtain.

**Ordering:** tasks 1, 2 and 3 are independent and touch disjoint files. Tasks
1 and 3 both edit a C++23 module interface (`Mesh.ixx`; `ObjMaterial.hpp`,
which sits in `ObjMaterial.ixx`'s global module fragment) and therefore both
need `-FreshContainer`. Task 2 edits only a module *implementation* unit and
does not.

### C++ Vulkan engine

## 2026-08-05 batch II — planner (every normal map in the C++ engine is sRGB-decoded before it is unpacked, so a flat texel unpacks to a -0.57 tilt rather than zero; `normalTexture.scale`, which `common/normal_map.slang` names in a comment as the thing `ObjMaterial` has no field for; the metallic-roughness texture, the one lit glTF slot the Rust twin samples and this engine has no field for; and `map_Bump`/`norm`, which both OBJ paths drop while both renderers now do normal mapping)

The actionable queue was empty when this batch was written (0 `- [ ]`, 16
`- [b]` across the whole file). The 2026-08-05 batch's three tasks all shipped
(`fc637887`, `caba3c20`, `ddbdb1b4`). Everything below was read out of the tree
at `ddbdb1b4`.

**First, normal maps are uploaded through the sRGB format.**
`Texture.cpp:124` is `constexpr vk::Format texture_format =
vk::Format::eR8G8B8A8Srgb` — one hardcoded format for every model texture, used
for the image (`:156`) and for the view (`:203`). `GltfLoader.cpp:724-732`
already says so in a `KNOWN LIMITATION` comment written when the normal slot
landed: "wrong for a normal map (linear tangent-space data — glTF requires it
be read WITHOUT sRGB decode) … Fixing it needs a per-slot format, out of scope
here." Normal mapping then shipped four commits later (`2030c374`), so that
comment now describes live, wrong output in all four shading paths.

The size of the error is not subtle. A flat normal-map texel is (128, 128,
255); through a UNORM view it samples as (0.502, 0.502, 1.0) and
`normal_map.slang:21`'s `sampledNormal * 2.0 - 1.0` gives (0.004, 0.004, 1.0) —
the geometric normal, as intended. Through the sRGB view the hardware decodes
128/255 to **0.2158**, and the same line gives **(-0.568, -0.568, 1.0)**. Every
normal-mapped surface in the engine is therefore tilted by a large constant
amount along -T and -B, before the map's own detail is applied. The Rust twin
gets this right and carries the exact mechanism this task needs: `CpuTextureRef`
has an `srgb: bool` (`scene/mod.rs:155`, `asset/gltf_loader.rs:364-376`) and
`gltf_loader.rs:649-665` passes `true` for base colour and emissive, `false`
for metallic-roughness, normal and occlusion.

The dedup key has to move with the format. `GltfLoader.cpp:704` keys
`imageSlot` on `(const cgltf_image *, const cgltf_sampler *)`, and
`gltfParseSuite.cpp:371-415` pins the consequence: a document whose
`baseColorTexture` and `normalTexture` name the same image lands on **one**
slot. One slot can only have one format, so that case is unfixable without
adding the colour space to the key — which is also why that existing test's
expectation has to change as part of this task, not be worked around.

**Second, `normalTexture.scale` has nowhere to go.**
`common/normal_map.slang:10` is a comment naming the gap outright:
"KHR\_texture\_transform normalScale factor — ObjMaterial has none yet." glTF
2.0 §3.9.3 defines the scale as applying to the tangent-space X and Y
components before normalisation; the Rust twin reads it
(`gltf_loader.rs:646`, `normal_scale`), packs it into
`material_factors.w` and applies it in `forward.slang`. The C++ loader never
reads `cgltf_texture_view::scale`, so an author who dials a normal map's
strength down gets no effect at all in this engine. It is the last
per-texture glTF scalar in the normal chain, and it is the only one of the
four `ObjMaterial` factors added in the last week (`metallic`, `roughness`,
`emissiveTextureID`, `normalTextureID`) whose companion value was left behind.

**Third, `metallicRoughnessTexture` is the last unread lit texture slot.**
`fromGltfMaterial` reads `pbr.metallic_factor` and `pbr.roughness_factor`
(`GltfLoader.cpp:152-154`) and every shading path consumes them —
`rasterizer.slang:92-97`, `deferred.slang:97,120`,
`raytrace.rchit.slang:136-151`, and the path tracer. But
`pbr.metallic_roughness_texture` is never touched: `parseCpu` assigns exactly
three slots (`:721` base colour, `:723` emissive, `:733` normal). So a glTF
whose roughness varies across a surface — the normal authoring case for
anything metal or worn — renders with one flat factor. The Rust twin samples it
(`gltf_loader.rs:652-654`, `forward.slang:369-387`) with the spec's channel
assignment (G = roughness, B = metallic, multiplied by the factors). Occlusion,
the fifth slot the Rust twin carries, deliberately stays out of this batch:
the C++ paths have no ambient or IBL term for it to attenuate (a grep for
`ambient`/`ibl` across `rasterizer.slang`, `deferred.slang` and `common/`
returns nothing but `sky_model.slang`'s doc comment), so there is nothing to
multiply it into yet.

**Fourth, both OBJ paths drop the normal map.** `ObjLoader.cpp:199` reads
`mp->diffuse_texname` and nothing else; `tiny_obj_loader.h:210` and `:238`
expose `bump_texname` (`map_Bump`/`map_bump`/`bump`) and `normal_texname`
(`norm`), and both are ignored. The Rust OBJ→glTF converter does the same, and
its comment justifying it — `obj_to_gltf.rs:110-114`, "`map_Bump`… have no
glTF equivalent" — was true when it was written and is now false: glTF's
`normalTexture` is exactly that equivalent, and both renderers implement it.

**Verification context.** Host GPU goldens are still blocked over RDP (the
`- [b]` near the end of this file) and `path_tracing` mode device-losts on the
host RX 9070 XT on unmodified `develop` (the `- [b]` at line ~2030). Tasks 1
and 4 are CPU-verifiable end to end: the format decision and the slot
assignment are both parse-time state the loaders already expose to
device-free suites. Tasks 2, 3 and 5 change shading math or generated glTF and
carry defaults chosen so an unaffected scene is bit-unchanged (`normalScale`
1.0, no MR texture, no `map_Bump`). **Do not claim a rendered result you
cannot obtain** — state which suites you ran.

**Ordering.** Task 1 first: tasks 3 and 4 both need the per-slot colour space
it introduces (metallic-roughness and normal data are both linear), and task 4
needs the `createFromFile` half of it. Tasks 2 and 3 both append a member to
`ObjMaterial` and both edit the same two layout gates, so whichever runs second
rebases on the first's offsets — the entries below spell out both cases. Task 5
is in the Rust submodule and is independent of all four.

### Rust WebGPU renderer (`third_party/OxidANT`)

## 2026-08-05 batch III — planner (one `KHR_texture_transform`, read from the base-colour slot and applied to all four, so an atlased base colour silently tiles the normal, metallic-roughness and emissive maps that never asked for it; a `-bm` factor read out of `bump_texopt` even when the map came from `norm`, which is the one directive whose own texopt holds it; `map_Ke`, which tinyobjloader parses into `emissive_texname` and both OBJ paths drop while all four C++ shading paths and the Rust twin sample an emissive texture; and a material table in `docs/model-loading.md` that is two rows short of the struct it documents, behind a gate that checks the doc's citations but never its coverage)

The actionable queue was empty when this batch was written (0 `- [ ]`, 16
`- [b]` across the whole file). Batch II's five tasks all shipped (`47371a1a`,
`e0e25ee6`, `4bf4bba0`, `54a39af2`, `4afd4669`). Everything below was read out
of the tree at `4afd4669`.

**First, there is one UV transform for four texture slots.**
`fromGltfMaterial` reads `KHR_texture_transform` from exactly one place —
`material.pbr_metallic_roughness.base_color_texture.has_transform` — and packs
it into `ObjMaterial::uv_transform_row0`/`uv_transform_row1`. `ObjMaterial.hpp`
says so on the member itself ("for the base-colour texture"), and so does
`docs/model-loading.md`'s table row. But the shaders apply
`transform_uv(uv, material)` to **every** texture sample: nine of the twelve
call sites are not the base-colour slot —
`rasterizer.slang` (normal, metallic-roughness, emissive),
`deferred.slang` (same three), `raytrace.rchit.slang` (same three) and
`path_tracing.slang` (normal, metallic-roughness, emissive). Only
`rasterizer.slang`'s `baseSample`, `deferred.slang`'s `texColor`,
`raytrace.rchit.slang`'s `albedo`, `path_tracing.slang`'s `hitColor`,
`alpha_test.slang` and `shadow_map.slang` are reading the slot the rows
actually came from.

glTF 2.0 defines `KHR_texture_transform` per `textureInfo`, not per material,
and the failure is not hypothetical: the extension's dominant use is atlasing
or tiling a base colour. A material with `scale: [4, 4]` on `baseColorTexture`
and a plain, untransformed `normalTexture` gets its normal map tiled four
times over in all four shading paths — and the inverse case (a transform on
`normalTexture` only) is dropped silently, because nothing outside the
`has_pbr_metallic_roughness` branch ever looks at `has_transform`. The Rust
twin is per-slot and is the reference: `forward.wgsl` builds the transformed
UV inline for `baseColorTex` alone, while `mrIn_0`, `normalIn_0`,
`emissiveIn_0` and `occlusionIn_0` are the raw per-slot UV. `scene/mod.rs`
names its field `base_uv_transform` for exactly this reason.

The struct is the right place to fix it, and appending is the pattern this
struct has used four times in the last week. `ObjMaterial` is 88 bytes with
`metallicRoughnessTextureID` last at offset 84; `glm::vec3` has alignment 4,
so three more row pairs append tightly at 88/100/112/124/136/148 for
`sizeof == 160`, and every existing offset is unchanged — which is what makes
`ObjMaterialLayoutUnit.MatchesTheSlangTwinScalarLayout` and the
`ObjMaterial_natural` entry able to prove the change mechanically without a
GPU.

**Second, the OBJ `-bm` factor is read from the wrong texopt.**
`ObjLoader.cpp`'s `loadTexturesAndMaterials` ends its per-material block with
an unconditional `material.normalScale = mp->bump_texopt.bump_multiplier;`,
three lines after a branch that deliberately prefers `mp->normal_texname`
(`norm`) over `mp->bump_texname` (`map_Bump`/`map_bump`/`bump`).
tinyobjloader keeps a separate `texture_option_t` per directive —
`tiny_obj_loader.h` declares `bump_texopt` and `normal_texopt` as distinct
members, and its `.mtl` reader routes `norm` through
`ParseTextureNameAndOption(&material.normal_texname, &material.normal_texopt, ...)`
and `map_Bump` through the `bump_texopt` pair. So two things go wrong at once:
`norm rock_n.png -bm 0.5` has its factor dropped (`bump_texopt` is still at
`InitTexOpt`'s 1.0 default), and a `.mtl` naming both directives, e.g.
`map_Bump height.png -bm 3.0` plus `norm rock_n.png`, applies the bump map's
3.0 to the normal map the loader deliberately chose instead.

The Rust converter already does this right, per directive: `parse_mtl`'s
`"norm" | "map_Bump" | "map_bump" | "bump"` arm calls `bump_scale_option` on
the tokens of the line it is currently reading, so the factor can only ever
come from the directive that won. This is the C++ half of a rule the two
loaders are supposed to share.

**Third, `map_Ke` is dropped by both OBJ paths.** `ObjMaterial` has carried
`emissiveTextureID` since `d0a25d21`, all four C++ shading paths sample it
through `common/emission.slang`'s `material_emission()`, and the Rust forward
pass samples `emissiveTex_0`. tinyobjloader parses `map_Ke` into
`emissive_texname` (`tiny_obj_loader.h`, beside `normal_texname`), and both
OBJ paths ignore it: the C++ `loadTexturesAndMaterials` assigns `textureID`
and `normalTextureID` and nothing else, and `parse_mtl`'s match arms cover
`Kd`, `Ke`, `d`, `Tr`, `map_Kd` and the four normal-map spellings. This is the
same shape as the `map_Bump`/`norm` pair that shipped yesterday (`54a39af2`
plus `4afd4669`), one slot over: an OBJ with a glowing-window texture loads its
`Ke` factor and then multiplies it by an implicit 1.0.

**Fourth, the material table documents twelve of fourteen members.**
`docs/model-loading.md`'s "Material fields and where they come from" table is
introduced as covering `ObjMaterial`, "the single struct both loaders fill and
every shader reads", and ends at `normalTextureID` — `normalScale` and
`metallicRoughnessTextureID`, both shipped in the last two days, have no row.
Three surviving rows are also stale: `emission`'s glTF source omits the
`KHR_materials_emissive_strength` fold-in that `fromGltfMaterial` performs and
its "Read by" column names only two of the four shading paths that now read it;
the `srgb` row says linear is chosen "for the normal-map view" when
`parseCpu` also passes `false` for metallic-roughness. `docs/shader-sharing.md`
has two of its own: it attributes `transform_uv` to
`common/material_fetch.slang` (the function lives in `common/base_color.slang`,
which explains in its own header comment why it had to move), and its
divergence list still says the rotation "is also unapplied" when
`fromGltfMaterial` builds a full `T*R*S` matrix including the negated rotation.

What makes this worth a task rather than a note is that the file already has
two gates — `BuildIntegrity.ModelLoadingDocCitesSymbolsNotLineNumbers` and the
`<!-- max-texture-count: N -->` marker check — and neither can see a missing
row. A gate that walks `ObjMaterial`'s member names and requires each to appear
in the table is mechanical, needs no GPU, and is the only thing that will stop
the next appended member from repeating this.

**Verification context.** Host GPU goldens are still blocked over RDP (the
`- [b]` near the end of this file) and `path_tracing` mode device-losts on the
host RX 9070 XT on unmodified `develop` (the `- [b]` at line ~2030). All five
tasks are accepted CPU-only: tasks 1–3 are parse-time state the loaders already
expose to device-free suites (task 1's shader half is additionally pinned by a
source-text gate, the same instrument `NormalMappingIsAppliedByEveryShadingPath`
uses), task 4 is in the Rust submodule, and task 5 touches only docs and a gate.
**Do not claim a rendered result you cannot obtain** — state which suites you
ran.

**Ordering.** Tasks 2 and 3 both edit `ObjLoader::loadTexturesAndMaterials`'s
per-material loop and should not run concurrently; either order works. Task 1
is independent of both but edits `ObjMaterial.hpp` (a header in
`ObjMaterial.ixx`'s global module fragment) and therefore needs
`-FreshContainer`; task 3 does not append a member and does not. Task 5 should
run **last** — tasks 1 and 3 each add a row the table will need, and the gate
task 5 adds would otherwise be RED the moment task 1 lands. Task 4 is in the
Rust submodule and is independent of all four.

### C++ Vulkan engine

### Rust WebGPU renderer (`third_party/OxidANT`)

### Docs

## 2026-08-05 batch IV — planner (refactor: the per-slot `KHR_texture_transform` that shipped this morning left twelve call sites each naming its own `_row0`/`_row1` pair by hand, in four shaders, where pairing the wrong slot's rows is a silent mis-sample; eighteen `file:line` citations in engine comments and shaders, at least eight of them verifiably pointing at unrelated code today, against a gate that already forbids exactly this — in one doc only; and a ten-line texture-teardown block that `DeferredRasterizer` writes twice verbatim and `Rasterizer` writes twice with different null-handling)

The actionable queue was empty when this batch was written (0 `- [ ]`, 16
`- [b]` across the whole file). Batch III's four tasks all shipped (`4b3f438d`,
`f01fb288`, `3143e92a`/`60175c97`, `697349d8`). Everything below was read out of
the tree at `697349d8`.

**First, the per-slot UV rows are spelled out by hand at every sample.**
Batch III's `4b3f438d` did the right thing structurally — `ObjMaterial` now
carries four independent `KHR_texture_transform` row pairs, one per texture
slot — but it wired them in the most fragile way available. Every non-base-colour
sample in the engine reads:

```
transform_uv(In.texCoords, material.normal_uv_transform_row0, material.normal_uv_transform_row1)
```

…with the slot name typed twice, as two separate arguments, on the same line as
a *different* slot's `resolve_texture_slot(obj, material.normalTextureID)`.
There are **twelve** such sites — three each in `rasterizer.slang`
(`:71`, `:97`, `:118`), `deferred.slang` (`:91`, `:99`, `:123`),
`raytrace.rchit.slang` (`:113`, `:142`, `:172`) and `path_tracing.slang`
(`:240`, `:289`, `:305`). The base-colour slot is the only one that does *not*
name its rows: `base_color.slang` already provides a
`transform_uv(float2 uv, ObjMaterial material)` overload whose whole documented
purpose is "the base-colour slot's `KHR_texture_transform`, by definition", and
its own comment ends by telling the reader that "the normal,
metallic-roughness and emissive slots each have their own pair and call the
(uv, row0, row1) overload above directly with those members."

That asymmetry is the defect. Typing `material.emissive_uv_transform_row0`
where `material.normal_uv_transform_row1` belongs compiles, runs, and produces
a wrongly-transformed normal map that no test can see — and it is a plausible
typo precisely because the four names differ by one word in the middle of a
150-column line. The fix is the one the base-colour slot already demonstrates:
one named accessor per slot, so the slot is named once and the row pair can
never be mismatched. This is the same "one rule, N hand-rolled copies" shape
this backlog has closed eight times in `Src/`; it has simply reappeared in
`Resources/ShadersSlang/`.

**Second, engine comments cite line numbers, and they have rotted.**
`BuildIntegrity.ModelLoadingDocCitesSymbolsNotLineNumbers`
(`buildIntegritySuite.cpp:5441`) already encodes the principle — its comment
says citations "rot within days - a function moves ten lines and the citation
now points at unrelated code, silently" — but it is scoped to exactly one file,
and its own comment justifies that scope with a claim that is now false:
"docs/model-loading.md is the one doc in the tree written to cite `file:line`
locations". It is not the one *place*. A grep for
`[A-Za-z_/.-]+\.(cpp|hpp|ixx|slang|wgsl|rs):[0-9]+` over `Src/` and
`Resources/ShadersSlang/` returns **eighteen** citations, and spot-checking
them at `697349d8` found most of them wrong:

| Citation site | Cites | What is actually there |
| --- | --- | --- |
| `SceneUboMarshal.hpp:30` | `CascadedShadowMap.cpp:342-352` | vertex-input attribute descriptions, not the `glm::ortho` cascade matrices |
| `SceneUboMarshal.hpp:71` | `clouds.slang:129-143` | `eyePosition`; the UBO unpack it means starts three lines later |
| `SceneUboMarshal.hpp:100` | `clouds.slang:126` | an `inv_projection` multiply; the `cam_pos.xyz` read is at `:129` |
| `SceneUboMarshal.hpp:101` | `deferred.slang:121` | the emissive-texture branch, no `cam_pos` in sight |
| `SceneUboMarshal.hpp:101` | `rasterizer.slang:55` | the `[shader("fragment")]` attribute; the read is at `:74` |
| `SceneUboMarshal.hpp:101` | `raytrace.rchit.slang:87` | the base-colour branch; the read is at `:116` |
| `SceneUboMarshal.hpp:112` | `rasterizer.slang:75` | `float3 albedo;`, not the `lightIntensity` unpack |
| `ObjMaterial.hpp:12` | `rasterizer.slang:84-86` | the untextured `else` branch; `material_emission` is at `:120` |
| `clouds.slang:40` | `raytrace.rchit.slang:78-81` | barycentric vertex-colour interpolation; the explicit-LOD rationale is at `:89-91` |
| `path_tracing.slang:260` | `raytrace.rchit.slang:78-81` | same, a second copy of the same wrong citation |
| `cascaded_shadow.slang:56` | `Texture.cpp:249` | the middle of a parameter list |

Only three survived the check (`cascaded_shadow.slang:22`,
`VulkanRenderer.cpp:690`, `VulkanSwapChain.cpp:52`) and those are luck, not
durability. Four more were not checked and are the executor's job. The point is
not to re-verify eleven numbers once; it is that the instrument to make the
class extinct already exists and is pointed at one markdown file.

**Third, `DeferredRasterizer` frees its textures twice, and `Rasterizer` frees
them twice differently.** `DeferredRasterizer::cleanUp` (`:125-134`) and
`DeferredRasterizer::recreateFrameResources` (`:151-160`) are ten lines of
byte-identical text — four `for (auto& tex : <vec>) { if (tex) tex->cleanUp(); }
<vec>.clear();` pairs plus the `depthBufferImage` guard-and-reset. `Rasterizer`
has the same pairing over two vectors, and the two copies **disagree**:
`cleanUp` (`:111-117`) guards both (`if (texture)`, `if (depthBufferImage)`),
while `recreateFrameResources` (`:138-143`) dereferences
`texture->cleanUp()` and `depthBufferImage->cleanUp()` unguarded. That is not a
hypothetical difference — it is the exact asymmetry that makes one of two
copies of a block the wrong one to read. `PostStage` is not affected (it owns no
textures), and a grep for the pattern over the rest of `Src/` finds no other
site, so this is a bounded two-file cleanup.

**Verification context.** Host GPU goldens are still blocked over RDP (the
`- [b]` near the end of this file) and `path_tracing` mode device-losts on the
host RX 9070 XT on unmodified `develop` (the `- [b]` at line ~2030). All three
tasks are accepted CPU-only. Tasks 1 and 2 are pinned by source-text gates, the
same instrument `NormalMappingIsAppliedByEveryShadingPath` and
`ModelLoadingDocCitesSymbolsNotLineNumbers` already use; task 3 is a
behaviour-preserving extraction whose only behavioural change is *adding* null
guards that the sibling copy already had. **Do not claim a rendered result you
cannot obtain** — state which suites you ran. Task 1 does change generated
SPIR-V; if `slangc` output is byte-identical, say so, and if it is not, say
that too rather than asserting pixels.

**Ordering.** All three are independent and can run in any order. Task 2 edits
comments in `SceneUboMarshal.hpp` and in the same four shaders task 1 rewrites,
so do not run 1 and 2 concurrently; either order works, and whichever runs
second rebases trivially. Task 3 touches only `renderer/Rasterizer.*` and
`renderer/DeferredRasterizer.*`.

### C++ Vulkan engine

## 2026-08-05 batch V — planner (`Pm`/`Pr`, the two PBR channels tinyobjloader parses into fields both OBJ paths drop, under a test whose comment asserts the channels do not exist; `KHR_materials_unlit`, which the Rust twin shipped in July as the fix for "every Sketchfab/mobile/AR flat-color export" and the C++ engine has never heard of; a `KHR_texture_transform` that the Rust loader still reads from the base-colour slot alone, four days after the C++ loader went per-slot — the same defect, now with the renderers swapped; and a "known glTF loader divergences" list that is one bullet long while the two loaders disagree on at least four material features)

The actionable queue was empty when this batch was written (0 `- [ ]`, 16
`- [b]` across the whole file). Batch IV's three tasks all shipped
(`f5e27d46`, `c1cd8fad`, `994cbf4a`). Everything below was read out of the tree
at `994cbf4a`.

**First, `Pm` and `Pr` are parsed and then thrown away, by both OBJ paths.**
`ObjMaterial` has carried `metallic` and `roughness` since the glTF PBR work,
and all four C++ shading paths read them (`material_roughness()` plus the
`metallic` term in `brdf_direct`). The Wavefront PBR extension defines exactly
these two as `Pm` and `Pr`, and this repo's vendored tinyobjloader parses both:
`material_t` declares `real_t roughness;` and `real_t metallic;`, `InitMaterial`
zeroes them, and the `.mtl` reader has `sr_parseReal(sr, &material.roughness, …)`
and `sr_parseReal(sr, &material.metallic, …)` arms. `ObjLoader.cpp`'s
`loadTexturesAndMaterials` assigns `diffuse`, `emission`, `dissolve`,
`shininess` and the three texture slots, and never touches either. So a `.mtl`
authored `Pm 1.0` renders as a dielectric and `Pr 0.15` renders at whatever
roughness `shininess` happens to imply.

What makes this worth naming rather than shrugging at is
`ObjParseUnit.MaterialsHaveZeroMetallic`, whose comment states the rationale as
"Wavefront .mtl has no metallic channel, so every OBJ material must come back
with ObjMaterial's default metallic (0.0)". The premise is false — the channel
exists, the parser reads it, and the test passes only because the loader
discards it. A test that asserts a field is zero *because the format cannot
carry it* is the strongest possible signal that nobody checked whether the
format can carry it.

There is one real trap, and it is why this is not a two-line change.
tinyobjloader defaults **both** fields to `0.0` and exposes no "was this
authored" flag, so `mp->roughness == 0.0` is indistinguishable between "no `Pr`
directive" (every model in `Resources/Models/` today) and "`Pr 0.0`, a perfect
mirror". `ObjMaterial::roughness` uses `-1.0` as its "derive it from
`shininess`" sentinel, so assigning `mp->roughness` unconditionally would move
every existing OBJ material from the shininess-derived value to a mirror
finish — a visible regression in every bundled scene. `metallic` has no such
problem: tinyobjloader's default and `ObjMaterial`'s default are both `0.0`, so
it can be assigned unconditionally and stays bit-identical where `Pm` is absent.

**Second, `KHR_materials_unlit` exists in one renderer.** The Rust twin shipped
it on 2026-07-22; `docs/webgpu-renderer-roadmap.md` records the reason in one
line — "Fixes every Sketchfab/mobile/AR flat-color export, which previously got
a full GGX response with IBL and shadows". `CpuMaterial` carries `unlit`,
`forward.rs` packs it into `material_flags.x`, and `forward.slang`'s `fs_main`
returns `albedo` at `if (prim.material_flags.x > 0.5)` — before any lighting,
per spec. A grep for `unlit` over the whole of `Src/` returns nothing. cgltf
parses the extension into `cgltf_material::unlit` and `fromGltfMaterial` never
reads it, so the same asset that renders flat in the WebGPU demo renders
shaded, shadowed and specular in the Vulkan engine.

The deferred path is the only half with a design question, and the G-buffer
already answers it: `geometry_fs_main` writes `g.outAlbedo = texColor` into an
`eR8G8B8A8Srgb` attachment, and `lighting_fs_main` reads `albedo.rgb` and
nothing else — the alpha channel is written every frame and read by no one.
Vulkan applies the sRGB transfer function to RGB only, so alpha round-trips
linearly and a 0.0/1.0 flag survives the store/`SubpassLoad` exactly. That is
the channel, and it costs no new attachment.

**Third, the Rust loader still has one UV transform for five slots — the
mirror image of the bug that shipped four days ago.** `4b3f438d` fixed the C++
side: `ObjMaterial` now carries four independent `KHR_texture_transform` row
pairs and `base_color.slang` gives each slot its own named accessor. The Rust
loader was cited in that batch as *the reference* for per-slot behaviour, and
on re-reading it is only half of one. `gltf_loader.rs` builds
`base_uv_transform` from `pbr.base_color_texture().and_then(|info|
info.texture_transform())` and nothing else; `CpuMaterial` has exactly that one
field; `PrimUniforms` carries exactly `base_uv_row0`/`base_uv_row1`; and
`forward.slang`'s `fs_main` builds `baseUv` from those rows while `mrIn`,
`normalIn`, `emissiveIn` and `occlusionIn` go to `Sample` as the raw per-slot
UV. So Rust is correct in the sense the C++ engine was wrong — it never applies
the base-colour transform to another slot — and wrong in the other direction:
a `normalTexture` or `occlusionTexture` carrying its own `KHR_texture_transform`
has it dropped silently. glTF defines the extension per `textureInfo`; both
halves are required, and each renderer currently implements one of them.

The slot order is already fixed by `uv_set_mask` (`uv_set_bit`'s "bit per slot:
0 base .. 4 occlusion"), so the new row pairs have an unambiguous order to
follow, and `fs_shadow_masked` — which already selects the base-colour UV set
from `material_flags.y` before applying `base_uv_row0/1` — is the shape the
four new slots should copy.

**Fourth, the divergence list is one bullet, and there are at least four
divergences.** `docs/shader-sharing.md`'s "Known glTF loader divergences (not
shader-shared, but the two renderers must stay honest about it)" section is
introduced as the place the two loaders stay honest with each other, and
contains a single entry, about TEXCOORD_1 on the base-colour slot. Read against
the tree at `994cbf4a` the section is silent about:

| Feature | C++ `GltfLoader` | Rust `gltf_loader` |
| --- | --- | --- |
| `KHR_materials_unlit` | not read | read, `material_flags.x`, returns before lighting |
| `occlusionTexture` + `occlusionStrength` | no `ObjMaterial` field, no slot | full: slot, strength, own UV-set bit |
| `KHR_texture_transform` per slot | all four slots | base colour only (task 3 above) |
| `alphaMode` `BLEND` | renders opaque (`alphaCutoff` = -1) | `alpha_blend`, its own pipeline |

The occlusion row is the interesting one, because it is a divergence that
should *stay*: glTF scopes `occlusionTexture` to indirect light, and neither C++
raster path has an indirect term at all (`rasterizer.slang`'s `fs_main` is
`brdf_direct` + shadow + emissive; the deferred lighting pass is the same). A
list that records "not applicable here, and why" is the difference between a
known limitation and the next planner re-deriving this paragraph. The existing
`ModelLoadingDocDocumentsEveryObjMaterialMember` gate is the shape to copy for
keeping it from rotting again.

**Verification context.** Host GPU goldens are still blocked over RDP (the
`- [b]` near the end of this file) and `path_tracing` mode device-losts on the
host RX 9070 XT on unmodified `develop` (the `- [b]` at line ~2030). All five
tasks are accepted CPU-only: tasks 1 and 2 are parse-time state the loaders
expose to device-free suites plus source-text gates (the instrument
`NormalMappingIsAppliedByEveryShadingPath` and
`MetallicRoughnessTextureIsSampledByEveryShadingPath` already use), tasks 3 and
4 are in the Rust submodule, and task 5 touches only docs and a gate. **Do not
claim a rendered result you cannot obtain** — state which suites you ran.

**Rust verification limits.** `cargo test`/`cargo build` do not link on this
host: the VC++ Build Tools install is incomplete and Git Bash's `link.exe`
shadows MSVC's. Verify tasks 3 and 4 with `cargo check`, `cargo clippy` and
`cargo fmt --check` from
`third_party/OxidANT`, say so explicitly in the commit
message, and let the always-on Linux lane (`scripts/linux/run-cargo-tests.sh`)
be the thing that actually runs the tests you add.

**Ordering.** Task 5 must run **last** — tasks 2 and 3 each delete a row from
the table it builds. Tasks 1 and 2 both edit
`ObjLoader.cpp`/`GltfLoader.cpp` and `ObjMaterial.hpp`; do not run them
concurrently, either order works, and task 2 appends a member so it needs
`-FreshContainer` (`ObjMaterial.hpp` sits in `ObjMaterial.ixx`'s global module
fragment). Task 1 changes no header and does not. Tasks 3 and 4 are both in the
Rust submodule and touch disjoint files (`gltf_loader.rs`/`forward.rs` versus
`obj_to_gltf.rs`), so they are independent of each other and of tasks 1–2.

### C++ Vulkan engine

### Rust WebGPU renderer (`third_party/OxidANT`)

### Docs

## 2026-08-05 batch VI — planner (three pure material rules that sit behind a binding declaration, so the two shaders that cannot import them re-derive them by hand — one of them under a gate whose comment writes the duplication up as a permanent exception; a glTF `shininess` derived from a roughness that is only ever read in the one case where it is pinned to 1.0; `Ns`, which the converter drops and the engine's own shader derives roughness from — the last row of the divergence matrix batch V built; and a WGSL staleness gate that treats every file under `common/` as an input to every shader, so any material edit marks `ssao.wgsl` stale)

The actionable queue was empty when this batch was written (0 `- [ ]`, 16
`- [b]` across the whole file). Batch V's five tasks all shipped (`135aebb8`,
`8a728c26`, `70658a67`, `fcac6d49`, `3af452d8`). Everything below was read out
of the tree at `3af452d8`.

**First, `material_fetch.slang` holds three functions that touch no binding,
and the binding it does declare is what stops two shaders from calling them.**
The module opens with `[vk::binding(2, 0)] StructuredBuffer<ObjectDescription>
objectDescription;` for `fetch_object_description`/`fetch_material`. Everything
after it is pure arithmetic over an `ObjMaterial` value: `material_roughness()`
(the authored-factor-or-Beckmann-from-`shininess` rule),
`material_metallic_roughness()` (glTF's G=roughness/B=metallic swizzle) and
`alpha_masked_out()` (the MASK product rule). `path_tracing.slang` and
`alpha_test.slang` declare their own `objectDescription` and therefore cannot
`import material_fetch` without an ambiguous-reference error — so both hand-roll
what they need: `alpha_test.slang`'s `ray_hit_masked_out` ends in
`return (sampledAlpha * material.dissolve) < material.alphaCutoff;`, a verbatim
copy of `alpha_masked_out`'s body, and `path_tracing.slang` writes
`hitColor *= (1.0 - material.metallic * metallicSample);` where the shared
swizzle would give it the same number.

The project already solved this exact problem twice and named the pattern:
`base_color.slang` and `emission.slang` and `normal_map.slang` each carry a doc
comment saying, in so many words, "deliberately its own module, not folded into
`material_fetch.slang`, because the ray-tracing entry points cannot import that
one — this module has no bindings, so every shading path can import it." Three
modules exist for that reason and the three remaining pure rules were left
behind. What makes it worth doing now rather than noting again is
`MetallicRoughnessTextureIsSampledByEveryShadingPath`: its comment
does not describe the duplication as debt, it *ratifies* it —
"path_tracing.slang is a documented exception … it is checked only for
referencing the texture ID and the metallic (B) channel, and must NOT duplicate
the roughness (G) channel read". A gate that forbids half of a duplication and
mandates the other half is the point at which the workaround has become the
design.

**Second, `fromGltfMaterial` keeps two roughness locals, and the one it feeds
to `shininess` can only ever be 1.0 where anything reads it.** `roughness`
starts at `1.0F`, `authoredRoughness` at the `-1.0F` sentinel, and both are
assigned from `pbr.roughness_factor` inside the same
`has_pbr_metallic_roughness` guard. `shininess` is then
`glm::mix(128.0F, 1.0F, glm::clamp(roughness, 0.0F, 1.0F))`. But
`material_roughness()` reads `material.shininess` only when
`material.roughness < 0.0` — that is, only when the material had no
`pbrMetallicRoughness` block at all, which is exactly the case where the local
`roughness` was never assigned and is still `1.0`, so `shininess` is `1.0`.
Every other value the `mix` can produce is computed, stored in the GPU material
buffer and read by nothing. `neutralMaterial()` — the stand-in for primitives
with no material — already hard-codes `.shininess = 1.0F` for the same reason,
without the arithmetic. `docs/model-loading.md`'s `shininess` row half-says this
already ("only ever set to a derived `mix(128, 1, roughnessFactor)` value, kept
as the OBJ-only fallback"), which is true and still leaves the reader to work
out that the derived value cannot vary at the point of use.

**Third, `Ns` is the last row of the divergence matrix, and it is a real
"same asset, two looks".** `3af452d8` built the matrix and its final row records
that a `.mtl` without `Pr` renders one way through `ObjLoader` (the shader
derives roughness from `Ns` via `sqrt(2/(Ns+2))`) and another way through
`obj_to_gltf` (a flat `roughnessFactor: 1.0` regardless of `Ns`). The
converter's module doc states the rationale — "`Ks`/`Ns` have no faithful PBR
equivalent and are dropped rather than guessed into metallic/roughness" — and
that is defensible in the abstract and wrong for *this* repo, because this repo
already has a canonical `Ns`→roughness mapping, in
`material_fetch.slang`'s `material_roughness()`, applied to every OBJ material
the C++ engine loads. The converter guessing the same value the engine already
guesses is not an invention; it is the difference between a conversion that
round-trips and one that does not. `Ks` genuinely has no equivalent and should
stay dropped.

**Fourth, the WGSL staleness gate treats `common/` as one input.**
`newest_shared_import` walks `Resources/ShadersSlang/common/` and returns the
newest mtime found anywhere in it; both
`CheckedInWgslIsNotOlderThanItsSlangSource` and
`CompiledShadersAreNotOlderThanSharedIncludes` then compare *every* output
against that single timestamp. Slang has no preprocessor, so the intent is
right — editing an imported module does invalidate its dependants — but the
implementation has no notion of which shader imports what. Editing
`material_fetch.slang` (which the last four batches did repeatedly) marks
`ssao.wgsl`, `bloom.wgsl`, `tonemap.wgsl`, `gpu_cull.wgsl` and
`occlusion_bbox.wgsl` stale, none of which import it, or anything that imports
it. The import graph is right there in the sources as `import <name>;` lines and
the resolution rule is one directory deep.

This does not fix the *other* false positive on the same gate, and the task
should not claim it does: when a shader genuinely does import the edited module
but its emitted WGSL is byte-identical, `slangc` plus `Copy-Item` leave the
destination's mtime untouched and the gate still reports it stale. That one
needs a content stamp written by the compile scripts, which live upstream in
ANTfrastructure — out of scope here, and worth stating in the test's comment so
the next reader does not re-derive it.

**Verification context.** Host GPU goldens remain blocked over RDP (the `- [b]`
near the end of this file) and `path_tracing` mode device-losts on the host
RX 9070 XT on unmodified `develop` (the `- [b]` at line ~2030). All five tasks
are accepted CPU-only: tasks 1, 2 and 3 are parse-time state plus source-text
gates in `commitTestSuite.exe`, task 4 is in the Rust submodule, task 5 is docs
plus a gate. **Do not claim a rendered result you cannot obtain** — say which
suites you actually ran.

**Rust verification limits (task 4).** `cargo test`/`cargo build` do not link on
this host: the VC++ Build Tools install is incomplete and Git Bash's `link.exe`
shadows MSVC's. Verify with `cargo check`, `cargo clippy` and
`cargo fmt --check` from `third_party/OxidANT`, say so
explicitly in the commit message, and let the always-on Linux lane
(`scripts/linux/run-cargo-tests.sh`) be what actually runs the tests you add.

**Ordering.** Tasks 1, 2 and 3 all edit
`Test/commit/VulkanEngine/buildIntegritySuite.cpp` — land them one at a time,
in that order; the second and third rebase trivially. Tasks 2 and 5 both edit
`docs/model-loading.md` (different rows), so serialize those two as well. Task 4
is the only one inside the Rust submodule and is independent of everything else
except that it deletes the divergence-matrix row task 5 does not touch.

### C++ Vulkan engine

### Rust WebGPU renderer (`third_party/OxidANT`)

### Docs

## 2026-08-05 batch VII — planner (the TBN matrix multiplied on the wrong side in the one module all five normal-mapped shading paths call, which the emitted WGSL confirms without a GPU; a GLFW cursor-position callback installed only while the right mouse button is down, against an ImGui backend that documents the opposite as mandatory; a per-primitive tangent pass that allocates and zeroes two vectors over the *whole* model's vertex array once per primitive; `map_d`, which two of the bundled sponza's 24 materials carry and both OBJ paths drop; and a doc that calls seven SPIR-V-only Slang modules "shared math")

The actionable queue was empty when this batch was written (0 `- [ ]`, 16
`- [b]` across the whole file). Batch VI's five tasks all shipped
(`0c5c57b0`, `c812eef8`, `22407124`, `f13797cb`, plus the docs follow-ups
through `bc021c43`). Everything below was read out of the tree at `bc021c43`.

**First, `apply_normal_map` multiplies the TBN basis on the wrong side.**
`common/normal_map.slang` ends with
`return normalize(mul(float3x3(t, b, n), nTs));`. In Slang/HLSL semantics
`float3x3(t, b, n)` builds the matrix with `t`, `b`, `n` as **rows**, and
`mul(M, v)` is `M·v` — component *i* is `dot(row_i, v)`. So the expression
evaluates to `(dot(t, nTs), dot(b, nTs), dot(n, nTs))`, which is the
world→tangent transform. The tangent→world transform this function is
documented to perform is `nTs.x*t + nTs.y*b + nTs.z*n`, i.e. `mul(nTs, M)`
with the same row-built matrix (or `mul(transpose(M), nTs)`).

This is not a subtle shading difference. Take the flat-texel case a normal
map spends most of its area on, `nTs = (0,0,1)`: the correct result is `n`,
and the expression returns `(t.z, b.z, n.z)`. For a floor with `n = (0,1,0)`,
`t = (1,0,0)`, `w = +1`, that is `b = cross(n, t) = (0,0,-1)` and the result
is `(0,-1,0)` — the geometric normal exactly inverted. Every normal-mapped
surface in the engine is being lit by a normal that is, at best, unrelated to
its surface.

The same expression is written a second time inline in
`forward/forward.slang` (`normalize(mul(float3x3(t, b, nGeom), nTs))`), so
the Rust/WebGPU renderer carries the identical defect. **You can verify the
Slang semantics offline, with no GPU and no compiler**: the emitted WGSL is
checked in, and
`crates/webgpu_renderer/src/shaders/forward.wgsl` reads
`(vec3<f32>(...)) * (mat3x3<f32>(t_0, cross(nGeom_1, t_0) * ..., nGeom_1))`.
WGSL `mat3x3` takes **columns**, and `v * M` is `result[j] = dot(v,
column_j)` — the same `(dot(t,nTs), dot(b,nTs), dot(n,nTs))` the HLSL reading
gives. Two independent readings of the same emitted artifact agreeing is what
makes this actionable while goldens are blocked.

The reason five source-text gates did not catch it is that all five check for
*calls* — `import normal_map;`, `apply_normal_map(`, `normalTextureID`,
`material.normalScale` — and none checks the one line of arithmetic that
those calls exist to share. Note also that the RT/PT normal transforms went
through exactly this row-vs-column reasoning in batch XVII
(`raytrace.rchit.slang`'s `mul(normalHit, (float3x3)WorldToObject())` and
`path_tracing.slang`'s twin, both correct); `apply_normal_map` was the one
place that reasoning was not applied.

**Second, the GLFW cursor-position callback is installed only during a
right-button drag, and ImGui is initialised with `install_callbacks = false`.**
`GUI.cpp` calls `ImGui_ImplGlfw_InitForVulkan(window, false)`, so ImGui sees
input *only* through the forwarding callbacks `Window::init_callbacks`
installs. That function installs key, mouse-button, scroll, char,
framebuffer-size, focus and cursor-enter callbacks — and **not**
`glfwSetCursorPosCallback`. The only three sites that touch it are
`handle_mouse_button_callback` (installs on right-press, clears on
right-release) and `Window::window_focus_callback` (clears on focus loss).
`imgui_impl_glfw.cpp`'s own header states the contract this violates: "If you
called ImGui_ImplGlfw_InitXXX() with install_callbacks = false, you MUST
install glfwSetCursorPosCallback() and forward it to the backend."

The consequence is mechanical: `ImGui_ImplGlfw_CursorEnterCallback` *is*
forwarded, so entering the window sets `bd->MouseWindow = window`, and
`ImGui_ImplGlfw_UpdateMouseData`'s polling fallback is gated on
`bd->MouseWindow == nullptr`. With the fallback disabled and the callback
uninstalled, ImGui's mouse position stops updating until the next right-drag.
The look-mode gate belongs in the pure `handle_mouse_callback` (where
`frontendInputSuite.cpp` can test it) rather than in which GLFW callback
happens to be installed — that is the same "GLFW-touching half stays at the
call site so this half stays testable" split the module's own comment already
describes.

**Third, `computeTangents` allocates over the whole model once per
primitive.** It opens with two `std::vector<glm::vec3>(vertices.size())`, but
`GltfLoader::processPrimitive` calls it per primitive with a `firstIndex`
that scopes the *work* to that primitive while the *allocation* stays sized
to every vertex parsed so far. Loading a P-primitive document therefore
allocates and zeroes Θ(P · V) bytes — for sponza-class glTF (hundreds of
primitives, hundreds of thousands of vertices) that is hundreds of megabytes
of pure memset on a code path the loader runs on every model switch. The OBJ
path calls it once with `firstIndex = 0` and is unaffected.

**Fourth, `map_d` is dropped by both OBJ paths, and the bundled sponza uses
it.** `Resources/Models/crytek-sponza/sponza_triag.mtl` has 24 materials: 23
`map_Kd`, 9 `map_Bump` (both now read) and **2 `map_d`** — the chain and
hanging-plant cut-outs. tinyobjloader parses that directive into
`material_t::alpha_texname` / `alpha_texopt`; `ObjLoader::loadMaterials`
never reads either, and `obj_to_gltf.rs` never emits anything for it. Since
every OBJ material also leaves `alphaCutoff` at the `-1` "not a MASK
material" sentinel, the cut-outs render as solid rectangles in all five
shading paths. This is the same shape as the `map_Ke`/`map_Bump` tasks that
shipped yesterday (`3143e92a`, `54a39af2`), with one extra decision baked in
below because OBJ has no `alphaMode` to read.

**Fifth, `docs/shader-sharing.md` calls seven SPIR-V-only modules "shared
math".** Its "Shared math lives in Slang modules under
`Resources/ShadersSlang/common/`" sentence names `material_fetch.slang`,
`material_rules.slang` and `cascaded_shadow.slang`, and the "What is wired
today" list adds `base_color.slang`, `emission.slang`, `normal_map.slang` and
`alpha_test.slang`. Walking the `import` graph, every entry point that
reaches any of those seven emits **spirv only**; the genuinely cross-target
modules are `aces`, `brdf`, `fullscreen` and `noise`, and `sky_model` is
shared between two *wgsl* shaders. The doc already carries a marker-block
table for entry-point targets with a gate behind it
(`shader-targets:begin`/`:end`), and `buildIntegritySuite.cpp` already has
the `import_closure` helper `f13797cb` added — the same two pieces answer
this question for `common/` modules.

**Verification context.** Host GPU goldens remain blocked over RDP (the
`- [b]` near the end of this file) and `path_tracing` mode device-losts on
the host RX 9070 XT on unmodified `develop` (the `- [b]` at line ~2030). No
task below may claim a rendered result. Tasks 1, 4 and 5 are source-text
gates plus (for 4) parse-time state in `commitTestSuite.exe`; task 2 is
device-free unit tests over the pure input helpers; task 3 is a CPU
benchmark plus a device-free unit test. **Say which suites you actually
ran.**

**Rust verification limits (tasks 1 and 4 touch the submodule).**
`cargo test`/`cargo build` do not link on this host: the VC++ Build Tools
install is incomplete and Git Bash's `link.exe` shadows MSVC's. Verify with
`cargo check`, `cargo clippy` and `cargo fmt --check` from
`third_party/OxidANT`, say so explicitly in the
commit message, and let the always-on Linux lane
(`scripts/linux/run-cargo-tests.sh`) run the tests you add. Regenerating
`forward.wgsl` is a `compile-slang-shaders.ps1` run, not a cargo build.

**Ordering.** Tasks 1, 4 and 5 all edit
`Test/commit/VulkanEngine/buildIntegritySuite.cpp` — land them one at a time,
in that order; each later one rebases trivially. Tasks 4 and 5 both edit
`docs/shader-sharing.md` (task 4 the shared-module prose and a new marker
block, task 5 the divergence matrix), so serialize those two as well. Tasks 2
and 3 are independent of everything.

### C++ Vulkan engine

### Rust WebGPU renderer (`third_party/OxidANT`)

### Docs

## 2026-08-05 batch VIII — planner (a 128³ noise volume whose generating kernel normalises its thread id by 256, so seven eighths of the domain is never generated and two of the four channels the cloud march reads are written as constants; a Henyey-Greenstein denominator whose sign puts the scattering peak away from the sun; a "powder effect" that raises transmittance as density rises; seven `endAndSubmitCommandBuffer` results discarded with `static_cast<void>`, one of which lets a failed BLAS build return success; and the one subsystem with a compute pair, a nine-control GUI panel and four UBO slots that no document owns)

The actionable queue was empty when this batch was written (0 `- [ ]`, 16
`- [b]` across the whole file). Batch VII's five tasks all shipped
(`e1d3fb8b`, `fafa67f2`, `0f22247c`, `159c1c74`, `bae7fc45`). Everything
below was read out of the tree at `bae7fc45`.

**First, `compute/noise.slang` generates one eighth of the volume it writes
into, and leaves half its channels constant.** `noise_main` opens with
`float3 uvw = float3(tid) / 256.0;`. The dispatch is
`kNoiseVolumeExtent / kNoiseWorkgroupSize` cubed = 16³ groups of 8³ threads,
so `tid` runs `[0, 128)` in each axis and `uvw` only ever reaches `0.5`. The
image is 128³ (`CloudDispatch.hpp`'s `kNoiseVolumeExtent`, which `a348bd9f`
introduced by lifting the literal `128` out of `Clouds::createTextures` and
`dispatchNoiseGeneration` — it never touched the shader's `256.0`). Two
consequences: `worley(uvw, 4.0)` is asked for four cells across the unit
domain and gets two, and the volume no longer tiles, because `sample_density`
addresses it with `abs(fmod(position + offset, 256.0)) / 256.0` — a wrap that
assumes the texture spans the whole `[0, 1)` domain.

The second half is the write itself:
`noiseVolume[tid] = float4(worleyVal, perlinVal, 0.0, 1.0);`. `sample_density`
reads all four components of both taps. `.b` is a constant `0.0`, so its
`max(0.0, noise.b - cloud.threshold)` term is identically zero for any
non-negative coverage threshold — one of the three weights in `baseDensity`
and one of the three in `fineDensity` are dead. `.a` is a constant `1.0`, and
it is the *cirrus* band:
`baseDensity * (1 - cirrus) + max(0, 1.0 - threshold) * scale * cirrus`. So the
"Cirrus effect" slider does not add cirrus — it linearly blends the
noise-driven density toward a value that is the same at every point in space,
i.e. it dissolves the clouds into a uniform slab. The default is 0.034, which
is why nobody has noticed.

`common/noise.slang` is worth reading before touching this: it carries
`snoise`/`fbm` under a header claiming "both renderers' compute/raster passes"
as consumers, and the one compute pass in the repo that *generates* noise does
not import it, hand-rolling `hash13`/`valueNoise`/`fbm_value` instead.

**Second, `phase_HG`'s denominator has the sign that points the scattering
peak away from the light.** It computes
`denom = 1.0 + g * g + 2.0 * g * cosTheta`, and the caller passes
`cosTheta = dot(rayDirection, normalize(-dirLight.direction))` — the cosine
between the view ray and the direction *toward* the sun. Henyey-Greenstein is
`(1 - g²) / (4π (1 + g² - 2g·cosθ)^{3/2})`; with a **plus**, the denominator is
largest at `cosθ = +1`, so the phase function is at its *minimum* when you look
straight at the sun and its maximum when the sun is behind you. With the
hard-coded `g = 0.5` that is the behaviour of `g = -0.5`: backscattering, where
clouds are strongly forward-scattering, and the silver lining renders on the
wrong side of the sky. Both readings of the convention agree — whether θ is
taken between the incident propagation direction and the outgoing one, or
between view ray and light vector, the sign in front of `2g·cosθ` is negative.

**Third, the powder effect is applied to the wrong quantity, in the wrong
direction.** Inside the march:

```
transmittance *= exp(-densityOfSample * dt);
if (transmittance < 0.01) break;
if (cloud.powder_effect) {
    float powderness = 1.0 - exp(-(densityOfSample * dt) / 2.0);
    transmittance = saturate(transmittance + powderness);
}
```

`powderness` grows with density, and it is *added* to transmittance — so the
denser the sample, the more transparent the volume becomes, undoing the
Beer-Lambert step on the line above. The powder/dark-edge term exists to
attenuate the *in-scattered* radiance at low optical depth (it is what makes
cloud edges read as dense rather than washed out); it belongs on the
`lightEnergy +=` accumulation, never on the transmittance, which must be
monotonically non-increasing along a ray. The checkbox defaults to **on**
(`GUISceneSharedVars`'s `cloud_powder_effect = true`), so this is the shipped
path, not a corner.

**Fourth, seven `endAndSubmitCommandBuffer` results are discarded with
`static_cast<void>`.** `CommandBufferManager::endAndSubmitCommandBuffer`
returns `bool` and is fully synchronous on return. The callers that learned to
read it are `Texture::uploadRgba` (`upload_submitted`) and
`ASManager::compactBLAS`'s copy step (`copy_submitted`), from the
"upload reports success it never verified" family the 2026-08-04 batches
shipped. Seven sites were never converted, and they are not equivalent:

- `ASManager::createBLAS`'s build submit is the severe one. `createBLAS`
  `return true`s unconditionally after it, so `createASForScene` proceeds to
  `compactBLAS` — which queries compacted sizes of structures that were never
  built — and then to `createTLAS`, which references them from the TLAS
  instance buffer. That is a device loss reported as a successful scene load.
- `ASManager::createTLAS`'s build submit, and `compactBLAS`'s
  compacted-size *query* submit. The latter already has the right wording on
  its two neighbouring error paths ("keeping uncompacted BLAS").
- `Clouds::createStorageTexture`'s layout-transition submit: on failure the
  image stays in `eUndefined` while the descriptor written later declares
  `eGeneral`. The `!commandBuffer` branch three lines above already calls
  `ASSERT_VULKAN` and states why.
- `Clouds::dispatchNoiseGeneration`'s submit: on failure the noise volume is
  undefined. The `graphicsFamilySupportsCompute()` guard above it already has
  the right sentence for that outcome.
- `VulkanRenderer`'s path-tracing accumulation-image transition: the
  `!commandBuffer` branch directly above already cleans up and returns.
- `VulkanImage::transitionImageLayout`'s standalone (own-command-buffer)
  overload, which returns `void` and therefore cannot report anything today.

**Fifth, no document owns the clouds subsystem.** `AGENTS.md`'s Docs table
gives every topic exactly one home, and path tracing — a comparable
single-mode subsystem — has `docs/path-tracing.md`. Clouds has two compute
kernels, a 128³ storage image, three dispatch constants in a shared header, a
four-`vec4` UBO block with a packing contract, a nine-control GUI panel, a
cross-frame WAR barrier pair in `VulkanRenderer::recordCommands`, a
queue-ownership rule with its own gate, and a compositing contract with
`post.slang` — and the only prose about any of it is scattered comments plus
whatever `docs/cpp-renderer-improvements.md` recorded at the time. The four
findings above were each reconstructed by re-reading the two shaders; the next
reader should not have to.

**Not in this batch, recorded so it is not lost:** `159c1c74` taught the C++
OBJ loader to read `map_d` into `ObjMaterial::alphaTextureID`, but the Rust
`obj_to_gltf.rs` still drops the directive entirely, so the bundled sponza's
chain and vase-plant cut-outs render solid through the Rust renderer and cut
out through the C++ one. It is **not** a mirror of the `map_Ke`/`map_Bump`
tasks: sponza's `map_d` names a *separate* image (`chain_texture_mask.jpg`,
`vase_plant_mask.jpg`), and glTF has no separate opacity texture — a faithful
conversion has to composite the mask's luminance into the base-colour image's
alpha channel and re-encode it. `image` is a **dev**-dependency of
`crates/webgpu_renderer`, built `default-features = false, features = ["png"]`,
so there is no JPEG decoder on the shipping path. Promoting an image codec into
the shipping dependency set (and the wasm bundle) is an owner decision, which
is why this is prose and not a task.

**Verification context.** Host GPU goldens remain blocked over RDP (the `- [b]`
near the end of this file) and `path_tracing` mode device-losts on the host
RX 9070 XT on unmodified `develop` (the `- [b]` at line ~2030). **No task below
may claim a rendered result.** Tasks 1, 2 and 4 are source-text gates (plus,
for 4, device-free unit tests over `SceneUboMarshal.hpp`) in
`commitTestSuite.exe`; task 3 is a source-text marker gate plus compile
coverage; task 5 is docs plus a gate. **Say which suites you actually ran**,
and say explicitly that the cloud appearance changes were not visually
confirmed.

**Ordering.** Tasks 1, 2, 4 and 5 all add tests to
`Test/commit/VulkanEngine/buildIntegritySuite.cpp` — land them one at a time,
in that order; each later one rebases trivially. Tasks 1 and 2 both edit
`Resources/ShadersSlang/compute/` (task 1 `noise.slang`, task 2 `clouds.slang`),
so serialize those two as well. **Task 5 must run last** — it documents the
contracts tasks 1, 2 and 4 establish, and writing it first guarantees a doc
that is wrong on the day it lands. Task 3 is independent of all of them: it
touches `ASManager.cpp`, `Clouds.cpp`, `VulkanRenderer.cpp` and
`VulkanImage.cpp`, none of which the other four edit. Task 3 changes no module
interface unless you take its step 7 option (b), which edits `VulkanImage.ixx`
and therefore needs `-FreshContainer`.

### C++ Vulkan engine

### Rust WebGPU renderer (`third_party/OxidANT`)

### Docs

## 2026-08-05 batch IX — planner (refactor: five shared Slang modules that each explain, in prose, why the ray-tracing entry points "cannot import `material_fetch`" — while two of the three of them do; a vertex stage that the forward and deferred rasterizers write out byte-identically, twice, whose two outputs a golden test asserts agree; and nineteen `[vk::binding(N, ...)]` literals for numbers `scene_types.slang` already names and a gate already pins against the host header)

The actionable queue was empty when this batch was written (0 `- [ ]`, 16
`- [b]` across the whole file). Batch VIII's five tasks all shipped
(`2802c163`, `9ae2679b`, `86371ffd`, `fb278765`, `45011e7d`). Everything below
was read out of the tree at `45011e7d`.

**First, the "RT/PT cannot import `material_fetch`" rationale is repeated in
five modules and is false in two of the three shaders it names.**
`common/material_fetch.slang` owns the `objectDescription` binding plus
`fetch_object_description()` / `fetch_material()`. Five sibling modules —
`common/base_color.slang`, `common/material_rules.slang`,
`common/normal_map.slang`, `common/emission.slang` and
`common/alpha_test.slang` — each carry a paragraph justifying their own
existence with the same claim: *"the ray tracing / path tracing entry points
already declare their own `objectDescription` binding and cannot also `import
material_fetch` (which declares the same binding again) without an
ambiguous-reference compile error."* Read the three shaders it names:

- `raytracing/raytrace.rchit.slang` **does** `import material_fetch` and calls
  `fetch_object_description(objIndex)`.
- `raytracing/raytrace.rahit.slang` **does** `import material_fetch` — next to
  `import alpha_test`, i.e. the exact combination the paragraph in
  `alpha_test.slang` says would be ambiguous.
- Only `path_tracing/path_tracing.slang` still declares
  `StructuredBuffer<ObjectDescription> objectDescription` itself.

So four fifths of the claim is stale, and the fifth (`alpha_test.slang`'s) is
true only of `path_tracing.slang`. Two concrete consequences follow, not just
a wrong comment:

- `rchit_main` already imports `material_fetch`, then declares `MaterialIDs*
  materialIDs` and `Materials* materials` purely to write
  `materials->m[materialIDs->i[PrimitiveIndex()]]` — which is
  `fetch_material()`'s entire body, character for character.
- `path_tracing_main` does the same thing twice (`objectDescription[...]` and
  the same two-pointer material lookup), and its reason not to import
  `material_fetch` does not survive inspection: `material_fetch` declares only
  `objectDescription`; the `textures`/`textureSamplers` arrays it needs come
  from `alpha_test.slang`, which declares no `objectDescription`. Dropping its
  own declaration and importing `material_fetch` should leave the descriptor
  set identical.

**Second, the forward and deferred rasterizers write the same vertex stage
twice.** `rasterizer/rasterizer.slang`'s `VsOut` / `vs_main` and
`deferred/deferred.slang`'s `GVsOut` / `geometry_vs_main` are byte-identical:
the same six interpolants with the same semantics, the same six assignment
lines (`svPosition`, `worldPosition`, `shadingNormal` via `transform_normal`,
`worldTangent`, `texCoords`, `fragmentColor`), and the same three-line
"tangent is a surface direction, not a normal" comment. This is not incidental
duplication — `goldenRenderSuite.cpp`'s forward/deferred parity oracle asserts
the two paths agree, so any drift between these two copies is a test failure
whose cause is two files that were supposed to be one. Both shaders already
import `common/push_constants.slang`, which established the pattern of a shared
module taking the push constant as a *parameter* (`transform_normal(pc,
normal)`) rather than declaring it.

**Third, `scene_types.slang` names the shared descriptor set's binding numbers
and almost nothing uses the names.** It declares `globalUBO_BINDING`,
`sceneUBO_BINDING`, `OBJECT_DESCRIPTION_BINDING`, `TEXTURES_BINDING`,
`SAMPLER_BINDING`, `SHADOW_MAP_BINDING`, `TLAS_BINDING`, `OUT_IMAGE_BINDING`
and `ACCUMULATION_IMAGE_BINDING`;
`buildIntegritySuite.cpp`'s `HostAndShaderSharedConstantsAgree` pins all nine
against `Src/GraphicsEngineVulkan/common/host_device_shared_vars.hpp`, and the
C++ side writes its descriptor layout with the names
(`VulkanRenderer::createSharedRenderDescriptorResources`). The shaders do not:
`common/alpha_test.slang` is the **only** file in the tree whose
`[vk::binding(...)]` attributes use them. Nineteen other declarations spell the
same numbers as bare integers, across `material_fetch.slang`,
`cascaded_shadow.slang`, `rasterizer.slang`, `deferred.slang`,
`shadow_map.slang`, `raytrace.rchit.slang`, `raytrace.rgen.slang` and
`path_tracing.slang`. The pin test therefore guards a rename that would move
the host and `scene_types.slang` in lockstep while every literal stayed put —
exactly the failure mode `NoShaderRedeclaresTheCascadeCount` was written to
close for `MAX_CASCADES`, and the same gate shape applies here.

**Verification context.** Host GPU goldens remain blocked over RDP (the `- [b]`
near the end of this file) and `path_tracing` mode device-losts on the host
RX 9070 XT on unmodified `develop` (the `- [b]` at line ~2030). **No task below
may claim a rendered result** — and none needs to. All three are pure source
refactors that must leave the emitted SPIR-V *byte-identical*, which is a
stronger and entirely GPU-free oracle than a golden image. Every task therefore
carries the same mandatory check:

```pwsh
# before touching anything
Copy-Item -Recurse Resources\ShadersSlang\build\spirv $env:TEMP\spirv-before
# ... make the change ...
pwsh -ExecutionPolicy Bypass -File .\scripts\windows\compile-slang-shaders.ps1
Compare-Object `
  (Get-ChildItem -Recurse $env:TEMP\spirv-before -File | Get-FileHash) `
  (Get-ChildItem -Recurse Resources\ShadersSlang\build\spirv -File | Get-FileHash) `
  -Property Hash, @{E={Split-Path $_.Path -Leaf}}
```
A non-empty `Compare-Object` result means the refactor changed behaviour — stop
and find out why rather than re-baselining. **Say which suites you actually
ran**, and say explicitly that no image was rendered.

**Ordering.** Land 1 → 2 → 3, one at a time. Task 1 deletes
`path_tracing.slang`'s own `objectDescription` declaration, which task 3 would
otherwise rewrite; task 3 rewrites `[vk::binding(...)]` lines in the two files
task 2 edits. Each later task rebases trivially over the earlier ones. All
three add a test to `Test/commit/VulkanEngine/buildIntegritySuite.cpp`, so
serialize for that reason too. No task changes a C++23 module interface, so
none needs `-FreshContainer`; none changes `Src/`'s compiled output at all
except task 1's optional step 6.

### C++ Vulkan engine

### Rust WebGPU renderer (`third_party/OxidANT`)

### Docs

**Not in this batch, recorded so it is not lost.** Two findings that are real
but do not belong to the three tasks above:

- **The four texture-slot sample blocks are written twice in the raster paths
  and twice more in the traced paths.** `rasterizer.slang`'s `fs_main` and
  `deferred.slang`'s `geometry_fs_main` contain byte-identical blocks for the
  alpha (`map_d`), normal, metallic-roughness and emissive slots — each a
  `if (material.XTextureID >= 0) { resolve_texture_slot(...); Sample(...); }`
  triple, including the four-line `map_d` comment reproduced verbatim.
  `raytrace.rchit.slang` and `path_tracing.slang` carry the same four blocks
  with `SampleLevel(..., 0.0)` instead of `Sample(...)`. A shared module cannot
  simply own them, because it would have to own the `textures[]` /
  `textureSamplers[]` arrays — which `rasterizer.slang`, `deferred.slang`,
  `shadow_map.slang`, `raytrace.rchit.slang` and `alpha_test.slang` each
  declare separately today (five copies of the same two bindings). Deciding
  whether one module owns those arrays for all five, or whether the sample
  helpers take the array as a parameter, is a design call worth its own batch,
  and it should land after task 3 has made the binding numbers symbolic
  everywhere.
- **`Src/shared/util/FileReader.ixx`'s `readTextFile` and `fileExists` have no
  production callers.** `Src/` reaches only `readBinaryFile` (`ShaderHelper.cpp`,
  `GltfLoader.cpp`) and `getBaseDir` (`ObjLoader.cpp`); the other two are
  driven only by `Test/commit/VulkanEngine/fileReaderSuite.cpp` and
  `Test/fuzz/shader_file_reader_fuzz_test.cpp`. This is **not** a deletion
  candidate — the fuzz target exercises all four together and `fileExists`'s
  comment records the `std::filesystem::exists` throw-on-permission-denied
  finding that motivated the `error_code` overload, which is exactly the kind
  of knowledge a dead-code sweep destroys. Recorded so the next sweep stops
  here instead of re-deriving it.

## 2026-08-05 batch X — planner (the path tracer's committed hit resolves every mesh past the first to mesh 0's vertex/index/material buffers — the out-of-bounds buffer-device-address read the tracked `VK_ERROR_DEVICE_LOST` has been hunted for since 2026-07-31, three of its four sibling call sites already get it right; the `instanceCustomIndex` the host stamps on every TLAS instance, which no shader reads; the four texture-slot sample blocks batch IX deferred, now that their stated prerequisite has landed; and the host half of the same object-index contract, still an inline accumulator with a stale comment and no test)

The actionable queue was empty when this batch was written (0 `- [ ]`, 16
`- [b]` across the whole file). Batch IX's three tasks all shipped
(`13f377b6`, `c9ba1be6`, `9ee460cb`). Everything below was read out of the tree
at `9ee460cb`, and the two correctness findings were additionally confirmed
against the **compiled** SPIR-V with `C:\VulkanSDK\1.4.350.0\Bin\spirv-dis`,
not just the Slang source.

**First, and this is the headline: `path_tracing.slang`'s committed hit drops
the geometry index, and that is an out-of-bounds buffer-device-address read on
any multi-mesh model.** The engine's object descriptions are one entry per
MESH, flattened across models (`VulkanRenderer.cpp`'s
`updateObjectDescriptions`, and `Kataglyphis::assignTextureOffsets`'s doc
comment). `ASManager::createTLAS` therefore stamps each TLAS instance's
`instanceCustomIndex` with that model's **first-mesh flat index** and expects
the traced shaders to add the geometry index (the mesh within the BLAS —
`createBLAS` pushes one `AccelerationStructureGeometryKHR` per mesh). Four call
sites compute that index. Three are right:

- `raytrace.rchit.slang`'s `rchit_main`: `InstanceIndex() + GeometryIndex()`
- `raytrace.rahit.slang`'s `rahit_main`: `InstanceIndex() + GeometryIndex()`
- `path_tracing.slang`'s two ray-query **candidate** loops (bounce and NEE
  shadow): `Candidate*InstanceIndex() + Candidate*GeometryIndex()`

The fourth — `path_tracing.slang`'s **committed** hit — is
`uint instanceIndex = rayQuery.CommittedInstanceIndex();` with no geometry
term, fifty lines below its own candidate loop that has one. Confirmed in the
emitted SPIR-V: `path_tracing.path_tracing_main.spv` contains
`OpRayQueryGetIntersectionGeometryIndexKHR` exactly **2** times against
`OpRayQueryGetIntersectionInstanceIdKHR` **3** times.

Consequence: for a hit on any mesh but the model's first, `obj` is mesh 0's
`ObjectDescription`, and `indices->i[primitiveID]` then indexes mesh 0's index
buffer with a primitive ID drawn from a *different, larger* mesh — a raw
`PhysicalStorageBuffer` read past the end of the allocation, with the fetched
`int3` then used to index `vertices->v[]`.

**This is almost certainly the root cause of the `- [b]` device-lost entry
near line 2030**, and every observation recorded there falls out of it:

- The reproducer scene is `Models/Dinosaurs/dinosaurs.obj`, which has **three**
  `o` shapes (`Plane`, `www_joel3d_com_Tricer`, `polySurface39_Tricera`) and
  `ObjLoader.cpp` makes **one Mesh per OBJ shape** — so it is a three-mesh
  model, and mesh 0 is the tiny `Plane`. Every hit on either dinosaur reads
  hundreds of thousands of elements past a two-triangle index buffer.
- "Consuming ONLY the material-fetched `hitColor` ... is safe; the instant the
  caller also consumes `v0.position`/`v0.normal`, the device is lost within
  0–2 frames" — exactly the dead-code-elimination boundary. Drop the vertex
  read and the whole `indices->i[primitiveID]` chain becomes unused and is
  eliminated; keep it and the OOB load is actually emitted.
- "`GoldenRender.RaytracedLargeMeshDoesNotLoseTheDevice` raytraces the same
  dinosaur mesh through the RT *pipeline* and does NOT lose the device ... the
  bug is confirmed scoped to `path_tracing.slang`/RayQuery compute" — because
  `rchit_main` adds `GeometryIndex()` and `path_tracing_main`'s committed hit
  does not. That entry's own closing advice was to look for "a compute-specific
  buffer-device-address indexing bug". This is it.
- The other two device-losing goldens
  (`GoldenRender.GuiInputSweepNeverCrashesOrLosesTheDevice`,
  `Integration.RenderModesSelectableInGui`) sweep render modes over the same
  default dinosaur scene, so they hit the same read.

I am **not** flipping that `- [b]` to `- [ ]`: confirming the device loss is
gone needs a host GPU run, which is still blocked (see below). Task 1 lands the
fix and its gate; whoever next gets a console session runs
`GoldenRender.PathTracingAccumulatesAndConverges` and closes the `- [b]` on
evidence.

**Second, no shader reads `instanceCustomIndex` at all.** Slang's HLSL-shaped
`InstanceIndex()` is the **TLAS instance index**, not the user-supplied custom
index — `spirv-dis raytrace.rchit.rchit_main.spv` shows
`OpDecorate %gl_InstanceID BuiltIn InstanceId` (SPIR-V `BuiltInInstanceId`, 6),
and `raytrace.rahit.rahit_main.spv` the same;
`BuiltInInstanceCustomIndexKHR` (5327) appears in neither. The custom-index
spelling is `InstanceID()` / `CandidateInstanceID()` / `CommittedInstanceID()`
(→ `OpRayQueryGetIntersectionInstanceCustomIndexKHR`, 6019, which appears
**0** times in `path_tracing.path_tracing_main.spv` today). So
`ASManager::createTLAS`'s `geometry_instance.instanceCustomIndex =
mesh_base_offset` is written and never read; the shaders substitute the TLAS
instance index, which equals the flat mesh base offset only while **every**
model holds exactly one mesh. `ASManager.cpp`'s comment still asserts that
equivalence ("== model_index while a Model holds one mesh, so this is a no-op
today") — false for the default scene since it became multi-mesh. Latent, not
firing today (the reproducer scene has one model, so both spellings give 0),
and it fires the moment a scene puts a multi-mesh model ahead of any other
model: model 1's descriptions get read at index 1 instead of 3.

**Third, the four texture-slot sample blocks, which batch IX deferred with a
stated prerequisite that has now landed.** Batch IX recorded this under "Not in
this batch" and said it "should land after task 3 has made the binding numbers
symbolic everywhere". Task 3 is `9ee460cb`. The finding stands verbatim:
`rasterizer.slang`'s `fs_main` and `deferred.slang`'s `geometry_fs_main` carry
byte-identical `if (material.XTextureID >= 0) { resolve_texture_slot(...);
Sample(...); }` triples for the alpha (`map_d`), normal, metallic-roughness and
emissive slots — including the four-line `map_d` comment reproduced verbatim —
and `raytrace.rchit.slang` / `path_tracing.slang` carry the same four with
`SampleLevel(..., 0.0)`. Underneath it, `[vk::binding(TEXTURES_BINDING, 0)]
Texture2D<float4> textures[MAX_TEXTURE_COUNT]` plus its sampler twin is
declared **five** times: `rasterizer.slang`, `deferred.slang`,
`shadow_map.slang`, `raytrace.rchit.slang`, `alpha_test.slang`.

**Fourth, `alpha_test.slang` still hand-rolls `fetch_material()`'s body, and
`material_fetch.slang` still carries the paragraph batch IX disproved.**
`ray_hit_masked_out()` opens with the `MaterialIDs*` / `Materials*` /
`materials->m[materialIDs->i[primitiveID]]` triple that *is*
`fetch_material(obj, primitiveID)`, character for character.
`material_fetch.slang`'s header comment still says the material rules live
elsewhere "so the ray tracing / path tracing entry points — which declare their
own objectDescription binding and cannot also import this module — can still
use them", while `rchit`, `rahit` and `path_tracing` all import it today.
`alpha_test.slang`'s own comment already records that the two shaders that
import it "import material_fetch and alpha_test together without conflict", so
the import is provably safe.

**Fifth, the host half of the object-index contract has no test.**
`ASManager::createTLAS` accumulates `mesh_base_offset` inline in the TLAS loop.
Its sibling contract half — `assignTextureOffsets`, which walks the same
per-model mesh counts to stamp `texture_offset` — was extracted into
`scene/ObjectDescription.ixx` and is unit-tested in
`Test/commit/VulkanEngine/objectDescriptionOffsetsSuite.cpp`, including the
overrun case. The base-offset accumulator was never given the same treatment,
so the number tasks 1 and 2 make the shaders actually read is produced by an
untested inline loop behind a stale comment. Same shape as
`BlasCompaction.hpp`'s `compactedSizesAreUsable` (device-free helper next to
its caller, tested in `blasGeometryLimitsSuite.cpp`).

**Verification context — read this before claiming anything.** Host GPU goldens
remain blocked over RDP (the `- [b]` near the end of this file) and
`path_tracing` mode device-losts on the host RX 9070 XT on unmodified
`develop`. **No task below may claim a rendered result.** The oracles available
without a GPU are strong and every task has one:

- Tasks 1, 2 and 5 change behaviour, so their oracle is the **emitted SPIR-V**,
  read with `spirv-dis` and pinned by a new gate. Opcode/BuiltIn numbers, taken
  from `C:\VulkanSDK\1.4.350.0\Include\spirv\unified1\spirv.hpp`:
  `OpRayQueryGetIntersectionInstanceCustomIndexKHR` 6019,
  `OpRayQueryGetIntersectionInstanceIdKHR` 6020,
  `OpRayQueryGetIntersectionGeometryIndexKHR` 6022,
  `BuiltInInstanceId` 6, `BuiltInInstanceCustomIndexKHR` 5327,
  `BuiltInRayGeometryIndexKHR` 5352.
- Tasks 3 and 4 are pure source refactors that must leave the emitted SPIR-V
  **byte-identical**:

```pwsh
# before touching anything
Copy-Item -Recurse Resources\ShadersSlang\build\spirv $env:TEMP\spirv-before
# ... make the change ...
pwsh -ExecutionPolicy Bypass -File .\scripts\windows\compile-slang-shaders.ps1
Compare-Object `
  (Get-ChildItem -Recurse $env:TEMP\spirv-before -File | Get-FileHash) `
  (Get-ChildItem -Recurse Resources\ShadersSlang\build\spirv -File | Get-FileHash) `
  -Property Hash, @{E={Split-Path $_.Path -Leaf}}
```

A non-empty `Compare-Object` result on task 3 or 4 means the refactor changed
behaviour — stop and find out why rather than re-baselining. **Say which suites
you actually ran**, and say explicitly that no image was rendered.

**Task 3's actual finding (relevant to task 4 too):** the SPIR-V was NOT
byte-identical after extracting the four texture-slot blocks into
`common/material_textures.slang`. Root cause, confirmed by disassembling
before/after with `spirv-dis` and diffing with SSA IDs normalized: identical
`OpCapability`/`OpExtension` lists, identical single-fully-inlined-`OpFunction`
shape (no `OpFunctionCall` either side), and identical opcode-count histograms
in 4 of 6 touched shaders (only `OpName` debug-info counts differed). The
other 2 shaders (`raytrace.rahit.rahit_main`, `path_tracing.path_tracing_main`)
each gained one extra `OpLoad` per call site of the newly-extracted
`alpha_test.slang` helper: before, two sibling `if` branches in the same
function shared one hoisted load of `obj.texture_offset`; after, each inlined
call to the shared helper re-loads it. Same address, immutable in between —
value-identical, not a behaviour change — but not byte-identical either, so
task 4 (which also moves code across a function boundary in `alpha_test.slang`)
should expect the same kind of diff and apply the same disassemble-and-diff
scrutiny rather than assuming a non-empty `Compare-Object` is a real
regression.

**Ordering.** Land 1 → 2 → 3 → 4 → 5, one at a time. Task 2 extends the gate
task 1 adds and edits the same four expressions; task 4 edits
`alpha_test.slang`, which task 3 also touches; tasks 1–4 all add to
`Test/commit/VulkanEngine/buildIntegritySuite.cpp`, so serialize for that
reason too. Only task 5 changes a C++23 module interface
(`scene/ObjectDescription.ixx`) and therefore needs `-FreshContainer`; tasks
1–4 change no `Src/` compiled output at all.

### C++ Vulkan engine

### Rust WebGPU renderer (`third_party/OxidANT`)

### Docs

**Not in this batch, recorded so it is not lost.**

- **`obj_to_gltf` drops `map_d` entirely, and `docs/shader-sharing.md` already
  calls it a "Real gap, not an intended divergence".** The C++ `ObjLoader`
  reads `map_d` into `ObjMaterial::alphaTextureID` and treats it as a `MASK`
  cut-out at `alphaCutoff = 0.5` (`159c1c74`), honoured in four shaders. The
  Rust converter parses only the scalar `d`/`Tr` directives and has no branch
  for a textured opacity map at all, so a `map_d` material converts with
  neither a cut-out nor per-texel alpha. Not sized here because the honest fix
  needs a decision the converter's current design does not support: glTF has no
  opacity-texture slot, so matching the C++ behaviour means compositing
  `map_d`'s red channel into `map_Kd`'s alpha and emitting a *new image file* —
  a converter that today only rewrites paths would gain an image decode/encode
  dependency. Worth its own batch with that trade-off stated up front; a
  warning-only stopgap would be cheap but leaves the renderers disagreeing.
- **`ObjMaterial::get_textureID()` has no production caller** — only
  `gltfParseSuite.cpp` and `objParseSuite.cpp` call it, on a struct whose
  `textureID` member is public and read directly everywhere in `Src/`. Too
  small to be worth a task on its own; fold it into the next dead-code sweep
  that touches `Src/shared/scene/`, and change the two call sites to the member.

## 2026-08-05 batch XI — planner (the path tracer's bounce ray is never renormalized, so `TMin`/`TMax` and the miss-gradient are all measured in units of a vector whose length runs from 0 to 2; a cloud noise lookup that `abs()`es its `fmod`, mirroring the whole volume about the origin instead of tiling it, on a box whose default extent is ±6300 world units either side of that plane; the guard-plus-default-plus-combine wrapper around three of the four texture slots, still written out eleven times across four shaders after batch X gave the *samples* one owner; no document anywhere saying which of the four C++ shading paths reads which `ObjMaterial` field, which is the exact gap batches XI–XVII kept re-discovering one field at a time; and a clouds subsystem whose own doc admits neither of its two shipped bug fixes has a numerical oracle)

The actionable queue was empty when this batch was written (0 `- [ ]`, 16
`- [b]` across the whole file). Batch X's four tasks all shipped
(`1df30964`, `f2cb4cb4`, `a94358e1`, `e5ad0863`, `b7f2ecc4`). Everything
below was read out of the tree at `b7f2ecc4`.

**Ordering matters.** Task 3 moves the emissive/metallic-roughness/normal
blocks behind named helpers; task 4's gate has to key on those helper names,
so do 3 before 4. Tasks 1, 2, 3 are shader-only (no C++ rebuild needed for
the shader itself — but every one of them lands a gate in
`buildIntegritySuite.cpp`, so all five still need a container build).

**Note on the `- [b]` entries.** None of them were flipped. In particular the
path-tracing device-lost entry near line 2030 and the SBT entry near line
3563 both need a live host GPU/console session to close, which this planner
cannot verify from a sandbox. Task 1 touches `path_tracing.slang` again; if
the executor *does* get a console session, running
`GoldenRender.PathTracing*` closes out the device-lost entry on evidence.

**Not in this batch, recorded so it is not lost.**

- **`clouds.slang` builds a `cloud.model_to_world` nobody reads, and its file
  header contradicts its own body.** `clouds_main` fills seven fields of
  `cloud.model_to_world` (the diagonal and the translation row) and then
  never passes it anywhere — `box_intersect` is only ever called with
  `cloud.inv_model_to_world`. Worse, the file header says "the inverse model
  matrix is precomputed on the CPU (Slang has no `inverse()` for SPIR-V)" and
  the comment above the inverse says "must be set on the CPU side … compute
  it here" — both false, and mutually contradictory: nothing on the C++ side
  ever writes either matrix, and the diagonal-plus-translate inverse is
  formed inline. ~10 dead lines and two wrong comments. Not sized now only
  because task 2 already touches this file and two shader edits in one
  session invite a merge conflict; pick it up next batch together with
  `PostStage::recordCommands`'s `clear_values` (its render pass declares
  `vk::AttachmentLoadOp::eLoad`, so the `{0.2, 0.65, 0.4, 1.0}` clear colour
  is never used) and `post.slang`'s doubled `noisyTxt.Sample(In.uv)`.

- **`obj_to_gltf`'s `map_d` gap is still open** — carried over from batch X's
  "not in this batch" list unchanged; it still needs the image
  decode/encode decision stated up front, and nothing in this batch moved it.

## 2026-08-05 batch XII — planner (refactor: a cloud model matrix that is filled in seven times and read zero, under a file header, a struct comment and an inline comment that all three describe a CPU-side inverse the C++ never writes; a post pass that hands `beginRenderPass` a clear colour its own `eLoad` attachment can never consume, next to a texture it samples twice for one texel; and the file:line citation gate — widened twice already, still stopping one directory short of `Test/`, where eighteen citations sit and five of them already point at unrelated code)

The actionable queue was empty when this batch was written (0 `- [ ]`, 16
`- [b]` across the whole file). Batch XI's five tasks all shipped
(`d2be7ae6`, `09812a21`, `53853c9a`, `bf3e7fbc`, `146fa517`). Everything below
was read out of the tree at `146fa517`.

Tasks 1 and 2 are the two items batch XI explicitly deferred to "next batch"
with the merge-conflict reason now gone (`09812a21` landed, so nothing else is
editing `clouds.slang`). Both were re-verified against the current tree rather
than carried over on trust. Task 3 is new.

**No ordering constraint** — the three touch disjoint files. Tasks 1 and 2
edit shaders that are `"targets": ["spirv"]` in `shader-manifest.json`, so
**no WGSL regeneration is needed** and the WGSL staleness gates cannot fire on
either; but both land gates in `buildIntegritySuite.cpp`, so all three need a
container build plus a direct `commitTestSuite.exe` run.

**Note on the `- [b]` entries.** None were flipped; none of the three tasks
below touches a blocker. The path-tracing device-lost entry near line 2030 and
the SBT entry near line 3563 still need a live host GPU/console session.

## 2026-08-05 batch XIII — planner (the one march-step bound whose ceiling exists on the host, in the GUI slider and in the shader's own comment — but not in the shader; a perf baseline that never grew the three rows for the function it was captured to protect, behind a comparator that reports an unmatched benchmark and exits 0; a "look mode" whose two predicates are named `should_capture_cursor`/`should_release_cursor` while the engine never calls `glfwSetInputMode` once, so a right-drag stops turning at the screen edge; the one `Scene` load entry point that neither resolves its path nor documents that it will not; and a Rust frame graph whose `PassDesc` doc promises a field it does not have)

The actionable queue was empty when this batch was written (0 `- [ ]`, 16
`- [b]` across the whole file). Batch XII's three tasks all shipped
(`5e365528`, `7a23ad06`, `8061e608`). Everything below was read out of the
tree at `732eee61`.

**Host GPU golden verification is still blocked over RDP** (see the `- [b]`
entry near the end of this file), so every task here is accepted on CPU
suites. That is not a compromise for four of the five: tasks 1, 2 and 4 land
or extend gates that run in `commitTestSuite.exe`, task 5 is `cargo test`.
Task 3 is the exception and says so in its own entry — the *pure* half is unit
tested, the `glfwSetInputMode` call itself is a one-line GLFW state change
with no headless observer.

**The headline is that `kMaxCloudMarchSteps` is a host-only clamp that three
places claim is a shader clamp too.** `CloudDispatch.hpp` declares four
march-step bounds. `SceneUboMarshal.hpp`'s `clampCloudMarchSteps` /
`clampCloudLightMarchSteps` apply all four before packing; `GUI.cpp`'s two
`SliderInt` ranges use all four. `clouds.slang` re-clamps — "a shader must not
trust a UBO" — but only three of the four literals are actually there:

| bound | host | GUI slider | `clouds_main` |
| --- | --- | --- | --- |
| `kMinCloudMarchSteps` (4) | yes | yes | yes (`max(scene.cloudParameters.w, 4.0)`) |
| `kMaxCloudMarchSteps` (128) | yes | yes | **no** |
| `kMinCloudLightMarchSteps` (1) | yes | yes | yes (`clamp(..., 1.0, 128.0)`) |
| `kMaxCloudLightMarchSteps` (128) | yes | yes | yes |

The comment sitting directly above that block says "The four literals here are
owned by `CloudDispatch.hpp`'s `kMinCloudMarchSteps`/`kMaxCloudMarchSteps`/
`kMinCloudLightMarchSteps`/`kMaxCloudLightMarchSteps` and pinned against them
by `BuildIntegrity.CloudMarchStepBoundsMatchTheShaderClamps`" — there are
three literals, and the gate checks three. `docs/clouds.md`'s constants table
says the opposite of the comment (`kMaxCloudMarchSteps` → "Host-only …
`clouds.slang` does not re-clamp this ceiling"), so the two documents already
disagree about the same line of code. The missing ceiling is also the more
expensive of the two: `num_march_steps` is the OUTER loop of a nested march
whose inner `light_march` is itself up to 128 steps, so an unclamped
`cloudParameters.w` is the one value in the UBO that can turn a compute
dispatch into a TDR. Task 1 makes the comment true rather than making the
doc's exception permanent.

**Second, `Test/perf/baselines/win-9070xt-32core.json` has 17 rows and
`perfSuite.cpp` registers 20.** The three missing ones are
`BM_ComputeTangents/1`, `/64` and `/512` — the benchmark added alongside the
per-vertex tangent work (`bb7092ae`) and then used as the acceptance evidence
for `0f22247c` ("scope `computeTangents`' accumulators to the touched vertex
range"), i.e. the one function in the suite with a *known* quadratic-in-
primitive-count history. The baseline's `context.date` is `2026-07-31`, before
either commit. `Compare-PerfBaseline.ps1`'s header states the behaviour that
makes this silent: "Benchmarks present in only one file are reported but never
fatal - the suite grows over time and that alone should not fail a
comparison." That is the right default for a *fresh* benchmark and the wrong
one for a benchmark that has been unmatched for five days; nothing anywhere
fails when the baseline stops covering the suite. Every other hand-maintained
list in this repo has a gate; this one does not.

**Third, "look mode" never captures anything.** `handle_mouse_button_callback`
enters look mode on a right-press through `should_capture_cursor` and leaves
it through `should_release_cursor`, and `WindowInputCallbacks.ixx` carries
three separate comments about re-seeding "after (re)capture". `grep -rn
"glfwSetInputMode\|GLFW_CURSOR" Src/` returns **nothing**: the OS cursor is
never hidden, never grabbed, and never confined. The consequence is not
cosmetic — with the cursor free, a right-drag that reaches a screen edge stops
producing motion events, so the camera stops turning mid-turn and the user has
to release, re-centre and drag again; on a multi-monitor desktop the drag
walks onto the other screen and every subsequent event is delivered to
whatever is there. The delta plumbing is already correct for the fix (it
accumulates raw deltas and re-seeds on every capture transition), so this is
the missing half of a feature the rest of the file assumes exists.

**Fourth, `Scene` has four load entry points and three path-resolution
policies.** `loadModel()` and `beginModelLoadAsync()` take no path and use
`sceneConfig::getModelFile()` (already resolved). `loadAdditionalModel()`
takes a `Resources/`-relative path and resolves it itself, with a comment
saying why ("callers pass a path relative to `Resources/`, and the working
directory differs between the app and the test executables").
`reloadModel()` takes the same-shaped parameter, resolves nothing, and is the
only one of the four with **no doc comment at all** in `Scene.ixx`. It works
today for exactly one reason: its single caller,
`VulkanRenderer::handleModelReloadRequest`, resolves first. A second caller
reading the header — or a test — passes what `getAvailableModelPaths()`
returns (relative to `Resources/`, e.g. `Models\Dinosaurs\dinosaurs.obj`) and
gets `reloadModel: '…' failed to load; scene is now empty.` after
`reloadModel` has already called `cleanUp()` and cleared `model_list`. The fix
is safe because `resolveResourceRelativePath` is idempotent on an
already-resolved absolute path: it builds `cwd + RELATIVE_RESOURCE_PATH /
relativePath`, and `operator/` with an absolute right-hand side replaces the
left, so the absolute path is tested for existence and returned unchanged.

**Fifth, `render/graph.rs` documents three properties and has one.**
`PassDesc`'s doc comment reads "A recorded pass: what it touches, and how to
encode it" — the struct is `name`/`reads`/`writes`, with no encode field or
callback. The module header's third selling point is "One place to add
cross-cutting behavior — timing scopes, debug markers, or a 'disable this
pass' toggle apply to every pass at once"; nothing in `src/` outside this file
references `forward_frame_graph` or `validate` (`grep` finds only the module
declaration in `render/mod.rs` and the file's own `#[cfg(test)] mod tests`).
What *does* work is `every_timed_pass_has_a_graph_row`, and it is one-way:
adding a `TimedPass` without a row fails, adding a row without timing passes
silently.

**Ordering.** Land 1 → 2 first and in that order: both add tests to
`Test/commit/VulkanEngine/buildIntegritySuite.cpp` and will otherwise conflict.
Tasks 3 and 4 each change a C++23 module interface
(`Src/shared/frontend/WindowInputCallbacks.ixx`,
`Src/GraphicsEngineVulkan/scene/Scene.ixx`) and therefore need
`-FreshContainer`; tasks 1 and 2 change no compiled `Src/` output at all.
Task 5 is Rust-only and independent of everything above. Task 1 edits a
shader that is `"targets": ["spirv"]` in `shader-manifest.json`, so **no WGSL
regeneration is needed** and the WGSL staleness gates cannot fire — but it
does need `compile-slang-shaders.ps1` before the SPIR-V staleness gate is
re-run.

### Rust WebGPU renderer (`third_party/OxidANT`)

**Not in this batch, recorded so it is not lost.**

- **`sceneConfig::scanAvailableModels()` latches on a partially-failed walk.**
  The `s_models_scanned = true` assignment is placed after the
  `exists(models_dir)` check specifically so that a call made before the
  working directory can see `Resources/Models` is free to retry — its comment
  says so. But it is placed *before* the `recursive_directory_iterator`
  constructor, which takes its own `std::error_code`; if that fails (or
  `it.increment(ec)` fails on the first entry) the empty list is cached for
  the rest of the process and the GUI shows "No loadable models" forever. Two
  lines to move. Not sized because inducing an iterator-construction failure
  in a test needs a filesystem fixture this suite has no pattern for; fold it
  into the next task that touches `SceneConfig.cpp` and can justify the
  harness.

- **`DeferredRasterizer::recordCommands` indexes `descriptorSets[0]` without
  the empty-span guard its sibling has.** `Clouds::recordComputeCommands`
  opens with an `if (descriptorSets.empty())` error-and-return;
  `DeferredRasterizer` and `Rasterizer` both dereference index 0 immediately.
  All callers pass a non-empty span today. Too small for its own task and the
  wrong shape for a gate; pick it up with the next change to either stage.

- **`obj_to_gltf`'s `map_d` gap is still open** — carried over unchanged from
  batches X, XI and XII. It still needs the image decode/encode decision
  stated up front, and nothing in this batch moved it.

## 2026-08-05 batch XIV — planner (two shadow tunables that exist in the struct, in the renderer and in a measured comment, and in no GUI control; a model scan that latches its empty result when the directory walk fails rather than when the directory is missing; the engine's own chronological change log, 44 commits stale and quoting a test count that is off by 573; seventeen read-only accessors that are the only non-`const` ones left in the tree; and the two flat-normal siblings of the one benchmark that had to be added by hand last cycle)

The actionable queue was empty when this batch was written (0 `- [ ]`, 16
`- [b]` across the whole file). Batch XIII's five tasks all shipped
(`f4e6d623`, `33f6e016`, `6c4191ab`, `d41eb037`, `1e2b1c50`). Everything
below was read out of the tree at `1e2b1c50`.

**Host GPU golden verification is still blocked over RDP** (see the `- [b]`
entry near the end of this file), so every task here is accepted on CPU
suites — and unusually, that costs nothing this cycle: none of the five
changes what a frame looks like. Tasks 1, 3, 4 and 5 land or extend gates in
`commitTestSuite.exe`; task 2 is a pure-function unit test. Task 1's two new
sliders are the only runtime-visible change, and they default to exactly the
values the renderer already passes.

**The headline is that `shadow_distance` and `cascade_split_lambda` are the
only two `GUISceneSharedVars` tunables with no control anywhere in
`GUI.cpp`.** Both are read every frame by `VulkanRenderer::updateUniformBuffers`
and handed to `CascadedShadowMap::updateCascades`; both carry the longest
comments in the struct, and both comments read as instructions to a user who
can change them:

| field | struct comment says | GUI control |
| --- | --- | --- |
| `pcf_radius` | — | `SliderInt("PCF radius", …)` |
| `cascaded_shadow_intensity` | — | `SliderFloat("Shadow intensity", …)` |
| `num_shadow_cascades` | — | `SliderInt("# cascades", …)` |
| `shadow_map_res_index` | — | `Combo("Shadow Map Resolution", …)` |
| `shadow_distance` | "How far shadows are fitted, independent of the camera far plane … Geometry past this is unshadowed, which is the intended trade." | **none** |
| `cascade_split_lambda` | "Defaults OFF because measurement did not support turning it on for the scenes we ship … **Raise it only for a camera that sits close to its subject, where it does help** (1.52 -> 1.01 cm/texel at 0.35). See the table in `CascadedShadowMap.cpp`." | **none** |

"Raise it only for…" is advice that currently requires an edit-and-rebuild.
Both are also safe to expose today without new clamping: `computeCascadeDataInto`
already clamps `splitLambda` into `[0,1]` itself and already falls back to
`farPlane` when `shadowDistance <= 0`. The same reading turns up one default
drift: `CascadedShadowMapParams::splitLambda` and `updateCascades`' trailing
parameter both default to `0.5F` while the GUI default — the one every frame
actually uses, and the one `guiSceneVarsRoundTripSuite.cpp` pins with a
measured rationale — is `0.0F`. That is the same "startup path reads its
default from the wrong side of the boundary" shape batch VIII already fixed
once for the cascade count.

**Second, `sceneConfig::scanAvailableModels()` latches on the wrong event.**
Its `s_models_scanned = true` was deliberately moved *after* the
`exists(models_dir)` check — its comment says why: "a call made before the
working directory finds `Resources/Models` must be free to retry". But it
sits *before* the `recursive_directory_iterator` constructor, which takes its
own `std::error_code`. If that constructor fails, the loop body never runs,
the two caches stay empty, and the empty result is cached for the life of the
process: the GUI shows "No loadable models (.obj/.gltf/.glb) found in
Resources/Models/" forever, and `defaultSelectedModelIndex` returns `-1`,
which is the value `handleModelTransformChange` gates the Position/Rotation
controls on. This was recorded in batch XIII's not-in-batch list as unsizable
because "inducing an iterator-construction failure in a test needs a
filesystem fixture this suite has no pattern for". That is no longer true and
is the reason it is sized now: **constructing a `recursive_directory_iterator`
over an existing regular file sets the error code**, so `AGENTS.md` — a file
guaranteed to exist at the repo root, already located by `repoRoot()` — is the
whole fixture.

**Third, `docs/cpp-renderer-improvements.md` is 44 commits stale.**
`AGENTS.md`'s routing table sends every agent refactoring the renderer to it
("log the change in `docs/cpp-renderer-improvements.md`") and its Docs table
names it the owner of the "C++ engine chronological change log". Its last row
landed at `fc637887`; since then 44 commits have touched `Src/` or
`Resources/ShadersSlang/` with no entry — including the glTF material
completion (normal/metallic-roughness/emissive textures, per-slot
`KHR_texture_transform`, `KHR_materials_unlit`, `Pm`/`Pr`, `map_Bump`/`norm`,
`map_Ke`, `map_d`), the shared-Slang-module consolidation, four ray/path
tracing correctness fixes, six clouds fixes, the TBN multiply-order fix, and
both input fixes. The header is worse than absent: it states "72 tests at
campaign start, 103 as of 2026-07-23" against a suite that today registers
676 `TEST`/`TEST_F`/`TEST_P` cases across 58 files. Every other
hand-maintained number in this repo has a gate; this one is quoted in the
first paragraph of the most-linked doc in the tree and has none.

**Fourth, seventeen accessors are the last non-`const` read-only members in
`Src/`.** `VulkanDevice` marks all sixteen of its query accessors `const`;
`Scene`, `Model`, `Mesh`, `Window`, `DeferredRasterizer`, `SkyBox`, `Clouds`
and `CascadedShadowMap` mark none of theirs. The blocker people assume — that
`Scene::findModel`/`findMesh` return mutable pointers — is not real:
`model_list` is `std::vector<std::shared_ptr<Model>>`, and `.get()` on a
`const shared_ptr<Model>&` yields a plain `Model*`, so constness does not
propagate through. The same is true of the four `unique_ptr`-backed
`get*Texture()`/`getMesh()` accessors.

**Fifth, `computeFlatNormals` and `fillMissingFlatNormals` have no
benchmark** while their sibling `computeTangents` does — and
`BM_ComputeTangents` only exists because the quadratic-accumulator fix
(`0f22247c`) needed acceptance evidence, i.e. it was added *after* the bug.
All three walk the same `indices[firstIndex..]` shape and all three are
called once per primitive by both loaders. `transformAABB` has none either,
and it is the one function here that runs per mesh per pass per frame (the
camera walk and the shadow walk each call it for every mesh in the scene).
Task 5 is measurement, not optimisation: the numbers are the prerequisite for
deciding whether the per-frame `transformAABB` deserves caching at all.

**Ordering.** Land 1 → 3 → 4 → 5 in that order: all four add tests to
`Test/commit/VulkanEngine/buildIntegritySuite.cpp` and will otherwise
conflict. Task 2 touches only `SceneConfig.{ixx,cpp}` and
`cameraSceneConfigSuite.cpp` and is independent of the other four. Tasks 1, 2
and 4 change C++23 module interfaces and therefore need `-FreshContainer`;
tasks 3 and 5 change no `Src/` output at all. No task in this batch touches a
`.slang` file, so no shader recompile and no WGSL regeneration is needed and
the staleness gates cannot fire.

### Not in this batch, recorded so it is not lost

- **`Camera::set_fov` / `set_near_plane` / `set_far_plane` have no production
  callers** — only `cameraSceneConfigSuite.cpp` calls them. That also means
  `PathTracingHistoryKey`'s omission of the projection matrix is currently
  harmless: the only runtime path that changes the projection is a resize,
  and `createPathTracingAccumulationResources` already resets the frame
  counter there. If a FOV control is ever added to the GUI, the key must gain
  the projection in the same change or the running mean will blend samples
  from two different fields of view. Not sized because the right fix depends
  on whether the setters are wanted at all — `Mesh`'s unread model-matrix
  setter was deleted for the same reason (`fc637887`), but a camera's FOV is a
  more defensible piece of API to keep unused.

- **`sample_alpha`/`sample_alpha_lod0` transform their UV with the
  *base-colour* slot's `KHR_texture_transform`** while the normal,
  metallic-roughness and emissive slots each got their own named accessor in
  `base_color.slang`. It is a no-op today by construction — `alphaTextureID`
  is set only by the OBJ loader (`map_d`) and OBJ materials always carry
  identity transform rows — so this is a latent inconsistency, not a bug. Fold
  it into the next task that touches `material_textures.slang` and can justify
  either an `alpha_uv()` accessor or a comment saying why the base-colour pair
  is the right one for `map_d`.

- **`DeferredRasterizer::recordCommands` still indexes `descriptorSets[0]`
  without the empty-span guard its sibling has** — carried over unchanged from
  batch XIII. `Clouds::recordComputeCommands` opens with an
  `if (descriptorSets.empty())` error-and-return; `DeferredRasterizer` and
  `Rasterizer` both dereference index 0 immediately. Still too small for its
  own task and still the wrong shape for a gate; pick it up with the next
  change to either stage.

## 2026-08-05 batch XV — planner (refactor: the four G-buffer channel indices, spelled as bare `0..3` in the shader, in two host loops and in a render-pass attachment table, under a gate comment that writes them off as "a permanent, unfixable violation"; the five `KHR_texture_transform` row pairs in `forward.slang`, hand-paired at eleven call sites in the one renderer the existing row-pairing gate's regex cannot see; and 102 verbatim copies of the same five-line violation-joining lambda in a gate suite whose own support header says it is "the one place those helpers live now")

The actionable queue was empty when this batch was written (0 `- [ ]`, 16
`- [b]` across the whole file). Batch XIV's five tasks all shipped
(`c1881290`, `6eb9e454`, `bc0745ac`, `1ee42ee9`, `59bbff3a`). Every
`file:line` below was read out of the tree at `59bbff3a`.

**Host GPU golden verification is still blocked over RDP** (see the `- [b]`
entry near the end of this file), so all three tasks here are accepted on CPU
suites alone — and none of them changes what a frame looks like. Task 1
renumbers nothing (every constant it introduces equals the literal it
replaces), task 2 factors five affine transforms into five named functions
without touching the arithmetic, and task 3 only edits test-failure message
formatting.

**Two of the three close a duplication the codebase has already fixed once,
on the other side of a boundary.** Task 1 is the same "name the binding, do
not spell the integer" change batch IX shipped for set 0 (`9ee460cb`), now for
set 1 — the difference is that the gate written in that commit explicitly
declares set 1 out of scope forever. Task 2 is the same per-slot UV-accessor
change batch IV shipped for the C++ shaders (`base_color.slang`'s
`normal_uv()` / `metallic_roughness_uv()` / `emissive_uv()`), now for the
WGSL-emitting `forward.slang` — the renderers swapped, exactly as in batch V.

**A scan for dead C++ API found essentially nothing this cycle**, which is
worth recording so the next planner does not repeat it: across all 281
function declarations in `Src/**/*.{ixx,hpp}`, the only identifier with no
reference other than its own definition is
`GpuTimingSubsystem::timestampMask()` (`GpuTimingSubsystem.ixx:238`) — already
noted as rejected in the batch-XX candidate list, and still one accessor,
still too thin for a task. The Rust-side pair that list named
(`ForwardRenderer::animation_duration`, `disable_gpu_timing`) no longer exists
in `crates/webgpu_renderer` at all. `MeshDrawRecorder.ixx` already owns the
bind-VB/bind-IB/`drawIndexed` sequence for every raster path, and all seven
`vk::WriteDescriptorSet` sites are still inside `DescriptorSetGroup.cpp`.
Do not re-run these three sweeps without new evidence.

### C++ Vulkan engine

### Rust WebGPU renderer (`third_party/OxidANT`)

### Test and gate infrastructure

## 2026-08-05 batch XVI — planner (the field of view: three setters with no production caller, no GUI control, and a path-tracing history key that does not include the projection the kernel builds every primary ray from; a GUI-coverage gate whose "every tunable has a control" list is 23 hand-typed strings, so the one failure mode it exists for — a NEW field with no control — still slips through silently; the eight-corner AABB transform, now that last cycle's benchmark answered the caching question it was captured to answer; a per-vertex tangent pass that runs Gram-Schmidt, a normalize and a cross once per INCIDENT TRIANGLE instead of once per vertex; and a "KEY Bindings" panel that lists six keys out of eight bindings and has never mentioned the right mouse button)

The actionable queue was empty when this batch was written (0 `- [ ]`, 16
`- [b]` across the whole file). Batch XV's three tasks all shipped
(`4b1445fe`, `ccd9c1ad`, `dd375e1d`). Every `file:line` below was read out of
the tree at `dd375e1d`.

**Host GPU golden verification is still blocked over RDP** (see the `- [b]`
entry near the end of this file), so every task here is accepted on CPU
suites alone and all five were chosen to be checkable that way. Task 1 is the
only one that can change a rendered frame at all, and only when the user
moves a slider that does not exist today; tasks 3 and 4 are output-identical
by construction (the equivalence is the acceptance test); tasks 2 and 5 touch
a gate, a panel string and one dead comment line. **No task in this batch
touches a `.slang` file**, so no shader recompile, no WGSL regeneration, and
the staleness gates cannot fire.

**Two of these close the same gap from opposite ends.** Batch XV's
"not in this batch" list recorded that `Camera::set_fov`/`set_near_plane`/
`set_far_plane` have no production caller, and that `PathTracingHistoryKey`'s
omission of the projection matrix "is currently harmless" *only* because
nothing can change the FOV at runtime — "if a FOV control is ever added to
the GUI, the key must gain the projection in the same change". Task 1 is that
change, both halves together, and it is sized rather than left as an owner
decision because the precedent is settled: batch XIV shipped GUI sliders for
`shadow_distance`/`cascade_split_lambda` on exactly the "documented tunable
that needs a rebuild to change" argument (`c1881290`). Task 2 then removes
the reason this keeps being a planner finding rather than a test failure —
`EveryTunableGuiSceneVarHasAControl` (`buildIntegritySuite.cpp:11379`) checks
a hand-typed list of 23 member names against `GUI.cpp`, so it catches a
control being *deleted* and cannot catch a member being *added*, which is the
exact direction both prior misses travelled.

**Tasks 3 and 4 are the follow-through on last cycle's measurement**, and
task 3 answers its open question rather than leaving it open: batch XIV added
`BM_TransformAABB` because "the numbers are the prerequisite for deciding
whether the per-frame `transformAABB` deserves caching at all". The numbers
are in (`Test/perf/baselines/win-9070xt-32core.json`: 7500.54 ns for 512
boxes, i.e. ~14.6 ns per box, ~11 µs/frame at sponza's 373 meshes × 2
passes). **Decision: it does not deserve caching** — a cache would need
invalidation on every model-matrix write for 0.07 % of a 16 ms frame. What it
does deserve is the standard center/extent form, which is the same answer in
a third of the arithmetic. The same baseline shows `BM_ComputeTangents/512`
at 45961.34 ns against `BM_ComputeFlatNormals/512` at 3210.57 ns — a 14×
ratio between two functions that walk the same `indices[firstIndex..]` shape;
task 4 is where most of that ratio comes from.

### C++ Vulkan engine

### Test and gate infrastructure

- [ ] **(S) (refactor) Derive `EveryTunableGuiSceneVarHasAControl`'s member list from the header instead of hand-typing it, and cover `GUIRendererSharedVars` too** — the gate as written catches a control being deleted and cannot catch the thing it was created for: a new field with no control.

  **Files to read:**
  - `Test/commit/VulkanEngine/buildIntegritySuite.cpp:11372-11412` — the gate,
    its `std::array<const char *, 23> kTunables` and the exemption comment
    above it. Its own header comment says the list "must be kept in sync with
    `GUISceneSharedVars.ixx`" — which is the hand-maintained claim this repo
    gates everywhere else.
  - `Src/GraphicsEngineVulkan/scene/GUISceneSharedVars.ixx:20-92` — the struct
    to parse: plain `type name = value;` members, plus the
    `available_shadow_map_resolutions` table and the `*_changed` /
    `*_requested` latches that must stay exempt.
  - `Src/GraphicsEngineVulkan/renderer/GUIRendererSharedVars.ixx:74-110` —
    `GUIRendererSharedVars`, which has no coverage gate at all. All nine
    members are referenced in `GUI.cpp` today, so the new check passes on
    arrival and exists to keep it that way.
  - `Test/commit/VulkanEngine/RepoFiles.hpp` — `readFileText`/`readFileLines`
    and `joinViolations`; use them rather than hand-rolling
    (`dd375e1d` removed 102 hand-rolled copies of the join).
  - `Test/commit/VulkanEngine/buildIntegritySuite.cpp:11048` —
    `ShaderSharingDocCoversEveryObjMaterialFieldPerShadingPath`, an existing
    "parse the struct, check every member" gate to copy the parsing shape from.

  **Steps:**
  1. Add a file-local helper that, given a header path and a struct name,
     returns the declared member names between that struct's `{` and its
     matching `}` — match `^\s*[\w:<>,\s\*&]+?\s+(\w+)\s*(=|;|\{)` per line,
     skipping lines that start with `//`, `static_assert`, or a nested
     `struct`/function. Assert the parse found a plausible count (`>= 20` for
     `GUISceneSharedVars`) so a regex that silently matches nothing cannot
     pass the gate.
  2. Replace `kTunables` with that call plus an explicit
     `kExempt` list carrying the existing rationale verbatim: the
     `*_changed`/`*_requested` latches, `selected_model_index`, and
     `available_shadow_map_resolutions`. A member that is neither found in
     `GUI.cpp` nor in `kExempt` fails with a message telling the author to add
     a control **or** an exemption with a reason.
  3. Add the twin check for `GUIRendererSharedVars` against
     `guiRendererSharedVars.` in `GUI.cpp`, exempting nothing initially
     (`gpuTimings` and `visibility` are read by the panels at `GUI.cpp:248-268`).
     Either a second `TEST` or a table-driven loop over both (struct, prefix)
     pairs — one gate per struct reads better in failure output.
  4. While in `GUI.cpp`, delete the dead commented-out
     `// ImGui::Checkbox("Ray tracing", &guiRendererSharedVars.raytracing);`
     at `:166` — superseded by the radio-button block at `:127-159`, which is
     the single source of truth for the three render modes.

  **Test:** the gate is the test. Verify both directions by hand before
  committing: add a throwaway `float zzz_probe = 0.0F;` to each struct and
  confirm each gate fails naming it, then remove.

  **Build:** `clangcl-debug` (no `Src/` output changes if step 4 is the only
  source edit, but `GUI.cpp` is compiled either way). Run:
  `pwsh -ExecutionPolicy Bypass -File .\scripts\windows\Build-Windows-Container.ps1 -Configurations clangcl-debug`
  then `.\build-clangcl-debug\commitTestSuite.exe --gtest_filter=BuildIntegrity.*`.

  **Context:** Land **after** task 1 (both edit `GUI.cpp` and this file, and
  task 1's `camera_fov` is the first member the derived gate will check).
  This is the same lesson as `PerfBaselineCoversEveryRegisteredBenchmark` and
  `ImprovementLogQuotesTheCurrentCommitSuiteTestCount`: a gate that reads a
  hand-maintained list is a gate on the list, not on the thing.

### Not in this batch, recorded so it is not lost

- **`Camera::set_near_plane`/`set_far_plane` still have no production caller**
  after task 1. Deliberate: their defaults are configuration-dependent (150
  in debug, 4000 in release — `Camera.cpp:51-75`), so a single GUI default
  would contradict one build, and `far_plane` interacts with the cascade fit
  (`shadowFar = min(shadowDistance, farPlane)`). Exposing them needs a
  decision about which default the slider shows, which is why task 1 stops at
  FOV.

- **`qem.rs::into_primitive` drops `morph_targets`/`morph_weights` with no
  comment**, while its clustering sibling `lod.rs::simplify_primitive:182-185`
  explains the same drop in place ("simplification changes the vertex count,
  so per-vertex morph deltas can't be carried"). Harmless today —
  `forward.rs:1383` excludes morphed primitives from LOD entirely — but the
  two files disagree about how much of that reasoning is written down. Fold
  into the next task that touches `qem.rs`; too thin for its own entry.

- **`DeferredRasterizer::recordCommands` still indexes `descriptorSets[0]`
  without the empty-span guard its sibling has** — carried unchanged from
  batches XIII and XV. Still too small for its own task and still the wrong
  shape for a gate; pick it up with the next change to either raster stage.

- **A scan of the Rust crate's `scene/qem.rs` and `scene/lod.rs` found
  nothing actionable** — worth recording because neither file had ever been
  mentioned in this backlog (0 occurrences before this batch). Both carry
  dense in-file test suites (`qem.rs:673-815`, `lod.rs:284-630`) plus
  integration suites (`tests/qem.rs`, `tests/lod_pipeline.rs`) covering
  order-independence, medoid attribute selection, fold rejection, degenerate
  input and the ratio conventions. `Simplifier::Quadric` and
  `simplify_primitive_qem` both have production callers (`forward.rs:1384-1388`).
  Do not re-run this sweep without new evidence.

