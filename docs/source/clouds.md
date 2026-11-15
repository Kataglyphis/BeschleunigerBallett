# Volumetric Clouds

The clouds subsystem (`Src/GraphicsEngineVulkan/scene/atmospheric_effects/clouds/`)
is the only one with a compute pair, a shared host/shader constants header, a
four-`vec4` `SceneUBO` block and its own GUI section. Everything here is
**shipped** behaviour; open work lives in `BACKLOG.md`.

## Pipeline shape

Two compute kernels, both Slang-authored and SPIR-V-only
(`Resources/ShadersSlang/compute/noise_volume.slang`, `compute/clouds.slang`). Both only bind
the engine's resources: the noise texel, the box, the density and the march are OxidANT's
shared `third_party/OxidANT/shaders/common/volumetric_clouds.slang`, which the Rust WebGPU
renderer's clouds (`clouds/cloud_noise.slang`, `clouds/cloud_march.slang`) import too, so the two
renderers' clouds cannot drift apart ([`shader-sharing.md`](shader-sharing.md)). The noise
reaches the shared code through its `ICloudNoise` interface; `clouds.slang`'s `CombinedNoise`
samples the combined `Sampler3D` with an explicit LOD. A local copy of the math fails
`BuildIntegrity.CppCloudsMarchThroughTheSharedModule`:

- **`noise_main`** runs exactly **once**, from `Clouds::init` via
  `dispatchNoiseGeneration`, filling a `kNoiseVolumeExtent`^3 3D storage
  texture (`RWTexture3D<float4>`) with the shared `noise_volume_texel`, which
  covers world-space `[0, 1)` once per axis. All four channels are written from independent Worley/value-noise
  terms: `clouds_main`'s `sample_density` weights every one of `.rgba` into
  either `baseDensity` or the cirrus band, so a constant channel silently
  flattens part of the cloud shape.
  `BuildIntegrity.CloudNoiseVolumeCoversItsFullDomainAndWritesEveryChannelTheMarchReads`
  pins both the extent and the no-bare-literal-channel contract.
- **`clouds_main`** runs **once per frame**, only while clouds are on (the
  inspector's CLOUDS switch or the outliner's eye; the
  `guiSceneSharedVars.clouds_enabled` gate in `VulkanRenderer::record_commands`),
  ray-marching the noise volume into a screen-sized 2D storage texture. It runs
  **after the main pass**, because the march stops at the depth that pass wrote
  (see The estimator, below).

`Clouds::init` runs `createTextures` -> `createDescriptorSets` ->
`createComputePipelines` -> `dispatchNoiseGeneration`, so the noise dispatch
comes last, after both descriptor sets point at the final image views. The
noise volume is written, transitioned and sampled entirely on the
**graphics** queue family (see Queue ownership below), so when
`VulkanDevice::graphicsFamilySupportsCompute()` is false,
`dispatchNoiseGeneration` warns and returns early instead of asserting; the
noise texture stays zero-initialized haze.

`clouds_main` binds two descriptor sets: its own (set 0: binding 0 the
`RWTexture2D` output, binding 1 a combined-image-sampler over the noise
volume, binding 2 the scene depth as a sampled image) and the engine's shared
render descriptor set (set 1: `GlobalUBO` at binding 0, `SceneUBO` at
binding 1), the one every other shading stage binds. That is how
`clouds_main` reads `globalUBO.inv_projection`/`inv_view` and
`scene.cam_pos`/`dirLight`/the four cloud `vec4`s without a bespoke uniform
buffer of its own.

Binding 2 holds the NDC depth of whatever the frame's main pass drew: the
forward or the deferred raster path's depth buffer (both stored and left in
`eShaderReadOnlyOptimal` by their render passes), or, when ray or path tracing
owns the frame, `tracedDepth`, an `r32f` image both tracers write at
`TRACED_DEPTH_IMAGE_BINDING` (the primary hit through the pixel centre, `1.0` on
a miss, like a cleared depth buffer). The path tracer traces that extra ray only
on a frame with clouds on (`trace_cloud_depth`). The RT closest-hit shader records the
hit distance before its `KHR_materials_unlit` early return: after it, an unlit
hit kept distance 0, which projected the eye itself, so the march stopped at
once and a cloud in front of an unlit surface vanished in RT alone. The set
is single, not per swapchain image, so
`VulkanRenderer::handleCloudDepthSourceChange` rewrites the binding,
after a `waitIdle`, only on the frame the source changes: a raster-path or
render-mode switch, or the TLAS arriving with RT or PT selected.
`recreateSwapChain` rewrites it unconditionally, since it recreates every
image the binding can name.

## The estimator

Per pixel, `clouds_main` reconstructs a view ray through the texel centre
from the precomputed `inv_projection`/`inv_view` (no per-pixel `inverse()`:
Slang has none for SPIR-V) and intersects the cloud box with iquilezles' slab
test (`box_intersect`; a ray direction with an exactly zero component takes
1e-30 there, so a sun straight overhead never meets `inf - inf`). The box is **axis-aligned**: centred on Offset
(`cloud.offset`), with half extents `cloud.radius` = Scale x Density x 10, so
Density sizes the box as well as scaling every noise sample. No matrix is
involved. Until 2026-11 the shader built a box matrix with `float4x4(1.0)`,
which Slang fills with 1.0 in all sixteen elements rather than making an
identity, wrote the translation into the row `mul(m, v)` drops, and applied
the radius twice: the volume was a sheared slab around `x + z = -1` that Offset
could not move. `BuildIntegrity.CloudBoxIsAnAxisAlignedBoxAroundItsOffset`
rejects a matrix built from one scalar anywhere in the shader tree, and
`GoldenRender.CloudOffsetMovesTheCloudVolume` is the rendered check. The noise
is sampled box-local (`position - cloud.offset`), so moving the box carries
its clouds along.

The march runs from the box entry to the nearer of the box exit and the
**scene depth** at that pixel (`cloud_scene_distance`: the binding-2 depth back
through `inv_projection` to a distance from the eye, which is the view-space origin), so geometry in front of
the box hides it and geometry inside it cuts the march short. It takes
`num_march_steps` steps of **constant length** (`marchLength /
num_march_steps`), not a fraction of the distance already travelled, which
once made the quality slider a de-facto density slider.
`BuildIntegrity.CloudRayMarchesUseAConstantStepLength` guards the distinction
in both the primary and the light march. Each step:

- Samples density from the noise volume (`sample_density`: two periods of
  box-local space, 256 and 64 units, both wrapped with `frac` into the
  volume's `[0, 1)` domain and blended) and accumulates Beer-Lambert transmittance
  (`transmittance *= exp(-density * dt)`, the *only* assignment to
  `transmittance` inside the loop, so it is monotonically non-increasing by
  construction).
- Marches a **second**, shorter ray toward the light (`light_march`,
  `num_march_steps_to_light` steps) from the sample point to get
  `lightTransmittance`, the self-shadowing term.
- Weights the in-scattered contribution by the Henyey-Greenstein phase
  function (`phase_HG`), whose denominator must SUBTRACT `2*g*cosTheta` so a
  positive `g` peaks forward (toward the sun, `cosTheta = +1`), not away
  from it.
- Optionally attenuates (never raises) that contribution by a "powder
  effect" term modelling self-shadowing of dense droplets. It multiplies into
  `lightEnergy`, and must never appear on the right-hand side of a
  `transmittance` assignment.

`BuildIntegrity.CloudScatteringKeepsItsPhaseSignAndItsMonotonicTransmittance`
pins the phase-function sign and the powder/transmittance separation as
text-shape guards; `GoldenRender.EnablingCloudsChangesTheFrameAndAddsDetail`
is the numerical oracle that covers both, so a regression in either meets a
cheap CI-side text-shape guard and a rendered-pixel check.

## Resource lifecycle

- **`recreateFrameResources(commandPool, width, height)`** (called from
  `VulkanRenderer` on swapchain resize) recreates only `cloudOutputTexture`
  at the new extent and rewrites its descriptor; the noise volume is
  content, not a frame resource, and stays untouched.
- **`shaderHotReload(sharedLayout)`** destroys and recreates both compute
  pipelines but deliberately does **not** call `dispatchNoiseGeneration`
  again: descriptor sets and both textures stay untouched, so a shader edit
  never regenerates the (expensive, one-shot) noise volume.
- **`cleanUp()`** is idempotent (a `device` null-check guards re-entry) and
  releases both descriptor sets, both pipeline+layout pairs and both
  textures.

## Queue ownership and barriers

The noise volume is an **exclusive** (single-queue-family) image, produced
and consumed entirely on the graphics family: `Clouds.cpp` dispatches it on
`device->getGraphicsQueue()` using the graphics command pool passed into
`init()`, never a separate compute queue or a transient pool of its own.
`BuildIntegrity.CloudResourcesAreProducedAndConsumedOnOneQueue` greps both
`Clouds.cpp` and `VulkanDevice.ixx` for the shapes that would reintroduce a
queue-family mismatch: `getComputeQueue()`/`createCommandPool`.

`cloudOutputTexture` is a **single** image, not duplicated per
frame-in-flight, so `VulkanRenderer::record_commands` orders access to it
with two explicit barriers around `clouds.recordComputeCommands`. Both are
`eGeneral` -> `eGeneral` colour barriers recorded by one file-local helper,
`recordGeneralImageBarrier`, and differ only in their stage and access masks:

1. A fragment-shader -> compute-shader barrier *before* the dispatch closes
   the cross-frame write-after-read gap against the *previous* frame's
   post-pass sample of the same image. A pipeline barrier orders against all
   previously submitted commands on the queue, not just the current command
   buffer, so it closes the gap even though the in-flight fence only
   guarantees the submission several frames prior has completed.
2. A compute-shader -> fragment-shader barrier *after* the dispatch orders
   this frame's write before the post pass's read of the same image
   (`PostStage`'s own subpass dependency only covers
   `eColorAttachmentOutput` -> `eColorAttachmentOutput` and cannot order a
   compute-shader write).

The helper names its stages by hand instead of using
`VulkanImage::transitionImageLayout`'s `eGeneral` -> `eGeneral` overload,
which derives both stages from the layout alone: `eAllCommands` ->
`eAllCommands`, a full pipeline stall every frame, for what needs one edge.

The scene depth needs two more edges. A raster depth buffer reaches the
compute read through its render pass's outgoing dependency
(`buildSampledOutputDependency` in `RenderPassHelper.hpp`: fragment tests ->
compute and fragment shader reads), and the next frame's clear waits for that
read because `buildExternalColorDepthDependency` lists the compute stage among
its sources. `tracedDepth` is single too, so the same helper orders it: the
tracer's write before the clouds' read, and, before the next frame's tracer
writes it again, the previous write and read.

Measured 2026-08-01 with the first barrier in place (RX 9070 XT,
`Invoke-SyncValidation.ps1`, `khronos_validation.validate_sync=true`): no
`SYNC-HAZARD` across `GoldenRender.CloudsAcrossManyFramesDoesNotLoseTheDevice`
(30+ frames with clouds on), nor in the frames the all-maximum case of the GUI
input sweep completed before the path-tracing device loss of that time. The
state before the fix was not measured under sync validation, so the barrier
closes a spec gap, not a hazard seen and then fixed. The Linux lane has
validated the same test on llvmpipe on every push since 2026-10-06
([`gpu-golden-testing.md`](gpu-golden-testing.md) § Synchronization
validation).

## UBO packing

`SceneUboMarshal.hpp`'s `fillSceneUboClouds` (the host packer) and
`volumetric_clouds.slang`'s `cloud_unpack` (the shader-side consumer; `clouds_main`
copies the four `SceneUBO` vectors into its `CloudParams`, and OxidANT's host packs the same
layout) are two hand-written mirrors of the same nine-value layout. The table maps
each GUI control (the CLOUDS section of the inspector's Sky tab, `GUI.cpp`) -> shader field ->
`SceneUBO` field/component; both sides must agree on it.
`BuildIntegrity.CloudUboPackingMatchesTheShaderUnpack` pins the shader half,
field by field, and `BuildIntegrity.CloudsDocTablesMatchTheirSources` pins
this table against that same test's data.

<!-- cloud-ubo:begin -->
| GUI control | Shader field | SceneUBO field |
| --- | --- | --- |
| `March steps` | `cloud.num_march_steps` | `cloudParameters.w` |
| `Light steps` | `cloud.num_march_steps_to_light` | `cloudLightMarch.x` |
| `Density` | `cloud.scale` | `cloudMeshScale.w` |
| `Coverage` | `cloud.threshold` | `cloudMeshOffset.w` |
| `Pillowness` | `cloud.pillowness` | `cloudParameters.x` |
| `Cirrus` | `cloud.cirrus_effect` | `cloudParameters.y` |
| `Powder effect` | `cloud.powder_effect` | `cloudParameters.z` |
| `Scale` | `cloud.radius` | `cloudMeshScale.xyz` |
| `Offset` | `cloud.offset` | `cloudMeshOffset.xyz` |
<!-- cloud-ubo:end -->

The CLOUDS switch (and the outliner's eye on Clouds) is not in this table: it gates
`recordComputeCommands`/`clouds_main`'s dispatch and `post.slang`'s
compositing branch through a push constant
(`PushConstantPost::clouds_enabled`), not a `SceneUBO` field.

## Dispatch and clamp constants

`CloudDispatch.hpp` is the single source for every dispatch-grid and
march-step-bound constant shared between `Clouds.cpp`, `SceneUboMarshal.hpp`,
`GUI.cpp`'s slider ranges and the two shaders' own defensive clamps. A shader
must not trust a UBO, so the shared `cloud_unpack` re-clamps both march-step counts on
both sides even though the host already clamped them before packing.

<!-- cloud-constants:begin -->
| Constant | Value | Shader token it pins |
| --- | --- | --- |
| `kNoiseVolumeExtent` | 128 | `volumetric_clouds.slang`'s `NOISE_VOLUME_EXTENT` and the dispatch-grid divisor |
| `kNoiseWorkgroupSize` | 8 | `noise_volume.slang`'s `[numthreads(8, 8, 8)]` |
| `kCloudWorkgroupSize` | 16 | `clouds.slang`'s `[numthreads(16, 16, 1)]` |
| `kMinCloudMarchSteps` | 4 | `volumetric_clouds.slang`'s `num_march_steps` floor |
| `kMaxCloudMarchSteps` | 128 | `volumetric_clouds.slang`'s `num_march_steps` clamp, upper bound |
| `kMinCloudLightMarchSteps` | 1 | `volumetric_clouds.slang`'s `num_march_steps_to_light` clamp, lower bound |
| `kMaxCloudLightMarchSteps` | 128 | `volumetric_clouds.slang`'s `num_march_steps_to_light` clamp, upper bound |
<!-- cloud-constants:end -->

`BuildIntegrity.CloudDispatchGridsMatchTheShaderWorkgroupSizes` pins the two
workgroup-size rows against the shaders' `[numthreads(...)]` attributes;
`BuildIntegrity.CloudMarchStepBoundsMatchTheShaderClamps` pins the four
march-step rows against `volumetric_clouds.slang`'s clamp literals; both are mirrored by
`BuildIntegrity.CloudsDocTablesMatchTheirSources` against this table's
values.

## Compositing contract

`clouds_main` writes `float4(cloudColor, 1.0 - transmittance)`: premultiplied
colour, alpha = one minus the accumulated transmittance. `post/post.slang`'s
`fs_main` is the sole consumer. While `pc_post.clouds_enabled` is set, it
composites every pixel in **linear HDR, before the tonemap**: the sky under
the scene by the scene's coverage alpha, then the clouds premultiplied-over
both, `color = cloud.rgb + (scene.rgb + sky * (1 - scene.a)) * (1 - cloud.a)`.
It writes alpha 1, so the result replaces the sky pass's pixel.

The sky pass draws the cubemap (or the disabled-sky navy) untonemapped, so the
sky's HDR radiance is the value whose ACES tonemap reproduces that pixel:
post samples the same cubemap in the same direction
(`common/skybox_radiance.slang`, shared with `skybox.slang`) and inverts
`aces_tonemap` (`aces_tonemap_inverse`). A cloud-free pixel therefore keeps the
sky pass's colour, and a cloud composites over the sky exactly as over
geometry of that radiance. Before, the sky got the clouds after the tonemap,
through the fixed-function alpha blend: premultiplied colour multiplied by
alpha a second time, in display space, so thin clouds looked darker over the
sky than over geometry. `GoldenRender.CloudsCompositeOverTheSkyAsOverGeometry`
compares one cloud over the navy sky and over a black card. Without clouds,
post keeps its scene alpha and blends over the sky pass as before.

## Verification

Every `BuildIntegrity.Cloud*` test named above, the doc-table check included,
is a pure-CPU source-text/constant check: none needs a GPU adapter. The GPU
tests are `GoldenRender.EnablingCloudsChangesTheFrameAndAddsDetail` (the
numerical oracle), `GoldenRender.CloudsAcrossManyFramesDoesNotLoseTheDevice`,
`GoldenRender.CloudOffsetMovesTheCloudVolume` (two 3-unit boxes either side of
the view axis each stay in their half; the other half keeps every pixel),
`GoldenRender.CloudsStopAtTheSceneDepth` (forward and deferred: a box behind
the shadow rig's slab shows around it, never on it),
`GoldenRender.CloudsStopAtTracedGeometry` (the same in RT and PT),
`GoldenRender.CloudsCompositeOverTheSkyAsOverGeometry` and
`GoldenRender.CloudsInFrontOfGeometryCoverItInEveryMode`. The last checks the
depth from the other side: a fog-filled box (coverage threshold 0) in front
of the slab and of an unlit green card must change over 90 % of both in
every mode (the card alone in PT, whose lit pixels move by noise), on the
pixels forward's own captures pick out. Measured 100 % in all four; with the
hit distance after the unlit return, RT changed 0 of 6941 card pixels.
