# BACKLOG

The single list of open work across the whole project — sized commitments and
unsized ideas together. Detailed per-area status lives in `docs/`
(`cpp-renderer-improvements.md`, `shader-sharing.md`) and, for the Rust WebGPU
renderer, in OxidANT's `third_party/OxidANT/crates/webgpu_renderer/docs/webgpu-renderer-roadmap.md`;
this file is what is still to do. Finished work leaves this file; its history is in git.

Sizes: S (< half a day), M (a day-ish), L (multi-day), XL (multi-week).
Checkbox items are sized and agreed; the prose sections below the fold are
candidates that have not been sized yet. A candidate graduates by acquiring a
size and a decision, or gets dropped.

Checkbox states: `- [ ]` actionable (the agentic loop's executor picks these
up), `- [b]` blocked/parked (waiting on a prerequisite or an owner decision —
the loop skips these and they do not count toward its pending-task queue),
`- [x]` done (pruned automatically; history lives in git).

**GPU verification is no longer blocked on the host.** Several entries used to wait for a
console session on the RX 9070 XT. Since 2026-09-29 the Linux lane runs `GoldenRender.*` and
`Integration.*` on llvmpipe, with sync validation since 2026-10-06, and Windows x64 runs its
Release suite on lavapipe (`docs/gpu-golden-testing.md`). Run 37486873224 passed every test
those entries named: the path-tracing, ray-tracing and cloud goldens and
`Integration.RenderModesSelectableInGui`. Only a fault that is specific to the RX 9070 XT
still needs the host.

## Sized

### C++ Vulkan engine

- [b] **Renderer-level RAII cleanup consolidation** (M, **blocked on being testable**) —
  the stage-level work landed 2026-07-19. `VulkanRenderer`'s hand-ordered `cleanUp()` and
  the device-lost special case in `App.cpp` are left: `App.cpp` skips `scene->cleanUp()` and
  `gui->cleanUp()` when the device is lost. Device loss cannot be induced here, so removing
  that guard would be an untestable change to the one path that only runs after something
  has gone wrong. First add a way to simulate device loss (a device-simulation layer, or
  fault injection behind a debug flag).
  Related, and owned by this entry: `Scene`'s destructor undoes that guard. `scene` is
  declared before `vulkan_renderer` (`App.cpp`), so `~Scene()` runs its `cleanUp()` after
  `VulkanRenderer::cleanUp()` has destroyed the VMA allocator and the logical device. It is
  benign only because the leaf `cleanUp()`s are idempotent.
- [b] **Confirm on the RX 9070 XT that the path-tracing device loss is gone** (S, needs the
  host GPU). The cause was found 2026-08-05: `path_tracing.slang`'s committed hit dropped the
  geometry index, so a hit on any mesh but a model's first read past mesh 0's index buffer.
  The kernel now adds `CommittedGeometryIndex()`, and the three tests that lost the device
  pass on llvmpipe in CI. Run
  `commitTestSuite.exe --gtest_filter='GoldenRender.PathTracing*:GoldenRender.GuiInputSweepNeverCrashesOrLosesTheDevice:Integration.RenderModesSelectableInGui'`
  on the host. Once it passes, retire the "Known issue" section of
  `docs/gpu-golden-testing.md`, together with the `excluded=3` count and the
  `--gtest_filter` line that `buildIntegritySuite.cpp` pins in it.
- [ ] **(S) Collapse the two cloud-output image barriers and the rationale comment written
  twice** (`VulkanRenderer.cpp`, `cloud_output_war_barrier` and `cloud_output_barrier`).
  The two `eGeneral -> eGeneral` colour barriers differ only in stage and access masks. Add
  a file-local helper that takes the image plus those masks, and call it twice: once
  `eFragmentShader -> eComputeShader`, `{} -> eShaderWrite` before `recordComputeCommands`,
  and once `eComputeShader -> eFragmentShader`, `eShaderWrite -> eShaderRead` after it. Do
  not change any mask; the goldens must not move. Keep the cross-frame WAR rationale once.
  Keep the 2026-08-01 sync-validation measurement and the reason this barrier is hand-written
  rather than `VulkanImage::transitionImageLayout`'s overload, which would cost
  `eAllCommands -> eAllCommands`.
  Test: `GoldenRender.CloudsAcrossManyFramesDoesNotLoseTheDevice` stays green, and the Linux
  lane's sync-validated run reports no `SYNC-HAZARD`.
- **glTF loader gaps** (M/L): MASK alpha and `KHR_texture_transform` are done. What is left:
  - *BLEND and sorting* (L): a sorted transparent pass through `PipelineBuilder`'s existing
    src-alpha blend state, with back-to-front ordering and a second draw list. Until then a
    transparent glTF renders opaque. Defer until a real transparent asset needs it.
  - *texcoord index* (M/L): the engine `Vertex` carries a single `texture_coords`, so a second
    UV set means a `TEXCOORD_1` attribute in the vertex layout, every mesh vertex buffer,
    every shader that reads UVs and both loaders. Defer until a real dual-UV asset needs it.
  - Adding a field to `ObjMaterial` crosses the C++/Slang module boundary. Cold-build after
    it (delete the build tree and use `-FreshContainer`).
- **Dynamic rendering + synchronization2 are hard-disabled** (L, `VulkanDevice.cpp`).
  Dynamic rendering would delete the framebuffer rebuild in `recreateSwapChain`, where the
  image-count edge cases live. Test: run the whole golden suite under both paths behind a
  toggle.
- **Incremental BLAS append** (M, an optimisation). `addModel` and `reloadModel` call
  `createASForScene`, which rebuilds every model's BLAS to add one. `createBLAS` is a batched
  build (one scratch buffer and one compaction pass across all models), so an
  `ASManager::appendBlas(model)` means extracting a single-model build path with its own
  scratch and compaction, then `createTLAS`. Guard it with the RT goldens and
  `GoldenRender.AddedModelAppearsInPathTracing`.

### Rust WebGPU renderer (`third_party/OxidANT`)

- [b] **Basis ETC1S/UASTC transcoding** (M, **blocked on a transcoder and a test asset**).
  KTX2 BCn passthrough is done, and supercompressed files are rejected with a clear error
  (`ktx2_loader.rs`). The viable transcoder is the `basis-universal` crate, a C++ binding,
  and no Basis-compressed KTX2 test asset is in the repo. Do it as a deliberate cycle:
  vendor `basis-universal`, generate an ETC1S and a UASTC `.ktx2` with `toktx`/`basisu`,
  then transcode to a BCn `CompressedFormat` on desktop and to ETC2/ASTC on the web path.
- [b] **Indirect draws** (M, blocked on GPU occlusion culling producing draw arguments).
  With CPU-side instance counts, indirect draws add a buffer round trip for nothing.
- [b] **WebXR** (XL) — parked.
- [b] **Colosseum demo scene** (blocked on the owner): pick a licensed photogrammetry scan
  and keep the asset out of git. LOD is ready (set `lod_enabled` before `upload_scene`).
  KTX2 is not, because Basis transcoding is missing (above).
- [b] **Stop an empty tile from iterating every light** (blocked on a conservative sphere
  bound). `forward.slang`'s `punctual_lighting` falls back to all lights when a tile's
  binned count is 0, so empty screen costs up to 256 iterations per fragment. The fallback is
  load-bearing: `tile_rect_for_light`'s 7-point AABB under-covers a sphere's true screen
  silhouette by up to `D / sqrt(D² - r²)`, even with every point in front of the camera.
  Near-plane straddling is already handled. Next step: a projection-aware sphere screen
  bound (Mara & McGuire 2013), then a completeness test with unconstrained random ranges,
  and only then remove the fallback.

### CI and release

- [ ] **Every test on every arch lane** (owner goal 2026-10-01; what is left of it).
  - **The Rust renderer tests run on Linux only.** Windows and riscv64 compile the crate
    (the bridge, the wasm demo) but leave `cargo test` to OxidANT's own lanes.
  - **The GUI input sweep** stays out of every CI lane for time (`docs/gpu-golden-testing.md`).
  - **Windows arm64 runs its GPU suites unvalidated.** `-StageTests` stages the image's
    validation layer from `C:\runtime\vulkan-layers` on both arches. The hub builds the
    arm64 layer there (CON64, in source 2026-10-06), so arm64 validates once a republished
    `:winarm64` carries it; until then it warns.
  - By design, not gaps: TSan is Linux-only (clang-cl has none), coverage is measured on
    Linux only, the Pester suites test Windows scripts on Windows x64, and
    `FileReaderUnit.ReadersRefuseCharacterDevicesInsteadOfBlocking` is POSIX-only.
- [b] **The riscv64 lane has not finished since its GPU arm was enabled** (found 2026-10-06;
  blocked on an owner decision). d977113a runs `Integration.*` and `GoldenRender.*` under
  QEMU after the CPU suites. On a 4-vCPU runner the arm needs about 14 h, against the job's
  6 h limit (run 37229293578: `PathTracingAccumulatesAndConverges` 6810 s,
  `PathTracingAntiAliasesGeometricEdges` timed out at 7200 s). Every riscv64 run since has
  been cancelled, so the lane gives no signal, the CPU suites' included.
  **Recommendation:** run the CPU suites on every push (about 35 min) and move the GPU arm
  to a weekly schedule, sharded to fit 6 h: the non-path-tracing tests in one job and one job
  per path-tracing test. OxidANT's riscv64 GPU suites already run weekly.
- [b] **Close the slangc 2026.8 floor** (S, waiting on BB's OxidANT pin). The image's Vulkan
  SDK 1.4.357.0 ships slangc 2026.13.1. In `:latest`, `compile-slang-shaders.sh` emits the 10
  combined WGSL files, and since OxidANT b7e6a8c (2026-10-06) they match the checked-in
  ones. Bump `third_party/OxidANT` to b7e6a8c or later once OxidANT's CI is green, then
  delete this entry.

---

Everything below is **unsized**: ideas, owner decisions and recurring chores that have not
been committed to.

## Owner decisions

- **The formatting sweep.** 134 of 217 own sources under `Src/` and `Test/` do not match
  `.clang-format` (measured 2026-09-29; `docs/code-quality.md` "Known state"). Container
  builds log the count without failing. Either one large commit right after a merge point
  plus a `.git-blame-ignore-revs` entry, or format-on-touch only.
- **The PCF radius slider is 1..20** (`GUI.cpp`), and `cascaded_shadow.slang` loops
  `(2r+1)^2` taps, up to 1681 per shadowed fragment. With hardware 2x2 comparison filtering
  the useful range is much smaller, but narrowing a user-facing slider is the owner's call.
- **`Src/KomputePlayground`** cannot configure when turned on: its `CMakeLists.txt` has the
  kompute acquisition commented out while still linking `kompute::kompute`, and
  `src/main.cpp` throws `std::runtime_error` without `<stdexcept>`. Repair it or delete it.
- **`pointShadowMap`** was removed as dead allocation. Re-add it, or the whole omni pass,
  when the point-light shadow feature is built.
- **Docs placement.** The root `docs/*.md` dev references (roadmaps,
  `cpp-renderer-improvements`, `shader-*`, the sRGB audit) are not on the published site.
  Decide per doc whether it moves into `docs/source/` and the toctree.

## C++ engine ideas

- **Unify the two shadow-bias formulas.** `cascaded_shadow.slang` uses
  `max(0.002 * (1 - N·L), 0.0005)` and `forward.slang` uses
  `clamp(0.002 * (1 - N·L) + 0.0005, 0.0005, 0.004)`, so the renderers disagree at grazing
  angles. Editing `forward.slang` regenerates the checked-in WGSL; the slangc floor that used
  to block that is gone (the last sized entry above).
- **Drop the per-frame `update_raytracing_descriptor_set` call in `drawFrame`.** It runs on
  `.raytracing` only, and every state change that invalidates those descriptors already goes
  through `updateAllDescriptorSets`. Deleting it saves three descriptor writes per frame. CI
  now runs the RT goldens, so the change is checkable.
- **`VulkanDevice` creates a queue on `compute_family` that it never retrieves.** Harmless
  (a graphics+compute family always exists, so it equals `graphics_family`), and now
  checkable in CI.
- **A `vkCheck()` helper** for the `auto r = device...createX(info); ASSERT_VULKAN(...);`
  idiom (52 sites, about 20 files). A whole-codebase sweep, so it wants a deliberate moment.
  Fold `PostStage.cpp`'s one C-style `createRenderPass` call into it.
- **Decouple `cloud.radius` from `cloud.scale`** (`clouds.slang`: `radius = cloudMeshScale *
  scale * 10`). It resizes the rendered volume, so the goldens will move.
- **Clouds at half resolution**: a quality trade-off, still open.
- **Animate the cloud volume** (needs a time uniform and a deterministic override for the
  goldens).
- **`GUI*` is a mutable cross-cutting dependency**: `Scene` and `VulkanRenderer` both read
  GUI state each frame. A settings struct owned by the app and passed by const reference
  would decouple them.
- **A stage registry**, so adding a pass does not mean editing `VulkanRenderer`. (A
  `SwapchainTarget` extraction was investigated 2026-07-23 and rejected: recreation is
  inherently cross-cutting.)
- **Prove the multi-object index arithmetic.** `GoldenRender.SecondModelLoadsAndRenders`
  proves a second model loads and contributes pixels, but not that the index is right; that
  needs two models whose materials differ enough to tell apart driver-independently.
- Small, fold into the next change that touches the file:
  - `DeferredRasterizer` and `Rasterizer` index `descriptorSets[0]` without the
    empty-span guard `Clouds::recordComputeCommands` has.
  - `ObjMaterial::get_textureID()` has no production caller; switch the two test call sites
    to the public member and delete it.
  - `sample_alpha`/`sample_alpha_lod0` transform their UV with the base-colour slot's
    `KHR_texture_transform`. A no-op today (only OBJ `map_d` sets `alphaTextureID`, with
    identity transforms); give it an `alpha_uv()` accessor or a comment.

## Rust renderer ideas (OxidANT)

- **Split `render_tonemapped`** (about 530 lines in `forward.rs`) and `ForwardRenderer::new`.
  It can silently reorder passes; `tests/headless.rs` and `tests/forward_ambient.rs` catch
  that, and they run on llvmpipe in OxidANT's lanes now.
- **`obj_to_gltf` drops `map_d`.** glTF has no opacity-texture slot, so matching the C++
  MASK cut-out means compositing `map_d`'s red channel into `map_Kd`'s alpha and writing a
  new image, which adds an image decode/encode dependency to a converter that today only
  rewrites paths.
- **`qem.rs::into_primitive` drops morph targets without the comment** its sibling
  `lod.rs::simplify_primitive` carries. Harmless (morphed primitives are excluded from LOD).
- **Render-graph v2**: automatic barrier placement and transient-resource aliasing, once the
  pass count grows again.
- **Texture streaming / bindless** for photogrammetry-scale scenes (the Colosseum case).
- **Golden-image CI** with reference images per GPU vendor, to catch shader regressions the
  driver-independent structural assertions miss.

## Testing and performance

- **Headless assertions for the shadow path** beyond the single darkened-pixel ratio, and for
  the post-processing chain. Both need careful oracle design (`docs/gpu-golden-testing.md`
  on why the tonemap is hard to isolate).
- **Benchmarks still missing**: `record_commands` wall time per render mode at a fixed scene
  and camera, and the upload path (`createBufferAndUploadVectorOnDevice`) for a few payload
  sizes. Gate the pure-CPU ones in CI; GPU timings are machine-dependent.
- **No budget on the GPU timings.** `KATAGLYPHIS_GPU_TIMING_JSON` and
  `Compare-RendererTimings.ps1` make per-pass timings comparable, but nothing asserts a budget
  for any `GpuTimedPass`.
- **`resolveModelPath` is about 4x slower on a miss** (8 parent-directory probes;
  `Test/perf/baselines/win-9070xt-32core.json`). Fine at startup, bad in a loop.

## Recurring validation runs

- **`clangcl-profile` (RelWithDebInfo)** after any perf-relevant change and before a
  release. It is the only configuration where the benchmarks mean something, and it builds
  `perfTestSuite.exe`.
- **Host-GPU sync validation** with `scripts/windows/Invoke-SyncValidation.ps1`
  (`docs/gpu-golden-testing.md`). Linux and Windows x64 CI validate on llvmpipe/lavapipe on
  every push; the script covers the real GPU.
- **Release build**: the only configuration with logging compiled out and validation layers
  absent.

## Toolchain and CI notes

- **clang-tidy on the module TUs does not fit a CI runner** (measured 2026-10-06). Peak
  working set per TU reaches 10.4 GB (`VulkanRenderer.cpp`), and the 16 GB `windows-2025`
  runner died with `LLVM ERROR: out of memory` on `App.cpp` (run 37466488394), so
  `Build-Windows.ps1` keeps the hub's default skip. A host with RAM to spare can tidy them with
  `-ModuleImportPattern '(?!)'` (120 s on 32 cores, 61 GB).
- **clang-cl coverage crashes the Debug suite at exit** (2026-10-02). With ASan the profile
  runtime comes from Microsoft's ASan DLL, and `initializeValueProfRuntimeRecord`
  dereferences a bad pointer (run 36997000491). Coverage stays OFF on Windows; revisit when
  the runtime matches the compiler or value profiling can be turned off.
- **`bugprone-unchecked-optional-access` is off for `Test/`**, because clang-tidy 23.1.1
  segfaults in its dataflow on gtest bodies. Re-enable it once an LLVM release fixes the
  crash.
- **Coverage is clang-only** (Linux). GCC and Windows contribute nothing to Codecov.
- **The `windows-clang-release-wix` package preset is still unbuilt.** If WiX packaging
  moves to ClangCL, `x64-Clang-Windows-Release`, which exists only for it, can go too.

## Build-time costs and papercuts

- **sccache can serve stale importers after a module-interface edit.** The released sccache
  hashes an importer without the BMIs it imports, and the shared WebDAV cache widens that to
  every build that reads it. Until mozilla/sccache#2876 ships in the image, rebuild with the
  cache bypassed after an `.ixx` edit (hub `docs/windows-container-build-performance.md`
  § *sccache on a C++23 modules build*).
- **An incremental container build can ship ODR-broken binaries after a module-interface
  change** (2026-07-20: consumers of a changed `.ixx` were not recompiled, an instant ASan
  heap-buffer-overflow at construction). Treat an ASan crash at object construction after an
  `.ixx` edit as build skew and cold-build first. Touching a source did not always recompile
  it either; consistent with the tar-transport mtimes, unverified since.
- **`-FreshContainer` strands the build cache** on the wcifs fallback path: the next build
  takes 367 s instead of 44 s.
- **Module dependency scanning** (`clang-scan-deps`) runs over all 53 `.ixx` files each
  configure; measure before assuming it is free.
- **Host `cmake` is 3.29 and cannot read `CMakePresets.json`** (`version: 10`), and host
  `ctest` cannot read the build trees; run the gtest executables directly.
- **No host LLVM** (since 2026-09-25); `docs/code-quality.md`'s clang-tidy commands need one.
- **Swapchain screenshots read black while the desktop session is locked**, with no error.
  Take a control capture first. The offscreen capture path the goldens use is not affected.
- **Restoring a file with `Move-Item` keeps its old mtime**, so ninja can skip the rebuild.
  Touch the file after restoring.
- **A build container can survive a successful build** (`wcifs teardown lock`). Compare the
  newest `logs/windows/build-summary-*.json` with the container start before assuming a build
  is still running.

## Recorded non-tasks

So the next sweep stops here instead of re-deriving them.

- `VulkanDevice::create_logical_device` (~290 lines) keeps its feature setup inline: it is a
  `pNext` chain of stack locals, and extracting it into returning helpers dangles them.
- The AS rebuild and descriptor refresh around `rebuildObjectDescriptions()` stay inline: the
  four scene-changed paths genuinely differ.
- The per-stage `createRenderPass` bodies differ (1, 2 and 3 subpass dependencies, different
  attachments) and are not a consolidation target.
- `FileReader.ixx`'s `readTextFile` and `fileExists` have no production caller but stay: the
  fuzz target drives all four readers, and `fileExists`'s comment records the
  throw-on-permission-denied finding.
- `Camera::set_near_plane`/`set_far_plane` have no GUI control on purpose: their defaults
  differ between debug and release, and `far_plane` feeds the cascade fit.
