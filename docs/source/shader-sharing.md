# Sharing Shader Code Between the C++ Vulkan Engine and the Rust WebGPU Renderer

**[Slang](https://shader-slang.com/) is the single source of truth.** One
`.slang` source compiles to **SPIR-V** (Vulkan/C++) and **WGSL** (WebGPU/Rust),
including shared entry points where the two renderers' passes coincide.

## The pipeline (current)

[`shader-build-pipeline.md`](shader-build-pipeline.md) owns the build wiring,
compile commands, output directories, staleness rules and fast iteration. On
the Rust side its combined WGSL emit replaces the crates' hand-written WGSL;
their `include_str!` calls are unchanged and now read Slang-emitted text.
`histogram.wgsl` is the one deliberate holdout (WGSL fallback policy below).

**Ownership:** OxidANT's `shaders/` owns all WebGPU Slang entry points and the
shared `aces`, `brdf`, `noise`, `fullscreen` and `sky_model` modules. This repo
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
still have to be kept in sync. `histogram.wgsl` is the one case today
(rationale in [`shader-build-pipeline.md`](shader-build-pipeline.md)).

## Architecture: Slang-native, not `#include`

Slang is module- and entry-point-centric: a no-entry-point math module cannot
be emitted to GLSL/WGSL text, and imported functions are name-mangled in the
emit, so sharing goes through Slang's module system, not textual `#include`:

- **Shared math** lives under `third_party/OxidANT/shaders/common/`. Four
  (`aces.slang`, `brdf.slang`, `noise.slang`, `fullscreen.slang`) are imported
  on both targets, so both renderers share them. The rest are shared within one
  target: all but `sky_model.slang` on the C++/SPIR-V side, across its raster,
  deferred, shadow-map, ray tracing and path tracing paths. The
  shared-module-targets table below is the exact, gated mapping.
- **Entry points** `import` the math modules and compile to the target(s)
  their renderer needs. Slang links modules internally, so mangling is a
  non-issue.
- Where the passes genuinely differ (they composite differently), each
  renderer keeps its own entry points over the same imported math.

## What is wired today

**Shared math modules** (imported by entry points, never emitted directly):

- `common/aces.slang` — ACES filmic tonemap (Narkowicz 2015) + exact IEC
  61966-2-1 sRGB encode.
- `common/brdf.slang` — the Epic Games / Unreal 4 microfacet PBR model
  (`distribution_ggx`, `geometry_smith`, `fresnel_schlick`, `brdf_direct`),
  used by both renderers' forward/RT shading.
- `common/noise.slang` — 3D simplex noise + fractal Brownian motion, exercised
  on both targets by `tests/noise_test.slang`; no production shader imports it yet.
- `common/fullscreen.slang` — the fullscreen-triangle vertex trick
  (`vid/2*4-1`), used by every fullscreen pass on both sides. Its `uv` is the
  one per-target difference in shared code: uv (0,0) is always the target's
  top-left texel, but clip-space y = -1 is the top row in Vulkan and the bottom
  row in WebGPU, so `fullscreen_vs` picks the V formula with `__target_switch`
  (`spirv`: `v = (y + 1) / 2`; every other target: `v = 1 - (y + 1) / 2`). The
  WGSL emit is byte-identical to the single formula it replaced.
  `fullscreen_uv_to_ndc` is its exact inverse, switched the same way, for the
  passes that reconstruct a position from the uv (`deferred.slang` on SPIR-V,
  `ssao.slang` on WGSL). `BuildIntegrity.NoShaderRedeclaresTheFullscreenUvMapping`
  fails on a hand-written copy of either direction. A wrong V mirrors the frame
  about its horizontal centre line, which a vertically symmetric test scene
  cannot show; `GoldenRender.WorldUpIsScreenUp` (C++) and
  `world_up_is_screen_up` (OxidANT `tests/headless.rs`) pin it.
- `common/material_fetch.slang` — the `objectDescription` binding and the
  `fetch_object_description`/`fetch_material` lookups on it. Spirv only: the
  raster (`rasterizer.slang`, `deferred.slang`, `shadow_map.slang`) and
  closest-hit/any-hit ray tracing (`raytrace.rchit.slang`,
  `raytrace.rahit.slang`) entry points. `path_tracing.slang` declares its own
  `objectDescription` binding, so it cannot import this module.
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
  `fromGltfMaterial` puts `baseColorFactor[0..2]` in `diffuse` and
  `baseColorFactor[3]` in `dissolve`, which `alpha_masked_out()` multiplies
  into the sampled alpha (the `ObjMaterial` table in
  [`model-loading.md`](model-loading.md)). Spirv only.
- `common/emission.slang` — the glTF emissive rule (`material_emission`:
  `emissiveFactor` \* sampled `emissiveTexture`, or the factor alone without a
  texture). Spirv only, all four raster/RT shading paths (`rasterizer.slang`,
  `deferred.slang`, `raytrace.rchit.slang`, `path_tracing.slang`).
- `common/normal_map.slang` — glTF tangent-space normal mapping
  (`apply_normal_map`, glTF SS3.9.3). Spirv only, the same four paths as
  `emission.slang`; `forward/forward.slang`'s `fs_main` (wgsl) reimplements
  the same math locally.
- `common/alpha_test.slang` — the glTF MASK alpha test for ray queries:
  `raytrace.rahit.slang`'s any-hit shader and `path_tracing.slang`'s ray-query
  candidate loop (which has no any-hit stage of its own). Spirv only.
- `common/sky_model.slang` — the analytic sky's horizon/zenith/ground gradient
  (`sky_gradient`), sun disk (`sun_disk`) and diffuse hemisphere irradiance
  approximation (`hemisphere_irradiance`), shared by `sky/sky.slang` (the
  Rust/WebGPU background) and `forward/forward.slang` (the analytic IBL
  fallback, whose reflections and irradiance must agree with the background).
  There is deliberately no combined `sky_radiance`: each caller takes its
  light direction/intensity from a different uniform block, so each keeps a
  thin local wrapper over `sky_gradient` + `sun_disk`. `tests/sky_constants.rs`
  pins the three gradient constants a third time on the Rust CPU side
  (`render::ibl::SKY_ZENITH`/`SKY_HORIZON`/`SKY_GROUND`, consumed by
  `EnvironmentImage::sky`'s panorama).

**Entry-point shader targets:** every Slang entry-point source below compiles
to exactly one target, so none is shared between the two renderers at the
entry-point level. Only the four `both` math modules and the CI guards below
cross targets. `tonemap/tonemap.slang` (WGSL) and `post/post.slang` (SPIR-V)
look like one pass but are separate sources: the C++ side's `post/post.slang`
also composites clouds via push constants, so it never merged with the Rust
tonemap shader.

<!-- shader-targets:begin -->
| File | Target |
| --- | --- |
| `bloom/bloom.slang` | wgsl |
| `compute/clouds.slang` | spirv |
| `compute/noise_volume.slang` | spirv |
| `deferred/deferred.slang` | spirv |
| `depth_resolve/depth_resolve.slang` | wgsl |
| `forward/forward.slang` | wgsl |
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
| `sky/sky.slang` | wgsl |
| `skybox/skybox.slang` | spirv |
| `ssao/ssao.slang` | wgsl |
| `tex_quad/tex_quad.slang` | wgsl |
| `tonemap/tonemap.slang` | wgsl |
<!-- shader-targets:end -->

**CI guards:** `tests/brdf_test.slang` and `tests/noise_test.slang` each
`import` a shared math module and dual-emit to SPIR-V + WGSL, so a change that
breaks either target's compile fails the manifest run in
`Build-SlangShaders.ps1` before it reaches either renderer. The table above
excludes them (see its gating test). Only `brdf` and `noise` are guarded for
dual emit; a spirv-only module has nothing to guard until it gains a real wgsl
consumer, which the table below makes checkable.

The local cloud-volume shader is `compute/noise_volume.slang` so Slang's
`import noise;` resolves OxidANT's `common/noise.slang`. A local file named
`noise.slang` shadowed that shared module in the compiler's include order;
the dual-target test then failed with `E30015 undefined identifier` on
`snoise` and `fbm` before it could test the shared math.

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
| `common/cascaded_shadow.slang` | spirv |
| `common/emission.slang` | spirv |
| `common/fullscreen.slang` | both |
| `common/material_fetch.slang` | spirv |
| `common/material_rules.slang` | spirv |
| `common/material_textures.slang` | spirv |
| `common/noise.slang` | both |
| `common/normal_map.slang` | spirv |
| `common/push_constants.slang` | spirv |
| `common/raster_geometry.slang` | spirv |
| `common/scene_types.slang` | spirv |
| `common/sky_model.slang` | wgsl |
<!-- shared-module-targets:end -->

**Status:** all C++ shaders are on Slang SPIR-V (all 8 loading sites), and all
Rust shaders on Slang-emitted WGSL except `histogram.wgsl` (WGSL fallback
policy above). The migration is complete.

## Known glTF loader divergences (not shader-shared, but the two renderers must stay honest about it)

| Feature | C++ `GltfLoader` | Rust `gltf_loader` | Note |
| --- | --- | --- | --- |
| Base-colour UV set beyond TEXCOORD_0 | `fromGltfMaterial` supports only TEXCOORD_0 and warns when a material's base-colour texture or `KHR_texture_transform` names anything else | `uv_set_bit` supports TEXCOORD_0/1 and warns only past that | The Rust masked-shadow pass (`forward.slang`'s `vs_shadow_masked`/`fs_shadow_masked`) honours the base-colour slot's per-slot UV mask like the forward pass, so a MASK material whose base-colour texture declares TEXCOORD_1 casts a shadow silhouette matching its forward-pass alpha test. The C++ shadow pass needs no such selector: it never reads past TEXCOORD_0. |
| `KHR_materials_unlit` | `fromGltfMaterial` reads `material.unlit` into `ObjMaterial::unlit`, honoured in all four shading paths (`rasterizer.slang`, `deferred.slang`, `raytrace.rchit.slang`, `path_tracing.slang`) | reads `material.unlit()` into `CpuMaterial::unlit` | Parity: both loaders and every shading path on both sides treat the extension identically. |
| `occlusionTexture` + `occlusionStrength` | `fromGltfMaterial` never reads `cgltf_material::occlusion_texture`; `ObjMaterial` has no occlusion field | reads `occlusion_texture()`/`occlusion_strength` into `CpuMaterial::occlusion_texture`/`occlusion_strength` | Intended divergence: glTF scopes `occlusionTexture` to indirect light, and neither C++ raster path has an indirect term (`rasterizer.slang`'s `fs_main` and the deferred lighting pass are `brdf_direct` + shadow + emissive only), so an AO map has nothing to attenuate. A future IBL or ambient term would make the slot meaningful. |
| Per-slot `KHR_texture_transform` | `fromGltfMaterial` gives base-colour, normal, metallic-roughness and emissive each their own transform (`uv_transform_row0`/`_row1`, `normal_uv_transform_row0`/`_row1`, `metallic_roughness_uv_transform_row0`/`_row1`, `emissive_uv_transform_row0`/`_row1`); no occlusion slot, since occlusion textures are not read (previous row) | the same four slots plus `occlusion_uv_transform` | Parity wherever C++ has a texture to transform; the only gap is the occlusion gap above, not a `KHR_texture_transform` bug. |
| `alphaMode` `BLEND` | `fromGltfMaterial`'s `alphaCutoff` sentinel maps `BLEND` to the same `-1` as `OPAQUE` (never discards), so a `BLEND` material renders opaque; rationale in `fromGltfMaterial`'s `alphaCutoff` comment | `AlphaMode::Blend` drives a real sorted back-to-front alpha-blend pass (the `alpha_blend` primitive flag and its distance sort) | Intended divergence: real `BLEND` compositing needs a sorted transparent pass this engine does not have yet, so it renders opaque. |
| Roughness default when `Pr` is unauthored (OBJ path) | `ObjLoader` treats an absent (or `0.0`) `Pr` as unauthored and leaves `ObjMaterial::roughness` at its sentinel, so `material_roughness()` derives it from `shininess`/`Ns` | the OBJ→glTF converter (`obj_to_gltf`) emits `roughnessFactor` from `Ns` via the same `sqrt(2/(Ns+2))` curve, clamped `[0.045, 1.0]`, when `Pr` is absent | Parity: both paths derive the same roughness from the `.mtl`'s `Ns` when `Pr` is unauthored; `material_roughness()` (`material_rules.slang`) is the shared reference both sides implement. |
| `map_d` (OBJ per-texel opacity map) | `ObjLoader::loadTexturesAndMaterials` reads `map_d` into `ObjMaterial::alphaTextureID` as a glTF `MASK` cut-out at the conventional `alphaCutoff = 0.5`, honoured in `rasterizer.slang`, `deferred.slang`, `shadow_map.slang` and `common/alpha_test.slang` | `obj_to_gltf` reads only the scalar `d`/`Tr` directives into a per-material alpha and emits `alphaMode: BLEND` when it is below `1.0` (`alphaMode` `BLEND` row above); it has no code path for a *textured* opacity map, so a `map_d` material converts with neither a cut-out nor per-texel alpha | Real gap: `obj_to_gltf` would need to emit a `baseColorTexture.a` channel (or a dedicated occlusion-style slot) from `map_d` to match the C++ side. |

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

- `metallic` is a full factor-or-texture read on three paths, but on
  `path_tracing.slang` only the metallic-roughness *texture* branch touches it
  (`material_textures.slang`'s comment says why that path skips
  `resolved_metallic_roughness_lod0()`).
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
| `shininess` | `resolved_metallic_roughness()` (fallback) | `resolved_metallic_roughness()` (fallback) | `resolved_metallic_roughness_lod0()` (fallback) | not read — no roughness/specular term at all (only the metallic half of `material_metallic_roughness()` is used) | not read — no lighting |
| `dissolve` | `alpha_masked_out()` | `alpha_masked_out()` | not read — the MASK cut-out for ray-traced hits runs in the any-hit shader (`raytrace.rahit.slang`, via `alpha_test.slang`) | not read — candidates are alpha-tested earlier, by `alpha_test.slang`'s `ray_hit_masked_out()` | `alpha_masked_out()` |
| `textureID` | `material.textureID` | `material.textureID` | `material.textureID` | `material.textureID` | `material.textureID` |
| `alphaCutoff` | `alpha_masked_out()` | `alpha_masked_out()` | not read — see `dissolve` | not read — see `dissolve` | `material.alphaCutoff` (outer guard) / `alpha_masked_out()` |
| `uv_transform_row0` | `transform_uv()` | `transform_uv()` | `transform_uv()` | `transform_uv()` | `transform_uv()` |
| `uv_transform_row1` | `transform_uv()` | `transform_uv()` | `transform_uv()` | `transform_uv()` | `transform_uv()` |
| `metallic` | `resolved_metallic_roughness()` | `resolved_metallic_roughness()` | `resolved_metallic_roughness_lod0()` | `material_metallic_roughness()`'s raw metallic term, gated on `metallicRoughnessTextureID >= 0` only — no untextured-factor fallback | not read — no lighting |
| `roughness` | `resolved_metallic_roughness()` | `resolved_metallic_roughness()` | `resolved_metallic_roughness_lod0()` | not read — only the metallic half (`.x`) of `material_metallic_roughness()` is used | not read — no lighting |
| `emissiveTextureID` | `resolved_emission()` | `resolved_emission()` | `resolved_emission_lod0()` | `resolved_emission_lod0()` | not read — no lighting |
| `normalTextureID` | `resolved_normal()` | `resolved_normal()` | `material.normalTextureID` (outer guard) / `resolved_normal_lod0()` | `material.normalTextureID` (outer guard) / `resolved_normal_lod0()` | not read — no normal mapping |
| `normalScale` | `resolved_normal()` | `resolved_normal()` | `resolved_normal_lod0()` | `resolved_normal_lod0()` | not read — no normal mapping |
| `metallicRoughnessTextureID` | `resolved_metallic_roughness()` | `resolved_metallic_roughness()` | `resolved_metallic_roughness_lod0()` | `material.metallicRoughnessTextureID` (outer guard before `sample_metallic_roughness_lod0()`) | not read — no lighting |
| `normal_uv_transform_row0` | `resolved_normal()` | `resolved_normal()` | `resolved_normal_lod0()` | `resolved_normal_lod0()` | not read — no normal mapping |
| `normal_uv_transform_row1` | `resolved_normal()` | `resolved_normal()` | `resolved_normal_lod0()` | `resolved_normal_lod0()` | not read — no normal mapping |
| `metallic_roughness_uv_transform_row0` | `resolved_metallic_roughness()` | `resolved_metallic_roughness()` | `resolved_metallic_roughness_lod0()` | `sample_metallic_roughness_lod0()`, called directly — same `metallic`-style exception | not read — no lighting |
| `metallic_roughness_uv_transform_row1` | `resolved_metallic_roughness()` | `resolved_metallic_roughness()` | `resolved_metallic_roughness_lod0()` | `sample_metallic_roughness_lod0()`, called directly — same `metallic`-style exception | not read — no lighting |
| `emissive_uv_transform_row0` | `resolved_emission()` | `resolved_emission()` | `resolved_emission_lod0()` | `resolved_emission_lod0()` | not read — no lighting |
| `emissive_uv_transform_row1` | `resolved_emission()` | `resolved_emission()` | `resolved_emission_lod0()` | `resolved_emission_lod0()` | not read — no lighting |
| `unlit` | `material.unlit` | `material.unlit` | `material.unlit` | `material.unlit` | not read — MASK alpha testing runs regardless of `unlit` |
| `alphaTextureID` | `material.alphaTextureID` | `material.alphaTextureID` | not read — see `dissolve` | not read — see `dissolve` | `material.alphaTextureID` |

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
