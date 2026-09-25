# Path Tracing

State of the path-tracing mode after the 2026-07-22 overhaul (commits
`4181e0da` → `110b1503`). Everything here describes **shipped** behaviour;
open work is listed at the end with its BACKLOG anchors.

## Pipeline shape

One compute kernel, `Resources/ShadersSlang/path_tracing/path_tracing.slang`,
dispatched by `PathTracing::recordCommands` between two image barriers that
move the rasterizer's offscreen target (`rgba16f` since the HDR unit,
`OUT_IMAGE_BINDING`) to `eGeneral` for the compute write and on to
`eShaderReadOnlyOptimal` for the post pass. Both sit in the frame's graphics
command buffer, so no queue-family ownership transfer is involved. The post pass then samples
that target like any other mode - path tracing replaces the *lighting*, not
the presentation path.

Ray traversal uses `GL_EXT_ray_query` against the same TLAS the RT mode
uses; geometry/material data arrive via buffer-device-address
(`ObjectDescription` → vertex/index/material-id/material arrays). The record
path dispatches only when a TLAS exists - during the async model load there
is nothing to trace against, and dispatching anyway used to consume
never-written descriptor sets.

Neither the bounce ray nor the NEE shadow ray traces `RAY_FLAG_FORCE_OPAQUE`:
a ray query has no any-hit stage, so glTF MASK cut-outs are alpha-tested
inline in each query's candidate loop instead, via the `ray_hit_masked_out`
helper shared with `raytracing/raytrace.rahit.slang`
(`Resources/ShadersSlang/common/alpha_test.slang`). A candidate triangle is
committed with `CommitNonOpaqueTriangleHit()` only when the test passes;
not committing is the discard. The shadow ray keeps
`RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH` - it still ends the search at the
first *committed* hit, so a masked-out candidate does not end the search
early.

## The estimator

Per pixel and frame, `samples_per_pixel` independent paths (GUI slider,
default 8), each up to `max_bounces` segments (GUI slider, default 8);
changing either slider resets the accumulation (a mean over two different
estimators is biased):

- **Primary ray** from the precomputed `inv_view`/`inv_projection` in
  `GlobalUBO` (the kernel used to invert both matrices *per sample* - 24
  4x4 inversions per pixel per frame). Jittered uniformly within the
  pixel's unit square (a box filter) rather than always through the exact
  centre, so `samples_per_pixel` and the temporal running mean anti-alias
  geometric edges rather than just denoise a point-sampled image.
- **Hit**: throughput *= albedo (texture, or `material.diffuse` when
  `textureID` is -1). Normals are transformed as directions
  (inverse-transpose via row-multiplying `worldToObject`); the hit normal is
  face-forwarded against the ray.
- **Next-event estimation**: one shadow ray per bounce toward the GUI
  directional light; unoccluded hits contribute
  `throughput * NdotL * (1/pi) * dirLight.color.rgb * dirLight.color.w`
  (the throughput carries the albedo; NEE carries the Lambertian 1/pi -
  measured, the missing factor left lit surfaces at 208.5 crop luminance vs
  188.2 correct, with forward at 158.1 and the gap being PT's indirect
  sky). This is the only connection to the GUI light - before it existed,
  the mode's sole light source was an accidental radiance-1 "white furnace"
  on miss. The BOUNCE path needs no 1/pi: cosine-weighted sampling of a
  Lambertian cancels the pi and the cosine exactly.
- **Bounce**: RTIOW-style cosine-ish sampling (normal + random unit vector),
  with the degenerate-scatter guard (near-zero sum falls back to the
  normal). The sampled direction is renormalized before it becomes the next
  segment's `rayDirection` - the unnormalized sum has length in `[0, 2]`,
  and `TMin`, `TMax` and the miss gradient below are all measured in units
  of that vector's length. From the fourth segment on, **Russian
  roulette** terminates low-throughput paths and reweights survivors
  (unbiased).
- **Miss**: a soft RTIOW gradient sky at half intensity ends the path.

Self-intersection is guarded by a 1e-4 normal offset **and** ray-query
`t_min = 0.001` (matches the RT path).

Setting `KATAGLYPHIS_PT_FURNACE=<radiance>` (read via `getenv` once per
`recordCommands` call, not cached, so several tests sharing one process each
see their own setting) forces every hit's albedo to 1 and replaces the
gradient sky with a uniform `<radiance>` environment; the shader gates on
`pc_ray.clearColor.w > 0.5`. This is the classic white-furnace test: an
unbiased estimator must converge every pixel to exactly the environment
radiance, for any geometry.
`GoldenRender.PathTracingPassesTheWhiteFurnaceTest` is the oracle.

## Temporal accumulation

The kernel folds the frame index into the RNG seed (without it, every frame
drew bit-identical samples) and maintains a running mean in a dedicated
`rgba32f` history image (`ACCUMULATION_IMAGE_BINDING`) - full float because
averaging in the output target (`rgba8` before the HDR unit) would quantize
and stall after a few frames.
There is exactly **one** history image, deliberately not per swapchain
image; a compute→compute barrier orders each frame's read-modify-write
against the previous frame's dispatch across command buffers.

The history resets whenever its premises die:

| Trigger | Where |
| --- | --- |
| Camera moved (view-matrix compare) | PT branch of `record_commands` |
| Quality sliders changed (different estimator = biased mean) | same |
| Swapchain resize (extent-sized image is recreated) | `recreateSwapChain` |
| AS rebuilt - model load/reload OR GUI transform moved the traced world | `updateRaytracingDescriptorSets` |

The AS-rebuild trigger was found the hard way: without it, the mean kept
blending frames of the *half-loaded* scene until the camera happened to
move, and that healing motion mimicked convergence well enough to make a
naive golden pass with per-frame sampling disabled.

## Verification

<!-- pt-goldens: PathTracingAccumulatesAndConverges, PathTracingAntiAliasesGeometricEdges, PathTracingRespondsToTheDirectionalLight, PathTracingHonorsTheQualityControls, RaytracedWorldFollowsTheModelTransform, PathTracingPassesTheWhiteFurnaceTest, RaytracedLargeMeshDoesNotLoseTheDevice, RaytracedShadowsAreDarkerThanLitGround -->

Eight PT- and RT-facing goldens in `Test/commit/VulkanEngine/goldenRenderSuite.cpp`,
each red/green-proven against the pre-fix shader (numbers are post-HDR;
lifting the UNORM ceiling widened several of them dramatically):

- `PathTracingAccumulatesAndConverges` - consecutive-frame changed-pixel
  fraction in a GUI-free crop: 7.6e-3 early, 3.1e-5 late; the
  frame-invariant seed measures exactly 0 early and fails.
- `PathTracingAntiAliasesGeometricEdges` - triples of pixels straddling a
  shadow-rig silhouette edge in a GUI-free crop must sometimes have a
  middle pixel strictly between the two flat luminances either side (a
  jittered, accumulated primary ray blends fg/bg at the boundary); a
  pixel-centre-only kernel gives hard transitions with no partial pixel.
- `PathTracingRespondsToTheDirectionalLight` - radiance 10 vs 0 on the
  shadow rig: swung fraction 0.751 (0.027 pre-HDR - the ceiling hid three
  quarters of the response) vs exactly 0 for the pre-NEE kernel.
- `PathTracingHonorsTheQualityControls` - bounce cap 8 vs 1 swings 0.255
  (all indirect sky light vanishes); hardcoded-bounds kernel: 7.3e-5.
- `RaytracedWorldFollowsTheModelTransform` - a GUI move must change the
  traced image: 0.397 vs exactly 0 on a stale TLAS.
- `PathTracingPassesTheWhiteFurnaceTest` - furnace-mode crop converges to
  mean luminance ~205 (matching `aces_tonemap(1.0) * 255`) with >98% of
  pixels within +-6 of that value; a biased estimator either drifts off
  205 or leaves visible geometry in the (supposedly uniform) image.
- `RaytracedLargeMeshDoesNotLoseTheDevice` - the RT pipeline must survive
  10 frames against the shipped large dinosaur-mesh debug scene; a
  diagnostic/regression guard scoping the `VK_ERROR_DEVICE_LOST` the PT
  reproducer hits on the same mesh (KNOWN ISSUE comment in
  `path_tracing.slang`, BACKLOG.md) to `path_tracing.slang`/`RayQuery`
  specifically versus the shared vertex-upload/BDA path.
- `RaytracedShadowsAreDarkerThanLitGround` - the RT closest-hit path, not
  PT: on the shadow rig the darkest decile of a GUI-free crop must stay
  under 5% of the brightest decile's luminance. Measured 0 vs 244 with the
  fix, 41.6 vs 248 when `raytrace.rchit.slang` seeds its payload with the
  unlit albedo (`d35e6212`).

The instrument lessons these goldens were built on (ImGui panel coverage,
measuring changed-pixel fractions in the panel-free right edge, dumping
amplified diff-map PNGs before trusting a metric) are collected in
[`gpu-golden-testing.md`](gpu-golden-testing.md)'s cautions section. The
PT-specific one: before the HDR unit, lit surfaces clamped at the UNORM
ceiling (186 after tonemap) and hid most of every light response.

Editing the kernel no longer needs a C++ rebuild: the Slang compile script
regenerates the `.spv` and the engine loads it at startup.
`scripts/windows/Build-SlangShaders.ps1` is the build/CI path.

## Open work (BACKLOG, "PT survey" section)

- RNG decorrelation (item 11, partially done via the frame fold): linear
  seeds still correlate neighbours within a frame.
