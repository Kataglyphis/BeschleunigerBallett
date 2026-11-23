# Sharing Shader Code Between the C++ Vulkan Engine and the Rust WebGPU Renderer

**[Slang](https://shader-slang.com/) is the single source of truth.** One
`.slang` source compiles to **SPIR-V** (Vulkan/C++) and **WGSL** (WebGPU/Rust),
including shared entry points where the two renderers' passes coincide.

## The pipeline (current)

[`shader-build-pipeline.md`](shader-build-pipeline.md) owns the build wiring,
compile commands, output directories, staleness rules and fast iteration. On
the Rust side its combined WGSL emit replaces the crates' hand-written WGSL;
their `include_str!` calls are unchanged and now read Slang-emitted text.
`histogram.wgsl` is the one hand-written WGSL file left
([`shader-build-pipeline.md`](shader-build-pipeline.md) says why).

**Ownership:** OxidANT's `shaders/` owns all WebGPU Slang entry points and the
shared modules under `shaders/common/`: `aces`, `brdf`, `brdf_energy_fit`,
`constants`, `fullscreen`, `hdr_safe`, `noise`, `path_estimator`,
`punctual_light`, `punctual_shadow`, `shadow_pcf`, `sky_model`,
`trace_constants` and `volumetric_clouds`. This repo
consumes them through `third_party/OxidANT/shaders`; Vulkan entry points and
Vulkan-specific material modules stay in `Resources/ShadersSlang/`. OxidANT
can compile and check its generated WGSL without a superproject checkout.

## Why Slang (not GLSL, not naga/WGSL-as-source)

WGSL used to be the shared-math source, translated by `naga` into GLSL the C++
engine `#include`d (**Historical note** below). That shared *functions* but not
*entry points*: the generated SPIR-V/GLSL carried WebGPU binding decorations
that do not match the Vulkan engine's descriptor layout.

Slang's parameter binding (`[vk::binding(i,s)]` → `@binding(i) @group(s)` in
WGSL) aligns the two binding models, so entry points are shareable too. Slang
also targets **SPIR-V directly**, ray tracing pipeline and ray query included.
WebGPU/WGSL has neither, so those shaders are Slang only for language unity on
the C++ side and emit one target.

### Target status (verified against slangc in VulkanSDK 1.4.350.0)

| Target | Status | Notes |
| --- | --- | --- |
| SPIR-V (Vulkan) | **stable** | Rasterization, compute, **ray tracing pipeline**, **ray query**, descriptor sets, push constants |
| WGSL (WebGPU) | **experimental** | Functional (official `wgpu-html5` example uses it). **No** ray tracing, ray query, mesh/tessellation/geometry shaders, wave intrinsics, `f64`, `i8`/`u8` |

**WGSL fallback policy:** a shared shader that Slang cannot express for the
WGSL target at all (not merely emit) gets hand-written WGSL and **no** Slang
source, rather than a Slang source nothing on the C++ side consumes that would
still have to be kept in sync. `histogram.wgsl` is the one hand-written file today, though
Slang can now express it ([`shader-build-pipeline.md`](shader-build-pipeline.md)).

## Architecture: Slang-native, not `#include`

Slang is module- and entry-point-centric: a no-entry-point math module cannot
be emitted to GLSL/WGSL text, and imported functions are name-mangled in the
emit, so sharing goes through Slang's module system, not textual `#include`:

- **Shared math** lives under `third_party/OxidANT/shaders/common/`. All of
  its fourteen modules but `punctual_shadow.slang`, WebGPU's alone, are imported
  on both targets. The C++-only modules sit under
  `Resources/ShadersSlang/common/`, shared across the raster, deferred,
  shadow-map, ray tracing and path tracing paths. The shared-module-targets
  table below is the exact, gated mapping.
- **Entry points** `import` the math modules and compile to the target(s)
  their renderer needs. Slang links modules internally, so mangling is a
  non-issue.
- Where the passes genuinely differ (they composite differently), each
  renderer keeps its own entry points over the same imported math.
- **Target-local modules** sit beside their entry points, not under `common/`:
  `forward/forward_lighting.slang` (the WebGPU forward and G-buffer lighting)
  and `tracer/bvh_traverse.slang`, `tracer/trace_shading.slang` (the WebGPU
  compute ray and path tracer). Slang
  finds them in the importer's own directory; they compile to WGSL only, and
  the reachable-function gate follows those imports as it does `common/`.

## What is wired today

**Shared math modules** (imported by entry points, never emitted directly):

- `common/aces.slang` — ACES filmic tonemap (Narkowicz 2015), its inverse
  `aces_tonemap_inverse` (the sky's photo faces enter as the radiance whose
  tonemap shows them) and the exact IEC 61966-2-1 sRGB encode. The five
  coefficients are `ACES_A` to `ACES_E`; `CloudAmbient.hpp`'s CPU pair takes
  `kAcesCoefficients`, which `BuildIntegrity.CloudAmbientAcesCurveIsAcesSlangs`
  holds to them.
- `common/constants.slang` — `PI` and `MIN_ROUGHNESS` (0.045), for `brdf`,
  `path_estimator`, `volumetric_clouds`, `material_rules.slang` and the path
  tracer's furnace card. Both.
- `common/trace_constants.slang` — every secondary ray's bounds:
  `TRACE_TMIN` 0.001, `TRACE_TMAX` 10000, `TRACE_SURFACE_OFFSET` 0.0001 along
  the face normal, and `TRACE_LIGHT_SPAN` (1 - 1e-4), where a punctual light's
  shadow ray stops. `raytrace.rgen`, `raytrace.rchit`, `path_tracing` and
  OxidANT's tracer. Both.
- `common/brdf.slang` — the Epic Games / Unreal 4 microfacet PBR model
  (`distribution_ggx`, `geometry_smith_k`, `fresnel_schlick`, `brdf_direct`),
  used by both renderers' forward/RT shading and next-event estimation, and
  `brdf_sampled`, the same BRDF with Epic's Smith remap for sampled light
  (`k = alpha / 2` instead of the analytic `(r + 1)^2 / 8`), which both path
  tracers' bounces weight by. Both compensate the GGX lobe's lost multiple
  scattering (Turquin 2019): `multiscatter_gain` scales the specular term by
  `1 + f0 (1 / E_ss - 1)`, and the diffuse term takes `1 - specular_albedo`
  (E_spec(mu)) instead of `1 - F`, times `1 - metallic`, so a white surface
  returns all of a uniform sky. `ggx_energy` reads E_ss, split by Fresnel,
  from `common/brdf_energy_fit.slang`, a generated 14 x 14 cubic B-spline
  fitted to `brdf_sampled`'s single-scatter albedo; both remaps take it. A
  shading point evaluates it once, `ggx_energy(dot(n, v), roughness)`, and hands
  it to `brdf_direct`, `brdf_sampled`, `brdf_smith_k` and `sample_bounce` as
  `energy`, so a light costs no fit of its own.
- `common/path_estimator.slang` — both path tracers' estimator: the PCG RNG
  and per-pixel seed (`rng_next`, `path_rng_seed`), Russian roulette
  (`russian_roulette_survives`) and `sample_bounce`, the one-sample MIS over
  the GGX visible-normal and cosine lobes. The C++ `path_tracing.slang`
  imports it; OxidANT's `tracer/traced.slang` too, and
  `tests/path_estimator_test.slang` emits every function to both targets.
- `common/shadow_pcf.slang` — the PCF kernel over a cascaded shadow map with a
  receiver-plane depth bias (`receiver_plane_depth_slope`, `pcf_visibility`):
  each tap compares at the depth of the receiver's plane beneath it, the slope
  taken from the shading normal and the affine light matrix, so no derivatives
  are needed. Taps go through the `IShadowTaps` interface, whose
  implementations bounds-check in the sampling statement:
  `cascaded_shadow.slang`'s `CombinedTaps` (the C++ forward and deferred
  paths) and OxidANT's `forward/forward_lighting.slang`. Both.
- `common/punctual_light.slang` — `KHR_lights_punctual` as both renderers
  shade it. The C++ engine reads it through `common/punctual_lights.slang`,
  which owns set 0's `PUNCTUAL_LIGHTS_BINDING` (a count row, then the four
  rows per light) and `PUNCTUAL_SHADOW_MAP_BINDING` (one depth cube per
  shadowed light): forward and deferred shadow the first four point and
  spot lights by their cubes and add the rest unshadowed, the closest-hit
  shader traces one shadow ray per light, and the path tracer samples one
  light per bounce. Both.
- `common/punctual_shadow.slang` — the WebGPU renderer's cube shadows as
  layers of one 2D depth array (slot * 6 + face): the row-3 unpack, the
  cube-map face selection and the depth formula `punctual_lights.slang`
  also holds. Its face-picked uv cannot leave [0,1], so its tap needs no
  range check. wgsl.
- `common/noise.slang` — 3D simplex noise + fractal Brownian motion, checked
  on both targets by OxidANT's `tests/noise_test.slang`; no production shader
  imports it yet.
- `common/fullscreen.slang` — the fullscreen-triangle vertex trick
  (`vid/2*4-1`), used by every fullscreen pass on both sides. Its `uv` is the
  one per-target difference in shared code: uv (0,0) is always the target's
  top-left texel, but clip-space y = -1 is the top row in Vulkan and the bottom
  row in WebGPU, so `fullscreen_vs` picks the V formula with `__target_switch`
  (`spirv`: `v = (y + 1) / 2`; every other target: `v = 1 - (y + 1) / 2`). The
  WGSL emit is byte-identical to the single formula it replaced.
  `fullscreen_uv_to_ndc` is its exact inverse, switched the same way, for the
  passes that reconstruct a position from the uv (`deferred.slang` on SPIR-V,
  `ssao.slang` on WGSL), and `fullscreen_ndc_to_uv` the reverse, for a
  projected point's texel (`ssao.slang`, `gpu_cull.slang`).
  `BuildIntegrity.NoShaderRedeclaresTheFullscreenUvMapping` fails on a
  hand-written copy in a `fullscreen` importer. A wrong V mirrors the frame
  about its horizontal centre line, which a vertically symmetric test scene
  cannot show; `GoldenRender.WorldUpIsScreenUp` (C++) and
  `world_up_is_screen_up` (OxidANT `tests/headless.rs`) pin it.
- `common/material_fetch.slang` — the `objectDescription` binding and the
  `fetch_object_description`/`fetch_material` lookups on it. Spirv only: the
  raster (`rasterizer.slang`, `deferred.slang`, `shadow_map.slang`),
  closest-hit/any-hit ray tracing (`raytrace.rchit.slang`,
  `raytrace.rahit.slang`) and path tracing (`path_tracing.slang`) entry points.
- `common/material_rules.slang` — the binding-free glTF material rules on the
  fetched `ObjMaterial`: `material_roughness`, `material_metallic_roughness`
  (glTF SS3.9.2 G=roughness/B=metallic channel swizzle) and `alpha_masked_out`
  (alphaMode MASK). Split out of `material_fetch.slang` so `path_tracing.slang`
  can call them, as with `base_color.slang` below. Spirv only, every C++
  shading path (raster, deferred, shadow map, ray tracing, path tracing).
- `common/material_textures.slang` — the one declaration of the
  `textures[]`/`textureSamplers[]` arrays (`TEXTURES_BINDING`/
  `SAMPLER_BINDING`), plus explicit-LOD (`SampleLevel`) and implicit-LOD
  (`Sample`) helpers for the alpha (map_d), normal, metallic-roughness and
  emissive slots. Both LOD shapes share one module because an implicit-LOD
  helper an entry point does not call is dead-code-eliminated before Slang's
  capability checking, so a compute/ray-tracing importer that calls only the
  explicit-LOD half never picks up a fragment-only capability. The base-colour
  slot is not here: its sampling stays inline at each call site, next to the
  per-shader control flow (albedo assembly, discard, G-buffer packing). Spirv
  only: `rasterizer.slang`, `deferred.slang`,
  `rasterizer/shadows/shadow_map.slang`, `raytrace.rchit.slang` and
  `alpha_test.slang`; `path_tracing.slang` gets it through `alpha_test.slang`
  and also imports it directly for its own base-colour sample.
- `common/skybox_radiance.slang` — the skybox's linear radiance per
  direction for the path tracer (`skybox_radiance_lod0`, compute has no
  derivatives) and the disabled-sky navy, which
  `BuildIntegrity.CloudAmbientNavyIsTheDisabledSkyRadiance` holds to
  `sky/sky.slang`'s. Spirv only. The drawn sky is OxidANT's `sky/sky.slang`
  (the entry-point table below). OxidANT's twin of the bounce, `bounce_sky`
  in `tracer/trace_shading.slang`, reads its own bindings and sky, so the C++
  kernel matches its rule, not its code ([`path-tracing.md`](path-tracing.md)).
- `common/cascaded_shadow.slang` — PCF cascaded shadow-map sampling
  (`calc_cascaded_shadow`). Spirv only: the raster and deferred lighting paths
  (`rasterizer.slang`, `deferred.slang`).
- `common/base_color.slang` — `base_color` (glTF base colour =
  `baseColorFactor * sampled texture`) and `transform_uv`
  (KHR_texture_transform), in a bindingless module so every spirv shading path
  can import it. It keeps `rasterizer/rasterizer.slang`,
  `deferred/deferred.slang`, `raytracing/raytrace.rchit.slang` and
  `path_tracing/path_tracing.slang` in agreement with the reference
  `prim.base_color * baseSample` in `forward/forward.slang` (wgsl, not an
  importer); before it, those four dropped `baseColorFactor` whenever a
  material also had a texture. The alpha half travels separately:
  the glTF loader puts `baseColorFactor[0..2]` in `diffuse` and
  `baseColorFactor[3]` in `dissolve`, which `alpha_masked_out()` multiplies
  into the sampled alpha (the `ObjMaterial` table in
  [`model-loading.md`](model-loading.md)). Spirv only.
- `common/emission.slang` — the glTF emissive rule (`material_emission`:
  `emissiveFactor` \* sampled `emissiveTexture`, or the factor alone without a
  texture). Spirv only, all four raster/RT shading paths (`rasterizer.slang`,
  `deferred.slang`, `raytrace.rchit.slang`, `path_tracing.slang`).
- `common/normal_map.slang` — glTF tangent-space normal mapping
  (`apply_normal_map`, glTF SS3.9.3). Callers hand it the authored geometric
  normal and the face side; on a doubleSided back face it negates the mapped
  normal as a whole, which reverses tangent, bitangent and normal together
  (glTF SS3.9.2). Negating only the normal before the call kept the tangent
  and mirrored the map's relief along it, until 2026-11 on all four paths.
  Spirv only, the same four paths as `emission.slang`;
  `forward/forward.slang`'s `fs_main` (wgsl) reimplements the same math
  locally.
- `common/hdr_safe.slang` (OxidANT's since 2026-11-23) — `hdr_safe`, the clamp
  every write to an RGBA16F HDR target ends in: radiance to `[0, HDR_MAX]` with
  `HDR_MAX = 65000`, under fp16's 65504, and NaN to 0, found by its bits
  because WGSL may assume floats are finite. Here: the forward and deferred
  outputs and the deferred G-buffer's emission, the ray-generation store, and
  the path tracer's per-sample sum and output store; in OxidANT the forward
  pass, the sky, bloom and the tracer. Both.
- `common/alpha_test.slang` — the glTF MASK alpha test for ray queries:
  `raytrace.rahit.slang`'s any-hit shader and `path_tracing.slang`'s ray-query
  candidate loop (which has no any-hit stage of its own). Spirv only.
- `common/volumetric_clouds.slang` — the volumetric clouds: the noise
  volume's texel (`noise_volume_texel`), `cloud_unpack` of the SceneUBO's four
  cloud `vec4`s, the axis-aligned box test, density, the light's optical
  depth (`light_optical_depth`), the sun's multiple-scattering octaves, the
  sky ambient (`CloudAmbient`) and `march_clouds`, which stops at the scene
  depth (`cloud_scene_distance`) and steps a long path more often. Every
  caller passes the ambient its host averaged from the sky it draws (C++:
  `compute/clouds.slang`, from the `SceneUBO`'s
  `cloudAmbientTop`/`cloudAmbientBottom`). The noise comes in through the `ICloudNoise`
  interface, which Slang specializes per caller: the C++ `compute/clouds.slang`
  passes its combined `Sampler3D`, OxidANT's `clouds/cloud_march.slang` a
  texture plus a sampler. Both. [`clouds.md`](clouds.md) owns the behaviour.
- `common/sky_model.slang` — the analytic sky's horizon/zenith/ground gradient
  (`sky_gradient`), sun disk (`sun_disk`) and diffuse hemisphere irradiance
  approximation (`hemisphere_irradiance`), shared by `sky/sky.slang` (both
  renderers' background), `forward/forward_lighting.slang` (the analytic IBL
  fallback, whose reflections and irradiance must agree with the background)
  and the WebGPU tracer's miss shading (`tracer/trace_shading.slang`). The C++
  engine never draws `sky.slang`'s procedural mode: without faces its cube map
  holds the same gradient, baked by `scene/sky_box/AnalyticSky.hpp`, which copies
  the three colours `BuildIntegrity.AnalyticSkyMatchesOxidantsSkyModel` pins.
  There is deliberately no combined `sky_radiance`: each caller takes its
  light direction/intensity from a different uniform block, so each keeps a
  thin local wrapper over `sky_gradient` + `sun_disk`. `tests/sky_constants.rs`
  pins the three gradient constants a third time on the Rust CPU side
  (`render::ibl::SKY_ZENITH`/`SKY_HORIZON`/`SKY_GROUND`, consumed by
  `EnvironmentImage::sky`'s panorama).

**Entry-point shader targets:** two entry-point sources serve both renderers.
`sky/sky.slang` is the sky each draws into its HDR target: the C++ engine
compiles its `vs_vulkan_main` (`SV_VulkanVertexID`, since `SV_VertexID` needs
draw parameters a Raspberry Pi 5's v3dv lacks) and `fs_main` to SPIR-V through
OxidANT's manifest rows, which build them in its CI too, and fills
`SkyUniforms` from `scene/sky_box/SkyUniforms.hpp`; the combined
`sky.wgsl` carries the extra entry unused. `fxaa/fxaa.slang` is FXAA after the
tonemap: the C++ engine compiles only its `fs_main`, behind `post.slang`'s
`SV_VulkanVertexID` vertex, and fills `FxaaUniforms` from
`renderer/FxaaUniforms.hpp` (z 0: post's intermediate holds sRGB-encoded
bytes). Every other source compiles to one target. `tonemap/tonemap.slang` (WGSL) and `post/post.slang` (SPIR-V) look
like one pass but are separate sources: the C++ side's `post/post.slang`
also composites clouds via push constants, so it never merged with the Rust
tonemap shader.

<!-- shader-targets:begin -->
| File | Target |
| --- | --- |
| `bloom/bloom.slang` | wgsl |
| `clouds/cloud_march.slang` | wgsl |
| `clouds/cloud_noise.slang` | wgsl |
| `compute/clouds.slang` | spirv |
| `compute/noise_volume.slang` | spirv |
| `deferred/deferred.slang` | spirv |
| `depth_resolve/depth_resolve.slang` | wgsl |
| `draw_cull/draw_cull.slang` | wgsl |
| `forward/forward.slang` | wgsl |
| `fxaa/fxaa.slang` | both |
| `gpu_cull/gpu_cull.slang` | wgsl |
| `ibl/ibl.slang` | wgsl |
| `occlusion_bbox/occlusion_bbox.slang` | wgsl |
| `path_tracing/path_tracing.slang` | spirv |
| `post/post.slang` | spirv |
| `rasterizer/rasterizer.slang` | spirv |
| `rasterizer/shadows/shadow_map.slang` | spirv |
| `raytracing/raytrace.rahit.slang` | spirv |
| `raytracing/raytrace.rchit.slang` | spirv |
| `raytracing/raytrace.rgen.slang` | spirv |
| `raytracing/raytrace.rmiss.slang` | spirv |
| `raytracing/shadow.rmiss.slang` | spirv |
| `sky/sky.slang` | both |
| `ssao/ssao.slang` | wgsl |
| `tex_quad/tex_quad.slang` | wgsl |
| `tonemap/tonemap.slang` | wgsl |
| `tracer/trace_debug.slang` | wgsl |
| `tracer/trace_resolve.slang` | wgsl |
| `tracer/traced.slang` | wgsl |
<!-- shader-targets:end -->

**CI guards:** OxidANT's vector kernels, `tests/<module>_test.slang` for
`brdf`, `noise`, `path_estimator`, `hdr_safe`, `aces` and `fullscreen`, run every
public function of their `common/` module once per case of
`tests/vectors/<module>.json` (inputs, f64 references, a tolerance per output;
OxidANT's `docs/shaders.md` § Vector kernels). Both targets are checked on a
device: OxidANT's `shader_vectors.rs` dispatches the WGSL, and
`Integration.SharedSlangKernelsMeetTheirVectors`
(`Test/commit/VulkanEngine/sharedShaderVectorsSuite.cpp`) the SPIR-V this
engine's build emits from the imported manifest rows, on a headless Vulkan
device, naming module, case and component of every miss. It loops over every
vector file, so the next module's kernel needs no edit here. The first two
were this engine's compile-only guards until 2026-11-21. The table above
excludes them (see its gating test). A spirv-only module has nothing to guard until it gains a real wgsl
consumer, which the table below makes checkable. OxidANT's
`tests/volumetric_clouds_test.slang` and `tests/shadow_pcf_test.slang` are the
reverse guards: SPIR-V only, they import `volumetric_clouds` with a
`Sampler3D` and `shadow_pcf` with a `Sampler2DArrayShadow` the way the C++
engine does, so OxidANT's own CI proves both still emit for this engine;
`tests/constants_test.slang` does the same for `constants` and
`trace_constants`. `BuildIntegrity.NoShaderCopiesASharedLeafModule` fails a
file in either tree that keeps a private copy of a leaf module's constant,
literal or function (OxidANT's `scripts/check-shared-shader-copies.py` holds
its own tree to the same rules).

The local cloud-volume shader is `compute/noise_volume.slang` so Slang's
`import noise;` resolves OxidANT's `common/noise.slang`. A local file named
`noise.slang` shadowed that shared module in the compiler's include order;
the dual-target test then failed with `E30015 undefined identifier` on
`snoise` and `fbm` before it could test the shared math.
`BuildIntegrity.NoVulkanShaderShadowsASharedModule` now fails on any local
`.slang` named like a file in OxidANT's `shaders/`, which is why this repo's
`common/hdr_safe.slang` went in the pin bump that brought OxidANT's.

**Shared-module targets:** the per-`common/`-module counterpart of the table
above, computed the same way (an import-graph walk over `shader-manifest.json`)
and gated by the same kind of test. `tests/*.slang` count as real consumers,
which is why `brdf` and `noise` show `both` although `noise` has no production
wgsl consumer yet. `(unused)` would mark a module under `common/` that nothing
imports.

<!-- shared-module-targets:begin -->
| Module | Target |
| --- | --- |
| `common/aces.slang` | both |
| `common/alpha_test.slang` | spirv |
| `common/base_color.slang` | spirv |
| `common/brdf.slang` | both |
| `common/brdf_energy_fit.slang` | both |
| `common/cascaded_shadow.slang` | spirv |
| `common/constants.slang` | both |
| `common/emission.slang` | spirv |
| `common/fullscreen.slang` | both |
| `common/hdr_safe.slang` | both |
| `common/material_fetch.slang` | spirv |
| `common/material_rules.slang` | spirv |
| `common/material_textures.slang` | spirv |
| `common/noise.slang` | both |
| `common/normal_map.slang` | spirv |
| `common/path_estimator.slang` | both |
| `common/punctual_light.slang` | both |
| `common/punctual_shadow.slang` | wgsl |
| `common/punctual_lights.slang` | spirv |
| `common/push_constants.slang` | spirv |
| `common/raster_geometry.slang` | spirv |
| `common/scene_types.slang` | spirv |
| `common/shadow_pcf.slang` | both |
| `common/sky_model.slang` | both |
| `common/skybox_radiance.slang` | spirv |
| `common/trace_constants.slang` | both |
| `common/volumetric_clouds.slang` | both |
<!-- shared-module-targets:end -->

**Status:** all C++ shaders are on Slang SPIR-V (all 8 loading sites), and all
Rust shaders on Slang-emitted WGSL except `histogram.wgsl`, still hand-written
though Slang can now express it.

## Known glTF loader divergences (not shader-shared, but the two renderers must stay honest about it)

Both renderers parse glTF with OxidANT's loader since 2026-11-18; the C++ column is what its cxx
bridge flattens (`crates/assets/src/flat.rs`) and `GltfLoaderRust.cpp` copies into `ObjMaterial`
and `Vertex`.

| Feature | C++ `GltfLoader` | Rust `gltf_loader` | Note |
| --- | --- | --- | --- |
| UV sets beyond TEXCOORD_0 | `uv_set_mask`'s low four bits become `ObjMaterial::uv1SlotMask`; `TEXCOORD_1` lands in `Vertex::texture_coords1` (a copy of TEXCOORD_0 when absent) | `uv_set_bit` reads each slot's `texCoord`, which `KHR_texture_transform`'s `texCoord` overrides, into `uv_set_mask`; TEXCOORD_2+ warns and samples TEXCOORD_0; `tangent_uv_set` follows the normal texture | Parity for the four slots both read, the shadow passes included (C++'s `shadow_map.slang` takes TEXCOORD_1 at location 5). |
| `KHR_materials_unlit` | the flat material's `unlit` becomes `ObjMaterial::unlit`, honoured in all four shading paths (`rasterizer.slang`, `deferred.slang`, `raytrace.rchit.slang`, `path_tracing.slang`) | reads `material.unlit()` into `CpuMaterial::unlit` | Parity: both loaders and every shading path on both sides treat the extension identically. |
| `occlusionTexture` + `occlusionStrength` | the flat material drops it (`flat.rs` masks off bit 4); `ObjMaterial` has no occlusion field | reads `occlusion_texture()`/`occlusion_strength` into `CpuMaterial::occlusion_texture`/`occlusion_strength` | Intended divergence: glTF scopes `occlusionTexture` to indirect light, and neither C++ raster path has an indirect term (`rasterizer.slang`'s `fs_main` and the deferred lighting pass are `brdf_direct` + shadow + emissive only), so an AO map has nothing to attenuate. A future IBL or ambient term would make the slot meaningful. |
| Per-slot `KHR_texture_transform` | the flat material gives base-colour, normal, metallic-roughness and emissive each their own transform (`uv_transform_row0`/`_row1`, `normal_uv_transform_row0`/`_row1`, `metallic_roughness_uv_transform_row0`/`_row1`, `emissive_uv_transform_row0`/`_row1`); no occlusion slot, since occlusion textures are not read (previous row) | the same four slots plus `occlusion_uv_transform` | Parity wherever C++ has a texture to transform; the only gap is the occlusion gap above, not a `KHR_texture_transform` bug. |
| `alphaMode` `BLEND` | the flat model sets `ObjMaterial::alphaBlend` and `MeshRange::alphaBlended`; forward and deferred skip the mesh, then draw it last, farthest first by its bounds centre's distance to the eye, src-alpha blended with depth test on and writes off (deferred shades it forward after the lighting pass); the tracers render it opaque | `AlphaMode::Blend` drives the same sorted pass (the `alpha_blend` primitive flag and its distance sort), forward and deferred; the tracer carries `ALPHA_BLEND` but shades it opaque too | Parity in raster. Neither renderer sorts within a mesh, so a self-overlapping BLEND mesh composites in index order. |
| Roughness default when `Pr` is unauthored (OBJ path) | `ObjLoader` treats an absent (or `0.0`) `Pr` as unauthored and leaves `ObjMaterial::roughness` at its sentinel, so `material_roughness()` derives it from `shininess`/`Ns` | the OBJ→glTF converter (`obj_to_gltf`) emits `roughnessFactor` from `Ns` via the same `sqrt(2/(Ns+2))` curve, clamped `[0.045, 1.0]`, when `Pr` is absent | Parity: both paths derive the same roughness from the `.mtl`'s `Ns` when `Pr` is unauthored; `material_roughness()` (`material_rules.slang`) is the shared reference both sides implement. |
| `map_d` (OBJ per-texel opacity map) | `ObjLoader::loadTexturesAndMaterials` reads `map_d` into `ObjMaterial::alphaTextureID` as a glTF `MASK` cut-out at the conventional `alphaCutoff = 0.5`, honoured in `rasterizer.slang`, `deferred.slang`, `shadow_map.slang` and `common/alpha_test.slang` | the in-memory GLB the native app opens an OBJ through (`app/obj_glb.rs`, `apply_masks`) composites the mask's red channel into the base colour's alpha and emits `alphaMode: MASK` at the same 0.5 cutoff; `obj_to_gltf::convert_file`'s glTF file output has no composited image, warns and draws the material solid | Parity in the app. Only a converted file loses the cut-out, since glTF has no opacity slot to write `map_d` into without a composited image. |

## Which C++ shading path reads which `ObjMaterial` field

A new `ObjMaterial` field is not done when *one* shading path reads it:
`BuildIntegrity.EveryObjMaterialFieldIsReadByAShader` only asks whether *some*
`.slang` source mentions `material.<field>`, so a field read by
`rasterizer.slang` and silently dropped by `path_tracing.slang` still passes.
This table is the per-path answer: one row per `ObjMaterial` member
(`common/scene_types.slang`), one column per C++ shading path
(`deferred.slang`'s is `geometry_fs_main`, `raytrace.rchit.slang`'s
`rchit_main`, `path_tracing.slang`'s `path_tracing_main`); the two alpha-only
consumers `shadow_map.slang`/`alpha_test.slang` share a column, since neither
shades a colour. A cell names the expression or helper that reads the field on
that path, or says "not read" and why. The asymmetries it pins, so they stop
being rediscovered one field at a time:

- `metallic` and `roughness` are a full factor-or-texture read on all four
  paths. Until 2026-11 `path_tracing.slang` read metallic on the texture
  branch alone, so an untextured factor (`metallic_card.gltf`'s 0.75) was
  ignored, and had no specular lobe to use roughness with.
- `shininess` is only reachable through `material_roughness()`'s
  negative-`roughness` fallback ("(fallback)" in its row).
- `dissolve` is read only inside `alpha_masked_out()`, so a path with no MASK
  alpha test of its own (`raytrace.rchit.slang`, `path_tracing.slang`'s own
  shading; their MASK test runs earlier, in `alpha_test.slang`'s
  `ray_hit_masked_out()`) never touches it.

| Member | `rasterizer.slang` | `deferred.slang` | `raytrace.rchit.slang` | `path_tracing.slang` | `shadow_map.slang` / `alpha_test.slang` |
| --- | --- | --- | --- | --- | --- |
| `diffuse` | `material.diffuse` (untextured) / `base_color()` (textured) | `material.diffuse` (untextured) / `base_color()` (textured) | `material.diffuse` (untextured) / `base_color()` (textured) | `material.diffuse` (untextured) / `base_color()` (textured) | not read — alpha testing only discards a fragment, never shades a colour |
| `emission` | `resolved_emission()` | `resolved_emission()` | `resolved_emission_lod0()` | `resolved_emission_lod0()` (skipped in furnace-test mode) | not read — no lighting |
| `shininess` | `resolved_metallic_roughness()` (fallback) | `resolved_metallic_roughness()` (fallback) | `resolved_metallic_roughness_lod0()` (fallback) | `resolved_metallic_roughness_lod0()` (fallback) | not read — no lighting |
| `dissolve` | `alpha_masked_out()` | `alpha_masked_out()` | not read — the MASK cut-out for ray-traced hits runs in the any-hit shader (`raytrace.rahit.slang`, via `alpha_test.slang`) | not read — candidates are alpha-tested earlier, by `alpha_test.slang`'s `ray_hit_masked_out()` | `alpha_masked_out()` |
| `textureID` | `material.textureID` | `material.textureID` | `material.textureID` | `material.textureID` | `material.textureID` |
| `alphaCutoff` | `alpha_masked_out()` | `alpha_masked_out()` | not read — see `dissolve` | not read — see `dissolve` | `material.alphaCutoff` (outer guard) / `alpha_masked_out()` |
| `uv_transform_row0` | `base_color_uv()` | `base_color_uv()` | `base_color_uv()` | `base_color_uv()` | `base_color_uv()` |
| `uv_transform_row1` | `base_color_uv()` | `base_color_uv()` | `base_color_uv()` | `base_color_uv()` | `base_color_uv()` |
| `metallic` | `resolved_metallic_roughness()` | `resolved_metallic_roughness()` | `resolved_metallic_roughness_lod0()` | `resolved_metallic_roughness_lod0()` | not read — no lighting |
| `roughness` | `resolved_metallic_roughness()` | `resolved_metallic_roughness()` | `resolved_metallic_roughness_lod0()` | `resolved_metallic_roughness_lod0()` | not read — no lighting |
| `emissiveTextureID` | `resolved_emission()` | `resolved_emission()` | `resolved_emission_lod0()` | `resolved_emission_lod0()` | not read — no lighting |
| `normalTextureID` | `resolved_normal()` | `resolved_normal()` | `material.normalTextureID` (outer guard) / `resolved_normal_lod0()` | `material.normalTextureID` (outer guard) / `resolved_normal_lod0()` | not read — no normal mapping |
| `normalScale` | `resolved_normal()` | `resolved_normal()` | `resolved_normal_lod0()` | `resolved_normal_lod0()` | not read — no normal mapping |
| `metallicRoughnessTextureID` | `resolved_metallic_roughness()` | `resolved_metallic_roughness()` | `resolved_metallic_roughness_lod0()` | `resolved_metallic_roughness_lod0()` | not read — no lighting |
| `normal_uv_transform_row0` | `resolved_normal()` | `resolved_normal()` | `resolved_normal_lod0()` | `resolved_normal_lod0()` | not read — no normal mapping |
| `normal_uv_transform_row1` | `resolved_normal()` | `resolved_normal()` | `resolved_normal_lod0()` | `resolved_normal_lod0()` | not read — no normal mapping |
| `metallic_roughness_uv_transform_row0` | `resolved_metallic_roughness()` | `resolved_metallic_roughness()` | `resolved_metallic_roughness_lod0()` | `resolved_metallic_roughness_lod0()` | not read — no lighting |
| `metallic_roughness_uv_transform_row1` | `resolved_metallic_roughness()` | `resolved_metallic_roughness()` | `resolved_metallic_roughness_lod0()` | `resolved_metallic_roughness_lod0()` | not read — no lighting |
| `emissive_uv_transform_row0` | `resolved_emission()` | `resolved_emission()` | `resolved_emission_lod0()` | `resolved_emission_lod0()` | not read — no lighting |
| `emissive_uv_transform_row1` | `resolved_emission()` | `resolved_emission()` | `resolved_emission_lod0()` | `resolved_emission_lod0()` | not read — no lighting |
| `unlit` | `material.unlit` | `material.unlit` | `material.unlit` | `material.unlit` | not read — MASK alpha testing runs regardless of `unlit` |
| `alphaTextureID` | `material.alphaTextureID` | `material.alphaTextureID` | not read — see `dissolve` | not read — see `dissolve` | `material.alphaTextureID` |
| `uv1SlotMask` | `base_color_uv()` / the per-slot accessors, through `slot_uv()` | `base_color_uv()` / the per-slot accessors, through `slot_uv()` | `base_color_uv()` / the `_lod0` accessors, through `slot_uv()` | `base_color_uv()` / the `_lod0` accessors, through `slot_uv()` | `base_color_uv()` |
| `alphaBlend` | `blend_coverage()` (the output alpha the transparent pass blends by) | not read — a BLEND mesh skips the G-buffer and is shaded by `rasterizer.slang` after the lighting pass | not read — BLEND renders opaque in ray tracing | not read — BLEND renders opaque in path tracing | not read — BLEND casts an opaque shadow |

## Beyond shaders

The larger cross-renderer items on the roadmap
(OxidANT's `third_party/OxidANT/crates/webgpu_renderer/docs/webgpu-renderer-roadmap.md`, Phase G):

- **Shared assets** — OBJ→glTF conversion, so both renderers load the same
  scenes (the C++ engine's `Resources/Models` becomes directly usable).
- **Side-by-side comparison harness** — same scene, same camera, diff the
  Vulkan and WebGPU screenshots. With shared BRDF math the images should match
  closely, so the diff becomes a regression net for *both* renderers.

## Historical note: the retired naga/WGSL-export route (2026-07-20 to 2026-07-30)

Before Slang, WGSL was to stay the source of truth:
`cargo run -p kataglyphis_webgpu_renderer --example export_shaders` (naga)
translated the Rust renderer's hand-written WGSL into SPIR-V/GLSL for the C++
side, opt-in via a `-ExportWgslShaders` flag on `Build-Windows.ps1` /
`Build-Windows-Container.ps1`, output to the gitignored
`Resources/Shaders/generated/`. It shared *functions* (a
`Resources/Shaders/generated/aces.glsl`/`brdf.glsl` `#include` POC worked) but
never *entry points*, for the binding-model mismatch above; that limit
motivated the Slang migration.

Retired 2026-07-30: nothing in the C++ engine consumed the exported output
(`grep -rn "\.spv" Src/` shows every SPIR-V load coming from
`Resources/ShadersSlang/build/spirv/`), so the flag, its build-script plumbing
and the `Resources/Shaders/generated/` gitignore entry were removed. The
`export_shaders` naga example stays in the Rust crate upstream, harmless and
still useful for inspecting naga's WGSL→SPIR-V output; this repo no longer
treats it as the sharing route.
