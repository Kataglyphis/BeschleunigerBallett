# Shader Build Pipeline (Slang → SPIR-V/WGSL)

How Slang becomes SPIR-V and WGSL here. `AGENTS.md` links here rather than
restating it.

## How it works

Shaders are [Slang](https://shader-slang.com/) in two pinned source trees:
Vulkan entry points and material modules under `Resources/ShadersSlang/`, and
shared math plus WebGPU entry points under `third_party/OxidANT/shaders/`.
The build scripts (`scripts/windows/Build-SlangShaders.ps1`,
`scripts/linux/compile-slang-shaders.sh`) compile each `.slang` file to:

- **SPIR-V** (`.spv`) for the C++ Vulkan renderer → `Resources/ShadersSlang/build/spirv/`
- **WGSL** (`.wgsl`) for the Rust WebGPU renderer → `Resources/ShadersSlang/build/wgsl/`.
  Each source in the manifest's `wgslMap` is also compiled without
  `-entry`/`-stage`, which puts all its entry points in one combined WGSL file,
  and copied into OxidANT's `crates/webgpu_renderer/src/shaders/` or
  `crates/gui/src/shaders/`, replacing the hand-written WGSL there.

**The compile is wired into the builds**, no opt-in flag:
`scripts/linux/cmake-configure-build.sh` compiles the Slang manifest before
every Linux build, and `Build-Windows.ps1` before its `clangcl-debug`,
`clangcl-release` and `msvc-release` builds (not `clangcl-profile` or
`msvc-debug`), so a `.slang` edit reaches the Vulkan engine on the next such
build. A bare `cmake --build` compiles no shaders. `slangc` is resolved from
`VULKAN_SDK\Bin`, then `PATH` (the Vulkan SDK ships it; verified against
1.4.350.0).

Both scripts are thin wrappers holding only this project's paths (manifest,
Slang source root, SPIR-V/WGSL output roots, and the repository root the
manifest's `wgslMap` destinations resolve against). The driver (slangc
resolution, `-I` expansion, the manifest walk, staleness checking, the combined
WGSL emit with its patch table, the toolchain floor and the varying-location
validator below) lives in ANTfrastructure (`linux/scripts/lib/slang-compile.sh`
and `windows/scripts/modules/WindowsSlang.Common.psm1`), so every Slang project
gets the same guarantees. Change behaviour there, in both twins, and keep the
`BuildIntegrity` tests in step.

Slang imports a module by filename basename. Keep local basenames distinct
from OxidANT's shared modules: `compute/noise_volume.slang` produces the cloud
volume, while OxidANT's `tests/noise_test.slang`, which this manifest compiles
through its import, imports `common/noise.slang`.
If the local file is called `noise.slang`, the include path selects it first
and compilation reports `E30015 undefined identifier` for the shared
`snoise` and `fbm` functions. The manifest compiles the test to both targets,
so it checks the imported shared module and the renamed volume shader together.

The C++ renderer loads pre-compiled SPIR-V with plain file I/O
(`Kataglyphis::Shared::readBinaryFile`, called from `ShaderHelper.cpp`); there
is no runtime shader compilation. Slang emits `"main"` as the SPIR-V entry
point name (not the Slang function name), so every `pName` in pipeline
creation is `"main"`.

`histogram.wgsl` in the Rust crate is generated from OxidANT's
`histogram/histogram.slang` since 2026-11-23, the last hand-written WGSL.
`InterlockedAdd` does not compile for the WGSL target, on a `RWStructuredBuffer`
or on `groupshared` memory alike, but slangc 2026.17 emits `Atomic<uint>` as
`atomic<u32>` in both: the global `array<atomic<u32>>` bins and the per-workgroup
`var<workgroup>` ones the histogram privatises into; OxidANT's histogram and
auto-exposure tests run that output through wgpu.

## Staleness rules

An output is reused only when it is newer than its source **and** every
`.slang` file under either source tree **and** both manifests
(conservative: an import or manifest edit rebuilds every dependent). The
compile scripts walk the manifest and recompile only stale entries.

The `BuildIntegrity` gates that check already-compiled or checked-in output
(`CompiledShadersAreNotOlderThanSharedIncludes` and
`CheckedInWgslIsNotOlderThanItsSlangSource`, in `Test/commit/VulkanEngine/`'s
`buildIntegrityShaderBinarySuite.cpp` and `buildIntegrityWgslSuite.cpp`) do **not** copy that
whole-tree rule: they compare each source against its `import_closure`, the
`.slang` files it (transitively) actually `import`s, and flag an artifact
stale only when one of *those* is newer. Rebuild-everything is right for
producing outputs; used to *check* a built tree, the "newest file anywhere
under common/" rule flagged artifacts whose real dependencies never changed,
which turned every checked-in WGSL red after an edit to one material module.
One gap remains either way: a dependent shader whose emitted output is
byte-identical after a `common/` edit is never re-copied, so its mtime stays
behind and the per-shader gate still reports it stale. Fixing that needs a
content stamp from the compile scripts themselves (upstream in
ANTfrastructure), not a smarter mtime comparison here.

The Windows ARM64 GPU job uses two separate checkouts: the x64 cross build
snapshots pinned OxidANT WGSL before the compiler overwrites it, clears old
combined outputs, and requires every new emit to match that snapshot. The ARM
host checks out the pinned OxidANT tree again. Their
checkout mtimes cannot be compared. The cross build stages the freshly emitted
WGSL with its Slang source trees. Artifact download also rewrites mtimes, so
`CheckedInWgslIsNotOlderThanItsSlangSource` compares every emitted WGSL body
with the host's checked-in file after CRLF/LF normalization. It also
compares every staged `.slang` source and both manifests with the host checkout,
so an artifact from another source revision cannot satisfy the proof. Missing
outputs or changed shader code fail. Local runs keep the source-to-output mtime
check in their own single checkout.
The provenance fixture writes its CRLF/LF samples in binary mode: Windows text-mode output would
add a second carriage return and make an unchanged source look different before mutation checks.

## The manifest is data, in one place

`Resources/ShadersSlang/shader-manifest.json` owns the Vulkan list and imports
`third_party/OxidANT/shaders/shader-manifest.json` through `wgslManifest`.
OxidANT owns the WebGPU list, combined-WGSL destinations, compiler floor and
depth-texture patches. `wgslDestinationPrefix` resolves those destinations
under the pinned submodule. Both compile scripts read that composition (PS via
`ConvertFrom-Json`, bash via `python3`, which is required and fails loud when
missing; `jq` is *not* in the ANTfrastructure Linux image). Add or retarget a
shader by editing the JSON, never the scripts: before this the two scripts had
drifted four ways, including Linux silently skipping compilation when slangc
was absent. `_comment` fields in the JSON carry the rationale that used to be
code comments.

OxidANT can regenerate its WGSL independently with its own
`scripts/linux/compile-slang-shaders.sh` or `scripts/windows/Build-SlangShaders.ps1`.
Its `--check` / `-Check` mode regenerates into a temporary tree and rejects drift
from committed WGSL; Linux `fmt-clippy` runs that check. Make WebGPU shader changes
there, commit the source and generated output together, then move this repo's
OxidANT pin. The source-integrity gates resolve both trees and retain the same
logical shader paths and checks.

## The combined WGSL emit needs slangc ≥ 2026.8

WGSL requires `@location(N)` on every non-builtin member of an inter-stage
(varying) struct. **slangc `2026.1-52-gc8ddf20bb`** (the build in Vulkan SDK
1.4.341.1, which the ANTfrastructure Linux image put on `PATH` when this was
found: then `:latest-cross` with `VULKAN_VERSION=1.4.341.1`) drops that
attribute in the **combined** emit (no `-entry`/`-stage`, which is how every
`wgslMap` file is produced), turning

```wgsl
    @location(0) uv_0 : vec2<f32>,
```

into a bare `uv_0 : vec2<f32>,` that wgpu/naga rejects. The **per-entry** emits
from the *same binary* are correct, so it is a whole-module-emit bug, not a
layout-assignment one, and not ours: the scripts copy slangc's output verbatim
apart from the `depthTexturePatches` regexes (and `bloom.wgsl`, one of the
affected files, has no patches at all).

It is **not** a Windows-vs-Linux divergence, though it looked like one while
the Windows host ran a newer SDK than the container. Verified 2026-08-02:
slangc **2026.8** emits `@location` correctly, and its Linux output is
**byte-for-byte identical** to the checked-in (Windows-generated) files; the
only Windows/Linux difference is CRLF vs LF, which `.gitattributes` normalises
away.

Two guards, in `shader-manifest.json`'s `minSlangcVersionForWgsl` (`2026.8`)
and in both compile scripts:

1. **Below the floor the combined emit is skipped entirely**, with a loud
   warning. SPIR-V still compiles, and the checked-in WGSL is left untouched
   instead of being overwritten with output naga rejects. After editing a
   `.slang` that feeds `wgslMap`, regenerate on a toolchain at or above the
   floor; `BuildIntegrity.CheckedInWgslIsNotOlderThanItsSlangSource` fails if
   that is forgotten.
2. **At or above the floor the emit is verified before it is copied**: a
   struct with at least one `@builtin`/`@location` member (an IO struct) and a
   member with neither fails the script, which names the file and lines and
   does *not* copy it into the Rust crate. An emit regression cannot be
   committed silently either.

`BuildIntegrity.CheckedInWgslVaryingStructsCarryLocations` applies the same
rule to the checked-in files on every CI platform, and
`BuildIntegrity.ShaderManifestPinsAMinimumSlangcVersionForWgsl` keeps the floor
from being deleted. Raise the floor rather than hand-editing generated WGSL;
bumping `VULKAN_VERSION` in the ANTfrastructure image re-enables WGSL
regeneration on Linux. The pinned hub's `versions.env` names
`VULKAN_VERSION=1.4.363.0`. The older 1.4.357.0 Windows SDK already shipped
slangc `2026.13.1`, above the floor. Whether the published
`:latest` carries this SDK was not checked here.

## Uniform-index twins for a device without non-uniform indexing

Every `textures[]` and `textureSamplers[]` subscript goes through
`material_texture_index` in `common/material_textures.slang`, which returns
`NonUniformResourceIndex(slot)`. For that slangc declares `ShaderNonUniform`
and `SampledImageArrayNonUniformIndexing`. On a device without
`shaderSampledImageArrayNonUniformIndexing` such a module is invalid usage,
which the validation layer reports when the module is created. So each raster
material shader has a twin without the decoration:

- `material_texture_index` reads the link-time constant `kUniformTextureIndex`
  (`extern static const bool`). Every entry module that imports
  `material_textures` exports it. `rasterizer.slang`, `deferred.slang` and
  `shadow_map.slang` export `true` under `#if defined(KATAGLYPHIS_UNIFORM_TEXTURE_INDEX)`,
  `false` otherwise; an importer without the export fails to link (slangc
  `E45001 unresolved external symbol`). Linking folds the select, so the 21
  default SPIR-V modules are byte-identical to the direct
  `NonUniformResourceIndex` form (slangc 2026.17).
- The twin `<stem>_uniform_index.slang` beside its base is two lines,
  `#define KATAGLYPHIS_UNIFORM_TEXTURE_INDEX` and `#include "<stem>.slang"`:
  the hub driver passes slangc no `-D`, so the define lives in a source file.
  Its manifest rows (forward's `fs_main` and `fs_depth_prepass`, the G-buffer's
  `geometry_fs_main`, the shadow casters' `shadow_fs_main`) emit
  `<stem>_uniform_index.<entry>.spv` like any other row. The vertex entries and
  `lighting_fs_main` never index the arrays and need no twin.
- `raytrace.rchit`, `raytrace.rahit` and `path_tracing` export `false` and have
  no twin: without the feature `VulkanDevice.cpp` turns hardware ray tracing off,
  and both traced modes with it.
- `VulkanDevice` calls `materialTextureIndexingFor` once at device creation and
  logs the choice in one line. Each material pipeline loads
  `materialShaderPath(path, device->materialTextureIndexing())`
  (`vulkan_base/PhysicalDeviceChoices.hpp`), which names the twin on such a
  device and the base everywhere else.
- A variable index into `textures[]` needs `shaderSampledImageArrayDynamicIndexing`,
  uniform or not. Without it neither variant is valid. The choice is then
  `None`, logged as a warning that names the missing feature, and the twin
  loads as the variant that needs fewer features.
- `KATAGLYPHIS_FORCE_UNIFORM_TEXTURE_INDEX` picks the twin on a device that has
  both features, so a golden on llvmpipe can create and draw it.

The twin is not exact. Its slot comes from the draw's object and the
triangle's material (`fetch_material(obj, primitiveID)`). A draw whose
triangles use different materials, such as an OBJ shape with several, may
sample the wrong texture there. That is the cost of keeping such a device as
a target.

### What v3dv on the Raspberry Pi 5 offers

The twins alone do not make the Pi 5 a target. `vulkaninfo` there (V3D 7.1.7,
Mesa 26.2.7) reports four limits the material shaders exceed:

| What the material shaders need | What v3dv reports |
| --- | --- |
| `shaderInt64`, as every twin declares `Int64` | false |
| `shaderSampledImageArrayDynamicIndexing`, for any variable `textures[]` index | false |
| 128 sampled images and 128 samplers per stage (`MAX_TEXTURE_COUNT`) | 16 and 24 (`maxPerStageDescriptorSampledImages`, `maxPerStageDescriptorSamplers`) |
| `descriptorIndexing`, `runtimeDescriptorArray`, `descriptorBindingPartiallyBound` | all false |

So the selection reports `None` on the Pi. `BACKLOG.md` holds the rest: modules
without `Int64`, sampling without a dynamic index, and at most 16 sampled
images per stage. The twins also keep `Geometry` and
`PhysicalStorageBufferAddresses`, which v3dv offers as `geometryShader` and
`bufferDeviceAddress`.

### Tests

`PhysicalDeviceChoicesUnit.*` covers the selection, v3dv's feature shape and
the file name. `BuildIntegrity.UniformIndexTwinsWrapTheirBaseAndEveryMaterialPipelinePicksOne`
checks the twin's two lines and the base's export. It also checks that `= true`
appears only under the define, and every `.spv` literal of a twinned shader
under `Src/`. `BuildIntegrity.UniformIndexTwinsDeclareNoNonUniformIndexing`
reads the compiled SPIR-V. A twin must declare neither capability nor
decoration. Every other module that samples `textures[]`, the traced ones
included, must declare both. A raster shader that declares the capability
without a twin fails, so a new material fragment entry needs its twin row.
`GoldenRender.UniformIndexTwinsDrawTheDefaultFramesWhenEachDrawHasOneMaterial`
forces the twins on llvmpipe. Their six pipelines must be created and draw
without a validation error. A rig with a MASK and a BLEND card, forward and
deferred, must draw byte-identical to the default shaders.

## Fast shader iteration

Edit a `.slang` file, then recompile:

```pwsh
pwsh -NoProfile -ExecutionPolicy Bypass -File .\scripts\windows\Build-SlangShaders.ps1
```

```bash
bash scripts/linux/compile-slang-shaders.sh
```

The C++ engine loads the `.spv` files at startup through
`Kataglyphis::Shared::readBinaryFile` (`File::readCharSequence` until
2026-08-04), so a shader-only change needs no C++ rebuild: recompile the
shaders and run.

## Historical note: the retired GLSL/glslc pipeline (until the Slang migration)

Shaders used to be hand-written GLSL under `Resources/Shaders/`, compiled per
file by `compile-shaders.ps1` via `glslc`, with staleness checked separately by
`ShaderHelper::compileShader` at runtime. That tree and script are gone:
`Resources/` holds only `ShadersSlang/`, `Models/` and `Textures/`, and the two
scripts under "How it works" are the only compile path. Two lessons from then
shaped the Slang scripts:

- **Silent reuse of a stale output is the worst failure mode.** The GLSL
  pipeline reused a `.spv` whenever the file existed, without comparing
  timestamps, so a shader edit could go unexecuted while the GPU kept running
  the old binary. It cost real debugging time on a shadow-rendering issue that
  turned out to be stale SPIR-V, not a logic bug. The Slang staleness rule
  above (reuse only when newer than source **and** every `.slang` file) exists
  because of it.
- **A compiler warning nobody acts on is indistinguishable from success.** A
  `glslc` include-path bug silently failed to compile the main rasterizer
  fragment shader on every build (only a `Write-Warning`, the previous `.spv`
  left on disk, build reported success) and was misdiagnosed as an intentional
  legacy limitation. `BuildIntegrity.EveryShaderSourceHasCompiledBinary` makes
  a silently skipped shader source fail a test instead of aging invisibly.

## Known gaps

- `Resources/ShadersSlang/build/` (compiled `.spv`/`.wgsl` output) is
  gitignored, so a fresh clone must run a compile script before the engine has
  anything to load. The staleness checks only protect an existing checkout;
  they do not replace the first compile.
