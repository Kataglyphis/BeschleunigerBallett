# Path Tracing

The path-tracing mode after the 2026-07-22 overhaul (commits `27fddfd8` → `81799ea2`).
Everything here is **shipped** behaviour; the PT survey's status is at the end.

## Pipeline shape

One compute kernel, `Resources/ShadersSlang/path_tracing/path_tracing.slang`,
dispatched by `PathTracing::recordCommands` between two image barriers that
move the rasterizer's offscreen target (`rgba16f` since the HDR unit,
`OUT_IMAGE_BINDING`) to `eGeneral` for the compute write and on to
`eShaderReadOnlyOptimal` for the post pass. Both sit in the frame's graphics
command buffer, so no queue-family ownership transfer is involved. The post
pass samples that target like any other mode: path tracing replaces the
*lighting*, not the presentation path.

Every sample's radiance and the kernel's output pass through `hdr_safe`
(`common/hdr_safe.slang`): radiance past fp16's 65504 would store as +Inf in
the `rgba16f` target, ACES would turn that into NaN, and one NaN sample would
stay in the running mean until the next reset.

A path-traced frame runs neither the raster pass nor the shadow-cascade and
punctual-cube passes: the kernel shades its own visibility, and NEE traces its
own shadow rays. The
RT mode skips both too. For the clouds, which march to the scene depth, the
kernel also traces one ray through each pixel centre and writes that primary
hit's NDC depth to `tracedDepth` (`TRACED_DEPTH_IMAGE_BINDING`, `1.0` on a
miss); see [`clouds.md`](clouds.md). That ray runs only while clouds are on
(the push constants' `trace_cloud_depth`, set from the frame's
`clouds_enabled`); until 2026-11 every frame paid for it.

Rays traverse with `GL_EXT_ray_query` against the RT mode's TLAS;
geometry/material data arrive via buffer-device-address (`ObjectDescription`
→ vertex/index/material-id/material arrays). The record path dispatches only
when a TLAS exists: during the async model load there is nothing to trace
against, and dispatching anyway used to consume never-written descriptor sets.

Neither the bounce ray nor the NEE shadow ray traces `RAY_FLAG_FORCE_OPAQUE`.
A ray query has no any-hit stage, so glTF MASK cut-outs are alpha-tested inline
in each query's candidate loop, via the `ray_hit_masked_out` helper shared with
`raytracing/raytrace.rahit.slang` (`Resources/ShadersSlang/common/alpha_test.slang`).
A candidate triangle is committed with `CommitNonOpaqueTriangleHit()` only when
the test passes; not committing is the discard. The shadow ray keeps
`RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH`: it ends the search at the first
*committed* hit, so a masked-out candidate does not end it early.

## The estimator

Per pixel and frame, `samples_per_pixel` independent paths (GUI slider,
default 8), each up to `max_bounces` segments (GUI slider, default 8).
Changing either slider resets the accumulation (a mean over two different
estimators is biased).

- **Primary ray** from the precomputed `inv_view`/`inv_projection` in
  `GlobalUBO` (the kernel used to invert both matrices *per sample*: 24 4x4
  inversions per pixel per frame), jittered uniformly within the pixel's unit
  square (a box filter) instead of always through the exact centre, so
  `samples_per_pixel` and the temporal running mean anti-alias geometric edges
  rather than just denoise a point-sampled image.
- **Hit**: the albedo (texture, or `material.diffuse` when `textureID` is
  -1, times the vertex colour), metallic and roughness as every raster path
  reads them: the factors times the metallic-roughness texture's B and G
  channels (`resolved_metallic_roughness_lod0`), the roughness product
  floored at 0.045 like the factor. Until 2026-11 a texel with G = 0 made
  GGX alpha 0, the visible-normal density 0/0 and the sample's weight NaN or
  1e6 times too large. Until 2026-11 only the
  texture branch applied metallic, so `metallic_card.gltf`'s untextured 0.75
  rendered as a dielectric. `KHR_materials_unlit` ends the path at its albedo
  before metallic or a lobe can touch it. Until 2026-11 the metallic multiply
  ran first, which darkened an unlit material with a metallic-roughness
  texture, and with the untextured factor applied, blackened one that omits
  `metallicFactor` (the spec's default is 1). Normals are transformed as
  directions (inverse-transpose via row-multiplying `worldToObject`). The
  triangle's winding picks the side (`CommittedTriangleFrontFace()`, as
  `SV_IsFrontFace` in raster and `HitKind()` in the RT closest-hit): a back
  face negates the interpolated normal, and a normal map's whole frame with it
  (`common/normal_map.slang`). The face normal is the triangle's own, from its
  three positions, turned to oppose the ray. Until 2026-11 both came from the
  interpolated normal, which near a smooth low-poly silhouette faces away from
  a camera the triangle faces. `GoldenRender.TracedHitsTakeTheirSideFromTheTriangleWinding`
  views a card whose vertex normals lean 70 degrees off its +Z winding from
  60 degrees the other way: ray traced must match forward (2.225 both; 0 with
  the side from the vertex normal), an emitter behind the card must add
  nothing to the path-traced card (0.347 either way; 9.71 with the old side
  and bound), and the path-traced card must shade like a flat twin (0.347
  both; 0.110 with the old side alone, 0.237 with the old bound alone). Those
  numbers are the old gradient sky's; under the drawn cube map since
  2026-11-16 the card reads 0.0150 alone, beside the emitter and as the flat
  twin, and its sanity bound fell from 0.02 to 0.005 (the controls were not
  re-measured).
- **The BRDF is raster's**: OxidANT's `brdf.slang`, Lambert times
  `(1 - F)(1 - metallic)` plus the GGX lobe `D G F / (4 n.v n.l)`, with
  `f0 = lerp(0.04, albedo, metallic)`. NEE evaluates `brdf_direct`, raster's
  function, with Epic's Smith remap for analytic lights, `k = (r + 1)^2 / 8`;
  a bounce weights by `brdf_sampled`, the same BRDF with the remap Epic uses
  for sampled light, `k = alpha / 2`. Until 2026-11-16 the bounce took
  `brdf_direct` too, which over-masks a sampled lobe at grazing angles: a
  white metal at roughness 0.3 returned 0.668 of a uniform sky at 60 degrees,
  0.901 now. Until 2026-11 the kernel had no specular lobe, so a metal only
  lost its diffuse share and a glTF material at the default `metallicFactor`
  1 rendered black.
- **Next-event estimation**: two shadow rays per bounce, one toward the GUI
  directional light and one toward a punctual light picked uniformly from
  `punctual_light_count()`, its radiance times that count so the estimate stays
  unbiased; an unoccluded light adds `throughput * brdf_direct(n, v, l, ..., radiance)`,
  what forward shades for that hit before its shadows. NEE is the only
  connection to the GUI light; before it, the mode's sole light source was an
  accidental radiance-1 "white furnace" on miss.
- **Bounce**: the RNG, the per-pixel seed, Russian roulette and the bounce
  are OxidANT's `common/path_estimator.slang` (`rng_next`, `path_rng_seed`,
  `russian_roulette_survives`, `sample_bounce`), which the Rust compute path
  tracer imports too, so a fix lands in both. One-sample MIS over the two
  lobes. With probability `p`, the
  Fresnel share of the lobes at `n.v` floored at 0.1 so a dielectric's
  highlight still gets samples, a GGX normal visible from the viewer (Heitz
  2018) reflects the view ray; otherwise the RTIOW cosine sample (normal plus
  a random unit vector, with the degenerate-scatter guard) runs. The
  throughput takes `f n.l / (p pdf_GGX + (1 - p) n.l / pi)`, the balance
  heuristic over the mixture, so either lobe may produce any direction.
  `pdf_GGX` is `D G1(v) / (4 n.v)` with the unfloored NDF and Smith's exact
  G1, the density the sampler draws. Since the 2026-11-15 OxidANT pin,
  `distribution_ggx` floors roughness at `MIN_ROUGHNESS` 0.045 (the
  material floor) and guards its denominator at 1e-20; its old `1e-6` guard
  capped the lobe's peak below roughness 0.154, so a smooth surface reflected
  only part of its light, here as in raster. Since 2026-11-21 the Smith
  remaps floor it too, so below the floor the lobe's masking matches its gain.
  The shading normal orients both lobes, and the face normal
  takes its place when it faces away from the viewer; a direction below it or
  below the triangle ends the path, which keeps a tilted normal map or vertex
  normal from leaking light through the surface. The sampled
  direction is renormalized before it becomes the next segment's
  `rayDirection`: `TMin` and `TMax` are measured in units of that vector's
  length. From the fourth segment on, **Russian
  roulette** terminates low-throughput paths and reweights survivors
  (unbiased).
- **Miss**: ends the path. A camera ray keeps the soft RTIOW gradient at
  half intensity, as OxidANT's path tracer keeps its procedural sky. A bounce
  sees the sky the raster draws (owner decision 2026-11-16), as OxidANT's
  `bounce_sky` (`shaders/tracer/trace_shading.slang`) lights it: the
  skybox's cube map at LOD 0 while the skybox is on
  (`skybox_radiance_lod0` in `common/skybox_radiance.slang`, its sRGB
  decode), the disabled sky's navy otherwise, times 0.35. Rust scales by
  its ambient strength times its environment intensity; the C++ engine has
  neither setting, so the constant is their defaults (0.35 and 1). The cube
  map is `SKYBOX_CUBEMAP_BINDING` in the ray-tracing set, compute only; it is
  uploaded once and stays `eShaderReadOnlyOptimal`, so no barrier guards it.
  The push constants' `skybox_enabled` carries the switch, and toggling it
  restarts the running mean (`PathTracingHistoryKey::skyboxEnabled`).

Self-intersection is guarded by a 1e-4 offset along the face normal **and**
ray-query `t_min = 0.001` (matches the RT path).

`KATAGLYPHIS_PT_FURNACE=<radiance>` gives every hit one white surface (albedo
1) and replaces both skies with a uniform `<radiance>` environment; the
shader gates on `pc_ray.clearColor.w > 0.5`. `KATAGLYPHIS_PT_FURNACE_METALLIC`
(default 0) and `KATAGLYPHIS_PT_FURNACE_ROUGHNESS` (default 0.5, clamped to
`material_roughness`'s 0.045 floor) pick that surface, through the push
constants' `furnace_metallic` and `furnace_roughness`. All three are read via
`getenv` once per `recordCommands` call, not cached, so several tests sharing
one process each see their own setting.

A single-scatter microfacet lobe returns only its directional albedo `E(mu)`
of the sky: Schlick-GGX Smith masks at grazing angles, and at roughness 1 half
the microfacets reflect below the horizon. Since 2026-11-21 (plan N1)
OxidANT's `brdf.slang` adds back the light the lobe loses to multiple
scattering (Turquin 2019): the specular term gains `1 + f0 (1 / E_ss - 1)`
and the diffuse term takes `1 - E_spec(mu)` instead of `1 - F`, so a white
surface returns the whole sky at every roughness and angle
([`shader-sharing.md`](shader-sharing.md)). The goldens' quadrature,
`white_single_scatter_albedo` (`GoldenMetrics.hpp`: `brdf_sampled`'s lobe in
2048 x 128 cells of equal probability, a `1 - F` diffuse), is what OxidANT's
fit of `E_ss` reproduces; the goldens only log it now. What a lone card
returns after one bounce, 2026-11-21 on aarch64 llvmpipe:

| Surface | Roughness | `mu` | Single scatter | Measured |
| --- | --- | --- | --- | --- |
| metal | 0.3 | 1 | 0.9875 | 0.9997 |
| metal | 0.3 | 0.5 | 0.9012 | 0.9998 |
| metal | 0.7 | 1 | 0.6710 | 0.9978 |
| metal | 0.7 | 0.5 | 0.6123 | 0.9982 |
| metal | 1 | 1 | 0.3069 | 0.9978 |
| metal | 1 | 0.5 | 0.4091 | 0.9974 |
| dielectric | 0.5 | 1 | 0.9958 | 0.9996 |
| dielectric | 0.5 | 0.5 | 1.0029 | 0.9996 |
| dielectric | 0.25 | 1 | 0.9997 | 1.0002 |
| dielectric | 0.25 | 0.5 | 1.0218 | 0.9996 |

The oracles, all in linear radiance from the HDR target:
`GoldenRender.PathTracingPassesTheWhiteMetalFurnaceTest` (the metal rows) and
`GoldenRender.PathTracingPassesTheWhiteFurnaceTest` (the dielectric rows),
each within 0.01 of 1, then the dielectric at roughness 0.5 on the shadow rig,
where 16 bounces compound any per-bounce error. From 2026-11-16 to 2026-11-21
the rig ran at roughness 0.8, where the single-scatter `E(mu)` stays below 1
for every `mu` and so capped the mean; with the gain every
roughness returns 1, and it is back at 0.5 (owner decision 2026-11-21).
It measures 0.9997 and must stay in 0.994-1.005, which a furnace albedo of
0.99 (0.9889) or 1.01 (1.0106) leaves
([`gpu-golden-testing.md`](gpu-golden-testing.md)).

## Temporal accumulation

The kernel folds the frame index into the RNG seed (without it, every frame
drew bit-identical samples) and keeps a running mean in a dedicated `rgba32f`
history image (`ACCUMULATION_IMAGE_BINDING`): full float, because averaging in
the output target (`rgba8` before the HDR unit) would quantize and stall after
a few frames. There is exactly **one** history image, deliberately not per
swapchain image; a compute→compute barrier orders each frame's
read-modify-write against the previous frame's dispatch across command
buffers.

The history resets whenever its premises die:

| Trigger | Where |
| --- | --- |
| Camera moved or projection changed (view and projection compare: FOV, docks, F1) | `PathTracingHistoryKey`, PT branch of `recordRaytracingOrPathTracing` |
| Light direction, colour or radiance changed | same |
| Quality sliders changed (different estimator = biased mean) | same |
| Swapchain resize (extent-sized image is recreated) | `recreateSwapChain` |
| AS rebuilt - model load/reload OR GUI transform moved the traced world | `updateRaytracingDescriptorSets` |

The AS-rebuild trigger was found the hard way: without it, the mean kept
blending frames of the *half-loaded* scene until the camera happened to move,
and that healing motion mimicked convergence well enough to make a naive
golden pass with per-frame sampling disabled.

## Verification

<!-- pt-goldens: PathTracingAccumulatesAndConverges, PathTracingAntiAliasesGeometricEdges, PathTracingRespondsToTheDirectionalLight, PathTracingHonorsTheQualityControls, PathTracingHonorsTheUntexturedMetallicFactor, RaytracedWorldFollowsTheModelTransform, PathTracingPassesTheWhiteFurnaceTest, PathTracingPassesTheWhiteMetalFurnaceTest, PathTracingFloorsAZeroRoughnessTexel, RaytracedLargeMeshDoesNotLoseTheDevice, RaytracedShadowsAreDarkerThanLitGround -->

Eleven PT- and RT-facing goldens in `Test/commit/VulkanEngine/goldenRenderSuite.cpp`,
each red/green-proven against the pre-fix shader. Numbers are post-HDR: before
the HDR unit, lit surfaces clamped at the UNORM ceiling (186 after tonemap)
and hid most of every light response; lifting it widened several margins
dramatically.

- `PathTracingAccumulatesAndConverges` - consecutive-frame changed-pixel
  fraction in a GUI-free crop: 7.6e-3 early, 3.1e-5 late; the
  frame-invariant seed measures exactly 0 early and fails.
- `PathTracingAntiAliasesGeometricEdges` - the shadow rig's slab against the
  sky: a jittered, accumulated primary ray blends that edge in 312 of 312
  columns, a pixel-centre-only kernel in none, so it fails. The oracle, and
  why every edge in the crop stopped being it on 2026-10-12:
  `gpu-golden-testing.md` § The riscv64 light variant.
- `PathTracingRespondsToTheDirectionalLight` - radiance 10 vs 0 on the
  shadow rig: swung fraction 0.751 (0.027 pre-HDR - the ceiling hid three
  quarters of the response) vs exactly 0 for the pre-NEE kernel.
- `PathTracingHonorsTheQualityControls` - bounce cap 8 vs 1 swings 0.255
  (all indirect sky light vanishes); hardcoded-bounds kernel: 7.3e-5.
- `PathTracingHonorsTheUntexturedMetallicFactor` - `metallic_card.gltf`
  (metallicFactor 0.75, no metallic-roughness texture) beside its dielectric
  twin, mirrored about the view axis at 2 spp and 2 bounces, read as linear
  radiance from the HDR target: the metal must return under 0.9 of the
  dielectric. Measured 1.402 against 1.701 (0.824) since the multiple-scattering
  gain gave the rough metal most of its lobe back (0.634 against 1.817 before,
  under a bound of 0.8); a kernel that never reads the metallicFactor lights
  both alike (1.700 against 1.701), as the texture-gated one did (1.816).
- `RaytracedWorldFollowsTheModelTransform` - a GUI move must change the
  traced image: 0.397 vs exactly 0 on a stale TLAS.
- `PathTracingPassesTheWhiteFurnaceTest` - a white dielectric on a lone card
  must return 1 of a uniform sky within 0.01 (the dielectric rows above),
  then the shadow rig at roughness 0.5 and 16 bounces must average
  0.994-1.005 with over 98% of its pixels within ±0.1 of the sky's 1
  (measured 0.9997, 99.9995 % within; a furnace albedo of 0.99 gives 0.9889
  and 1.01 gives 1.0106, both failing). With `brdf.slang`'s diffuse back on
  `1 - F` the card at roughness 0.25, `mu` 0.5 returns 1.0218 and the rig
  1.0198, both failing. The controls below were
  measured on the rig at roughness 0.5 against the earlier bounds 0.98-1.002,
  each failing: a mixture density
  that weights the lobes 50/50 instead of by the selection chance (card
  1.114-1.605, rig 1.280), one at 1.5 times the selection chance (card
  1.007-1.034, rig 1.084), a visible-normal density with `1 / (2 n.v)` for
  `1 / (4 n.v)` (card 0.914-0.965, rig 0.876), and a 1.5 % gain on every
  bounce (card 1.005-1.018, rig 1.011). Until 2026-11-15 the test graded the
  rig's tonemapped frame (mean 225.6-237.6, 98 % within ±6 of 231.6); its
  uniformity check caught the first three, but the 1.5 % gain passed at 232.0.
  A card the rays miss shows the sky's 1 as well, so each view first renders
  one segment (`pathTracingMaxBounces` 1: a hit stays black, a miss shows the
  sky) and its centre square must stay below 0.01 (measured 0), and the rig's
  crop must be at least half covered there (measured 0.748). Turned away from
  the card the views read 1 there and fail, while the card checks read 1 and
  pass; with the camera looking straight up the rig covers none of the crop.
- `PathTracingPassesTheWhiteMetalFurnaceTest` - a white metal on a lone card
  at roughness 0.3, 0.7 and 1 must return 1 of a uniform sky within 0.01 (the
  table above). With `multiscatter_gain` returning 1 the cards fall to their
  single scatter (0.9873, 0.9009, 0.6695, 0.6112, 0.3062, 0.4081) and fail,
  while the dielectric furnace still passes. With the sampling density
  missing Smith's G1, roughness 0.7 at `mu` 0.5 returned 0.452 against the
  single scatter's 0.524 (2026-11-16), and failed. The same one-segment frame
  first proves the card under both views' centre square.
- `PathTracingFloorsAZeroRoughnessTexel` - a white dielectric whose
  metallic-roughness texel has G = 0 beside one at the 0.045 floor, at 2 spp
  and 2 bounces: the two radiances must agree within 3%. Measured 1.4305
  against 1.4317; without the floor on the product the G = 0 card averages
  6409 (the 0/0 density meets `max(pdf, 1e-6)`), and fails.
- `RaytracedLargeMeshDoesNotLoseTheDevice` - the RT pipeline must survive
  10 frames against the shipped large dinosaur-mesh debug scene; it scopes the
  `VK_ERROR_DEVICE_LOST` the PT reproducer hit on the same mesh to
  `path_tracing.slang`/`RayQuery` rather than the shared vertex-upload/BDA
  path (`gpu-golden-testing.md` § Known issue: path-tracing compute
  device-lost on the large dinosaur mesh, BACKLOG.md).
- `RaytracedShadowsAreDarkerThanLitGround` - the RT closest-hit path, not
  PT: on the shadow rig the darkest decile of a GUI-free crop must stay
  under 5% of the brightest decile's luminance. Measured 0 vs 244 with the
  fix, 41.6 vs 248 when `raytrace.rchit.slang` seeds its payload with the
  unlit albedo (`e852ac99`).

Seven more goldens cover the kernel without the `PathTracing` prefix the
marker counts: `PathTracedMetalCardReflectsTheSkyAndTheSun` (a white metal at
roughness 0.3 mirrors the sky at 200 luma, 219 under the old gradient, and,
tilted halfway to the sun, a white highlight; without the lobe 0 and 71),
`PathTracedMirrorReflectsTheSkyboxItDraws` (a white metal card at the
roughness floor, sun off: with the skybox off it returns 0.35 times the navy,
measured (0, 0, 0.1749); with it on, 0.35 times the sky the raster draws
behind the camera, sRGB-decoded from the swapchain, within 5 % plus 0.001
per channel, measured (0.0234, 0.0209, 0.0240) against (0.0232, 0.0206,
0.0236) on aarch64 llvmpipe; the old gradient on bounces fails both),
`PathTracedUnlitCardKeepsItsAlbedo` (an unlit card's HDR value matches
forward's to 0.01; metallic first gave 0),
`HdrTargetStaysBelowTheHalfFloatMaximum`,
`NormalMappedBackFaceFlipsItsWholeTangentFrame` and
`MirroredModelKeepsItsWindingAndNormalMapRelief` (all four modes; a mirrored
instance's bitangent follows the determinant's sign), and
`ReloadedModelIsVisibleInPathTracing`, which reloads the dielectric
`cube_textured.gltf` and finds its green and magenta checker: the metal card
it reloaded before went black without a lobe, and the frame still changed,
because the shadow rig vanished.

The instrument cautions these goldens were built on are in
[`gpu-golden-testing.md`](gpu-golden-testing.md)'s cautions section.

Editing the kernel needs no C++ rebuild: the Slang compile script regenerates
the `.spv` and the engine loads it at startup.
`scripts/windows/Build-SlangShaders.ps1` is the build/CI path.

## Open work (BACKLOG, "PT survey" section)

None: all 11 items of the 2026-07-22 PT survey are resolved (`8cf33378`).
The last, RNG decorrelation (item 11), closed as a measured null result:
lag-1 autocorrelation of the depth-2 noise field is -0.012 (lag-16: +0.015),
because the LCG pre-step plus the PCG output hash already decorrelate
adjacent linear seeds. `PathTracingAccumulatesAndConverges` logs both lags,
unasserted, so a future seed change shows its noise character.
