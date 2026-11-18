# Model loading

How a model file becomes drawable geometry in the Vulkan engine: the two
loaders, the async parse/upload split that keeps the frame loop responsive, and
the multi-mesh flow that turns one file into per-mesh draws. The chronological
change log is `docs/source/cpp-renderer-improvements.md`.

## Two loaders, one shape

`ObjLoader` (`scene/ObjLoader.{ixx,cpp}`) and `GltfLoader`
(`scene/GltfLoader.{ixx,cpp}`) load `.obj` and `.gltf`/`.glb` into the SAME
CPU-side arrays, so everything downstream is loader-agnostic. Both expose:

- `parseCpu(path)` — reads and decodes the file into flat arrays
  (`vertices`, `indices`, per-triangle `materialIndex`, `materials`, plus a
  per-mesh `meshRanges` table, see below). Touches **no Vulkan**, so it can run
  on any thread.
- `uploadParsed()` — builds the Vulkan-side `Model` (buffers, textures, meshes)
  from the last `parseCpu` result. **Must** run on the thread that owns the
  device.
- `adoptParsed(other&&)` — moves another loader's parse results in, so a
  device-owning loader uploads what a device-free worker produced without
  copying the (tens-of-MB) arrays.

`loadModel(path)`, the synchronous path, is `parseCpu` then `uploadParsed`
back-to-back on the calling (device-owning) thread.

### Which nodes a glTF load walks

`GltfLoader::parseCpu` walks a single scene, not every node in the document:
the default scene (`data->scene`) if the document names one, else the first
entry of `data->scenes`. Only nodes reachable from that scene's roots are
loaded. A document with no `scenes` array at all (`cgltf_validate` permits
this) falls back to every node, with a warning, since that is the only geometry
available. OxidANT's Rust loader walks the same nodes (`walk_nodes` in
`crates/assets/src/gltf_loader.rs`, the scene-less fallback since its
`gltf-bridge` change); keep the two in sync.

### Two glTF parsers behind one switch

`parseCpu` runs one of two parsers into the same arrays, picked by
`KATAGLYPHIS_GLTF_LOADER`: `cgltf` is the cgltf parser in `GltfLoader.cpp`;
unset or anything else is OxidANT's loader, the one the WebGPU renderer uses,
called over the cxx bridge (`load_gltf_flat` in `crates/cxx_bridge`, flattened
by `crates/assets/src/flat.rs`) and copied into the arrays by
`GltfLoaderRust.cpp`. `parseCpu(path, GltfParser)` picks one explicitly. Both
bake world space, reverse a mirrored node's winding, give a primitive without
`NORMAL` one vertex per corner with its face's normal, and hand textures over
as the file's encoded bytes, deduped on `(image, sampler, srgb)`; the Rust
parser takes a texture's core `source` only, so a `KHR_texture_basisu`-only
texture gets no slot (stb reads no KTX2), and a panic or error comes back as a
failed parse, never an exception. `GltfLoaderDiff.*` parses every fixture under
`Resources/Models/GltfTest`, and every fetched glTF scene, with both and fails
on any field that differs, materials included, except one class it prints
instead: a generated tangent at a corner whose mirrored UV triangles cancel,
whose direction is rounding noise in both. `Vertex.cpp`'s `computeTangents` turns off
clang's fused multiply-add, which Rust never emits, so the rest agree within the
suite's tolerances. The Rust loader is the default since 2026-11-18 (owner
decision, on the timings below); cgltf goes once every lane has run on it. `BM_GltfLoaderParser_*` times
both on one file (`/0` cgltf, `/1` Rust). On `linux-profile-clang`, 8-core
aarch64 WSL, 2026-11-18, median of eight: `Colosseum-gltf` (206 MB of buffers)
cgltf 481 ms with a 37 % spread, Rust 394 ms at 3 %; `cube_textured.gltf`
16.2 us against 11.1 us. The Colosseum row skips unless
`KATAGLYPHIS_FETCH_ALL_ASSETS` fetched the scene.

The same walk reads each visited node's `KHR_lights_punctual` light, mesh or no mesh, posed by the
node's world transform (`GltfLoader::readLight`, as the Rust loader's `read_light`). The lights ride
on the `Model`, so `adoptParsed` moves them with the geometry, and the renderer places them by the
model's matrix when it packs the punctual-light buffer.

## The async parse/upload split

The parse is device-free and dominates load time (2802 ms of a 2818 ms load on
the bundled 27 MB `dinosaurs.obj`), so it runs off the render thread:

1. `AsyncModelParse` (`scene/AsyncModelParse.ixx`) runs `parseCpu` on a worker
   thread against a fresh device-free loader (`ObjLoader{}` / `GltfLoader{}`),
   chosen by file extension via `isGltfModelPath` (`scene/ModelFileKind.ixx`),
   the same predicate `Scene::loadModel` routes through.
2. When the worker finishes, `Scene::pollModelLoad` (`scene/Scene.cpp`, the
   `pendingModelParse.parsedGltf()` branch) hands the worker's loader to a
   **device-owning** uploader via `adoptParsed` and calls `uploadParsed`, the
   only ~15 ms the frame loop pays. The glTF and OBJ arms are symmetric.

So `adoptParsed` must move every field `uploadParsed` reads, `meshRanges`
included; miss one and the async path silently loads different geometry than
the synchronous one.

```mermaid
flowchart LR
    subgraph worker["Worker thread (no device)"]
        P["parseCpu(path)\nflat arrays + meshRanges"]
    end
    subgraph main["Render thread (owns device)"]
        Poll["Scene::pollModelLoad\nwhen worker isFinished()"]
        A["uploader.adoptParsed(move)\ntakes the arrays, no copy"]
        U["uploader.uploadParsed()\nbuffers, textures, one Mesh per range"]
        Add["Scene::add_model\none ObjectDescription per mesh"]
    end
    Start["AsyncModelParse.start(path)"] --> P
    P -->|arrays moved| Poll --> A --> U --> Add
```

## One file, many meshes

A `Model` holds `std::vector<Mesh>` (`scene/Model.ixx`). One file becomes a
multi-mesh model through a per-mesh **`MeshRange`**, a slice of the flat arrays:

```
struct MeshRange {
    vertexBase, vertexCount, indexStart, indexCount, triStart, triCount, doubleSided, alphaBlended
};
```

`doubleSided` carries glTF `material.doubleSided` per range, so `uploadParsed`
hands it to `add_new_mesh` and the raster pass can disable back-face culling
for that mesh alone. `alphaBlended` carries `alphaMode` `BLEND` the same way
(`Mesh::isAlphaBlended`): the opaque forward and deferred draws skip the mesh,
and `collectBlendDraws` (`renderer/MeshDrawRecorder.cpp`) draws it after them,
farthest first by the squared distance from the eye to its world-bounds centre,
through `buildTransparentScenePipeline` (src-alpha blending that accumulates
coverage, depth test on, depth writes off). Deferred shades it forward after
the lighting pass, against the geometry pass's depth. Both are always `false`
for OBJ, which has neither concept; an OBJ `d` below 1 stays opaque.

The type lives in its own module, `kataglyphis.vulkan.mesh_range`
(`scene/MeshRange.ixx`), `export import`ed by both loaders. It exports
`MeshRange`, the `MeshSlice` return type and `sliceMeshRange`, the one shared
implementation of "sub-vertices copied, sub-indices re-based by `-vertexBase`,
per-range material subset" that both loaders' `uploadParsed` call.

`parseCpu` records one range per sub-object and keeps the flat arrays intact
(the `ObjParseUnit`/`GltfParseUnit` tests still assert on the flat getters);
`uploadParsed` slices each range via `sliceMeshRange` into its own
`add_new_mesh`. A sub-object is:

- **glTF** — one **primitive**. Primitives already parse into disjoint
  contiguous vertex ranges, so the range is recorded directly.
- **OBJ** — one **shape** (`o`/`g` group). OBJ shapes share one attribute pool
  and vertices are deduplicated, so `loadVertices` resets the dedup map **per
  shape** to keep each shape's vertices in one contiguous block the slice can
  address. The cost is a duplicate of any vertex shared across distinct shapes
  (rare between separate objects, and pixel-identical either way).

A single-primitive glTF or single-shape OBJ yields exactly one range spanning
everything, behaviour-identical to the pre-split single-mesh path.

## What downstream gets for free

Object identity is per **mesh**, not per model, so splitting a file into
meshes feeds the systems that were already mesh-aware:

- **Object descriptions** — `Scene::add_model` flattens one `ObjectDescription`
  per mesh; `objectIndex` is the flat mesh index (sum of prior models' mesh
  counts + local index), pushed per draw in the forward/deferred/shadow record
  loops.
- **Culling** — the record loops iterate meshes with per-mesh AABBs, so a
  multi-mesh model culls per mesh instead of all-or-nothing.
- **Acceleration structures** — one BLAS per model with one **geometry** per
  mesh; RT/PT kernels fetch the per-mesh material with
  `instanceCustomIndex (= the model's first-mesh flat index) + gl_GeometryIndexEXT`
  (`GoldenRender.TracedSecondModelResolvesItsOwnMeshAndTextures`). An `addModel`
  or a finished load builds only the new model's BLAS: a BLAS stays while the
  scene holds the model object it was built from at its index (`BlasReuse.hpp`),
  so a reload rebuilds; every scene change rebuilds the TLAS
  (`GoldenRender.AddedModelBuildsOnlyItsOwnBlas`).

## Material fields and where they come from

`ObjMaterial` (`Src/shared/scene/ObjMaterial.hpp`) is the single struct both
loaders fill and every shader reads. `fromGltfMaterial` (`GltfLoader.cpp`) is
the glTF mapping; `ObjLoader::loadTexturesAndMaterials` is the `.mtl` mapping.
A `—` means that format has no source for the field, so the struct keeps its
default (documented on the member in `ObjMaterial.hpp`). "Read by" names the
shading paths at a glance; the exact per-path expression, or "not read" and
why, is in
[`shader-sharing.md`'s "Which C++ shading path reads which `ObjMaterial`
field"](shader-sharing.md#which-c-shading-path-reads-which-objmaterial-field).

| Member | glTF source | `.mtl` source | Read by |
| --- | --- | --- | --- |
| `diffuse` | `pbrMetallicRoughness.baseColorFactor.rgb`; glTF's default `(1, 1, 1)` if the material has no `pbrMetallicRoughness` block, and for the no-material fallback (owner decision 2026-11-17) | `Kd` | `rasterizer.slang`, `deferred.slang`, `raytrace.rchit.slang`, `path_tracing.slang` (untextured fallback / `base_color()` blend) |
| `emission` | `emissiveFactor`, scaled by `KHR_materials_emissive_strength` when present (`1.0` otherwise) | `Ke` | all four shading paths, via `common/emission.slang`'s `material_emission()` |
| `shininess` | — (pinned to `1.0`, the OBJ-only fallback `material_roughness()` falls back to; `roughness_factor` is not read here) | `Ns` | `material_rules.slang`'s `material_roughness()`, OBJ materials only |
| `dissolve` | `baseColorFactor.a` | `d` | `material_rules.slang`'s `alpha_masked_out()` and, for BLEND, `blend_coverage()` |
| `textureID` | dedup'd slot into `textureImages` (`imageSlot` map, keyed on `(const cgltf_image *, const cgltf_sampler *, bool srgb)`) | dedup'd slot into `textures` (`pathSlot` map, keyed on `(resolved texture path, bool srgb)`) | all five entry points, via `texture_offset + material.textureID` (see below) |
| `sampler` (not an `ObjMaterial` field — tracked separately, index-parallel with `textureImages`, via `GltfLoader::getTextureSamplerDescs()`) | `textures[].sampler`'s wrap modes and mag/min/mipmap filters, mapped to `GltfSamplerDesc` (`vulkan_base/SamplerBuilder.ixx`) | — (OBJ has no sampler concept; every OBJ texture keeps the repeat/linear/linear default) | `Model::addSampler`, when building the texture's `vk::Sampler` |
| `srgb` (not an `ObjMaterial` field — tracked separately, index-parallel with `textures`/`textureImages`, via `ObjLoader::getTextureSrgbFlags()`/`GltfLoader::getTextureSrgbFlags()`) | `true` for base-colour/emissive views, `false` for the normal and metallic-roughness views | `true` for `map_Kd`/`map_Ke`, `false` for `norm`/`map_Bump`/`map_d` | `Texture::createFromMemory`/`createFromFile`, selecting `eR8G8B8A8Srgb` vs. `eR8G8B8A8Unorm` |
| `alphaCutoff` | `material.alpha_cutoff` when `alphaMode == MASK`, else `-1` | — | `material_rules.slang`'s `alpha_masked_out()`, `shadow_map.slang` |
| `uv_transform_row0` / `uv_transform_row1` | `KHR_texture_transform`'s T\*R\*S rows on the base-colour texture (the extension is per-slot — see the normal/metallic-roughness/emissive row pairs below for the other three); identity rows if the extension is absent on this slot | — | `base_color.slang`'s `base_color_uv()` |
| `metallic` | `pbrMetallicRoughness.metallicFactor`, clamped `[0,1]`; glTF's default `1` without the block and for the no-material fallback | — | `rasterizer.slang`/`raytrace.rchit.slang` (`f0`, `brdf_direct`), `deferred.slang` (G-buffer normal's `.a`), `path_tracing.slang` (`f0` and `brdf_direct`, for NEE and the sampled lobes) |
| `roughness` | `pbrMetallicRoughness.roughnessFactor`, clamped `[0,1]`; glTF's default `1` without the block and for the no-material fallback, so a glTF material never reaches the `-1` ("unauthored") sentinel | — | `material_rules.slang`'s `material_roughness()` |
| `emissiveTextureID` | dedup'd slot into `textureImages` (same `imageSlot` map as `textureID` — an emissive view naming the same `(image, sampler)` pair as the base-colour view lands on the same slot); `-1` if the material has no `emissiveTexture` | dedup'd slot into `textures` (same `pathSlot` map as `textureID`, keyed `(resolved path, srgb=true)`); `-1` if the material has no `map_Ke` | all four shading paths, via `common/emission.slang`'s `material_emission()` |
| `normalTextureID` | dedup'd slot into `textureImages` (same `imageSlot` map as `textureID`/`emissiveTextureID`); `-1` if the material has no `normalTexture` | dedup'd slot into `textures` (same `pathSlot` map as `textureID`, keyed `(resolved path, srgb=false)`); `norm` preferred over `map_Bump`/`map_bump`/`bump` when both are present; `-1` if the material has neither directive | all four shading paths, via `common/normal_map.slang`'s `apply_normal_map()`, using `Vertex::tangent` for the TBN basis |
| `normalScale` | `normalTexture.scale`, guarded on the texture pointer (cgltf zero-initialises the field when there is no `normalTexture`); `1.0` if the material has no `normalTexture` | the winning normal-directive's own `-bm` option (`norm`'s if present, else `map_Bump`/`map_bump`/`bump`'s); `1.0` if neither directive is present | `common/normal_map.slang`'s `apply_normal_map()` |
| `metallicRoughnessTextureID` | dedup'd slot into `textureImages` (same `imageSlot` map as `textureID`/`emissiveTextureID`/`normalTextureID`), uploaded linear; `-1` if the material has no `pbrMetallicRoughness` block or no `metallicRoughnessTexture` | — | `material_rules.slang`'s `material_metallic_roughness()` in all four shading paths; `path_tracing.slang` uses the metallic (`.x`) half only, having no roughness term |
| `normal_uv_transform_row0` / `_row1` | `KHR_texture_transform`'s T\*R\*S rows on the `normalTexture`; identity rows if the extension is absent on this slot | — | the normal sample in all four shading paths, via `base_color.slang`'s `normal_uv()` |
| `metallic_roughness_uv_transform_row0` / `_row1` | `KHR_texture_transform`'s T\*R\*S rows on `pbrMetallicRoughness.metallicRoughnessTexture`; identity rows if the extension is absent on this slot | — | the metallic-roughness sample in all four shading paths, via `base_color.slang`'s `metallic_roughness_uv()` |
| `emissive_uv_transform_row0` / `_row1` | `KHR_texture_transform`'s T\*R\*S rows on `emissiveTexture`; identity rows if the extension is absent on this slot | — | the emissive sample in all four shading paths, via `base_color.slang`'s `emissive_uv()` |
| `unlit` | `1` if the material declares `KHR_materials_unlit`, else `0` | — (`.mtl` has no unlit concept) | all four shading paths return the base colour unshaded when set: `rasterizer.slang`/`raytrace.rchit.slang` return early, `deferred.slang` carries the flag through `outAlbedo.a`, `path_tracing.slang` terminates the path at the hit |
| `alphaTextureID` | — (glTF carries opacity in `baseColorTexture.a`, not a separate texture slot) | dedup'd slot into `textures` (same `pathSlot` map as `textureID`, keyed `(resolved path, srgb=false)`); `-1` if the material has no `map_d` | `rasterizer.slang`, `deferred.slang`, `shadow_map.slang`, `common/alpha_test.slang`, sampled `.r` at the base-colour UV and folded into the alpha `material_rules.slang`'s `alpha_masked_out()` tests |
| `uv1SlotMask` | bit per slot (0 base colour, 1 metallic-roughness, 2 normal, 3 emissive) whose `texCoord` is 1; `KHR_texture_transform`'s `texCoord` overrides the textureInfo's; TEXCOORD_2+ warns and stays on 0 | — (`0`: `ObjLoader` copies UV0 into `texture_coords1`, so either set samples the same) | every texture sample, via `base_color.slang`'s `slot_uv()`; `map_d` follows the base-colour bit |
| `alphaBlend` | `1` when `alphaMode == BLEND`, else `0` | — | `material_rules.slang`'s `blend_coverage()`, the forward shader's output alpha; the tracers ignore it and render BLEND opaque |

## Textures, samplers and the 128-slot budget

<!-- max-texture-count: 128 -->

Every texture from every loaded model lands in one flat, fixed-size global
array bound at `TEXTURES_BINDING`/`SAMPLER_BINDING`, sized by
`common/host_device_shared_vars.hpp`'s `MAX_TEXTURE_COUNT` constant:
`const int MAX_TEXTURE_COUNT = 128;`. Both loaders economise against that
budget:

- **glTF dedup by image, sampler and colour space** —
  `GltfLoader::parseCpu`'s `imageSlot` map keys on the triple
  `(const cgltf_image *, const cgltf_sampler *, bool srgb)`, not the material
  or the texture slot (base-colour vs. emissive vs. normal). One
  decode+upload per triple, however many materials or texture slots reference
  it: a material whose base-colour and emissive views name the same PNG shares
  one slot (`textureID == emissiveTextureID`). Two textures that share an image
  but name different samplers, or need different colour spaces, still get
  distinct slots; collapsing them would make the second texture's wrap/filter
  settings unobservable, or upload one of the two through the wrong
  `VkFormat`. A base-colour/normal pair sharing one image therefore gets
  **two** slots, one uploaded sRGB (`Texture.cpp`'s `uploadRgba` picks
  `eR8G8B8A8Srgb`), the other UNORM (`eR8G8B8A8Unorm`): glTF normal maps are
  linear tangent-space data, not gamma-encoded colour.
- **OBJ dedup by resolved path** — `ObjLoader::loadTexturesAndMaterials`'s
  `pathSlot` map keys on the path `resolveObjTexturePath` returns, not the raw
  `map_Kd` string, so two materials naming the same file through different
  `.mtl` spellings (`tex.png` vs `./tex.png`) share one slot.
- **A failed texture still occupies its slot** — when `Texture::createFromFile`
  fails for a non-empty name, `ObjLoader::uploadParsed`'s `addTextureOrDefault`
  call substitutes the default texture instead of skipping the slot, because
  `textureID` is a dense counter over non-empty names and skipping would shift
  every later `textureID` down by one.
- **Sampler dedup by mip level and glTF sampler description** —
  `Model::addSampler` (`scene/Model.cpp`) keys on a
  `SamplerKey{ mipLevel, GltfSamplerDesc }` via `findSampler`
  (`vulkan_base/SamplerBuilder.cpp`): N textures share one `vk::Sampler` only
  when both their mip count and their wrap/filter settings match.
- **Flattening into the global array** — `assignTextureOffsets`
  (`scene/ObjectDescription.ixx`) stamps each mesh's `texture_offset` with its
  model's running offset into the flattened array, advancing by that model's
  texture count once its meshes are done; `planFlattenedTextureSlots` (same
  module) is the mirror image that builds the array, in model order, capped at
  `MAX_TEXTURE_COUNT` and padded with slot 0 past the cap. All five entry
  points (`rasterizer`, `deferred`, `shadow_map`, `path_tracing`,
  `raytrace.rchit`) compute and clamp `textureId` the same way,
  `clamp(int(obj.texture_offset) + material.textureID, 0, MAX_TEXTURE_COUNT - 1)`,
  so a model that pushes the total past budget samples a wrong (but in-bounds)
  slot instead of reading out of the descriptor array.

Both loaders fill `Vertex::texture_coords1`, the second UV set (vertex
attribute location 5): `GltfLoader::processPrimitive` reads `TEXCOORD_1` and
copies TEXCOORD_0 where a primitive has none, as the Rust loader does;
`ObjLoader` always copies. When a glTF primitive has no `TANGENT`,
`computeTangents` builds the frame from the UV set its normal texture samples.

Both loaders also emit per-vertex colours (glTF `COLOR_0` in
`GltfLoader::processPrimitive`'s attribute switch; OBJ `attrib.colors` in
`ObjLoader::loadVertices`) and fill missing normals from the shared
`scene/Vertex.{ixx,cpp}` module, through different functions:
`GltfLoader::processPrimitive` gives a primitive without a `NORMAL` accessor
one vertex per triangle corner and calls `computeFlatNormals`, so each face
keeps its own normal, as the Rust loader does; `ObjLoader::loadVertices` calls
the sibling `fillMissingFlatNormals`, which fills only corners left at a zero
normal (a file with no `vn` at all behaves like `computeFlatNormals`; one with
`vn` on only some faces keeps those).

## `map_Kd` texture path resolution

Both loaders resolve an OBJ material's `map_Kd` the same way:

**Normalise `\` to `/` in the `map_Kd` value; resolve the result relative to
the directory containing the `.mtl`; if that file does not exist, retry under
a `textures/` subdirectory of the same directory; if that misses too, warn
and fall back to the default texture.**

- The `\`-to-`/` normalisation deliberately deviates from "as written in the
  `.mtl`": a Windows-authored relative path (`textures\wood.png`) must still
  resolve on a platform where `\` is just another filename character.
- Relative to the `.mtl` first is what the OBJ/MTL format specifies. The
  `textures/` retry exists because every textured OBJ in `Resources/Models`
  (`VikingRoom`, and the AssetANT scenes `crytek-sponza`, `Pillum` and
  `Sulo`/`WolfStahl`) keeps its textures in a `textures/` sibling directory and
  names them by bare filename, not by the format's convention.
- A path that resolves to neither candidate degrades silently to the
  untextured default (Vulkan) or a missing image (glTF) instead of failing the
  whole load, so both sides log a warning naming both candidates: the only
  signal a wrong or missing texture leaves.
- An empty base directory (a bare filename with no directory component) is
  treated as `.` on both sides, so the candidates stay relative instead of
  resolving against the filesystem root.

Where:

- **C++** (`scene/ObjLoader.cpp`, `resolveObjTexturePath`) - builds both
  candidate paths and records the first that `std::filesystem::exists`.
- **Rust** (`crates/webgpu_renderer/src/asset/obj_to_gltf.rs`,
  `convert_file`) - same two candidates, applied when copying the texture next
  to the converted glTF. The glTF `uri` it emits is always the bare filename as
  written in the `.mtl`, whichever candidate matched, so the converted document
  stays self-contained next to its `.bin`.

## Invariants worth preserving

- `parseCpu` leaves the flat `getVertices()`/`getIndices()`/
  `getMaterialIndices()`/`getMaterials()` exactly as before the split: the CPU
  parse tests key on them, and the slice reads from them.
- Every `MeshRange` must tile the flat arrays contiguously (no gap/overlap),
  and every index in a range must stay inside that range's own vertex block, or
  the `-vertexBase` re-base in `sliceMeshRange` corrupts the mesh. The
  `MultiShape…`/`MultiPrimitive…RecordsPerPrimitiveMeshRanges` parse tests
  assert this at parse level, and `Test/commit/VulkanEngine/meshRangeSliceSuite.cpp`
  unit-tests `sliceMeshRange` directly.
- Each mesh currently shares the full `materials` array (its `materialIndex`
  holds the original indices). Trimming to a per-mesh material subset is an
  optional optimisation tracked in the backlog; the slice loop is already
  shared in `sliceMeshRange`, so that trim edits one place for both loaders.
