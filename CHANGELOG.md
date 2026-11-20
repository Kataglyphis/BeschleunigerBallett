# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Every push to `main` releases `v<VERSION.txt's version>` once every lane is green (`release.yml`),
with this file's `## [<version>]` section as the release notes, so before a merge raise
`VERSION.txt` and turn `## [Unreleased]` into that section. The notes of v1.4.2 and earlier are on
the GitHub releases themselves.

## [Unreleased]

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
