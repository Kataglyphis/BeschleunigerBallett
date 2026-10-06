# Volumetric Clouds

The clouds subsystem (`Src/GraphicsEngineVulkan/scene/atmospheric_effects/clouds/`)
is the only one with a compute pair, a shared host/shader constants header, a
four-`vec4` `SceneUBO` block and its own GUI panel. Everything here describes
**shipped** behaviour; open work lives in `BACKLOG.md`.

## Pipeline shape

Two compute kernels, both Slang-authored and SPIR-V-only
(`Resources/ShadersSlang/compute/noise.slang`, `compute/clouds.slang`):

- **`noise_main`** runs exactly **once**, from `Clouds::init` via
  `dispatchNoiseGeneration`, filling a `kNoiseVolumeExtent`^3 3D storage
  texture (`RWTexture3D<float4>`) that covers world-space `[0, 1)` once per
  axis. All four channels are written from independent Worley/value-noise
  terms - `clouds_main`'s `sample_density` weights every one of `.rgba` into
  either `baseDensity` or the cirrus band, so a constant channel silently
  flattens part of the cloud shape (`BuildIntegrity.
  CloudNoiseVolumeCoversItsFullDomainAndWritesEveryChannelTheMarchReads`
  pins both the extent and the no-bare-literal-channel contract).
- **`clouds_main`** runs **once per frame**, only while the GUI's "Enable
  Clouds" checkbox is on (`VulkanRenderer::recordComputeCommands`'s
  `guiSceneSharedVars.clouds_enabled` gate), ray-marching the noise volume
  into a screen-sized 2D storage texture.

`Clouds::init` order is `createTextures` -> `createDescriptorSets` ->
`createComputePipelines` -> `dispatchNoiseGeneration` - the noise dispatch is
therefore the last init step, after both descriptor sets already point at the
final image views. The noise volume is written, transitioned and sampled
entirely on the **graphics** queue family (see Queue ownership below), so
`dispatchNoiseGeneration` warns and returns early - the noise texture stays
zero-initialized haze - when `VulkanDevice::graphicsFamilySupportsCompute()`
is false, rather than asserting.

`clouds_main` binds two descriptor sets: its own (set 0: binding 0 the
`RWTexture2D` output, binding 1 a combined-image-sampler over the noise
volume) and the engine's shared render descriptor set (set 1: `GlobalUBO` at
binding 0, `SceneUBO` at binding 1) - the same set every other shading stage
binds, which is how `clouds_main` reads `globalUBO.inv_projection`/
`inv_view` and `scene.cam_pos`/`dirLight`/the four cloud `vec4`s without a
bespoke uniform buffer of its own.

## The estimator

Per pixel, `clouds_main` reconstructs a view ray from the precomputed
`inv_projection`/`inv_view` (no per-pixel `inverse()` - Slang has none for
SPIR-V), intersects an axis-aligned box (the cloud "mesh": iquilezles' ray-box
formula, `box_intersect`, with the inverse model matrix formed in the shader
itself from the box half-extents and offset, since a diagonal scale+translate
inverts trivially) and, on a hit, ray-marches it in `num_march_steps` steps of
**constant length** (`marchLength / num_march_steps`, not a fraction of
distance already travelled -
`BuildIntegrity.CloudRayMarchesUseAConstantStepLength` guards the distinction,
which once made the quality slider a de-facto density slider). Each step:

- Samples density from the noise volume (`sample_density`: two world-space
  periods, 256 and 64 units, both wrapped with `frac` into the volume's
  `[0, 1)` domain and blended) and accumulates Beer-Lambert transmittance
  (`transmittance *= exp(-density * dt)`, the *only* assignment to
  `transmittance` inside the loop - march-loop-internal, monotonically
  non-increasing by construction).
- Marches a **second**, shorter ray toward the light (`light_march`,
  `num_march_steps_to_light` steps) from the sample point to get
  `lightTransmittance`, the self-shadowing term.
- Weights the in-scattered contribution by the Henyey-Greenstein phase
  function (`phase_HG`), whose denominator must SUBTRACT `2*g*cosTheta` so a
  positive `g` peaks forward (toward the sun, `cosTheta = +1`), not away from
  it.
- Optionally attenuates (never raises) that contribution by a "powder effect"
  term modelling self-shadowing of dense droplets - it multiplies into
  `lightEnergy`, and must never appear on the right-hand side of a
  `transmittance` assignment.

`BuildIntegrity.CloudScatteringKeepsItsPhaseSignAndItsMonotonicTransmittance`
pins both the phase-function sign and the powder/transmittance separation as
text-shape guards; `GoldenRender.EnablingCloudsChangesTheFrameAndAddsDetail`
is the numerical oracle that now covers them, so a regression in either has
both a cheap CI-side text-shape guard and a host-only rendered-pixel check.

## Resource lifecycle

- **`recreateFrameResources(commandPool, width, height)`** (called from
  `VulkanRenderer` on swapchain resize) recreates only `cloudOutputTexture`
  at the new extent and rewrites its descriptor - the noise volume is
  content, not a frame resource, and is left untouched.
- **`shaderHotReload(sharedLayout)`** destroys and recreates both compute
  pipelines but deliberately does **not** call `dispatchNoiseGeneration`
  again - descriptor sets and both textures are untouched, so a shader edit
  never regenerates the (expensive, one-shot) noise volume.
- **`cleanUp()`** is idempotent (a `device` null-check guards re-entry) and
  releases both descriptor sets, both pipeline+layout pairs and both
  textures.

## Queue ownership and barriers

The noise volume is an **exclusive** (single-queue-family) image, produced
and consumed entirely on the graphics family - `Clouds.cpp` dispatches it on
`device->getGraphicsQueue()` using the graphics command pool passed into
`init()`, never a separate compute queue or a transient pool of its own
(`BuildIntegrity.CloudResourcesAreProducedAndConsumedOnOneQueue` greps both
`Clouds.cpp` and `VulkanDevice.ixx` for the shapes that would reintroduce a
queue-family mismatch: `getComputeQueue()`/`createCommandPool`).

`cloudOutputTexture` is a **single** image, not duplicated per
frame-in-flight, so `VulkanRenderer::record_commands` orders access to
it with two explicit barriers around `clouds.recordComputeCommands`. Both are
`eGeneral` -> `eGeneral` colour barriers recorded by one file-local helper,
`recordCloudOutputBarrier`, and differ only in their stage and access masks:

1. A fragment-shader -> compute-shader barrier *before* the dispatch, closing
   the cross-frame write-after-read gap against the *previous* frame's
   post-pass sample of the same image (a pipeline barrier orders against all
   previously submitted commands on the queue, not just the current command
   buffer, so this closes the gap even though the in-flight fence only
   guarantees the submission several frames prior has completed).
2. A compute-shader -> fragment-shader barrier *after* the dispatch, ordering
   this frame's write before the post pass's read of the same image
   (`PostStage`'s own subpass dependency only covers
   `eColorAttachmentOutput` -> `eColorAttachmentOutput` and cannot order a
   compute-shader write).

The helper names its stages by hand rather than going through
`VulkanImage::transitionImageLayout`'s `eGeneral` -> `eGeneral` overload: that
one derives both stages from the layout alone, which gives `eAllCommands` ->
`eAllCommands`, a full pipeline stall every frame, for what needs one edge.

Measured 2026-08-01 with the first barrier in place (RX 9070 XT,
`Invoke-SyncValidation.ps1`, `khronos_validation.validate_sync=true`): no
`SYNC-HAZARD` across `GoldenRender.CloudsAcrossManyFramesDoesNotLoseTheDevice`
(30+ frames with clouds on), nor in the frames the all-maximum case of the GUI
input sweep completed before the path-tracing device loss of that time. The
state before the fix was not measured under sync validation, so the barrier
closes a spec gap rather than a hazard that was seen and then fixed. The Linux
lane has validated the same test on llvmpipe on every push since 2026-10-06.

## UBO packing

`SceneUboMarshal.hpp`'s `fillSceneUboClouds` (the host packer) and
`clouds_main`'s cloud-parameter unpack block (the sole shader-side consumer)
are two hand-written mirrors of the same nine-value layout. The table below
is the GUI control (`GUI.cpp`'s "Cloud Settings" tree node) -> shader field
-> `SceneUBO` field/component mapping both sides must agree on;
`BuildIntegrity.CloudUboPackingMatchesTheShaderUnpack` pins the shader half
of it, and `BuildIntegrity.CloudsDocTablesMatchTheirSources` (below) pins
this table against that same test's data.

<!-- cloud-ubo:begin -->
| GUI control | Shader field | SceneUBO field |
| --- | --- | --- |
| `# march steps` | `cloud.num_march_steps` | `cloudParameters.w` |
| `# march steps to light` | `cloud.num_march_steps_to_light` | `cloudLightMarch.x` |
| `Density` | `cloud.scale` | `cloudMeshScale.w` |
| `Coverage threshold` | `cloud.threshold` | `cloudMeshOffset.w` |
| `Pillowness` | `cloud.pillowness` | `cloudParameters.x` |
| `Cirrus effect` | `cloud.cirrus_effect` | `cloudParameters.y` |
| `Powder effect` | `cloud.powder_effect` | `cloudParameters.z` |
| `Scale` | `cloud.radius` | `cloudMeshScale.xyz` |
| `Translation` | `cloud.offset` | `cloudMeshOffset.xyz` |
<!-- cloud-ubo:end -->

The "Enable Clouds" checkbox is not in this table - it gates
`recordComputeCommands`/`clouds_main`'s dispatch and `post.slang`'s
compositing branch through a push constant
(`PushConstantPost::clouds_enabled`), not a `SceneUBO` field.

## Dispatch and clamp constants

`CloudDispatch.hpp` is the single source for every dispatch-grid and
march-step-bound constant shared between `Clouds.cpp`, `SceneUboMarshal.hpp`,
`GUI.cpp`'s slider ranges and the two shaders' own defensive clamps. A shader
must not trust a UBO, so `clouds_main` re-clamps both march-step counts on
both sides even though the host already clamped them before packing.

<!-- cloud-constants:begin -->
| Constant | Value | Shader token it pins |
| --- | --- | --- |
| `kNoiseVolumeExtent` | 128 | `noise.slang`'s `NOISE_VOLUME_EXTENT` and its dispatch-grid divisor |
| `kNoiseWorkgroupSize` | 8 | `noise.slang`'s `[numthreads(8, 8, 8)]` |
| `kCloudWorkgroupSize` | 16 | `clouds.slang`'s `[numthreads(16, 16, 1)]` |
| `kMinCloudMarchSteps` | 4 | `clouds.slang`'s `num_march_steps` floor |
| `kMaxCloudMarchSteps` | 128 | `clouds.slang`'s `num_march_steps` clamp, upper bound |
| `kMinCloudLightMarchSteps` | 1 | `clouds.slang`'s `num_march_steps_to_light` clamp, lower bound |
| `kMaxCloudLightMarchSteps` | 128 | `clouds.slang`'s `num_march_steps_to_light` clamp, upper bound |
<!-- cloud-constants:end -->

`BuildIntegrity.CloudDispatchGridsMatchTheShaderWorkgroupSizes` pins the two
workgroup-size rows against the shaders' `[numthreads(...)]` attributes;
`BuildIntegrity.CloudMarchStepBoundsMatchTheShaderClamps` pins the four
march-step rows against `clouds.slang`'s clamp literals; both are mirrored by
`BuildIntegrity.CloudsDocTablesMatchTheirSources` against this table's
values.

## Compositing contract

`clouds_main` writes `float4(cloudColor, 1.0 - transmittance)` -
premultiplied colour, alpha = one minus the accumulated transmittance.
`post/post.slang`'s `fs_main` is the sole consumer: while
`pc_post.clouds_enabled` is set, it composites with
`color = cloud.rgb + color * (1.0 - cloud.a)` and the matching alpha blend -
a straight premultiplied-over, which is exactly the contract `clouds_main`'s
write satisfies. Both live in the same push-constant-gated branch, so
enabling clouds without compositing (or the reverse) is not reachable through
the GUI toggle.

## Verification

- `BuildIntegrity.CloudDispatchGridsMatchTheShaderWorkgroupSizes` - dispatch
  grid constants against `[numthreads(...)]`.
- `BuildIntegrity.CloudMarchStepBoundsMatchTheShaderClamps` - march-step
  bound constants against the shader's defensive clamps.
- `BuildIntegrity.CloudNoiseVolumeCoversItsFullDomainAndWritesEveryChannelTheMarchReads` -
  noise volume extent and full-channel coverage.
- `BuildIntegrity.CloudResourcesAreProducedAndConsumedOnOneQueue` - single-queue
  ownership of the noise volume.
- `BuildIntegrity.CloudRayMarchesUseAConstantStepLength` - constant per-step
  march length, in both the primary and light marches.
- `BuildIntegrity.CloudScatteringKeepsItsPhaseSignAndItsMonotonicTransmittance` -
  phase-function sign and the powder/transmittance separation.
- `BuildIntegrity.CloudUboPackingMatchesTheShaderUnpack` - host packer vs.
  shader unpack, field by field.
- `BuildIntegrity.CloudsDocTablesMatchTheirSources` - this document's two
  marker-block tables against the constants and field pairs above.

All are pure-CPU source-text/constant checks - none needs a GPU adapter.
