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
volume, while `tests/noise_test.slang` imports OxidANT's `common/noise.slang`.
If the local file is called `noise.slang`, the include path selects it first
and compilation reports `E30015 undefined identifier` for the shared
`snoise` and `fbm` functions. The manifest compiles the test to both targets,
so it checks the imported shared module and the renamed volume shader together.

The C++ renderer loads pre-compiled SPIR-V with plain file I/O
(`Kataglyphis::Shared::readBinaryFile`, called from `ShaderHelper.cpp`); there
is no runtime shader compilation. Slang emits `"main"` as the SPIR-V entry
point name (not the Slang function name), so every `pName` in pipeline
creation is `"main"`.

`histogram.wgsl` in the Rust crate is hand-written, with no Slang source:
Slang's `InterlockedAdd` on `RWStructuredBuffer` is not supported for the WGSL
target (atomics require `array<atomic<u32>>` storage). That is the WGSL
fallback policy in [`shader-sharing.md`](shader-sharing.md).

## Staleness rules

An output is reused only when it is newer than its source **and** every
`.slang` file under either source tree **and** both manifests
(conservative: an import or manifest edit rebuilds every dependent). The
compile scripts walk the manifest and recompile only stale entries.

The `BuildIntegrity` gates that check already-compiled or checked-in output
(`CompiledShadersAreNotOlderThanSharedIncludes`,
`CheckedInWgslIsNotOlderThanItsSlangSource`, in
`Test/commit/VulkanEngine/buildIntegritySuite.cpp`) do **not** copy that
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
`VULKAN_VERSION=1.4.357.0`, whose Windows SDK ships slangc `2026.13.1` (read
from the host SDK on 2026-09-25), above the floor. Whether the published
`:latest` carries this SDK was not checked here.

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
