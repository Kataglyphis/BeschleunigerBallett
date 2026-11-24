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

- A scene starts at its own glTF camera, and its credit is in the About window (plan B9, owner rule 2026-11-19):
  OxidANT's framing moved out of the WebGPU renderer into `kataglyphis_render_core` (`framing.rs`, with
  `OrbitCamera` and the fly speed), and the bridge's `frame_scene` hands it the loaded file's cameras, its active one,
  its world-space box and the camera's fixed pose. A scene with a camera starts at the one
  `scenes[scene].extras.active_camera` names, else at its first; a scene without one is fitted to its bounds only
  when the fixed pose would need a far plane past 50 000 near planes (an 8 km Colosseum), and every other scene keeps
  the fixed pose, as before. A framing sets the eye, yaw and pitch (the orbit's yaw + 180 and -pitch), both clip
  planes and the fly speed, and the Camera tab's field of view and the Sun tab's shadow distance, which the renderer
  reads every frame; the next load of an unframed scene restores the fixed pose. The credit is `asset.copyright`, else the `Attribution:` and `License:` lines of AssetANT's
  `ATTRIBUTION.txt` beside the model (its first 64 KiB, a regular file only), so crytek-sponza is credited too, and a
  failed load clears it; the About window shows it under the facts,
  as OxidANT's does. `GoldenRender.ASceneWithItsOwnCameraIsDrawnFromIt` draws an unlit red card at x = 100 that only
  its camera sees, and `SceneFramingUnit` holds the rule, the convention and the credit's source.

- An analytic ambient term (BACKLOG "An analytic ambient term with an Ambient slider", plan C4 stage 1): forward,
  deferred and the closest-hit shader add OxidANT's `analytic_ambient` (`common/ambient.slang`) after the sun's
  shadow, the procedural sky's hemisphere irradiance and its reflected gradient blurred towards it by roughness
  squared, which Rust's forward pass takes without a baked environment. It takes no sun disk: `brdf_direct`'s GGX
  lobe carries the sun, and a disk here kept a glossy surface's glint inside the sun's shadow (review, OxidANT pin
  `6054002`); `GoldenRender.AMirrorInTheSunsShadowReflectsTheSkyAndNoSun` holds a smooth metal card mirroring the sun
  under the rig's slab to the brightest sky radiance per unit of Ambient in forward, deferred and ray-traced frames. Shadowed ground was black; it takes
  the sky's blue now. The Sky tab's ENVIRONMENT section has Rust's Ambient slider (0-2, default 0.35), the Sun's
  `ambient`, which reaches the shaders in `dirLight.direction.w`. The path tracer gets the sky through its misses and
  keeps its 0.35 bounce scale. `GoldenRender.TheSkyLightsShadowedGroundInEveryRasterAndRayTracedFrame` reads the
  shadow's linear radiance in forward, deferred and ray-traced frames: none at ambient 0, the sky's blue at the
  default, scaled by exactly 2 / 0.35 at the slider's top, and the same in all three. `BuildIntegrity` names the three
  readers of `dirLight.direction.w` and fails a private `analytic_ambient` or `ambient_split_sum`.
- `Integration.F5RequestsAShaderReload` presses F5 in the editor and sees the reload request reach the renderer;
  `BuildIntegrity.ShaderReloadRequestsAreInEveryBuild` fails a request site inside an `NDEBUG` block.
- The Sun's ranges hold at run time: every editor slider (`studioSlider`) clamps a Ctrl+click entry to its range
  (`StudioWidgetsUnit.ASliderHoldsItsRangeForATypedValue` types -1 and 7 into a [0, 2] slider), and a glTF's Sun is
  sanitized once at load through the bridge (`GltfParseUnit.AFilesSunArrivesInTheEditorsRanges`: colour (3, -0.5,
  inf) arrives as (1, 0, 0.92)). `EveryShaderDerivesTheLightVectorByNegation` requires `.xyz` on every
  `dirLight.direction` read but the three ambient readers' `.w`.
- The Ambient slider no longer restarts the path tracer's mean: `makePathTracingHistoryKey` keys on the light's
  direction alone (`PathTracingHistoryUnit.TheAmbientInTheLightsWDoesNotInvalidateTheHistory`).
- `ForwardLightingRespondsToTheDirectionalLight` reads its radiance-0 crop over geometry only, with the navy sky
  masked out: the cube-map sky alone had put the crop at 29.8 against the bound of 30. Geometry reads 0 over 161257
  pixels (204.6 lit); a forward sun lobe blind to the radiance stays at 115.3 and fails.

- `security-audit.yml` calls the hub's lockfile audit: osv-scanner over `uv.lock`, where a new HIGH or CRITICAL
  advisory fails a push (a planted pyyaml 5.3 did) and the Monday run reports every one. `retry-disk-refusal.yml`
  re-runs a Linux x64, on-request or release job that `prepare-linux-ci-host` refused for disk. The code quality
  measurements grade the tree with `quality-report: true`, and `quality-report.toml` names the docs toctree, without
  which that run was incomplete. The lint gates enforce all four workflow conventions (`sha-pin`, `permissions`,
  `job-timeout`, `artifact-error`), each at 0 findings, in CI and in `run-lint-gates.sh`.
- The hub's hardening gates read the shipped binaries (CON294): the Linux release job runs `verify-elf-hardening.sh`
  on the .deb's `usr/lib/graphicsengine/GraphicsEngine`, and `Build-Windows.ps1` runs `Assert-PeHardening` over every
  `.exe` and `.dll` of the portable bundle, in both Windows lanes. On the binaries of runs 45001331996 and
  45001332269, built before the hub pin, the ELF gate found no BIND_NOW and the PE gate no CFG bit in
  `GraphicsEngine.exe`. The arm64 lane's staged gtest entries carry an empty `allowed_skips` (CON292), so a skipped
  test fails the run by name.
- `GoldenRender.TracedCameraRaysStartAtTheNearPlane`: an unlit card half the near distance in front of the eye, wider
  than the frustum there, shows in no mode, and a smaller one beyond the near plane fills the frame's centre in forward,
  ray-traced and path-traced frames; with the camera rays started at the eye, the traced frames showed the near card
  over the whole border. `BuildIntegrity.NoShaderCopiesASharedLeafModule` also fails a hand-written near- or far-plane
  unprojection, inline or through a clip-space point declared a statement before (the old cloud pass's), or the eye
  as a ray origin.
- `BuildIntegrity.NoShaderCopiesASharedLeafModule` fails a `.slang` file in either tree that keeps a private copy of
  a shared leaf module's constant, literal (3.14159, 0.045, 65000, an ACES coefficient, a ray bound), NaN check or
  function; each rule must also still match its owner, which caught the first draft's HDR literal pattern matching
  65500 instead of 65000. `NoVulkanShaderShadowsASharedModule` fails a local `.slang` named like any file in OxidANT's
  `shaders/`, since Slang imports by basename; `CloudAmbientAcesCurveIsAcesSlangs` holds `kAcesCoefficients` to
  `aces.slang`; `NoShaderRedeclaresTheFullscreenUvMapping` also fails a hand-written NDC-to-uv.
- FXAA for the C++ renderer (plan C3): post renders into a UNORM LDR intermediate per swapchain image, and a final
  `fxaa` stage runs OxidANT's `fxaa.slang` `fs_main` (behind post's `SV_VulkanVertexID` vertex) into the swapchain
  image, then the GUI. The Renderer inspector's FXAA control picks Off, Raster (the forward and deferred frames) or
  Always, the default, since forward has no MSAA. The `Fxaa` timed pass is the Rust renderer's name; it holds the GUI
  while it runs. The golden harness pins FXAA off; `GoldenRender.FxaaSmoothsTheRasterStaircaseAndLeavesFlatRegionsAlone`
  turns it on.
- `Integration.SharedSlangKernelsMeetTheirVectors` (`sharedShaderVectorsSuite.cpp`, plan X3): every vector file in
  OxidANT's `shaders/tests/vectors/` runs its kernel's SPIR-V once per case on a headless Vulkan compute device and
  checks each component against its f64 reference and tolerance; a miss names module, case and component. It loops
  over the files, so OxidANT's next module needs no edit here. OxidANT's `shader_vectors.rs` checks the WGSL of the
  same kernels against the same files. `lsanSuppressions.cpp` also names `glfwInit`: a test that only probes GLFW, as
  this one first did, finds libX11 unloaded by exit, so its known leak shows without an X11 frame.
- `VK_EXT_debug_utils` whenever the loader lists it, not only with the validation layer (plan X8), so the
  command-buffer labels reach Profile and Release captures. `debug::nameObject` names any vulkan.hpp handle: the
  per-pass targets (the forward, deferred and G-buffer attachments, the FXAA intermediate, the path-tracing
  accumulation, the traced depth) through the attachment helpers' new name argument, every graphics pipeline through
  `PipelineBuilder::setDebugName`, the compute pipelines after their SPIR-V entry (`clouds.clouds_main`) and the
  ray-tracing pipeline. Naming every other object is left for later. `VulkanInstance(false)` creates an instance
  without the layer, which `Integration.DebugLabelsAndNamesWorkWithoutTheValidationLayer` labels and names through.
- `perfTestSuite` times each bridged OxidANT entry through cxx and `guarded()`; the Rust Criterion half is OxidANT's:
  `BM_BridgeDefaultSceneSettings`, `BM_BridgeSettingRanges`, `BM_BridgeSanitize` (a NaN and a far value, so both of
  its branches run), `BM_BridgeParityViewportRemap`, `BM_BridgeParityLens`, `BM_BridgeParityModelTransform` and
  `BM_BridgeLoadGltfFlatWith` (`cube_textured.gltf`, the bridge call without `GltfLoader`'s copy into engine types).
  Their `win-9070xt-32core.json` rows are labelled placeholders from the aarch64 WSL Profile build (5-32 ns, 14 us
  for the load) until the RX 9070 XT host refreshes them.

### Changed

- **Textures are decoded in Rust, off the render thread** (BACKLOG "Texture ingest in Rust" steps 1-2, plan B5;
  OxidANT pin `d83dca1`). The bridge hands every texture slot over as a full RGBA8 mip chain, level 0 first and packed
  back to back, which `Texture::createFromLevels` uploads in one submit without a blit (a level over
  `maxImageDimension2D` is skipped, and a chain none of whose levels fits gets the default texture). OxidANT decodes
  PNG, JPEG, BMP, TGA, GIF, PNM and Radiance HDR on up to eight cores of the parse thread under a 1 GiB budget of
  decodes in flight and box-filters them with the WebGPU renderer's filter, so both renderers sample the same chains;
  channels expand and HDR is gamma-encoded as stb did. A `KHR_texture_basisu` KTX2 is preferred over the core image
  and transcoded level by level, a short KTX2 chain completed by halving, so a basisu-only glTF, which drew
  untextured, is textured: `GoldenRender.BasisuOnlyTextureDrawsItsRampInEveryMode` reads OxidANT's UASTC ramp across
  the cube in forward, deferred and ray-traced frames, and `GltfFixtureRoundTrip` its six levels. An image over
  16384x16384 pixels, read from its header, gets no slot; `Texture::createFromMemory`'s stb is left with PSD and PIC
  under the same cap, and anything else that does not decode gets no slot either instead of reaching stb.
  `Colosseum-gltf`'s 69 images make 760 MB of chains, held from the parse until the upload; OxidANT's `load_flat`
  peaks at 1.28-1.30 GiB resident there (745 MiB with the encoded files) and takes 1.44-1.49 s on 8 cores, against 0.8-1.1 s, before
  the render thread decoded each image with stb and blitted its mips. `GltfLoader::getTextures()` replaces
  `getTextureImages()`; `VulkanBufferManager::copyImageBuffer` takes a mip level and a buffer offset;
  `planChainUpload` (`scene/TextureFit.hpp`) picks the levels that fit (`TextureLoadUnit.AChainPlan*`). The bridge's
  crate budget moves from 56 to 60: GIF's `gif`, `weezl` and `color_quant` (owner), and OxidANT's new
  `kataglyphis_bridge_support`.
- **One OBJ reader, OxidANT's** (BACKLOG "One OBJ reader, in kataglyphis_assets", plan B6; OxidANT pin `7d1bea4`).
  `GltfLoader` loads `.obj` too: the bridge's `load_gltf_flat` dispatches it by extension to
  `kataglyphis_assets::obj::load_obj_flat`, the reader the WebGPU renderer opens OBJ through. It is tolerant like
  tinyobjloader (a malformed line is skipped and logged, a number that is not finite reads 0, relative indices
  resolve, a face over 2048 corners is skipped, only a file without a drawable face fails), splits polygons as
  tinyobjloader did, makes one mesh per `usemtl` run, ended by its `o`/`g` group or its material (owner
  2026-11-24), and hands every texture over as the file's bytes for stb, so BMP and TGA reach the engine without a
  re-encode; `map_d` crosses as `GltfMaterial::alpha_texture`. Every `mtllib` and texture is confined to the OBJ's
  folder like a glTF's buffers and images (owner 2026-11-24), and a texture resolves beside its `.mtl`. A corner
  without `vn` takes its own face's flat normal. The owner's material rules (2026-11-19) apply: `d` below 1 or `Tr`
  above 0 blends, a `map_Ke` without any `Ke` glows at factor 1, no `Kd` (or no `.mtl`) is white, no `Pr` and no
  `Ns` is roughness 1. Before the switch `ObjLoaderDiff` compared tinyobjloader's `ObjLoader` with the bridged
  reader triangle by triangle on all 33 OBJ files under `Resources/Models` with every AssetANT scene fetched, plus
  OxidANT's mask card: equal triangle counts everywhere; the differences were the owner's rules, tinyobjloader's
  aarch64 build fusing its quad-diagonal test (440 of Wolf-Stahl's 338794 triangles split along the other
  near-equal diagonal), `ObjLoader` sharing a corner without `vn` between the faces of a group, so it took another
  face's normal (CornellBox's boxes, sibenik, the Sulo v2 lamp; both readers' first versions did), and its merge of
  equal-valued vertices. Mesh counts: Sponza 385 (was 373 per group), Dinosaurs 3, sibenik 1087 (was 1).
- The ANTfrastructure pin moves 734ab966 -> 04981676 (renovate-local.sh): IWYU's report helpers (CON295 (a)), PIE on a
  real target (CON297), quality-report without a docs table (CON298) and the sixth workflow convention, `run-filter`,
  which grades every `workflow_run` filter as a GitHub pattern and is armed from the start (BB: 0 findings).
- Hardening runs at the project's scope, not from `myproject_enable_local_hardening` (removed): from a function its
  `CMAKE_POSITION_INDEPENDENT_CODE` reached no target, so the Linux programs were PIE only by clang's default. Now
  `GraphicsEngine` compiles with `-fPIE` and links with `-pie` beside `-z relro -z now`.
- The static-analysis job runs IWYU over the units without modules (`run-static-analysis-format.sh --only-iwyu`,
  `logs/code-quality/iwyu.txt`): suggestions are reported, a unit IWYU cannot read fails the step (35 units today, 33 with suggestions); the wrapper
  also fails a `fatal error:` line, which the hub's count of `: error:` misses (a missing header passed green).
- The Linux compiler caches are capped at `SCCACHE_CACHE_SIZE=1200M` (was 1500M): six keys held 9.49 of the
  repository's 10 GB beside CodeQL's 0.98, so a save evicted another lane's cache.
- spdlog and nlohmann_json add their headers as SYSTEM (`third_party/CMakeLists.txt`, a9333e7e; hub CON295 (c)
  step 1), so their 124 and 12 gcc warnings no longer count as this repo's.
- One set of defaults (BACKLOG "One set of defaults: the Sun, the wind and F5", plan I6): the C++ Sun is OxidANT's
  (radiance 5, warm white (1, 0.97, 0.92), towards (0.5, 0.8, 0.3), ambient 0.35; was 10, white, a light travelling
  along (-0.55, -1, -0.35) and no ambient), so `SceneSettingsDrift` compares it field by field.
  `directional_light_direction` holds the way towards the Sun, as OxidANT's `SunSettings`; `fillSceneUboDirectionalLight` turns it into the travel
  direction the shaders and cascades read (`sunTravelDirection`), a glTF Sun is published negated, and the Sun tab
  still shows and edits the way the light travels, as Rust's. The bridged Sun has its ranges in `setting_ranges()`
  and is sanitized like the other sections (clamped, NaN and infinity at the default, a zero direction straight up),
  which the drift suite holds to the editor's constants (`kMaxSunRadiance`, `kMaxAmbient`, `kMaxSunDirectionAxis`).
  The wind range was already ±100. F5, the File menu's Reload shaders and the Renderer tab's button reload the shaders
  in every build, not only Debug, as Rust's viewer. OxidANT pin `8d2bc42`.

- ANTfrastructure pin to `734ab966`, through `renovate-local.sh --submodule-path`: every Windows link passes
  `/NXCOMPAT /DYNAMICBASE /guard:cf` (CON290), so `GraphicsEngine.exe` gets the CFG bit, unset in both bundles of
  run 45001332269; every Linux link through `myproject_options` adds `-z relro -z now` (CON294), where the .deb of
  run 45001331996 shipped a PIE `GraphicsEngine` with RELRO but no BIND_NOW. The pin also brings
  `ctest-run.sh --fail-on-log-pattern` (CON291), `allowed_skips` for staged gtest entries (CON292), one `::error`
  per failed lint gate (CON293) and the ELF and PE hardening gates (CON294).
- `run-ctest.sh --virtual-display` fails a test that logs `SYNC-HAZARD` or `VUID-` through the hub's
  `--fail-on-log-pattern` (CON291), naming it; its own scan, now gone, counted `SYNC-HAZARD` alone and passed a ctest
  log holding a VUID. `Invoke-SyncValidation.ps1` fails on `VUID-` too, with a Pester case. `Invoke-LavapipeTests.ps1`
  already failed on both.
- OxidANT pin to `d0b6725`: a skin whose joints mirror space keeps its triangles' fronts out (`d0b6725`): `load_flat`
  reverses a triangle whose blended joint matrix has a negative determinant at two or three of its corners before it
  bakes the rest pose, where only the tangent sign used to flip, so such a skin drew inside out wherever back faces
  are culled. `GltfFixtureRoundTrip` takes OxidANT's `skinned_mirrored.gltf` (two single-sided cards, the left one
  placed by a joint scaled -1 in X), whose fact holds every triangle's face to its normals and two triangles left of
  the origin. No shipped scene has a skin, so no frame changes. The pin also brings the Rust renderer's VSync switch
  (`9da66be`, on by default and Fifo, as this engine's).
- OxidANT pin to `e817d6c` (plan I12): its WebGPU passes are target-neutral Slang (the IBL bake takes `fullscreen_vs`,
  whose uv a SPIR-V build of its own triangle would have mirrored; `ssao` and `gpu_cull` already took
  `fullscreen_ndc_to_uv`), and `histogram.wgsl`, the last hand-written WGSL, comes from `histogram/histogram.slang`
  through `Atomic<uint>`. The shader-targets table lists it; `ShaderSharingDocMatchesTheManifestTargets` no longer
  special-cases it, and `NoShaderRedeclaresTheFullscreenUvMapping` fails a `fullscreen` importer that builds its own
  triangle. The histogram's NaN check is `hdr_safe.slang`'s new `is_finite`, which reads the exponent bits, where
  the hand-written WGSL compared the EV with itself; `NoShaderCopiesASharedLeafModule` fails such a self-comparison.
  This engine binds none of these passes, so nothing here renders differently.
- Traced camera rays start at the near plane (owner 2026-11-19), through OxidANT `f2360d5`'s `camera_ray.slang`:
  `raytrace.rgen`, the path tracer's camera and cloud-depth rays and the cloud compute pass take
  `camera_ray_through_pixel`/`camera_ray` over `scene_types.slang`'s `clip_to_world(GlobalUBO)`, the product of the
  two inverses the UBO already carries, so the UBO layout and its gates stay as they were. A camera ray runs from the
  near plane (`TMin` 0) to the far plane (`TMax` its length) instead of from the eye (`TMin` 0.001) to 10000, as
  raster clips; later path segments keep `TRACE_TMIN`/`TRACE_TMAX`. A frame changes only where geometry lies closer
  than the near plane or beyond the far plane; no golden's does. The ray-generation shader's and the path tracer's
  `uv * 2 - 1` and the clouds' texel-to-NDC go through `fullscreen_uv_to_ndc`.
- OxidANT pin to `2d604b4` (shared Slang leaf modules, plan I8): `common/constants.slang` (`PI`, `MIN_ROUGHNESS`),
  `common/hdr_safe.slang` (`HDR_MAX`, and `hdr_safe` with the bit-test NaN check WGSL needs) and
  `common/trace_constants.slang` (`TRACE_TMIN`, `TRACE_TMAX`, `TRACE_SURFACE_OFFSET`, `TRACE_LIGHT_SPAN`) are OxidANT's,
  `aces.slang` gains `aces_tonemap_inverse` (`sky.slang`'s former `untonemapped`) and `fullscreen.slang`
  `fullscreen_ndc_to_uv`. This repo's `common/hdr_safe.slang` is gone, so `import hdr_safe;` reaches OxidANT's;
  `material_rules.slang`'s `MIN_MATERIAL_ROUGHNESS`, the path tracer's furnace 0.045 and every `0.001`, `10000.0`,
  `0.0001` and `1 - 1e-4` ray bound in `raytrace.rgen`, `raytrace.rchit` and `path_tracing` take the shared names.
  `CloudAmbient.hpp`'s CPU ACES pair reads one `kAcesCoefficients`. Every value is the one the copies held, and
  `isnan` and the bit test agree on every float, so no frame changes. The pin also brings OxidANT develop's `url`
  2.5.9 for RUSTSEC-2026-0592 (`ac40e7b`) and the CON288 work of the text and inference crates (`c10095f`,
  `5bf5639`, `c881224`), which the bridge does not build.
- OxidANT pin to `5551f29`, on OxidANT develop's `e3f554f`: glTF files with `KHR_mesh_quantization` (gltfpack's
  integer positions, normals, tangents and texture coordinates) and posed skins load through the bridge; `load_flat`
  bakes a skin's rest pose (joint world times inverse bind) and leaves unweighted vertices in mesh space; a file
  without the extension reads u8 and u16 texture coordinates normalized whatever their flag, as before.
  `GltfParseUnit.QuantizedFixturesArriveAsTheirFloatTwins` reads OxidANT's quantized cube and skinned bar against
  their float twins (positions within 2e-4, normals 1.5e-2), and `GltfFixtureRoundTrip` takes both fixtures. No
  shipped scene has a skin or quantized attributes, and `skinned_bar.gltf`'s rest pose is its bind pose, so no frame
  changes.
- The C++ shaders evaluate the GGX energy fit once per shading point (OxidANT `d0433b1`'s `brdf_direct`,
  `brdf_sampled`, `brdf_smith_k` and `sample_bounce` take it as `energy`): `rasterizer.slang` and `deferred.slang`
  once per pixel for the sun and every punctual light (`punctual_lighting` takes it too), `raytrace.rchit.slang` once
  per hit, `path_tracing.slang` once per path vertex for both NEE lights and the bounce. Each passes
  `ggx_energy(dot(N, V), roughness)`, the value `brdf_smith_k` computed itself, so no frame should change.
- `Compare-RendererTimings.ps1` compares the Rust renderer's scopes as this engine's (OxidANT `5e1d194`): its point
  and spot light cubes time as `PunctualShadows`, apart from `ShadowCascades`; its `Main` spans whichever of
  `Forward`, `GBuffer` + `DeferredLighting`, `RayTrace` or `PathTrace` drew; its traced kernels' scopes run through
  their resolve, as this `RayTrace` spans its sky fill, so `TraceResolve` is gone. `$RustOptionalPasses` takes
  `PunctualShadows` (a scene without a point or spot light records none) instead of `TraceResolve`, and `Histogram`,
  which the Rust dump's default manual exposure never records, so a real dump failed validation; `Main` is required.
  The Pester suite's Rust fixture follows, a dump without `Main` fails validation.
  `BuildIntegrity.GpuTimingMatchesTheRustRenderersTaxonomy` keeps `Post` and `CloudUpsample` as the only C++ scopes
  and checks that every shared scope nests in the same parent as the Rust `TimedPass::nested_in()` says.
- The C++ sky goes into the HDR scene target (plan C2): OxidANT's `sky/sky.slang`, through its new `vs_vulkan_main`,
  draws at depth 1 under LessOrEqual, forward between the opaque and BLEND draws, deferred after its lighting pass, and
  in a ray-traced frame wherever the primary ray missed (alpha 0, where `tracedDepth` is 1); a path-traced frame keeps
  its own camera miss. Post tonemaps the sky with the scene, a BLEND surface composites over it in linear HDR, and post
  overwrites the swapchain image instead of blending over a sky pass. The cube map enters as the radiance whose ACES
  shows the photo (`untonemapped`), so the displayed sky is unchanged; a half-transparent card over the navy sky now
  holds red 0.5 and blue half the navy's radiance in the HDR target, where it held blue 0 at alpha 0.5. The `Sky`
  timed pass is gone: `Forward`, `DeferredLighting` and `RayTrace` include the sky, as the Rust renderer's do.
- OxidANT pin to `3b813d2`, on OxidANT develop: the C2 sky entry, the C3 FXAA docs, N1's
  multiple-scattering GGX compensation, X3's vector kernels, and the Rust renderer's FXAA default moving to Always as
  here. Its review moved two more things in: `brdf.slang`'s Smith remaps floor the roughness at `MIN_ROUGHNESS` as the
  NDF does (a white metal at roughness 0 returned 1.21 of a uniform sky at n.v 0.01; every caller here clamps first,
  so no frame changes), and OxidANT's manifest builds the sky's and FXAA's SPIR-V entries this engine draws, so this
  manifest drops its three rows for them and compiles them through the import. `GoldenMetrics.hpp`'s quadrature
  floors its remap the same way.
- The furnace goldens prove what they look at: their checks expect the sky's 1, which a card the rays miss shows too.
  Each card view of `PathTracingPassesTheWhiteFurnaceTest` and `PathTracingPassesTheWhiteMetalFurnaceTest` first
  path-traces one segment, where a hit stays black (no light, no bounce) and a miss shows the sky, and its centre
  square must stay below 0.01; the rig's crop must be at least half covered by geometry there (measured 0.748). With
  the card or the rig out of view the controls fail and the old checks pass.
- `Integration.SharedSlangKernelsMeetTheirVectors` opens its compute device without probing GLFW, through
  `GpuTestGuard.hpp`'s new `SKIP_WITHOUT_HEADLESS_DEVICE`, which
  `BuildIntegrity.EveryGpuTestGuardsThroughTheSharedHeader` accepts beside `SKIP_WITHOUT_GPU`. It fails a kernel whose
  SPIR-V `LocalSize` is not 1x1x1 before dispatching one invocation per case, waits for the device after a fence
  timeout before destroying anything, and fails a short or unmapped read-back.
- `gpu-timing-budgets.json`'s `Post` budget follows its own rule, 1.5 times the slowest run: 5.7 ms (was 5.6).
- Every BRDF of the C++ engine compensates the GGX lobe's lost multiple scattering (plan N1, OxidANT's `brdf.slang`):
  the specular term gains `1 + f0 (1 / E_ss - 1)` and the diffuse term takes `1 - E_spec(mu)` instead of `1 - F`, in
  the forward, deferred, ray-traced, punctual and path-traced shading alike. A white surface in a uniform sky returns
  that sky: the furnace goldens expect 1 within 0.01 where they expected the single-scatter albedo (0.671 for a white
  metal at roughness 0.7 head-on), the metal adds roughness 1 (single scatter 0.307), and the path-traced furnace rig
  goes back to roughness 0.5 (owner decision 2026-11-21) with bounds 0.994-1.005 (measured 0.9997; a furnace albedo of
  0.99 or 1.01 gives 0.9889 or 1.0106). `GoldenMetrics.hpp`'s quadrature is `white_single_scatter_albedo` and only
  logged. `EmissiveMaterialBrightensTheFrame` compares the emissive card with its own twin without emission (the
  metallic card it used brightened to 226 against the emissive 233), and `PathTracingHonorsTheUntexturedMetallicFactor`
  bounds the metal at 0.9 of its dielectric twin instead of 0.8 (measured 0.824; an unread metallicFactor gives 1).
  `Compare-RendererTimings.ps1`'s note on the Rust FXAA default follows the pin.
- A develop run finishes instead of being cancelled by a newer push (owner D9, 2026-11-21): 27 of the last 50 x64 runs
  had been cancelled, many after 70 to 92 minutes, and left their commits without a verdict. Runs of other refs still
  cancel.
- cppcheck is off by default (owner D10): the image carries no binary, so ON was skipped without a word.
- The exception-free filesystem rule moves from BuildIntegrity to the `throwing_filesystem_calls` pattern row of the
  new `quality-report.toml`, which the hub's lint gates bind with max = 0. The BuildIntegrity check could not fire for
  `create_directories(` or `directory_iterator(`, whose names contain "ec"; the row catches the one-argument throwing
  overloads, the iterator declaration and a bare `current_path()`, and `test-filesystem-pattern.sh` with a mutation
  recipe proves it.
- A glTF's default morph pose reaches the C++ renderer (plan B2): OxidANT's `load_flat` blends the node's `weights`,
  else the mesh's, into the vertices before world space, so `cube_morph.gltf` (mesh weight 1.0, no animation) draws
  lifted by its target here as in the Rust renderer, which keeps blending from the neutral pose itself.
  `GltfParseUnit.DefaultMorphWeightsArriveBakedIntoThePositions` reads the lifted cube through the bridge. OxidANT pin
  to `7951bd0`, on OxidANT develop's `a02dcce`.
- `GoldenRender.MultiPrimitiveGltfLoadsAsMultipleMeshes` checks its pixels: face-on to `two_primitives.gltf`, the left
  quad's mean radiance must be red-dominant and the right one's green-dominant (each channel over twice the others). The
  mesh count alone passed with the in-place upload's index re-base zeroed.
- VSync is on by default (plan X15, owner 2026-11-19), as the Rust renderer and the browser present: the swapchain
  takes Fifo, where it took Mailbox whenever the surface offered it. `GUIRendererSharedVars::vsync` is the choice, in
  the Renderer panel's new DISPLAY section and the Render menu; off takes Mailbox, else Immediate, else Fifo, and a
  toggle rebuilds the swapchain. The golden harness pins it off; `GoldenRender.VsyncTogglesBetweenFifoAndAnUncappedMode`
  turns it on and off again (lavapipe under Xvfb offers Immediate, Mailbox, Fifo and FifoRelaxed). The Rust renderer
  keeps its fixed Fifo and has no switch yet.

### Removed

- tinyobjloader (`third_party/TINY_OBJ_LOADER`, its licence row) and `ObjLoader` with it, `computeTangents`,
  `fillMissingFlatNormals`, `sliceMeshRange`, `addMeshesForRanges`, `isGltfModelPath`, `vertexTangentSuite`, the
  `obj_parsing_fuzz_test` target (OxidANT's `tests/obj_flat.rs` proptest fuzzes the reader now), `BM_ObjParse_*`,
  `BM_ObjLoaderParseCpu_Suzanne`, `BM_ComputeTangents` and `BM_FillMissingFlatNormals`, and `obj_parsing_fuzz_test`
  from `reusable-linux.yml`'s fuzz-seed loop, whose stale entry `BuildIntegrity.EveryRegisteredFuzzTargetRunsInCi`
  now fails as it did for the Windows lists. `ObjParseUnit` tests the OBJ mapping through `GltfLoader`;
  `BM_GltfLoaderParseCpu_SuzanneObj` times the bridged parse (5.99 ms against `ObjLoader`'s 8.68 on `linux-profile-clang`, aarch64 WSL, medians of five).
- 66 `GltfParseUnit` cases that tested OxidANT's parser rather than the mapping into `Vertex`, `MeshRange` and
  `ObjMaterial` (plan X5): each has its twin in OxidANT's `crates/assets/tests/flat.rs`, 18 of them new there.
  `gltfParseSuite.cpp` keeps the 18 cases the C++ side decides (the loader's views and moves, the parse result, the
  sampler enums, shininess, the lights and their packing, the UTF-8 path, the `mask_card` fixture the goldens read,
  the parity of the generated tangents with `computeTangents`), and `GltfFixtureRoundTrip.*` every fixture's mapping.
- The four `GltfTest` fixtures only those cases read: `mask_card_hc`, `uv_transform_rotation_card`,
  `uv_transform_slots_card` and `vertex_colored_quad`.
- `Resources/ShadersSlang/tests/brdf_test.slang` and `noise_test.slang` and their manifest rows (plan X3): OxidANT's
  vector kernels of the same names replace them, and this engine's build compiles them through the imported manifest.
- `skybox.slang`, the swapchain sky pass and its `sky` stage, post's sky rebuild under the clouds and its ACES inverse,
  and post's cube map and shared-set bindings.
- The loader-frame leak suppression of 1.6.0 (`leak:setup_loader_term_phys_devs` in `lsanSuppressions.cpp`): with the
  llvmpipe run on lavapipe alone no GPU test uses it (run 44784592908 on runner image 20261116.202.1 listed only
  `libX11.so` under "Suppressions used").

### Fixed

- The Windows packages no longer ship googletest: its install rules put `include/gtest`, `include/gmock`, four
  `.lib`s, `lib/cmake/GTest` and `lib/pkgconfig` into the bundle, ZIP, MSI and NSIS of both arches (run 45176371019).
  `INSTALL_GTEST` is OFF, and `Build-Windows.ps1`'s *Bundle layout* step fails a bundle with `include/`,
  `lib/cmake/`, `lib/pkgconfig/` or a `.lib`, as `check-deb-layout.sh` fails a .deb. The Linux .deb, tar.gz and
  AppImage carried none of it.
- spdlog builds without LTO: GCC's LTO link of `GraphicsEngine` and `perfTestSuite` warned 8 times in its bundled
  fmt (`format.h:1209 -Wstringop-overflow=`, job 137728645504), which a SYSTEM include path cannot silence there.

- The Sun's colour holds [0, 1] for a typed value: the colour picker's RGB and HSV float fields took any number
  (ImGui's `ColorEdit3` has no clamp flag; only its hex field clamps), so -1 or 7 reached the light, and a saturation
  of 5 made green and blue negative. The Sun tab edits it through `studioColorEdit3`, which clamps each channel after
  a change. `StudioWidgetsUnit.AColourPickerHoldsItsRangeForATypedValue` opens the picker and types 0.25, -1 and 7
  into red and 5 into saturation; `BuildIntegrity.EditorColourEditsHoldTheirRange` fails an `ImGui::ColorEdit` or
  `ImGui::ColorPicker` call under `Src/` outside the widget.
- The Kompute playground's write-after-write hazard: `OpAlgoDispatch`'s own barrier makes `OpSyncDevice`'s copies
  visible to shader reads only, so the dispatch's storage write raced the copy into the output tensor (job
  136455950642). An `OpMemoryBarrier` from the copies' transfer writes to the compute shader's writes now sits between
  them. Kompute's `VK_EXT_debug_report` callback had logged the hazard at debug level without its `SYNC-HAZARD` id,
  so neither the test nor `run-ctest.sh`'s scan failed; the playground now registers its own callback on Kompute's
  instance and exits 1 on any validation warning or error.
- `retry-disk-refusal.yml` names its lanes as the glob patterns `workflow_run` reads: `[ci <lane>]` held characters a
  bracket may not, so every `workflow_run` event since 882d55de ended in a `startup_failure` of a workflow GitHub calls
  BuildFailed (run 45195400022 among 23), and the unescaped `+` of "build + test" matched no plus, so the x64 lane would
  never have been retried. Both are escaped now.
- A NaN or infinite shadow distance or split lambda takes the field's default in the cascade math, 60 and 0, as
  OxidANT's `sanitize` gives them (owner 2026-11-20): `CascadedShadowMapMath.cpp` fitted a NaN distance to the far
  plane and passed a NaN lambda through `std::clamp`, which also made +inf logarithmic.
  `CascadedShadowMapUnit.NonFiniteShadowDistanceTakesTheDefault` and `NonFiniteSplitLambdaTakesTheDefault`.
- An FXAA stage without its descriptor set or pipeline left the swapchain image unwritten and the GUI undrawn: post
  rendered into the intermediate, and FXAA returned early. `fxaaFilters` takes `FxaaStage::ready`, so post keeps the
  swapchain image then; `RenderModeUnit.FxaaFiltersRasterFramesOrEveryFrame` holds both.
- A double-click at frames slower than about 0.67 s (about 1.5 FPS): `Integration.LibraryDoubleClickLoadsTheRow`
  failed in the weekly riscv64 `raster` shard (run 44852183781). The editor sets ImGui's `MouseDoubleClickTime` every
  frame to 1.5 times the last two frames' time, since the down, up and down reach ImGui a frame each, but capped it at
  2 s, which two QEMU frames outlast. `gui/DoubleClickWindow.hpp`'s `doubleClickWindow` drops the cap (still at least
  0.3 s), and `DoubleClickWindowUnit.OutlastsTheTwoFramesBetweenThePressesAtAnyFrameRate` holds it. A test-side
  widening cannot work: the editor rewrites the value before every `NewFrame`.
- The GPU timings restart when new geometry arrives (the first load, a reload, `addModel`). The rolling averages and
  the `KATAGLYPHIS_GPU_TIMING_JSON` export held the about 240 sky-only frames a Debug run draws while Dinosaurs
  parses, against its six frames of the scene, so on llvmpipe Forward read 8-15 ms while the G-buffer, whose average
  starts at the mode switch, read 510-635 ms. Per frame of the scene both cost about half a second there (Forward
  525-545 ms, GBuffer 500-526 ms, DeferredLighting 11-16 ms), and a pipeline's first frame compiles it on llvmpipe
  (Forward 1070 ms, GBuffer 1181 ms, DeferredLighting 1356 ms once). The half second is ASan's: in the Profile build
  the passes cost 76, 66 and 10-11 ms, and ASan's runtime preloaded into it brings back the Debug numbers.
  `GoldenRender.GpuTimingJsonDumpIsWrittenAndSane`
  bounds the dump's frames by the frames drawn after the load. `gpu-timing-budgets.json`'s llvmpipe profile is
  re-derived from frames of the scene: Main 747.9 ms, Forward 741.2, Post 4.6, ShadowCascades 2066.6 (was 23.0, 22.3,
  5.7 and 35.9).
- Stale doc facts: the arm64 cross configurations, the Pester suites, the lint gates, the hub's `VULKAN_VERSION`, the
  local `cmake/` modules, the riscv64 shard counts and `Build-Windows-Container.ps1`'s parameter binding. The README's
  quick start builds in the CI image through `cmake-configure-build.sh`, which compiles the Slang shaders before the build.

## [1.6.0] - 2026-11-20

Every package (DEB, TGZ, AppImage, ZIP, MSI, NSIS, MSIX) now carries the original license texts of the shipped C/C++ libraries, Dear ImGui's fonts and the Rust crates the C++ bridge links, under `share/licenses/GraphicsEngine/`; the DEB gate and the MSIX check read them back.

Selected Windows MSIX builds now fail on packaging errors and verify the project and Sponza license texts in the finished artifact.

The installed Crytek Sponza scene now includes its full CC BY 3.0 license and a source notice.
The Linux DEB layout check now rejects a missing or altered project license.
The Windows MSIX uses the project logo instead of transparent placeholders.
The Windows GraphicsEngine executable embeds the project icon via the shared CMake helper.

### Added

- The settings drift test (plan B10 step 1): `SceneSettingsDrift.*` compares OxidANT's bridged
  `default_scene_settings`, `setting_ranges` and `sanitize` (`oxidant_bridge/settings.h`) field by field with
  `GUISceneSharedVars{}`, the editor's bounds and the engine's clamps (`clampCloudMarchSteps`,
  `clampCloudMeshScale`, `clampPcfRadius`, `shadowResolutionForIndex`). The editor's literal bounds became named
  constants beside `kMinShadowDistance` in `GUISceneSharedVars.ixx`, which `GUI.cpp` reads. The Sun is the one
  named difference, until BACKLOG "One set of defaults"; `BuildIntegrity.EveryGuiSceneVarIsComparedWithOxidantsSettingsOrExempt`
  makes a new scene var name its comparison or its reason.
- Cross-renderer parity (plan I5): `RendererParity.*` asserts `sanitizeViewportFraction`, `makeViewportRemap`,
  the projection `updateUniforms` builds and `makeGuiModelTransform` against OxidANT's `parity_viewport_remap`,
  `parity_lens` and `parity_model_transform` (`oxidant_bridge/parity.h`), Y flipped for Vulkan; and
  `FrustumUnit.ReachesEverySharedVerdictOxidantsFrustumReaches` reads the 24 verdicts of OxidANT's
  `crates/render_core/tests/vectors/frustum_verdicts.json`, which its Rust frustum test reads too.
- One glTF fixture set (plan X5): `OxidantFixtures/GltfFixtureRoundTrip.*` parses every glTF in OxidANT's
  `crates/assets/tests/assets` (morphs, skins, Basis, cameras, unlit, alpha) and checks that each reaches
  `Vertex`, `MeshRange` and `ObjMaterial` as the bridge returns it, the ranges tiling the arrays and every index
  re-based into its own range. `RepoFiles.hpp`'s `oxidantGltfFixture` finds them; the arm64 staging copies them
  and the frustum verdicts (`Arm64StagedShaderSources.Tests.ps1`).
- The bridge's crate graph is gated (plan X4): `cmake/CargoLicenses.cmake` passes the hub stager
  `--forbid-package` for wgpu, naga, winit, egui, tokio, gstreamer and ort, and `--max-runtime-packages 56`, the
  runtime packages measured on all five bridge targets; `BuildIntegrity.TheBridgeCrateGraphForbidsTheHeavyFamiliesAndHasABudget`.
- A skipped GPU test fails on Linux (plan X2): `run-ctest.sh --virtual-display` passes the hub's
  `--fail-on-skipped '^(GoldenRender|Integration)\.'`, so ctest's Skipped no longer counts as a pass; none of
  the 96 skipped on llvmpipe. The riscv64 GPU shards run their ctest through the hub's `ctest_run_main` with the
  same flag (`run-riscv64-tests.sh --gpu-ctest`, under Xvfb and the fp16 shim). `run-cargo-tests.sh` exports
  `KATAGLYPHIS_REQUIRE_GPU=1` and runs under Xvfb, as OxidANT's lane does.
  `BuildIntegrity.LinuxGpuRunsFailOnASkippedGpuTest` and `LinuxGpuRunsRequireTheGpuBeforeTheirVirtualDisplay`
  hold all three scripts to it.
- A second glTF parser behind `KATAGLYPHIS_GLTF_LOADER=rust`: OxidANT's loader, the WebGPU renderer's, called over
  the cxx bridge (`load_gltf_flat`) and copied into `GltfLoader`'s arrays by `scene/GltfLoaderRust.cpp`, so
  `adoptParsed`, `uploadParsed` and `AsyncModelParse` are unchanged. cgltf stays the default.
  `GltfLoaderDiff.*` parses every `GltfTest` fixture and every fetched glTF scene with both parsers and fails on
  any field that differs. The bridge's namespace is `kataglyphis::oxidant`.
- The Rust glTF loader is the default (owner decision 2026-11-18): `GltfLoader::parseCpu` runs OxidANT's
  parser unless `KATAGLYPHIS_GLTF_LOADER=cgltf`. cgltf stays for one round of every lane, then goes.
- `BM_GltfLoaderParser_CubeTextured` and `BM_GltfLoaderParser_Colosseum` time both glTF parsers on the same
  file. On aarch64 WSL the Rust parser takes `Colosseum-gltf` in 394 ms against cgltf's 481 ms (medians of
  eight), and steadier; the Colosseum row skips when the scene is not fetched.
- OxidANT moves to `0b79367`: the WebGPU renderer gets raster shadows for point and spot lights at parity with
  this engine's (four 512 px cubes, the same slots, depth range, face order and PCF), as layers of one 2D depth
  array so a WebGPU compatibility device samples them too.
- The skybox is Poly Haven's CC0 `kloofendal_43d_clear_puresky` by Greg Zaal, replacing the DOOM2016 faces,
  which carried no licence. It is an AssetANT release (`kloofendal_43d_clear_puresky-v1`, six 512 px faces, 0.5 MiB)
  that `Resources/skyboxes.lock` pins and every fetch extracts into `Resources/Textures/Skybox`; the packages install
  it with its `ATTRIBUTION.txt` and `LICENSES/`, and `share/licenses/` carries `CC0-1.0.txt` and its NOTICE.
  OxidANT moves to the matching rename (`SKYBOX_FACES`, `SKYBOX_DIR`).
- Without skybox faces (`-DKATAGLYPHIS_FETCH_ASSETS=OFF`, a failed fetch, a broken face) the engine bakes
  OxidANT's procedural sky into the cube map (`sky_model.slang`'s zenith-horizon-ground gradient,
  `scene/sky_box/AnalyticSky.hpp`) and logs a warning, where it drew black; the sky pass, the cloud composite,
  the cloud ambient and the path tracer's bounces all see it (owner decision 2026-11-18).
  `KATAGLYPHIS_SKYBOX_DIR` loads the six faces from another folder. `GoldenRender.WithoutSkyboxFacesTheSkyIsTheAnalyticGradient`
  and `BuildIntegrity.AnalyticSkyMatchesOxidantsSkyModel` hold both.
- The docs site's WebGPU demo shows the same sky: `docs-build-web.sh` stages the faces and their CC0 notice in
  `webgpu-demo/kloofendal_43d_clear_puresky/`, which OxidANT's demo fetches beside the page (owner decision
  2026-11-18, OxidANT `4417f31`); an unfetchable archive keeps its procedural sky with a warning.
- OxidANT moves to `0b9e638`: its glTF loader rejects accessors that made gltf 1.4.1 panic (a POSITION naming
  a missing accessor, a format its semantic does not allow, no elements, a range past its view). Each one, opened
  from the model picker, aborted the engine through the bridge. A proptest over mutated glTF files, which found them,
  replaces the cgltf fuzz target (`a_mutated_file_is_rejected_or_flattens_consistently` in OxidANT's
  `crates/assets/tests/flat.rs`).
- OxidANT moves to `398021a`. Its C++ bridge is a file per area (`gltf.rs`, `diagnostics.rs`; namespaces
  `kataglyphis::oxidant::gltf` and `::diagnostics`), returns errors, panics and log records as values, and drops
  the `rusty_cxxbridge_integer` probe. Its loader keeps only the vertices a primitive's indices use (primitives on
  one accessor each carried, and bounded, the whole of it), gives equal samplers one texture slot, reads the
  document, its buffers and its images as regular files only, a buffer inside the document's directory, and hands
  over the first directional light as the Sun. Its WebGPU ray-traced mode lights every punctual light, as this
  engine's closest-hit shader has since `e43eb33e`, and `march_clouds` loses its seven-argument overload, which
  leaves `sky_model.slang` to WebGPU alone. `GltfParseUnit.PrimitivesSharingAnAccessorEachKeepOnlyTheCornersTheyDraw`
  and `GltfParseUnit.EqualSamplersShareATextureSlotWhateverTheirIndex` hold this engine to the compaction and the
  slot key (against the old loader: 1000 vertices a mesh instead of 3, four slots instead of two).
- Raster shadows for punctual lights in the C++ renderer: the first four point and spot lights get a
  512 px depth cube each (one cube-array image, set 0's `PUNCTUAL_SHADOW_MAP_BINDING`), redrawn
  every frame by a new `punctual_shadows` stage, one six-view multiview pass per light through the
  cascades' caster shader, alpha test included. Forward and deferred shadow those lights with five
  2x2-PCF taps and a normal and light offset of a texel against acne; directional punctual lights and
  the lights past the fourth stay unshadowed, and ray and path tracing keep their shadow rays. A
  light without a range gets its far plane from the scene's bounds. The shadows inspector's Light
  shadows switch turns them off; the pass is timed as `PunctualShadows`, optional to
  `Compare-RendererTimings.ps1`. The GPU timing pool keeps five spare queries, as lavapipe writes a
  timestamp after a multiview pass to one query per view. A device without `imageCubeArray` is now
  rejected at selection, with the reason logged, as one without dynamic rendering is.
  `GoldenRender.PunctualLightShadowsTheCardOntoThePlane{InForward,InDeferred}`, `PunctualShadowUnit.*`.
- Punctual lights in the C++ renderer: `KHR_lights_punctual` point, spot and directional lights,
  read on every visited node (mesh-less ones too) and placed by their model's matrix, reach the
  shaders as set 0's `PUNCTUAL_LIGHTS_BINDING` (a count row, then OxidANT's four rows per light)
  and shade through OxidANT's shared `punctual_light.slang`. Forward and deferred add them
  unshadowed, as OxidANT's raster passes do; the closest-hit shader traces a shadow ray per light
  and the path tracer samples one light per bounce. `GoldenRender.PunctualLightsPoolOnThePlane*`
  (one per mode, on OxidANT's `point_light.gltf`) and `GltfParseUnit.ReadsPunctualLightsPosedByTheirNodes`.
- Half-resolution C++ clouds, OxidANT's switch: the sky inspector's Half resolution (on by
  default since 2026-11-16, as OxidANT's) marches a half-size target, each texel to the nearest depth of its 2x2
  pixels, and a new `cloud_upsample` stage fills the full-size one from its four nearest texels,
  weighted by OxidANT's shared `cloud_upsample_depth_weight`. The upsample is timed as
  `CloudUpsample`, optional to `Compare-RendererTimings.ps1`. On aarch64 llvmpipe the clouds cost 41 ms
  instead of 100 (25 the march, 16 the upsample) and stay within 0.013 levels on average, with no
  cloud on the geometry in front. `GoldenRender.HalfResolutionCloudsKeepOffTheSlabAndCloseToTheFullMarch`.
- `BM_RecordCommandsForward`, `...Deferred`, `...Raytracing` and `...PathTracing` time
  `record_commands` on the golden scene and camera, recording image 0 without submitting it.
  Without a display or Vulkan device (every CI lane) they report skipped, not an error; without
  hardware ray tracing the traced two skip.
- Wind for the C++ clouds: the sky inspector's Wind row (units per second, default 4, 0, 1.5 as
  OxidANT's) drifts the noise through the box by velocity times the app's time, wrapped in double
  to the 256-unit noise period, in `cloudLightMarch.yzw`. Tests leave the time at 0, where nothing
  moves. `GoldenRender.CloudWindMovesTheNoiseAndRepeatsWithItsPeriod`.
- `scripts/windows/Test-GpuTimingBudget.ps1` checks a GPU timing dump against a host's
  per-pass budgets in `Test/perf/baselines/gpu-timing-budgets.json`, in milliseconds and as a
  share of the frame. Local only: CI never gates on timings.
- Three goldens that say where a shadow lands and what post does with a texel.
  `ShadowFootprintMatchesTheAnalyticCaster` predicts the shadow rig's footprint per pixel by ray
  casting (two light tilts, one camera per cascade, the model moved and turned) and asks for 97 %
  agreement away from the edges; `WidestPcfLeavesLitGroundLit` keeps lit ground lit at the widest
  PCF radius; `PostEncodesEachHdrTexelThroughAcesAndSrgb` checks every opaque texel of one frame
  against ACES and sRGB of the HDR texel post read.
- Benchmarks for the engine's CPU half of a model load (`ObjLoader::parseCpu` on Suzanne,
  `GltfLoader::parseCpu` on the textured cube) and of an upload (`sliceMeshRange` plus the
  staging copy at 64, 4096 and 65536 quads). `BM_ObjParse_*` and `BM_GltfParse_*` now time the
  engine's own tinyobj and cgltf builds.

- glTF `alphaMode` `BLEND` composites in forward and deferred: BLEND meshes leave the opaque
  draws and are drawn last, farthest first by distance to their bounds' centre, alpha-blended
  with depth test on and depth writes off; deferred shades them forward after the lighting pass,
  as the Rust renderer does. Ray and path tracing still render BLEND opaque. Over the sky, post
  composites a BLEND surface at its coverage.
  `GoldenRender.BlendCardsCompositeInDepthOrderFromBothSides` checks two half-transparent cards
  over an opaque one from both sides, in both raster modes.
- glTF `TEXCOORD_1`: the vertex layout carries a second UV set (attribute location 5), the glTF
  loader reads it (TEXCOORD_0 is copied where a primitive has none, and always for OBJ), and each
  texture slot samples the set its `texCoord` names, `KHR_texture_transform`'s `texCoord`
  override included, in forward, deferred, ray and path tracing and the shadow pass. Generated
  tangents follow the normal texture's set.
  `GoldenRender.EmissiveOnTexCoord1FollowsTheSecondUvSetInEveryMode` checks an emissive texture
  on each set in all four modes.
- `GoldenRender.TracedSecondModelResolvesItsOwnMeshAndTextures` proves the ray and path tracers'
  per-model object index and texture offset: a second cube must show its own checker after a
  two-mesh first model whose texture is grey.
- `GoldenRender.TracedFramesFollowTheRasterModeAndTheSwapchain`: a ray-traced frame is unchanged
  by a switch to deferred mode and by a swapchain recreation.
- A docked editor replaces the floating ImGui window: menu bar with a renderer switch, scene
  outliner, a model library grouped by folder, an inspector per object (Model, Sun, Camera, Sky,
  Renderer), a profiler with frame-time and GPU pass graphs, and a status bar. A graphite theme
  follows the monitor's scale, F1 hides the editor, and the camera centres on the part of the
  window the docks leave free. A library click only picks a model; a double-click, Enter or Load
  opens it, since a load stalls the GPU and replaces the scene. A focused panel no longer swallows
  WASD; Esc first leaves the panel, then quits. The app window opens at 1600x900, scaled to the
  monitor.
- `BuildIntegrity.CppCloudsMarchThroughTheSharedModule` fails on a local copy of the cloud math;
  the cloud gates read `volumetric_clouds.slang`, the reachable-function gate follows imports of a
  module beside its importer (`forward/forward_lighting.slang`, `tracer/`), and the unused-import
  gate counts a module's constants as uses.
- Every test executable links the hub's crash handler (`kataglyphis_add_crash_dump`). The ARM64
  lavapipe job sets `KATAGLYPHIS_CRASH_DUMP_DIR` instead of WER `LocalDumps`, which the runner
  never wrote, and uploads the minidump with `commitTestSuite.exe` and its PDB (#97).
- Verify the Renovate wrapper and shared config in CI without applying updates.
- Shared CI quality reports, Ruff/ty analysis, PowerShell lint and complexity reporting,
  and a manual Python refactor preview. Generated reports stay under ignored `logs/`.
- Input-level tests for the editor's main flows (`editorInputSuite.cpp`): mouse and key events
  through the window's GLFW callbacks, one transition per frame, with each control found by
  ImGui's own hit test (`ImGuiInputDriver.hpp`). A library click picks without loading and Enter
  loads the pick; a double-click loads the row; the outliner's Renderer row opens its tab, whose
  culling switch reaches the renderer; View > Inspector closes the dock and an outliner row
  reopens it; the outliner's eye toggles the skybox; F1 hides and restores the editor.

### Fixed

- The llvmpipe GPU run loads lavapipe only: `run-ctest.sh --virtual-display` sets
  `VK_LOADER_DRIVERS_SELECT='*lvp*'` unless the caller names an ICD. On GitHub's `ubuntu-26.04` runner image
  20261116.202.1 another ICD found a DRM node and leaked a block and its threads' memory while the loader
  enumerated devices, which failed every x64 clang GPU test under LeakSanitizer (runs 44753690114,
  44761639606, 44770349057). `lsanSuppressions.cpp` also names the loader frame
  (`leak:setup_loader_term_phys_devs`) for the direct block.
- The cgltf `GltfLoader` gives a primitive without `NORMAL` a vertex per triangle corner, as glTF's flat
  normals need; a corner shared by two faces kept only the last face's normal. Normals and generated tangents are
  now finished in the mesh's space and then baked, as the Rust loader does, and a zero or non-finite authored
  `TANGENT` is replaced by a perpendicular instead of normalizing to NaN (3605 vertices of `Colosseum-gltf`).
- `BM_RecordCommands*` skip, instead of failing the perf gate, where they cannot draw: on
  `windows-11-arm`, whose Vulkan loader finds no driver (instance creation aborted the suite), and
  in the Windows container, where llvmpipe acquires no swapchain image (every mode reported
  `error_occurred`, run 43987654040). The site publish gets 90 minutes: the demo's 61 MB
  `Colosseum-web.glb` never finished in 30, so the site had not published since 2026-11-15.
- The C++ clouds are lit by the sky the engine draws: the DOOM2016 cube map's mean radiance over
  each hemisphere (sRGB-decoded and untonemapped, as the sky pass shows it, computed once per
  load from at most 64 texels per face edge), or the untonemapped navy with the skybox off,
  through the `SceneUBO`'s new `cloudAmbientTop`/`cloudAmbientBottom`. They were lit by OxidANT's
  analytic sky, a bright blue-grey under either. `GoldenRender.CloudsAreLitByTheSkyTheyDraw`.
- Shutting down after a lost device no longer releases the scene's and the editor's GPU
  resources on a destroyed VMA allocator: `App::run` skipped their cleanup on a lost device, the
  renderer then destroyed the allocator and the device under them, and a Debug build aborted in
  VMA ("Some allocations were not freed"). Every resource holds the device, which holds the
  instance and owns the window surface, so the device goes with its last user whatever the
  order; `App::run` tears down by scope alone, the renderer first, which drains the GPU, and has
  no device-lost branch. `VulkanRenderer::simulateDeviceLossForTesting()` makes every later
  fence wait report `VK_ERROR_DEVICE_LOST`;
  `Integration.SimulatedDeviceLossEndsTheRunAsAFailureAndTearsDownCleanly` and
  `Integration.TeardownAfterACleanRunReleasesTheDeviceAfterItsLastUser` tear down in
  `App::run`'s order and check when the device and the instance go.
- A model placed with a mirroring transform (a negative scale) renders right side out in the
  forward and deferred rasterizers, which cull it by its reversed winding now, and its normal map
  keeps its relief in every mode, since the bitangent follows the mirror.
- The cloud box is the axis-aligned box the inspector describes: Offset moves it with its
  clouds, and Scale and Density set its extent. It was a sheared slab that Offset could not
  move, because Slang fills a `float4x4(1.0)` with ones instead of making an identity.
- Clouds stop at the scene: geometry in front of the cloud box hides it, in raster, ray-traced
  and path-traced frames alike. Over the sky they composite in linear HDR before the tonemap,
  as over geometry, instead of being darkened by their alpha a second time after it.
- The path tracer honours a material's metallic factor without a metallic-roughness texture, and
  reflects through raster's GGX lobe, importance-sampled from the visible normals and weighted
  against the diffuse lobe by one-sample MIS: metals, and glTF materials at the default
  `metallicFactor` 1, are no longer black there. An unlit material ends its path at its albedo
  before metallic can darken it.
- Clouds in front of an unlit surface no longer vanish in ray-traced frames: the closest-hit
  shader recorded no hit distance for an unlit hit.
- On a double-sided back face a normal map's whole tangent frame flips, in all four shading
  paths; flipping only the normal mirrored the map's relief along the tangent.
- Ray- and path-traced hits take their front or back side from the triangle's winding, as raster
  does, not from the interpolated vertex normal, and a path-traced bounce that would leave
  through the triangle ends the path: near a smooth low-poly silhouette the vertex normal can
  face away from the camera while the triangle faces it.
- A metallic-roughness texel with a green channel of 0 no longer undoes the 0.045 roughness
  floor; in the path tracer the GGX density was 0/0 there.
- The path tracer traces its extra per-pixel ray for the clouds' depth only while clouds are on.
- Every write to an HDR target clamps radiance below fp16's maximum (65000, as OxidANT), so a
  strong emitter or a sharp highlight can no longer store +Inf and tonemap to a black NaN.
- `run-ctest.sh --virtual-display` and the riscv64 GPU shards start Xvfb with `-noreset`: a
  golden's GLFW probe left the server without clients, and a window opened during its reset
  found no display, failing one random GPU test now and then.
- In the editor, pressing Raster cancels a ray- or path-tracing pick that waits for a model,
  and a model the library does not list (an absolute `KATAGLYPHIS_MODEL_OVERRIDE`) keeps its
  name and its transform controls.
- The model library marks the model the scene actually loaded, also with
  `KATAGLYPHIS_MODEL_OVERRIDE` or when the default scene is missing, and a load resets the
  inspector's position and rotation to the identity the model is placed at.
- Ray- and path-traced frames skip the unused shadow-cascade pass, so the profiler no longer
  times one there, and the editor shows raster while ray or path tracing waits for a model.
- Root the locked formatter wiring check's `-ScriptPath` in the Windows Pester job; the hub harness resolved the relative path against itself and failed before any suite ran.
- Load ANTfrastructure's `CPackCommon` at the top level: first included from `Src/`, its include guard left the Linux closure's script paths empty, and every CPack package failed with `bash: : No such file or directory`.
- Format `cmake/SponzaLicenses.cmake` as the pinned cmake-format expects, which the Linux lane's format check rejected.
- Install the Windows CMake formatter from ANTfrastructure's locked uv tool project.
- Install the documentation environment from a Renovate-managed `uv.lock` through the shared uv helper.
- Reuse ANTfrastructure's test-directory fixture for PowerShell performance-baseline tests
  while keeping suite-scoped cleanup.
- Keep per-test ARM64 lavapipe stdout, stderr and gtest JSON in ignored `logs/` and upload them on
  failure; report the child exit code and JSON state so intermittent crashes can be diagnosed.
  Fail the evidence upload when its files are missing, so a failed test cannot lose its diagnostics silently.
- Run PowerShell lint through ANTfrastructure's reusable workflow and bounded pinned-module installer.
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
  on the test Pi's desktop (Debian 13, v3dv).
- **The `.deb` puts the app in the desktop's menu, and ships no GLFW development files** (2026-10-18).
  The desktop entry, the AppStream file and the icons were installed to a relative `usr/share/`, which
  the `.deb`'s `/usr` prefix made `/usr/usr/share/...`, where no desktop looks; they go to `share/`
  now, so the `.deb` has them in `/usr/share/` and CPack's AppImage still finds the entry by name.
  GLFW's install rules put its headers, `libglfw3.a`, its CMake package and `glfw3.pc` into every
  package (`/usr/include/GLFW`, `/usr/lib` in the `.deb`, a clash with `libglfw3-dev`);
  `GLFW_INSTALL` is off, the engine links it statically. `scripts/linux/check-deb-layout.sh` reads
  every lane's `.deb` after packaging (it fails on the 1.5.2 one with all four faults), and its
  synthetic-deb suite runs with Linux x64.
- A shader reload (F5) restarts the path tracer's running mean, as OxidANT's does: the reloaded kernel's
  samples were averaged into the old kernel's mean. The editor's PATH TRACING section shows the mean's
  samples per pixel ("Accumulated", from the new `RendererFeedback::pathTracingAccumulatedFrames`).
  `GoldenRender.ShaderHotReloadRestartsThePathTracedMean`.
- The frame clock is a double with an injectable `frame_delta`, like OxidANT's `FrameClock::tick_at`: a float
  clock rounded the camera's frame delta to 15.6 ms after 36 hours of uptime and to 0.125 s after 12 days. A NaN
  delta becomes 0, and a non-finite light direction or GUI model transform field falls back as in Rust.
  `FrameInputUnit.TwelveDaysOfUptimeStillGiveEqualFrameDeltas` and its NaN, light and transform twins.
- glTF's bare `NEAREST` and `LINEAR` min filters read the base level only (a nearest mip, the LOD clamped to
  [0, 0.25], no anisotropy), as OxidANT does; C++ minified them through the whole mip chain.
  `GoldenRender.AMipmaplessMinFilterSamplesOnlyTheBaseLevel`, `GltfParseUnit.MipmaplessMinFiltersReadTheBaseLevelOnly`.
- A texture larger than the device's `maxImageDimension2D`, or a skybox face larger than `maxImageDimensionCube`,
  is halved on the CPU (a 2x2 box, sRGB averaged in linear space) until it fits, with a warning, as OxidANT
  starts at the first level that fits; `vmaCreateImage` failed and `vkCheck` aborted the engine. A model under
  `Resources/Models` or `KATAGLYPHIS_SKYBOX_DIR` reached it. `TextureLoadUnit.*`, `SkyBoxUnit.FacesPastTheCubeLimitAreHalvedUntilTheyFit`,
  and `BuildIntegrity.OversizedTexturesAreFittedBeforeTheirImageIsCreated` for the two call sites.
- Six equal but non-square skybox faces fall back to the analytic sky; they made a cube-compatible image with
  width != height, which Vulkan forbids. `SkyBoxUnit.RejectsSixEqualNonSquareFaces`.
- Every material texture index goes through `NonUniformResourceIndex`, and the device enables
  `shaderSampledImageArrayDynamicIndexing` where it is offered: a multi-material OBJ shape, a ray hit and a path
  bounce index the arrays per invocation, which is undefined without the decoration.
  `BuildIntegrity.EveryMaterialTextureIndexIsNonUniform`, `BuildIntegrity.TheDeviceRequestsSampledImageArrayDynamicIndexing`.
- The furnace goldens' oracle floors the GGX roughness at 0.045 and guards `d * d` at 1e-20, as `brdf.slang`
  does; its 1e-6 guard on `PI * d * d` clipped every peak below a roughness of 0.154 (a white metal at 0.12
  integrated to 0.60 instead of 1.00). It integrates the GGX lobe over half vectors uniform in the lobe's CDF:
  its 320-step grid uniform in angle could not resolve a narrow lobe either, and gave a white metal 0.73 instead of
  0.97 at roughness 0.12 and mu 0.3, and 0.03 to 1.16 instead of about 1 at the floor. Latent, since the goldens
  use 0.25 to 0.7, where the two agree within 1e-4. It moved to `GoldenMetrics.hpp`; `GoldenMetrics.FurnaceOracle*`,
  `BuildIntegrity.FurnaceOracleUsesBrdfSlangsConstants`.
- A glTF's first directional light is the Sun, OxidANT's rule (owner decision 2026-11-18): the load publishes its
  direction and colour (`RendererFeedback::publishedSun`), `applyRendererFeedback` gives them to the editor's Sun,
  whose radiance stays the editor's, and it no longer reaches the punctual-light rows. The Colosseum's 4098 lx sun
  was shaded there as a second, unattenuated sun in forward and deferred and a second, shadowed one in the tracers.
  `GoldenRender.AGltfDirectionalLightBecomesTheSunInsteadOfAPunctualLight` on the new `GltfTest/directional_light.gltf`,
  `GltfParseUnit.TheFirstDirectionalLightIsTheSunNotAPunctualLight`, `RenderSettingsUnit.*Sun*`.
- A glTF loader panic comes back as a failed parse in Release and Profile too, as in Debug: their builds link the
  bridge with OxidANT's `bridge` cargo profile (release, `panic = "unwind"`; owner decision 2026-11-18), selected
  through Corrosion's `INTERFACE_CORROSION_CARGO_PROFILE`. Under the release profile's abort `guarded()` caught
  nothing, and a malformed file from the model picker ended the engine. `BuildIntegrity.NonDebugBuildsLinkTheBridgeWithAPanicThatUnwinds`.
- A model path with an umlaut loads on Windows: the bridge gets the path's UTF-8 bytes (`Shared::utf8PathBytes`),
  where it got the ANSI code page's and rejected them as not UTF-8. `GltfParseUnit.ParsesAModelUnderADirectoryWithAnUmlaut`.
- The Rust loader's warnings (dropped triangles, skipped primitives, unreadable images) reach the engine's log:
  `logOxidantRecords` drains the records OxidANT keeps per thread into spdlog after each bridged call, on the
  calling thread. `RustBridgeUnit.ALoaderWarningReachesSpdlogAndLeavesNothingBehind`, `RustBridgeUnit.AWorkerThreadLogsItsOwnLoaderWarnings`.

### Changed

- The bridge review's rules (owner decisions 2026-11-19 and 2026-11-20), with OxidANT `a7c4ebe`: OxidANT's
  frustum and draw cull take the engine's culling rules (an inverted or NaN box is visible, the planes are
  normalised with the 1e-4 margin, a plane without a normal is skipped), and `frustum_verdicts.json` gains the cases
  the two used to decide differently, 30 in all; `FrustumUnit.ReachesEverySharedVerdictOxidantsFrustumReaches` reads
  its new `matrix` frusta. `sanitize` holds every cloud field to the editor's bounds and puts a NaN or infinite
  cloud or shadow value back to its default (the shadow Strength became 1.0 before, not 0.65), which
  `SceneSettingsDrift.SanitizeHoldsEveryFloatToTheEditorsBoundsAndANonFiniteOneToItsDefault` checks against
  `GUISceneSharedVars{}`; both renderers' packing stays lenient, the step counts and the Scale and Density floors
  alone, since goldens on both sides march Density above the editor's top of 1. `default_scene_settings()` carries
  OxidANT's Sun, compared with the engine's as the named difference until BACKLOG "One set of defaults". An empty
  frame has aspect 1 in both renderers: `viewportAspectRatio` returns 1 for a zero width too (it returned 0, an
  infinite projection), and `RendererParity.LensMatchesOxidantsWithRowOneNegated` adds 1600x0, 0x768 and 0x0. An
  empty `KATAGLYPHIS_MIKKTSPACE_TANGENTS` counts as unset in the engine, as in OxidANT's viewer; it turned
  MikkTSpace on.
- OxidANT moves to `d44af00` (`kataglyphis_render_core`, the settings and parity bridges, load options, the glTF
  fixtures in `kataglyphis_assets`, images confined to the glTF file's folder and decoded from their bytes, the
  bridged crates without unsafe code or explicit panics, tests that run without the source tree on the arm64
  device) and ANTfrastructure to `092a1e20` (the stager's `--forbid-package` and `--max-runtime-packages`,
  `ctest-run.sh`'s `--fail-on-skipped` and its 1500 s default per-test timeout).
  `corrosion_add_cxxbridge` lists `settings.rs` and `parity.rs`.
- The glTF loader reads no environment (plan X14): `GltfLoadOptions` reaches OxidANT's `load_gltf_flat_with`
  through `GltfLoader::parseCpu`, `loadModel`, `AsyncModelParse::start` and `Scene`, and `App::run` reads
  `KATAGLYPHIS_MIKKTSPACE_TANGENTS` once (`gltfLoadOptionsFromEnvironment`). Tests ask for MikkTSpace through the
  options; `BuildIntegrity.TheMikkTSpaceVariableIsReadOnlyAtTheEngineStart`.
- The cloud wind drag spans ±100 per axis, as OxidANT's (owner 2026-11-19; was ±50).
- `Resources/Models/GltfTest/cube.glb` and `point_light.gltf`, byte-identical copies of OxidANT's fixtures, are
  gone; the tests read OxidANT's. `cube_textured.gltf` stays, because the editor library lists it and two
  goldens reload it through the library; `BuildIntegrity.GltfTestHoldsNoCopyOfOxidantsFixturesButTheLibrarysCheckerCube`
  keeps it byte-identical and refuses any other copy.
- `GltfLoader` keeps OxidANT's `GltfModel` alive through an untyped `std::shared_ptr` and uploads from
  views of it instead of a second copy (plan I13): `getVertices()`, `getIndices()` and `getMaterialIndices()` are
  `std::span`s over the bridge's `rust::Vec`s, `getTextureImages()` spans over the encoded images, which
  `Texture::createFromMemory` reads in place, and `addMeshesForRangesInPlace` stages each primitive straight from
  the parse, `Mesh` re-basing its indices into the staging memory (`rebaseIndices`, through
  `VulkanBufferManager::createBufferAndUploadOnDevice`'s fill callback). The OBJ upload keeps `sliceMeshRange`.
  On aarch64 WSL (`linux-profile-clang`) parsing `Colosseum-gltf` (`BM_GltfLoaderParseCpu_Colosseum`)
  peaks at 755 MiB resident instead of 1013 (getrusage's max RSS), its time within the run-to-run noise
  (medians 531-756 ms before, 559-736 after), and `BM_UploadCpuHalf/65536`, now the in-place staging, takes
  1.34 ms instead of 2.03 (14.3 against 9.4 GiB/s). A moved-from loader reads as unparsed, as after `adoptParsed`,
  instead of keeping views into the parse it gave away. `GltfParseUnit.LoadersShareTheBridgeArraysInsteadOfCopyingThem`,
  `GltfParseUnit.AMovedFromLoaderReadsAsUnparsed`, `BuildIntegrity.GltfLoaderUploadsTheBridgeArraysWithoutACopy`,
  `MeshRangeSlice.*`.
- One GPU-timing taxonomy with the Rust renderer (plan I5): `Main` now times, nested inside it, `Forward`,
  `GBuffer` and `DeferredLighting` (stamped around the G-buffer pass through `DeferredRasterizer::recordCommands`'
  new `afterGeometryPass` hook), `RayTrace` or `PathTrace`, the Rust `TimedPass` names, listed indented under Main
  in the profiler. The `KATAGLYPHIS_GPU_TIMING_JSON` dump names each nested pass's parent under `nested_in`, which
  `Test-GpuTimingBudget.ps1` leaves out of the frame a share divides by; `Compare-RendererTimings.ps1` requires
  `Forward` of the forward run. The profiler averages over 32 frames, as Rust does (was 30), and the stamps go
  through `writeTimestamp2` in `GpuTimingSubsystem::beginFrame`/`writeTimestamp`/`endFrame`.
  `gpu-timing-budgets.json`'s llvmpipe profile budgets `Forward` (15.3 ms, a 0.33 share) from five runs that fit
  every existing budget. `GoldenRender.GpuTimingSplitsMainIntoTheScopeThatDrew`, `GpuTimingExportUnit.*`,
  `BuildIntegrity.GpuTimingMatchesTheRustRenderersTaxonomy`.
- A failed Windows package step prints the tail of every CPack generator log (NSIS, WiX) from
  `_CPack_Packages`, through the hub's `Write-CPackGeneratorLog`; ANTfrastructure moves to `ff0235f5` for it.
- The Windows NSIS installers build again: since the glTF bridge, OxidANT's `profiling` license texts were
  staged under their whole repo path, and the NSIS source path reached 268 characters. The hub's
  `stage-cargo-licenses.py` now stages a workspace text as `workspace/<file name>`; ANTfrastructure moves to
  `d9379d1a`.
- A glTF material without `pbrMetallicRoughness`, and the fallback for a primitive without a material,
  take glTF's defaults in the cgltf `GltfLoader` (owner decision 2026-11-17): white, metallic 1, roughness 1,
  instead of the engine's 0.8 dielectric whose roughness came from `shininess`. The Rust loader already did; the
  two parsers now agree on every material.
- The C++ clouds march at half resolution by default (owner decision 2026-11-16; OxidANT flips
  its own default with it). The cloud goldens now grade the half path with unchanged thresholds;
  `HalfResolutionCloudsKeepOffTheSlabAndCloseToTheFullMarch` and
  `BuiltInStagesRecordInTheFrameOrder` still render the full march.
- A path-traced bounce that misses sees the sky the raster draws, as the Rust renderer's does:
  the skybox's cube map (or the navy with the skybox off) times 0.35, the Rust default ambient
  strength times environment intensity, which the C++ engine has no setting for. It was a fixed
  white-to-blue gradient at half intensity, which camera rays keep. Toggling the skybox restarts
  the running mean. `GoldenRender.PathTracedMirrorReflectsTheSkyboxItDraws` checks a mirror card
  against both skies; under the dimmer DOOM2016 sky, `AddedModelAppearsInPathTracing` and
  `PathTracedMaskCardShowsItsCutout` read past the noise in the shadow rig's slab shadow.
- `GoldenRender.GuiInputSweepNeverCrashesOrLosesTheDevice` runs in CI again, weekly: `linux-x64.yml`'s
  `gui-sweep` job (Sundays, or a dispatch with `gui-sweep`) builds the debug tree and runs only it on
  llvmpipe with sync validation, under a 90-minute ctest timeout; every push still excludes it.
- The Linux x64 lane uploads the gcc job's coverage to Codecov too, flagged `gcc` (the
  `gcovr.cfg` Cobertura report over `Src/`, from the CPU suites); clang's upload is now flagged `clang`.
- The C++ renderer records its passes through a stage registry
  (`kataglyphis.vulkan.render_stage_registry`): each pass is a `RenderStage` with an explicit
  order and record, skip, resize and shader-reload hooks, and `VulkanRenderer::addRenderStage`
  adds one without editing the renderer. Frames, pass names and GPU timing names are unchanged.
- The renderer no longer holds the GUI: `App::run` owns the settings the editor writes
  (`RenderSettings`) and the feedback it shows (`RendererFeedback`), and passes the settings to
  `VulkanRenderer` by const reference each frame. The one-shot requests and the inspector's reset
  after a load are applied by `applyRendererFeedback` after the renderer's update; frames render
  as before.
- `resolveModelPath` and the skybox lookup cache the `Resources` directories above the working
  directory, so a miss probes each of them once instead of every parent directory up to eight
  levels. Hits resolve to the same path as before; a chdir or a file created after a miss is
  still seen.
- Vulkan creation and allocation calls go through `vkCheck` (`common/Utilities.hpp`): it unwraps
  a `ResultValue` or checks a bare result and, as `ASSERT_VULKAN` did, logs critical and aborts on
  failure. All 40 call sites in 23 files moved and the macro is gone; `BuildIntegrity` rejects it.
- OxidANT moves to `7c81619`: the Rust renderer picks its adapter (discrete before integrated before
  CPU, `--adapter`/`KATAGLYPHIS_ADAPTER`, a Use button that relaunches the viewer), names it in a
  browser and hints at Windows' High performance setting on an integrated GPU, scales its internal
  resolution (Auto holds a frame-rate target) and offers Low/Medium/High/Ultra presets, Medium by
  default on a browser's integrated GPU. The web demo the docs build publishes carries all of it.
  RUSTSEC-2026-0543 (chrono unmaintained) is ignored there until a move to jiff or time.
- OxidANT moves to `fe273c9`: the Rust renderer marches clouds at half resolution by default, as
  the C++ one now does, and its debug build compiles the renderer crate at opt-level 1 (the path
  tracer's CPU BVH build drops from 16-21 s to under 2 s). No shader, crate or licence text changes.
- OxidANT moves to `ae01294`: its renderer inflates Zstandard-supercompressed UASTC KTX2 with
  `ruzstd` 0.9.1, bounded per level, instead of `basisu`'s `ruzstd` 0.7.3 (RUSTSEC-2026-0344, a
  decompression bomb). Only the wasm demo's staged crate texts move with it.
- OxidANT moves to `04f7a0e`. Its texture streaming evicts levels the camera no longer needs,
  copies resident levels on the GPU when a texture steps, and caps the CPU level chains it keeps
  (256 MiB interactive). Rust only: no shader, crate or licence text changes.
- OxidANT moves to `216b0a1`. Its Rust renderer draws every scene draw indirectly, from
  arguments a GPU draw cull writes (frustum and occlusion per draw, casters per cascade; a new
  `DrawCull` GPU timing pass, which `Compare-RendererTimings.ps1` requires of the Rust dump, as
  every raster frame with draws records it), and streams material mips by screen-space need. The
  WGSL-only `draw_cull/draw_cull.slang` joins the imported shader manifest; no crate is added, so
  the packaged and the wasm demo's licence texts are unchanged. The C++ engine renders as before.
- OxidANT moves to `026ef42`. Its Rust renderer records every frame through render graph v2
  (derived pass order, culling, pooled and aliased transient targets) and transcodes Basis
  ETC1S/UASTC KTX2 and glTF `KHR_texture_basisu` to BC7, ETC2 or ASTC; the docs site's wasm demo
  carries the two new crates' licence texts (`basisu`, Apache-2.0; `ruzstd`, MIT). The shared
  `brdf.slang` drops `geometry_smith` and `traced.slang` its dead `sky_model` import, so
  `BuildIntegrity.EveryImportedSlangModuleIsUsed` loses its exemption list and
  `tests/brdf_test.slang` calls `geometry_smith_k`. The C++ engine renders as before.
- OxidANT moves to `78303c2`. The C++ path tracer imports its shared `common/path_estimator.slang`
  (RNG, seed, Russian roulette, the bounce) instead of keeping copies, and a bounce weights by
  `brdf_sampled`, Epic's Smith remap for sampled light (`k = alpha / 2`): a white metal at
  roughness 0.3 returns 0.901 of a uniform sky at 60 degrees instead of 0.668. The furnace
  oracle follows the remap; the rig furnace runs at roughness 0.8, where the BRDF's directional
  albedo stays below 1, with bounds 0.977-0.988.
- PCF compares each tap at the receiver plane's depth beneath it (OxidANT's
  `common/shadow_pcf.slang`), so a receiver sloped against the light no longer shadows itself
  at larger radii: the shadow rig's darkened pixels at the default radius 2 drop from 21.1 % to
  7.8 % of the frame, with the shadow kept.
- The clouds take OxidANT's estimator: a sky ambient (the analytic sky for the C++ engine),
  three sun scattering octaves, exact in-scattering per step, up to four times the slider's
  steps on a long path, and a noise volume whose every channel tiles, sampled from its middle at
  the box centre. Scale alone sizes the box now; Density only thins the cloud. The default
  Scale becomes 630 x 3.15 x 630, the same box at the default Density.
- `Compare-RendererTimings.ps1` treats the Rust renderer's FXAA pass as optional.
- The C++ renderer requires Vulkan 1.3's dynamic rendering and synchronization2, and a device
  without either is skipped with the reason logged. Every pass (shadow cascades, forward,
  deferred geometry and lighting, sky, post with the editor) begins with `vkCmdBeginRendering`
  instead of a render pass and framebuffer, so a swapchain recreation rebuilds no framebuffers.
  Every barrier is a `vkCmdPipelineBarrier2` with explicit stage and access masks. The deferred
  lighting pass texel-fetches the G-buffer as sampled images instead of reading input
  attachments.
- The frame submit is `vkQueueSubmit2`, waiting on the acquired swapchain image at the stage
  the sky pass's first barrier starts from. Synchronization validation on the Linux and Windows
  GPU lanes also checks load operations after another pass's store and shader descriptor
  accesses, two hazard classes it skipped by default.
- Adding a model builds only that model's bottom-level acceleration structure and rebuilds the
  TLAS; the models already in the scene keep theirs. A reload still rebuilds, since it replaces
  the model.
- The ray-tracing descriptor sets are no longer rewritten every ray-traced frame: every change
  they depend on (the TLAS, the raster mode, the swapchain) already rewrites them.
- The device creates queues on the graphics and presentation families only; the compute family
  it also created one on was never used, since compute work runs on the graphics queue.
- The OBJ `map_d` cut-out samples through its own `alpha_uv()` accessor, which shares the
  base-colour transform, and `ObjMaterial::get_textureID()` is gone.
- OxidANT moves to its Rust renderer parity work (`eb97c364`): the Rust WebGPU renderer gets the
  C++ editor (docks, model library, inspectors, profiler), the fly camera, the Colosseum as its
  default scene, a deferred path, the C++ shadow controls and cube-map skybox, volumetric clouds,
  and compute-shader ray and path tracing. Its WebGPU demo starts on an embedded cube and fetches
  the Colosseum on request.
- The C++ clouds march through OxidANT's shared `common/volumetric_clouds.slang`, which the Rust
  renderer's clouds use too; `compute/clouds.slang` and `compute/noise_volume.slang` only bind the
  engine's resources to it. The behaviour is the C++ one, except that a ray direction with an
  exactly zero component (a sun straight overhead) no longer meets `inf - inf` in the box test.
- The shared `brdf.slang` NDF floors roughness at 0.045 and guards its denominator at 1e-20
  instead of 1e-6, which had clipped every specular peak below roughness 0.154: highlights on
  glossy materials get brighter and smaller in every C++ shading path too.
- `Resources/assets.lock` pins AssetANT's `Colosseum-gltf` and `Colosseum-web`. Neither is in
  `KATAGLYPHIS_DEFAULT_ASSETS`: `-DKATAGLYPHIS_FETCH_ALL_ASSETS=ON` fetches both, and the docs
  build fetches `Colosseum-web` for the demo, both from releases published on 2026-11-15.
- The docs build puts `Colosseum-web.glb` beside the WebGPU demo, with the scene's
  `ATTRIBUTION.txt` and `LICENSES/` in `webgpu-demo/Colosseum-web/` and the editor fonts' notice
  and texts in `webgpu-demo/fonts/`; without the archive the demo publishes without it, with a
  warning.
- `Compare-RendererTimings.ps1` treats the Rust passes a forward timing run never records
  (`GBuffer`, `DeferredLighting`, `Clouds`, `RayTrace`, `PathTrace`, `TraceResolve`) as optional,
  like `OcclusionCull`.
- The local Renovate run of 2026-11-14 moved the submodules to their default-branch tips: Dear
  ImGui `8c36ecfb` (its default arrows are now stroked chevrons, not filled triangles), stb
  `9600522d`, VMA v3.5.0, nlohmann/json `81888ff9`, google/benchmark `cf4d7fed`, spdlog
  `6d9805df`, FuzzTest `fcde2c27` (`main`; its Abseil `20260817.0` now equals `ABSL_TAG`), toml++
  `cfd71597`, OxidANT and ANTfrastructure. The docs stack takes Sphinx 9.2.0, myst-parser 5.2.0 and
  sphinx-design 0.8.0; `uv.lock` takes its lock maintenance and the docs theme's dynamic version.
- The Windows lanes run lavapipe 26.3.0 at each arch's native vector width: the hub drops the
  `LP_NATIVE_VECTOR_WIDTH=256` pin that BACKLOG suspects of the intermittent arm64 crash, since Mesa
  26.3 deleted the BVH sort that needed it.
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
- GPU tests share one guard, `Test/commit/VulkanEngine/GpuTestGuard.hpp`: with OxidANT's `KATAGLYPHIS_REQUIRE_GPU`
  set, `SKIP_WITHOUT_GPU()` and `SKIP_WITHOUT_FRAME_CAPTURE()` fail instead of skipping, and so does a golden's
  render helper through `frameCaptureAvailable` (the punctual-light and Sun goldens skipped on their own flag).
  `run-ctest.sh --virtual-display` and the riscv64 GPU shards set it, so a broken Xvfb or GLFW platform no longer
  turns the Linux GPU suites green. `BuildIntegrity.EveryGpuTestGuardsThroughTheSharedHeader` (which also refuses a
  test that asks a harness for frame capture itself), `BuildIntegrity.LinuxGpuRunsRequireTheGpuBeforeTheirVirtualDisplay`,
  `GpuTestGuardUnit.*`.
- The C++ bridge crate builds with Cargo's `--locked` (`corrosion_import_crate(... LOCKED)`).
- `--version` prints `GraphicsEngine <version> (Vulkan <API version>, OxidANT <version>)`, asking Rust, so the packages'
  `--version` smoke tests prove the bridge links and runs, and the About window gains an OxidANT row. A start no
  longer prints the bridge's 42 to stdout. `RustBridgeUnit.ReportsTheOxidantVersionItWasBuiltFrom` replaces
  `RustBridgeUnit.CallsIntoRust`. `Main.cpp` imports its modules below its includes: GCC 16 redefined the standard
  library when `<cctype>` followed the imports of `config` and `oxidant_diagnostics`, which the GNU preset's build
  stopped on; `BuildIntegrity.NoTranslationUnitIncludesAHeaderAfterAnImport` holds every .cpp to that order.
- `corrosion_add_cxxbridge` lists OxidANT's bridge files (`gltf.rs`, `diagnostics.rs`), each its own
  `oxidant_bridge/<area>.h`, included from .cpp files only. `BuildIntegrity.CxxbridgeFilesAreExactlyOxidantsBridgeFiles`,
  `BuildIntegrity.BridgeHeadersAreIncludedFromCppFilesOnly`.
- Docs no longer contradict the code: the modules both renderers share (eight of OxidANT's ten), `path_tracing.slang`'s
  `material_fetch` import, the clouds' eight-argument march, the path tracer's punctual-light NEE, the in-memory
  GLB's `map_d` cut-out, why `histogram.wgsl` is hand-written, and the hub helper that sets the bridge's `CXXFLAGS`.

### Removed

- The cgltf glTF parser: OxidANT's Rust loader is the only one (owner decision 2026-11-18, after every lane
  passed on it as the default). `KATAGLYPHIS_GLTF_LOADER`, `GltfLoaderDiff.*`, `gltf_parsing_fuzz_test`, the
  `BM_GltfParse_*` and `BM_GltfLoaderParser_*` benchmarks and the `third_party/cgltf` submodule go with it;
  `BM_GltfLoaderParseCpu_CubeGlb` and `BM_GltfLoaderParseCpu_Colosseum` join `BM_GltfLoaderParseCpu_CubeTextured`.
  The packages no longer carry cgltf's license text.
- Dead dependencies (owner decision 2026-11-18): the vendored `third_party/KTX` tree (64.7 MB, mostly test images
  and library sources; linked as headers that no source included), the `third_party/tomlplusplus` submodule (built with modules, imported by nothing; the packages
  no longer carry its license text) and the configure-time, unpinned `cargo install cxxbridge-cmd`
  (`_myproject_find_or_install_cxxbridge`, whose result nothing read: Corrosion uses a cxxbridge at the lock's
  `cxx` version or builds exactly that one). `vertex::computeFlatNormals` and `BM_ComputeFlatNormals` with its
  baseline rows go too; nothing but the benchmark called it.

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
