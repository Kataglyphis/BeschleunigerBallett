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

- [b] `CI` **Prove the renderer on the RX 9070 XT** (S, **waits for the host**). The riscv64 GPU shards ran at
  8216e01a (run 44852183781): 15 of 16 jobs passed, and the raster shard's one failure,
  `Integration.LibraryDoubleClickLoadsTheRow`, was the editor's 2 s cap on the double-click window, gone since
  Phase 3. Both Windows lanes and both arm64 lanes run every release. Still unrun on the RX 9070 XT: the
  golden suites plus `Invoke-SyncValidation.ps1`, above all the cloud and glossy-material goldens, the
  GPU-timing goldens (`writeTimestamp2`, the nested scopes, sync-validated on llvmpipe only), the glTF and
  reload goldens over the in-place upload, the sky drawn into the HDR target in every mode, FXAA, the furnace
  and metallic goldens under multi-scatter GGX, and `Integration.SharedSlangKernelsMeetTheirVectors` (the
  vector tolerances assume correctly rounded f32 sin and cos); since Phase 4 also
  `MultiPrimitiveGltfLoadsAsMultipleMeshes`' pixel check, the shadow goldens and `Compare-RendererTimings.ps1`
  over the Rust scopes re-cut as this engine's. Since Phase 5: the app opens at the refresh rate and the
  DISPLAY toggle and Render > VSync switch present modes without a device loss
  (`VsyncTogglesBetweenFifoAndAnUncappedMode`), `GpuTimingJsonDumpIsWrittenAndSane`,
  `Integration.DebugLabelsAndNamesWorkWithoutTheValidationLayer`, a RenderDoc capture of Profile or Release
  showing the pass labels and object names, and the seven `BM_Bridge*` placeholder rows refreshed with
  `perfTestSuite`. Since Phase 6: the ray- and path-traced goldens with `TracedCameraRaysStartAtTheNearPlane`,
  the vector suite's `hdr_safe` cases (NaN and the infinities as bits) and `camera_ray` on the AMD driver, and
  by eye the traced modes with the editor docked (the lens shift through `clip_to_world`), Sponza in Release
  at its far plane of 4000 (traced coverage as raster's) and the camera close to geometry (the traced modes
  clip at the near plane as raster does). Since Phase 7: the Sky tab's Ambient slider and the Sun tab's
  direction drag (it still shows the way the light travels and must move the shadows the same way), F5 in a
  Profile or Release build, and `Test-GpuTimingBudget.ps1` over Forward, Deferred and RayTrace, which the
  analytic ambient's three `pow` per pixel reach.

The entries tagged *plan* come from the bridge and feature plan of 2026-11-18 (80 candidates, each
checked against the code; the ids in parentheses are the plan's). Its rules: bridge load-time and tool
work into wgpu-free Rust crates, never per-mesh or per-draw calls; share GPU work as dual-target
Slang in OxidANT, each renderer keeping its own host pass; a bridged function returns errors as values,
takes UTF-8 paths and runs inside `guarded()`.



### P2: medium

- [ ] `CI` **Warnings that count, and the cache cap's first runs** (S each). Since 2026-11-23 (hub pin
  04981676, run 45233255466): explicit PIE at the project's scope, IWYU as a report (32 of 35 units with
  suggestions), and no spdlog `-Wstringop-overflow` in the profiling job since spdlog builds without LTO;
  `SCCACHE_CACHE_SIZE` is 1200M per key. Left: watch the next few runs' clang-tests and sanitizer build times
  and the cache total (under 10 GB); 1200M sits at or below the debug working set (about 1.5 GB at zlib-1),
  and the owner keeps it there even if some keys miss (2026-11-23). At the next hub pin (d2aaa551 or later)
  drop BB's own `: fatal error:` count in `run-static-analysis-format.sh`, which the hub's IWYU count now has,
  and once hub CON299 lands, replace `Build-Windows.ps1`'s inline "Bundle layout" step with its install-tree
  check. Then CON295's ratchet, once the hub ships it, and warnings as errors on the product targets.


- [b] `C++` **Confirm on the RX 9070 XT that the path-tracing device loss is gone** (S, needs the
  host GPU). Cause found 2026-08-05: `path_tracing.slang`'s committed hit dropped the
  geometry index, so a hit on any mesh but a model's first read past mesh 0's index buffer.
  The kernel now adds `CommittedGeometryIndex()`, and the three tests that lost the device
  pass on llvmpipe in CI. Run
  `commitTestSuite.exe --gtest_filter='GoldenRender.PathTracing*:GoldenRender.GuiInputSweepNeverCrashesOrLosesTheDevice:Integration.RenderModesSelectableInGui'`
  on the host. Once it passes, retire the "Known issue" section of
  `docs/source/gpu-golden-testing.md`, with the `excluded=3` count and the `--gtest_filter` line
  `buildIntegritySuite.cpp` pins in it.

Loader:

- [ ] `Assets` **Texture ingest in Rust, step 3** (M-L, plan B5; steps 1-2 are in since Phase 10). Upload BC7, ETC2
  or ASTC natively where the device samples them (KTX2 as stored, Basis transcoded to the device's format), so the C++
  engine stops holding RGBA8 chains: Colosseum's 760 MB of chains put `load_flat`'s peak at 1.3 GiB (745 MiB with the
  encoded files). WebP is new work. Streaming chains per slot from the parse thread to the upload is the fallback if a
  scene's chains pass the 1 GiB decode budget. The WebGPU renderer still copies each base once to build its chain
  (`generate_mips` borrows the `Arc`'d `CpuTexture`); an owned or `Cow` `LevelChain` would remove that copy.
Shaders:

- [ ] `Shared` **Single-source constants and complete layout gates** (M, plan I7; the leaf modules are in
  since Phase 6). `static const int` lines in a file under `Resources/ShadersSlang/common` that C++ includes
  (not the reverse: the C++ header sits outside every `-I` root); `limits.slang` in OxidANT; a naga layout
  table over the ~20 unchecked Rust Pod structs; ArrayStride, struct size and `cloudAmbientTop/Bottom` in the
  C++ SPIR-V contract.
- [ ] `Shared` **Shared material Slang** (L, plan I9; the leaf modules are in since Phase 6). Owner
  2026-11-19: metallic is clamped to [0, 1] and MASK is an explicit alpha-mode flag plus cutoff, both as Rust
  does. Normal mapping with the BC5 z rebuild, the texture transform and `gltf_material.slang`; each exists
  three times today. The C++ push constants shrink from 116 to 68 bytes.
- [ ] `Shared` **Shared shadow and lighting Slang** (L, plan I10; the leaf modules are in since Phase 6). C++
  takes Rust's G-buffer packing (owner 2026-11-19); the cascade retry comes with C11. Dual-target punctual
  shadows, `cascade_visibility`, `surface_lighting`, `gbuffer.slang` and the cloud upsample.
  `forward_lighting.slang`'s two shadow-map NDC-to-uv conversions and its `uv_y_sign` of -1 are still WebGPU's
  alone.
- [ ] `Shared` **The analytic ambient in Rust's ray-traced mode** (S-M). Since Phase 7 Rust's forward pass and
  every C++ raster and ray-traced frame add `ambient.slang`'s `analytic_ambient`, but Rust's tracer does not:
  with no environment the analytic term, with one the IBL maps bound in `TRACE_LIGHTS_GROUP`. OxidANT's
  browser test asserts a black ray-traced shadow, which changes with it.
- [b] `Shared` **A shared path integrator and one C++ hit fetch** (L, plan I11; after the shared lighting
  Slang; `camera_ray.slang` is in since Phase 6). Owner 2026-11-19: a camera miss shows the drawn sky, bounce
  misses sample the drawn sky through the IBL, camera rays start at the near plane; furnace NEE, the call
  sites and the depth ray are the item's to settle; the bounce sky reads the Sun's Ambient slider, which the
  C++ path tracer's fixed 0.35 bounce scale ignores since Phase 7. `trace_path<S: ITracedScene>` in OxidANT,
  C++ over RayQuery, Rust over `bvh_traverse`; BB's `traced_hit.slang` folds the hit fetch of rchit and the
  path tracer and the three MASK candidate loops.

C++ catches up:

- [ ] `C++` **IBL and the occlusion map for C++** (L, plan C4 stages 2-3; the analytic ambient term is in
  since Phase 7). Owner 2026-11-19: both renderers bake the IBL from the drawn skybox (Rust follows with
  OxidANT's IBL entry), and C++ takes Rust's G-buffer packing (occlusion in albedo.a, unlit in metallic's
  sign).
- [ ] `C++` **Exposure, bloom, SSAO and auto exposure** (L, plan C5; the HDR sky is in since Phase 3, the
  Slang histogram since Phase 6). Exposure as Rust's two-float storage buffer, bloom from OxidANT's shader,
  SSAO on the ambient and IBL terms only (owner 2026-11-19, both renderers), auto exposure once the histogram
  is Slang.
- [ ] `C++` **One set of C++ scene targets** (M-L, plan C6 step 1). One set per swapchain image, and both
  raster paths always allocated, cost 116 B/px (about 229 MiB at 1080p); afterwards 12 B/px forward or
  32 deferred. Allocate the inactive raster path lazily. Cross-frame hazards change: sync validation.
- [b] `C++` **Render scale with the Rust AutoScaler** (L, plan C6 step 2; after one set of targets).
  Every pass reads the swapchain extent today and must take a scene extent instead.
- [ ] `C++` **Remove the stalls on the C++ frame path** (L, plan C7). Move the PNG/JPEG decode from the
  device thread onto the `AsyncModelParse` worker and batch texture uploads through a staging ring; route
  the editor reload through the worker and keep the old scene until the new one lands (a failed reload
  empties the scene today); keep the TLAS with `eAllowUpdate` and update it in the frame's command buffer
  instead of `waitIdle` plus a rebuild on every transform change, resetting the path-traced mean
  explicitly. `addModel` stays synchronous for its 33 golden callers.
- [ ] `C++` **Tile the path-tracer dispatch; furnace, sample count and restart in the editor** (M, plan
  C8). Port Rust's `plan()`: at most 4 M path segments per dispatch in 256 px tiles and sample chunks, the
  mean counting samples, the seed from the absolute sample index. At the GUI maxima one dispatch can hold
  about 2,000 rays per pixel against Windows' 2 s TDR.
- [ ] `C++` **Editor parity: F12 screenshot, model argument, drop, fly speed, sky label** (M, plan C9).
  An offscreen F12 capture into `screenshots/`, an absl positional model argument, `glfwSetDropCallback`
  through the asynchronous load, a fly-speed slider, and the Skybox subtitle reading Procedural when
  `skyIsAnalytic()`.
- [ ] `C++` **Device-loss breadcrumbs and `VK_EXT_device_fault`** (M, plan C10). Markers at the debug-label
  scopes (the path tracer records inside `main`), `vkCmdWriteBufferMarker2AMD` or NV checkpoints where
  present, the fault query after a loss; feature-gated, best effort.
- [ ] `C++` **Name every Vulkan object** (M, plan X8's remainder). Since Phase 5 `VK_EXT_debug_utils` is on
  wherever the loader lists it and the per-pass targets, the graphics, compute and ray-tracing pipelines carry
  names (`nameObject`, `PipelineBuilder::setDebugName`); buffers, descriptor sets, samplers, acceleration
  structures and command buffers are still unnamed in a capture.
- [ ] `CI` **GPU-assisted validation for buffer-device-address reads** (M, plan X6). `robustBufferAccess`
  is off and every shader reads materials through raw BDA. A weekly GPU-AV subset on Linux llvmpipe and an
  `Invoke-SyncValidation.ps1` switch; scan the log for GPU-AV VUIDs.
- [ ] `CI` **Compare the two renderers' frames in Linux CI** (M, plan X1). Move the frame metrics into
  `GoldenMetrics.hpp` with tests (one bucket rule; the PowerShell copy uses Rec. 601, the goldens Rec.
  709), render Dinosaurs with `headless_render` on llvmpipe in the `static-analysis` job and grade it
  structurally. Keep `Compare-RendererPixels.ps1`'s modes, thresholds and exit codes.
- [ ] `C++` **Apply the decided cross-renderer rules** (L, plan C11). The shared `cascade_visibility` with
  the coarser-cascade retry (C++ counts a missed cascade as lit), then each rule the owner decided on
  2026-11-19 in both renderers, with a golden on each side: BLEND materials cast opaque shadows in every
  mode (Rust's raster casts none today), a path-traced camera miss shows the drawn sky (both draw a
  gradient today), AO darkens the ambient and IBL terms only (Rust darkens direct light too).
- [b] `C++` **4x MSAA on the forward path** (L, plan C3 step 2; after FXAA). PipelineBuilder hard-codes one
  sample; a depth resolve for the clouds.

New in both:

- [ ] `Shared` **A tonemapper choice and dither** (M, plan N2, in both renderers; the HDR sky is in since
  Phase 3). Narkowicz ACES, Khronos PBR Neutral and AgX selected by a uniform; dither in the encoded domain.
  Both renderers invert ACES for the LDR cube today, and AgX has no closed-form inverse.
- [b] `Shared` **Path-traced reference mode, firefly clamp and .hdr export** (L, plan N7; after the
  shared integrator). A target sample count with a resolve-only path in C++, `clamp_sample` off by default,
  a Radiance `.hdr` encoder in a wgpu-free crate returning bytes, EXR natively.
- [ ] `Shared` **Debug views, wireframe, debug lines and a GPU memory panel** (L, plan N5; the leaf modules
  are in since Phase 6). Material and G-buffer channels from a shared `debug_view.slang`, wireframe as a line
  list over an edge index buffer, `vmaGetHeapBudgets` with `VK_EXT_memory_budget`.
- [b] `Shared` **Cloud shadows** (M-L, plan N3; after the shared shadow Slang and integrator). Bake
  T = exp(-light_optical_depth) into a light-space map covering the cloud box's projection; raster, RT and
  both path tracers read the same map.
- [ ] `Assets` **EXT_meshopt_compression** (M-L, plan N4). A pure-Rust decoder, fuzzed through `load_flat`'s
  proptest, with a licence row; then Colosseum-web can be republished through gltfpack. Since Phase 5 gltfpack
  output without `-cc` loads (`KHR_mesh_quantization`, OxidANT 0bbea5a).
- [b] `Shared` **A view file shared by both renderers** (L, plan N6; after the settings schema and the
  framing). serde types bridged as `view_to_json`/`view_from_json`, `--view` in both applications and the
  golden dump; it unblocks a per-pixel renderer comparison.
- [b] `Shared` **Better path-tracer sampling** (XL in seven parts, plan N8; after the shared integrator).
  Power-weighted light picks (S), emitter NEE with MIS (L), environment importance sampling (M-L),
  Sobol or blue noise (M), ray cones (M), a thin lens (S), stochastic BLEND (S-M). One GPU-free alias-table
  builder in kataglyphis_assets that survives NaN and zero-sum weights.

### P3: low

- [b] `Rust` **WebXR** (XL) — parked.
- [ ] `Assets` **Credit and framing polish** (S). The Rust renderer credits from `asset.copyright` only; give it
  `scene_meta::credit`'s ATTRIBUTION.txt fallback, so its OBJ scenes are credited as the engine's are. C++ shows the
  credit in About only; a library-row tooltip as Rust's would show it before a load.
- [ ] `C++` **Editor parity, part 2: adapter by name, orbit and wheel zoom, one key table** (M, plan C9).
  `--adapter` or `KATAGLYPHIS_ADAPTER` by name substring with the list in the Renderer tab; orbit and
  zoom with Rust's constants; one table drives the handlers and the key panel.
- [ ] `CI` **A parity ledger of the GPU tests** (M, plan X18). `Test/parity-ledger.json` maps each of the
  95 C++ GPU tests to its Rust twin or a reason; BuildIntegrity fails on an unmapped test or a stale row.
- [ ] `Shared` **Rotate the photo sky with the sun's azimuth** (M, plan X19), on by default in both renderers
  (owner 2026-11-19). The three C++ cube lookups (with post's cloud composite) take a yaw; Rust needs only a
  flag in `sky.slang`.
- [ ] `Assets` **Skybox faces and the cloud ambient from Rust** (M, plan B4). One
  face decode and average for both renderers; the navy gate needs a new form.
- [ ] `C++` **Fit the cascades in Rust** (M, plan B7). The first per-frame bridge call (about 1 µs, no
  allocation); brings Rust's near plane at the nearest caster to devices without depthClamp, where C++
  loses tall casters' shadows.
- [b] `C++` **Spot-light face masks** (M, plan B8; **measure the cube cost on a GPU host first**). One
  `cone_face_mask` in Rust; Rust skips faces, C++ needs per-mask pipelines or an `SV_ViewID` cull.
- [b] `Shared` **Keep punctual shadow cubes whose light and casters did not change** (L, plan X10;
  **measure first**). A fingerprint per slot; C++ tracks the layout per slot instead of discarding the
  whole cube array every frame.
- [b] `C++` **LOD chains at load time from Rust's QEM** (L, plan B11; per-level material ids first).
  Harden `qem.rs` (`remap[i]` panics on bad indices), return each level's error, pick the level by
  screen-space error at draw time; shadows and BLAS keep full detail.
- [ ] `C++` **glTF animation through one Rust animator** (XL, plan B12; the skin's rest pose is in since Phase
  5). Owner 2026-11-19: C++ animates, so FlatModel carries the animation data instead of a baked snapshot.
  Extract the animator from `ForwardRenderer` into a wgpu-free crate first; skinning in shared Slang, a
  compute pre-skin with a BLAS update for the traced modes. Since Phase 6 `load_flat` reverses a triangle
  whose joints mirror space at the rest pose; a pose that changes a joint's mirror sign needs the winding per
  frame, or both faces drawn.
- [ ] `C++` **GPU occlusion culling, then multi-draw indirect** (L + L, plan C12). A Hi-Z pyramid
  (R32Float over single-mip storage views), not occlusion queries (owner 2026-11-19; the Rust roadmap's
  queries row changes with it). GPU frustum culling alone gains nothing over the exact CPU cull.
- [b] `C++` **Tiled punctual lights from the bridged tile grid** (M, plan C13; after the shared lighting
  Slang).
- [b] `C++` **A compute-BVH fallback for the ray-traced mode** (L, plan C14 step 2; **after a device
  without ray tracing runs the raster path at all**). The RT pipeline stays as the showcase and the A/B
  against ray query (owner 2026-11-19), so step 1, the ray-query kernel, is dropped.
- [b] `C++` **Runtime-sized texture descriptor arrays** (L, plan C15; **waits for a host run**).
  `PARTIALLY_BOUND` with a capacity from the device limits; the fixed 128 slots stay as the fallback, so
  the Raspberry Pi 5 stays a target (owner 2026-11-19).
- [ ] `C++` **Material shaders for a device without non-uniform indexing** (M, plan C15). Since 2026-11-19
  every material module wraps its index in `NonUniformResourceIndex` and so declares
  `SampledImageArrayNonUniformIndexing`, which a device without `shaderSampledImageArrayNonUniformIndexing`
  (v3dv on the Pi 5) rejects at module creation. Owner 2026-11-19: keep the Pi; compile a variant without
  the decoration, select it when the feature is missing, and say so in one log line at device selection.
  A unit test of the selection and a BuildIntegrity check that both variants exist.
- [b] `Shared` **HDR output and a grading LUT** (L, plan N2; after the tonemapper choice).
- [b] `Shared` **PCSS with an angular sun, cascade blending, contact shadows** (plan N3; after one shared
  cascade selection; contact shadows need a forward depth prepass).
- [b] `C++` **glTF instancing** (L, plan N4). A BLAS per instanced range, TLAS instances, instanced draws.
- [b] `Shared` **Preferences, camera bookmarks and flythroughs** (M + L, plan N6; after the view file).
- [b] `Shared` **Temporal anti-aliasing, upscaling, specular anti-aliasing** (XL, plan N9; after FXAA and
  render scale).
- [b] `Shared` **KHR_materials extensions** (XL, plan N10; after the shared material Slang, N1 and the
  texture ingest). ior and specular first.
- [b] `Shared` **Editor scene tools: outline, GPU picking, light and material inspectors, gizmo, undo**
  (XL, plan N11; after the settings schema and the framing).
- [ ] `Shared` **Hillaire atmosphere with aerial perspective and height fog** (L, plan N12). A third sky
  mode beside the photo sky and the gradient; the photo sky stays the default (owner 2026-11-19).
- [b] `Shared` **A shared SVGF denoiser** (L, plan N13; after the shared integrator and the reference mode).
- [b] `Shared` **Ray-traced shadows and AO in the deferred raster path** (L, plan N14; after the TLAS
  update and SVGF).
- [b] `Shared` **GTAO with bent normals; SSR with Hi-Z pyramids** (L + L, plan N15; after the C++ ambient
  and IBL; the pyramid is C12's Hi-Z).
- [b] `Shared` **GPU particles** (XL, plan N16; parked behind the P2 work). The Rust half first, as the
  roadmap's compute-playground row.
- [ ] `Shared` **Reversed-Z for the camera in both renderers** (L, plan N17). Owner 2026-11-19: reversed-Z
  yes, photometric light units no. Moves depth-reading goldens and passes on each side.
- [b] `Shared` **Coverage-preserving MASK mips** (M, plan X11; after the texture ingest).
- [b] `Assets` **The full Rust settings schema as cxx shared structs** (L, plan B10; when the view file
  needs it).
- [ ] `CI` **Budget llvmpipe's GPU passes from a build without ASan** (S). Phase 5 found the 0.5 s G-buffer
  was the Debug build's ASan: its interceptors slow llvmpipe's mesh passes 7-9 times (Profile: Forward 76 ms,
  GBuffer 66, ShadowCascades 180; the same binary with ASan preloaded 557, 539, 1600), and the validation
  layer adds nothing. The `wsl-aarch64-llvmpipe` budget still comes from Debug dumps on a warm Mesa cache (a
  cold cache fails its Forward bound). Take it from the Profile perf rig instead, or say in the entry that it
  measures ASan. The export after `GpuTimingSubsystem::restart()` also holds the load frame (on llvmpipe the
  frame that compiles the pass), and `RendersNonBlankFrame` leaves about three slices for the dump: skip the
  first slice after a restart, or draw more frames before it (Phase 5 review).
- [ ] `C++` **Why the shadow cascades dominate the llvmpipe frame** (S). In Profile on llvmpipe ShadowCascades
  takes about 180 ms against Forward's 76: the four cascade views redo the vertex work. Measure per cascade
  and the vertex share before a host budgets the pass; a geometry-shader or multiview pass, or culling per
  cascade, would be the candidates.
- [b] `C++` **`GltfLoader`'s index views back to `std::span` members** (S; **waits for ANTfrastructure
  CON281**). GCC 16.2 crashed on two `std::span<const unsigned int>` data members (f7e9c7fc keeps a pointer
  and a count). Restore the span members once the image's GCC compiles them, checked first under a 4 GB
  cgroup.

---

Everything below is **unsized**: ideas, owner decisions and recurring chores not yet
committed to.

## Owner decisions

- **No standing omni shadow allocation.** `pointShadowMap` went as dead allocation; the
  punctual-light cubes that replaced it (2026-11-17, `PunctualShadowMaps`) allocate only for a
  scene's shadowed point and spot lights and keep a 1x1 placeholder otherwise. Keep it that way.
- **Panics come back through the bridge in every build** (2026-11-18). OxidANT's `bridge` cargo profile
  (release with `panic = "unwind"`) serves BB's non-Debug configurations, so `guarded()` turns a panic
  into an error there too.
- **C++ adopts a glTF's first directional light as the Sun** (2026-11-18), as Rust does: direction and
  colour from the file, the intensity stays the GUI's.
- **The vendored KTX tree, toml++ and the configure-time `cargo install` of cxxbridge are gone**
  (2026-11-18), as dead dependencies.

- **The plan's cross-renderer rules** (2026-11-19), each in the entries that name it:
  - Rendering: C++ takes Rust's G-buffer packing; both bake the IBL from the drawn skybox; AO acts on
    the ambient and IBL terms only; BLEND materials cast opaque shadows; a path-traced camera miss shows
    the drawn sky; traced camera rays start at the near plane; reversed-Z yes, photometric units no.
  - Defaults: Rust's Sun (radiance 5, warm white, ambient 0.35); a cloud wind range of ±100; VSync on;
    F5 in every native build; Rust's LOD off until measured; the photo sky turns with the sun's azimuth.
  - Loaders: the OBJ reader is tolerant with one mesh per `usemtl` group; a skinned node's own transform
    is ignored; scenes are framed only with their own camera or a too large far plane.
  - Platforms and passes: the Raspberry Pi 5 stays a target (128-slot fallback, a variant without
    non-uniform indexing); the RT pipeline stays; occlusion culling uses a Hi-Z pyramid; the Hillaire
    atmosphere is a third sky mode.

  - Later rules: the shared material Slang clamps metallic and flags MASK as Rust does; the OBJ reader takes
    Rust's material rules; textures decode as PNG, JPEG, BMP, TGA, GIF, PNM and HDR; C++ gets glTF animation
    through one Rust animator; both glTF loaders confine images and buffers to the glTF file's folder.

- **The bridge foundations' review rules** (2026-11-19, after Phase 2):
  - Frustum culling follows the C++ rules in both renderers and `draw_cull.slang`: an inverted or invalid box
    is visible, planes are normalised and a box up to 1e-4 outside still counts, a zero-normal plane is
    skipped.
  - The bridged cloud `sanitize` clamps every field to its editor range, and a NaN falls back to the default,
    as the shadow settings already do.
  - The Sun (direction, colour, radiance, ambient) joins `default_scene_settings()`; the drift test names its
    fields as the expected difference until C++ takes Rust's Sun with the ambient term.
  - A zero-height frame has aspect 1 in both renderers.
  - An empty `KATAGLYPHIS_MIKKTSPACE_TANGENTS` counts as unset, in both renderers; only the Rust viewer and
    the C++ engine read it, the Rust tools and tests load with the defaults.
  - `Resources/Models/GltfTest/cube_textured.gltf` stays: two path-tracing goldens reload it through the
    editor library, and `GltfFixtureRoundTrip` keeps it byte-identical to OxidANT's copy.
- **The review's follow-up rules** (2026-11-20):
  - The renderers' cloud packing keeps only the clamps that protect the shader; only the editor and the
    bridged `sanitize` hold the editor ranges, so a test or a view file may still ask for a density above 1.
  - NaN and ±inf fall back to the field's default everywhere, the shadow strength included (0.65, not 1.0).
  - Every empty frame, zero width or zero height, has aspect 1 in both renderers.
  - The Sun's ranges, `sanitize` and direction convention come with "One set of defaults".

- **Phase 3's rules** (2026-11-21): the shared vector kernels keep tight tolerances and loosen a trig output
  to the Vulkan/WGSL bound only on evidence from a device; their f64 references come from OxidANT's Python
  model; BB runs one test over every vector file. Multi-scatter GGX keeps the 14x14 spline; BB's furnace rig
  is at roughness 0.5; the lobe choice stays F(n.v). `fxaaMode` is a renderer setting, both renderers default
  FXAA to Always (Rust's presets inherit it); the ray-traced sky fills by the target's alpha; the editor's
  double-click window has no upper cap; the metallic-factor golden's bound is 0.9; `leak:glfwInit` stays.

No decision of the 2026-11-18 plan, its Phase 2 or the review's follow-up or Phase 3 is open.

## Rust renderer ideas (OxidANT)

- **[P3] Bindless material textures** for scenes with thousands of materials. OxidANT streams,
  evicts and GPU-copies material mips since 04f7a0e; bindless is blocked on slangc's WGSL output
  (no `binding_array`, no `NonUniformResourceIndex`; OxidANT's `BACKLOG.md`, "Texture streaming
  past the mip tail").
- **[P3] Golden-image CI** with reference images per GPU vendor, to catch shader regressions the
  driver-independent structural assertions miss.

- **[P3] Kulla-Conty's reciprocal multi-scatter.** Owner 2026-11-21: Turquin's view-only diffuse share stays
  (the BRDF is not reciprocal); Kulla-Conty needs an average energy E_avg besides E_ss, worth it once a
  bidirectional method arrives.
- **[P3] Pick the path tracer's specular lobe by E_spec.** Phase 6 measured it against the unbiased F(n.v)
  split on lavapipe (128x128, a 1024 spp reference): the MSE ratio F/E was at most 1.064 in the furnace and
  0.997-1.006 on the shadow rig, at the same cost per sample, since the 0.1 floor clamps both at most angles
  and NEE dominates the rig. `path_estimator` keeps F(n.v) (owner 2026-11-21); revisit if the floor goes or
  the path-traced goldens are re-baked anyway.
- **[P3] The GGX energy as a bound table.** A buffer or texture is the cheapest lookup per evaluation, but
  changes `brdf_direct`/`brdf_sampled` and adds bindings to two OxidANT and five BB pipelines; Slang's
  `var<private>` table cost lavapipe 2.3-2.9x. Only if the spline shows up on a device profile.

- **[P3] Count BB's GGX energy evaluations in BuildIntegrity.** OxidANT's `tests/brdf.rs` counts `ggx_energy`
  call sites in its WGSL since Phase 4; BB's `rasterizer`, `deferred`, `raytrace.rchit` and `path_tracing`
  Slang evaluate it once each, guarded only by review. On lavapipe the hoist changed no frame time (LLVM had
  already moved the fit out of the light loop), so the 1.2-1.35x cost of multi-scatter GGX is not per light; a
  device profile would say where it is.

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
- **[P3] `bugprone-unchecked-optional-access` is off for `Test/`**: clang-tidy segfaults in its
  dataflow on gtest bodies. Re-tested 2026-11-16 with the pinned 23.1.5 in the arm64 image, the
  check alone over every `Test/` TU: still a SIGSEGV in `dataflow::transfer`, now on one TU
  (`buildIntegritySuite.cpp`, `ShadowLightMatricesAreProvisionedInOnePlace`'s `reinit_span->first`);
  the other 67 finish, with 100 findings, every one behind an `ASSERT`. Re-enable it, and rewrite
  those, once an LLVM release fixes the crash.
- **[P3] The GUI input sweep runs weekly, on x64 only** (`linux-x64.yml`'s `gui-sweep`, since
  2026-11-16; 862 s of a 22 min job on its first run, 43948778560, so ctest stops it at 3600 s and
  the job at 120 min). Every other suite runs on every arch lane
  and every push. By design, not gaps: TSan is Linux-only (clang-cl has
  none), coverage is measured on Linux only, the Pester suites test Windows scripts on Windows x64,
  `FileReaderUnit.ReadersRefuseCharacterDevicesInsteadOfBlocking` is POSIX-only, and riscv64 runs
  four path-traced goldens in their light variant (AGENTS.md § The riscv64 lane). OxidANT's own
  lanes run the Rust renderer tests on Windows x64, arm64 and riscv64; this repo's on Linux.

## Build-time costs and papercuts

- **[P3] An incremental container build can ship ODR-broken binaries after a module-interface
  change** (2026-07-20: consumers of a changed `.ixx` were not recompiled, an instant ASan
  heap-buffer-overflow at construction). Treat an ASan crash at object construction after an
  `.ixx` edit as build skew and cold-build first. Touching a source did not always recompile
  it either; consistent with the tar-transport mtimes, unverified since.
- **[P3] `-FreshContainer` strands the build cache** on the wcifs fallback path: the next build
  takes 367 s instead of 44 s.
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

- A burst of develop pushes can lose the middle commit's CI verdict, since GitHub keeps one waiting run per
  concurrency group (runs 44929269007, 44930561429). Owner 2026-11-23: keep the groups as they are and push
  less often.
- `agents-drift` (2026-11-23): the hub scopes it to its AI consumers, and BB's AGENTS.md is not in the hub's
  six-section template (22 findings on a trial run). `core.hooksPath` on the Pi's clone is that host's, not
  this repo's.
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
- Module dependency scanning (`clang-scan-deps`) is not a configure cost (measured 2026-11-16 on
  `linux-debug-clang` in the arm64 image). It is one ninja edge per C++ source, all 187 TUs and not
  only the 57 `.ixx`, rerun only when its source changes: a reconfigure took 1.5 s (16 s the first
  time after a build) and left 0 scan edges to run. All 187 scans cost 4.97 s at `-j4`, 19.8 s of
  CPU against 1837 s for the compiles (about 1 %), and the build after them recompiled nothing.
- The `windows-clang-release-wix` package preset needs no lane of its own: `Build-Windows.ps1`'s
  `--target package` on the same `x64-ClangCL-Windows-Release` tree runs the same CPack generators
  (NSIS, ZIP and WIX, the hub's `CPackCommon` with `WIX_DEFAULT ON`), and the Windows x64 lane's
  MSI lands in `dist/windows-x64` from it.
