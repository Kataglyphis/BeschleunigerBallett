# Sharing Shader Code Between the C++ Vulkan Engine and the Rust WebGPU Renderer

**Yes — via [Slang](https://shader-slang.com/), with Slang as the single
source of truth.** One `.slang` source compiles to **SPIR-V** (Vulkan/C++)
and **WGSL** (WebGPU/Rust), unifying the shader codebase between both
renderers, including shared entry points where the two renderers' passes
coincide.

## The pipeline (current)

Compile commands, output directories, staleness rules, and fast shader
iteration are owned by
[`shader-build-pipeline.md`](shader-build-pipeline.md) — this document does
not restate them. The compile step is wired into the build scripts (no
opt-in flag needed): `scripts/linux/cmake-configure-build.sh` compiles the
Slang manifest before every Linux build, and `Build-Windows.ps1` does so before
its `clangcl-debug`, `clangcl-release` and `msvc-release` builds (not before
`clangcl-profile` or `msvc-debug`), so a `.slang` edit reaches the Vulkan
engine on the next such build. A bare `cmake --build` compiles no shaders. `slangc` is resolved from
`VULKAN_SDK\Bin` then `PATH` (the Vulkan SDK ships it; verified against
1.4.350.0).

**Rust side:** the compile scripts additionally compile each shared shader
*without* `-entry`/`-stage` (Slang then emits every entry point of that file
into one combined WGSL file) and copy the result straight into the Rust
crate's shader directories (`crates/webgpu_renderer/src/shaders/`,
`crates/gui/src/shaders/`), replacing the hand-written WGSL there. The
crate's `include_str!` calls are unchanged — they just now read
Slang-emitted text. `histogram.wgsl` is the one deliberate holdout (see
below).

## Why Slang (not GLSL, not naga/WGSL-as-source)

WGSL used to be the shared-math source of truth here, translated with
`naga` into GLSL the C++ engine `#include`d (see **Historical note**
below for why that route was retired). That approach could share
*functions* but not *entry points*: the generated SPIR-V/GLSL carried
WebGPU binding decorations that do not match the Vulkan engine's descriptor
layout.

Slang removes that limit. Its parameter-binding system
(`[vk::binding(i,s)]` → `@binding(i) @group(s)` in WGSL) is the lever that
aligns the two binding models, so entry points can be shared too, not just
math. It also targets **SPIR-V directly**, including the ray tracing
pipeline and ray query — neither of which WebGPU/WGSL has, so those shaders
are Slang-authored for language unity on the C++ side even though they only
ever emit one target.

### Target status (verified against slangc in VulkanSDK 1.4.350.0)

| Target | Status | Notes |
| --- | --- | --- |
| SPIR-V (Vulkan) | **stable** | Rasterization, compute, **ray tracing pipeline**, **ray query**, descriptor sets, push constants |
| WGSL (WebGPU) | **experimental** | Functional (official `wgpu-html5` example uses it). **No** ray tracing, ray query, mesh/tessellation/geometry shaders, wave intrinsics, `f64`, `i8`/`u8` |

**WGSL fallback policy:** if Slang can't express a particular shared shader
for the WGSL target at all (not merely emit it), that shader's WGSL is
hand-written with **no** Slang source. `histogram.wgsl` is the one case
today (rationale in
[`shader-build-pipeline.md`](shader-build-pipeline.md)).

## Architecture: Slang-native, not `#include`

Slang is module- and entry-point-centric. A no-entry-point math module
cannot be emitted to GLSL/WGSL text directly, and imported functions are
name-mangled in the emit, so sharing works through Slang's own module
system rather than textual `#include`:

- **Shared math** lives in Slang modules under `Resources/ShadersSlang/common/`.
  A handful (`aces.slang`, `brdf.slang`, `noise.slang`, `fullscreen.slang`)
  are imported by entry points on both targets and so are genuinely shared
  *between* the two renderers; most of the rest (`material_fetch.slang`,
  `material_rules.slang`, `cascaded_shadow.slang`, `base_color.slang`,
  `emission.slang`, `normal_map.slang`, `alpha_test.slang`) are shared only
  *within* one target — today all of them within the C++/SPIR-V side, across
  its raster, deferred, shadow-map, ray tracing and path tracing shading
  paths. The shared-module-targets table further down is the exact, gated
  mapping this bullet summarizes.
- **Entry points** are Slang shaders that `import` the math modules and
  compile to whichever target(s) their renderer needs. Mangling is a
  non-issue because Slang links modules internally.
- Each renderer keeps its own entry points where the passes genuinely
  differ (they composite differently); both `import` the same math.

## What is wired today

**Shared math modules** (imported by entry points, never emitted directly):

- `common/aces.slang` — ACES filmic tonemap (Narkowicz 2015) + exact IEC
  61966-2-1 sRGB encode.
- `common/brdf.slang` — the Epic Games / Unreal 4 microfacet PBR model
  (`distribution_ggx`, `geometry_smith`, `fresnel_schlick`, `brdf_direct`),
  shared by both renderers' forward/RT shading.
- `common/noise.slang` — 3D simplex noise + fractal Brownian motion, used
  by both renderers' cloud/procedural passes.
- `common/fullscreen.slang` — the shared fullscreen-triangle vertex trick
  (`vid/2*4-1`), used by every fullscreen pass on both sides.
- `common/material_fetch.slang` — the `objectDescription` binding and the
  `fetch_object_description`/`fetch_material` lookups built on it. Shared
  only within the spirv target, across the raster (`rasterizer.slang`,
  `deferred.slang`, `shadow_map.slang`) and closest-hit/any-hit ray tracing
  (`raytrace.rchit.slang`, `raytrace.rahit.slang`) entry points. Not imported
  by `path_tracing.slang`, which already declares its own `objectDescription`
  binding and cannot re-import the same binding from this module.
- `common/material_rules.slang` — the pure, binding-free glTF material rules
  built on the fetched `ObjMaterial`: `material_roughness`,
  `material_metallic_roughness` (glTF SS3.9.2 G=roughness/B=metallic channel
  swizzle) and `alpha_masked_out` (alphaMode MASK). Split out of
  `material_fetch.slang` so `path_tracing.slang`, which cannot import that
  module, can still call them - same reasoning as `base_color.slang` below.
  Shared only within the spirv target, across every C++ shading path (raster,
  deferred, shadow map, ray tracing, path tracing).
- `common/material_textures.slang` — the one owning declaration of the
  `textures[]`/`textureSamplers[]` arrays (`TEXTURES_BINDING`/
  `SAMPLER_BINDING`), plus explicit-LOD (`SampleLevel`) and implicit-LOD
  (`Sample`) helpers for the alpha (map_d), normal, metallic-roughness and
  emissive texture slots. Both LOD shapes live in one module: an
  implicit-LOD helper unused by a given entry point is dead-code-eliminated
  before Slang's capability checking runs, so a compute/ray-tracing importer
  that only calls the explicit-LOD half never picks up a fragment-only
  capability. The base-colour slot is not covered here - its sampling stays
  inline at each call site, next to the shading control flow (albedo
  assembly, discard, G-buffer packing) that differs per shader. Shared only
  within the spirv target, across `rasterizer.slang`, `deferred.slang`,
  `rasterizer/shadows/shadow_map.slang`, `raytrace.rchit.slang` and
  `alpha_test.slang` (and, transitively through `alpha_test.slang`,
  `path_tracing.slang`, which also imports it directly for its own
  base-colour sample).
- `common/cascaded_shadow.slang` — PCF cascaded shadow-map sampling
  (`calc_cascaded_shadow`). Shared only within the spirv target, by the
  raster and deferred lighting shading paths (`rasterizer.slang`,
  `deferred.slang`).
- `common/base_color.slang` — `base_color` (glTF base colour =
  `baseColorFactor * sampled texture`) and `transform_uv`
  (KHR_texture_transform), split out into its own bindingless module so every
  spirv shading path can import it. It is what keeps
  `rasterizer/rasterizer.slang`, `deferred/deferred.slang`,
  `raytracing/raytrace.rchit.slang`, and `path_tracing/path_tracing.slang`
  in agreement with `forward/forward.slang`'s (wgsl, not an importer of this
  module) reference `prim.base_color * baseSample` — before it existed, those
  four discarded `baseColorFactor` whenever a material also had a texture.
  The alpha half of the same rule (`baseColorFactor[3]`) is not yet carried
  this way — it needs a new `ObjMaterial` field, since `fromGltfMaterial`
  only reads `baseColorFactor[0..2]` today. Shared only within the spirv
  target.
- `common/emission.slang` — the glTF emissive rule (`material_emission`:
  `emissiveFactor` \* sampled `emissiveTexture`, or just the factor with no
  texture). Shared only within the spirv target, across all four raster/RT
  shading paths (`rasterizer.slang`, `deferred.slang`, `raytrace.rchit.slang`,
  `path_tracing.slang`).
- `common/normal_map.slang` — glTF tangent-space normal mapping
  (`apply_normal_map`, glTF SS3.9.3). Shared only within the spirv target,
  across the same four shading paths as `emission.slang`;
  `forward/forward.slang`'s `fs_main` (wgsl) reimplements the identical math
  locally rather than importing it.
- `common/alpha_test.slang` — the shared glTF MASK alpha test for ray
  queries: `raytrace.rahit.slang`'s any-hit shader and
  `path_tracing.slang`'s ray-query candidate loop (which has no any-hit stage
  of its own). Shared only within the spirv target.
- `common/sky_model.slang` — the analytic sky's horizon/zenith/ground
  gradient (`sky_gradient`), sun disk (`sun_disk`), and diffuse hemisphere
  irradiance approximation (`hemisphere_irradiance`), shared by `sky/sky.slang`
  (the Rust/WebGPU background) and `forward/forward.slang` (the analytic IBL
  fallback whose reflections and irradiance the background must agree with).
  A combined `sky_radiance` is deliberately not part of the module: each
  caller sources its light direction/intensity from a different uniform
  block, so each keeps a thin local wrapper over `sky_gradient` + `sun_disk`.
  The same three gradient constants are pinned a third time on the Rust CPU
  side (`render::ibl::SKY_ZENITH`/`SKY_HORIZON`/`SKY_GROUND`, consumed by
  `EnvironmentImage::sky`'s panorama) by `tests/sky_constants.rs`.

**Entry-point shader targets:** every Slang entry-point source below compiles
to exactly one target today — none is currently shared between the two
renderers at the entry-point level (only `aces.slang`, `brdf.slang`,
`fullscreen.slang` and `noise.slang` among the shared math modules above, and
the CI guards below, cross both targets — the rest of that list is shared
only within one target; see the shared-module-targets table below).
`tonemap/tonemap.slang` (WGSL) and
`post/post.slang` (SPIR-V) look like a shared pass but are two separate
sources: the C++ side's `post/post.slang` also handles cloud compositing via
push constants, so it never merged with the Rust tonemap shader.

<!-- shader-targets:begin -->
| File | Target |
| --- | --- |
| `bloom/bloom.slang` | wgsl |
| `compute/clouds.slang` | spirv |
| `compute/noise.slang` | spirv |
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
`import` a shared math module and dual-emit to SPIR-V + WGSL, so a change
that breaks either target's compile fails the manifest run in
`Build-SlangShaders.ps1` before it reaches either renderer. They are
deliberately excluded from the table above (see its gating test). Only
`brdf` and `noise` are guarded for dual emit today; a spirv-only module has
nothing to guard until it gains a real wgsl consumer — the
shared-module-targets table below is what makes that fact checkable instead
of merely asserted.

**Shared-module targets:** the per-`common/`-module counterpart to the
shader-targets table above, computed the same way (an import-graph walk over
`shader-manifest.json`, with `tests/*.slang` counted as real consumers - it
is exactly why `brdf` and `noise` show `both` below despite `noise` having no
production wgsl consumer yet), and gated by the same kind of test. Of the
`common/` modules, only four are reachable from both a spirv and a wgsl
entry point; the rest are shared within a single target (`(unused)` would
mark a module under `common/` that nothing currently imports at all).

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

**Status:** all C++ shaders are on Slang SPIR-V (all 8 loading sites), and
all Rust shaders are on Slang-emitted WGSL (`histogram.wgsl` excepted — see
the WGSL fallback policy above). The migration described in earlier
revisions of this document as "in progress" is complete.

## Known glTF loader divergences (not shader-shared, but the two renderers must stay honest about it)

| Feature | C++ `GltfLoader` | Rust `gltf_loader` | Note |
| --- | --- | --- | --- |
| Base-colour UV set beyond TEXCOORD_0 | `fromGltfMaterial` supports only TEXCOORD_0 and warns when a material's base-colour texture or `KHR_texture_transform` names anything else | `uv_set_bit` supports TEXCOORD_0/1 and warns only past that | The Rust masked-shadow pass (`forward.slang`'s `vs_shadow_masked`/`fs_shadow_masked`) honours the per-slot UV mask for the base-colour slot the same way the forward pass does, so a MASK material whose base-colour texture declares TEXCOORD_1 casts a shadow silhouette that matches its forward-pass alpha test; the C++ shadow pass has no equivalent selector because it never reads past TEXCOORD_0 in the first place. |
| `KHR_materials_unlit` | `fromGltfMaterial` reads `material.unlit` into `ObjMaterial::unlit`, honoured in all four shading paths (`rasterizer.slang`, `deferred.slang`, `raytrace.rchit.slang`, `path_tracing.slang`) | reads `material.unlit()` into `CpuMaterial::unlit` | Parity, not a gap — both loaders and every shading path on both sides treat the extension identically. |
| `occlusionTexture` + `occlusionStrength` | `fromGltfMaterial` never reads `cgltf_material::occlusion_texture` — `ObjMaterial` has no occlusion field at all | reads `occlusion_texture()`/`occlusion_strength` into `CpuMaterial::occlusion_texture`/`occlusion_strength` | Intended divergence: glTF scopes `occlusionTexture` to indirect light, and neither C++ raster path has an indirect term — `rasterizer.slang`'s `fs_main` and the deferred lighting pass are `brdf_direct` + shadow + emissive only, so there is nothing for an AO map to attenuate. A future IBL or ambient term is what would make the slot meaningful. |
| Per-slot `KHR_texture_transform` | `fromGltfMaterial` gives base-colour, normal, metallic-roughness and emissive each their own transform (`uv_transform_row0`/`_row1`, `normal_uv_transform_row0`/`_row1`, `metallic_roughness_uv_transform_row0`/`_row1`, `emissive_uv_transform_row0`/`_row1`); no occlusion slot, since occlusion textures are not read at all (previous row) | the same four slots plus `occlusion_uv_transform` | Parity everywhere C++ has a texture to transform; the remaining gap is exactly the occlusion-support gap above, not a `KHR_texture_transform` bug. |
| `alphaMode` `BLEND` | `fromGltfMaterial`'s `alphaCutoff` sentinel maps `BLEND` to the same `-1` as `OPAQUE` (never discards) — a `BLEND` material renders opaque; rationale in `fromGltfMaterial`'s `alphaCutoff` comment | `AlphaMode::Blend` drives a real sorted back-to-front alpha-blend pass (the `alpha_blend` primitive flag and its distance sort) | Intended divergence: real `BLEND` compositing needs a sorted transparent pass this engine does not have yet, so it falls back to the common cut-out case (`MASK`) instead. |
| Roughness default when `Pr` is unauthored (OBJ path) | `ObjLoader` treats an absent (or `0.0`) `Pr` as "not authored" and leaves `ObjMaterial::roughness` at its sentinel, so `material_roughness()` derives it from `shininess`/`Ns` | the OBJ→glTF converter (`obj_to_gltf`) emits `roughnessFactor` from `Ns` via the same `sqrt(2/(Ns+2))` curve, clamped `[0.045, 1.0]`, when `Pr` is absent | Parity, not a gap — both paths derive the same roughness from the same `.mtl`'s `Ns` when `Pr` is unauthored; `material_roughness()` (`material_rules.slang`) is the shared reference both sides implement. |
| `map_d` (OBJ per-texel opacity map) | `ObjLoader::loadTexturesAndMaterials` reads `map_d` into `ObjMaterial::alphaTextureID` and treats it as a glTF `MASK` cut-out at the conventional `alphaCutoff = 0.5`, honoured in `rasterizer.slang`, `deferred.slang`, `shadow_map.slang` and `common/alpha_test.slang` | `obj_to_gltf` reads only the scalar `d`/`Tr` directives into a per-material alpha and emits `alphaMode: BLEND` when it is below `1.0` (see the `alphaMode` `BLEND` row above); it has no code path for a *textured* opacity map at all, so a `map_d` material converts with neither a cut-out nor per-texel alpha | Real gap, not an intended divergence: `obj_to_gltf` would need to emit a `baseColorTexture.a` channel (or a dedicated occlusion-style slot) from `map_d` to match the C++ side. |

## Which C++ shading path reads which `ObjMaterial` field

A new `ObjMaterial` field is not "done" when *one* shading path reads it —
`BuildIntegrity.EveryObjMaterialFieldIsReadByAShader` only asks whether
*some* `.slang` source anywhere mentions `material.<field>`, so a field read
by `rasterizer.slang` and silently dropped by `path_tracing.slang` still
passes it. The table below is the finer-grained, per-shading-path answer:
one row per `ObjMaterial` member (`common/scene_types.slang`), one column
per C++ shading path — `rasterizer.slang`, `deferred.slang`
(`geometry_fs_main`), `raytrace.rchit.slang` (`rchit_main`),
`path_tracing.slang` (`path_tracing_main`), and the two alpha-only
consumers `shadow_map.slang`/`alpha_test.slang` sharing one column, since
neither ever shades a colour. Each cell names the expression or helper that
actually reads the field on that path, or says plainly "not read" and why —
including the asymmetries this table exists to stop being rediscovered one
field at a time: `metallic` is a full factor-or-texture read on three paths
but, on `path_tracing.slang`, only the metallic-roughness *texture* branch
touches it (see `material_textures.slang`'s comment on why that path skips
`resolved_metallic_roughness_lod0()`); `shininess` is only ever reachable
through `material_roughness()`'s negative-`roughness` fallback; `dissolve`
is read only inside `alpha_masked_out()`, so a path with no MASK alpha test
of its own (`raytrace.rchit.slang`, `path_tracing.slang`'s own shading —
their MASK test runs earlier, in `alpha_test.slang`'s `ray_hit_masked_out()`)
never touches it.

| Member | `rasterizer.slang` | `deferred.slang` | `raytrace.rchit.slang` | `path_tracing.slang` | `shadow_map.slang` / `alpha_test.slang` |
| --- | --- | --- | --- | --- | --- |
| `diffuse` | `material.diffuse` (untextured) / `base_color()` (textured) | `material.diffuse` (untextured) / `base_color()` (textured) | `material.diffuse` (untextured) / `base_color()` (textured) | `material.diffuse` (untextured) / `base_color()` (textured) | not read — alpha testing only discards a fragment, never shades a colour |
| `emission` | `resolved_emission()` | `resolved_emission()` | `resolved_emission_lod0()` | `resolved_emission_lod0()` (skipped in furnace-test mode) | not read — no lighting |
| `shininess` | `resolved_metallic_roughness()`, via `material_roughness()`'s negative-`roughness` fallback | `resolved_metallic_roughness()`, via `material_roughness()`'s negative-`roughness` fallback | `resolved_metallic_roughness_lod0()`, via `material_roughness()`'s negative-`roughness` fallback | not read — no roughness/specular term at all (only the metallic half of `material_metallic_roughness()` is used) | not read — no lighting |
| `dissolve` | `alpha_masked_out()` | `alpha_masked_out()` | not read — MASK cut-out for ray-traced hits runs in the any-hit shader (`raytrace.rahit.slang`, via `alpha_test.slang`), not here | not read — candidates are alpha-tested earlier by `alpha_test.slang`'s `ray_hit_masked_out()`, before this kernel's own material-driven shading runs | `alpha_masked_out()` |
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

The bigger cross-renderer wins on the roadmap
(OxidANT's `third_party/OxidANT/crates/webgpu_renderer/docs/webgpu-renderer-roadmap.md`, Phase G) are:

- **Shared assets** — OBJ→glTF conversion so both renderers eat the same
  scenes (the C++ engine's `Resources/Models` becomes directly usable).
- **Side-by-side comparison harness** — same scene, same camera, diff the
  Vulkan and WebGPU screenshots. With shared BRDF math the images should
  match closely, which turns the diff into a regression net for *both*
  renderers.

## Historical note: the retired naga/WGSL-export route (2026-07-20 to 2026-07-30)

Before Slang, the plan was to keep WGSL (not Slang) as the source of truth:
`cargo run -p kataglyphis_webgpu_renderer --example export_shaders` (naga)
translated the Rust renderer's hand-written WGSL into SPIR-V/GLSL for the
C++ side, opt-in via a `-ExportWgslShaders` flag on `Build-Windows.ps1` /
`Build-Windows-Container.ps1`, output to the gitignored
`Resources/Shaders/generated/`. It could share *functions* (a
`Resources/Shaders/generated/aces.glsl`/`brdf.glsl` `#include` POC worked)
but never *entry points*, for the binding-model mismatch reason described
above — that limit is what motivated the Slang migration.

Retired 2026-07-30: nothing in the C++ engine ever consumed the exported
output (`grep -rn "\.spv" Src/` shows every SPIR-V load coming from
`Resources/ShadersSlang/build/spirv/`), so the flag, its build-script
plumbing, and the `Resources/Shaders/generated/` gitignore entry were
removed. The `export_shaders` naga example itself stays in the Rust crate
upstream — it is harmless and still useful for inspecting naga's WGSL→SPIR-V
output — this repo just no longer treats it as the sharing route.
