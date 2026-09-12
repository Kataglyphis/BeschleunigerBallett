# Shader Build Pipeline (Slang → SPIR-V/WGSL)

How Slang becomes SPIR-V and WGSL here. `AGENTS.md` links here rather than
restating it.

## How it works

Shaders are written in [Slang](https://shader-slang.com/) under
`Resources/ShadersSlang/`. The build scripts
(`scripts/windows/Build-SlangShaders.ps1`,
`scripts/linux/compile-slang-shaders.sh`) compile each `.slang` file to:

- **SPIR-V** (`.spv`) for the C++ Vulkan renderer → `Resources/ShadersSlang/build/spirv/`
- **WGSL** (`.wgsl`) for the Rust WebGPU renderer → `Resources/ShadersSlang/build/wgsl/`
  (combined WGSL files are also copied into the Rust crates per the manifest's `wgslMap` — `crates/webgpu_renderer/src/shaders/` and `crates/gui/src/shaders/`)

Both scripts are thin wrappers: they hold only this project's paths (manifest,
Slang source root, the SPIR-V/WGSL output roots and the repository root the
manifest's `wgslMap` destinations resolve against). The driver itself —
slangc resolution, `-I` expansion, the manifest walk, staleness checking, the
combined WGSL emit with its patch table, the toolchain floor and the
varying-location validator described below — lives upstream in ANTfrastructure
(`linux/scripts/lib/slang-compile.sh` and
`windows/scripts/modules/WindowsSlang.Common.psm1`), so any Slang project gets
the same guarantees. Change behaviour there, in both twins, and keep the
`BuildIntegrity` tests in step.

The C++ renderer loads pre-compiled SPIR-V via `File` I/O — there is no
runtime shader compilation. Slang emits `"main"` as the SPIR-V entry point
name (not the Slang function name), so all `pName` values in pipeline
creation use `"main"`.

`histogram.wgsl` in the Rust crate is hand-written with no generating Slang
source at all: Slang's `InterlockedAdd` on `RWStructuredBuffer` is not
supported for the WGSL target (atomics require `array<atomic<u32>>`
storage), and the WGSL fallback policy above is that such a shader's WGSL
stays hand-written rather than kept in sync with a Slang source nothing
consumes on the C++ side.

## Staleness rules

An output is reused only when it is newer than its source **and** every
`.slang` file under the Slang tree (conservative — an import edit rebuilds
every dependent). The compile scripts walk the manifest and recompile only
stale entries.

The `BuildIntegrity` gates that verify already-compiled/checked-in output
(`CompiledShadersAreNotOlderThanSharedIncludes`,
`CheckedInWgslIsNotOlderThanItsSlangSource`, in
`Test/commit/VulkanEngine/buildIntegritySuite.cpp`) do **not** mirror that
whole-tree conservatism: they compare each source against `import_closure`,
the set of `.slang` files it (transitively) actually `import`s, and only flag
an artifact stale when one of *those* files is newer. This matters because
the two rules verify different things — the compile scripts' conservative
rebuild-everything is the correct policy for producing outputs from scratch,
while a gate that reused the same "newest file anywhere under common/" logic
to *check* an already-built tree would flag artifacts whose real dependencies
never changed, which is exactly what made editing one material module turn
every checked-in WGSL red. One gap survives either way: a dependent shader
whose emitted output is byte-identical after a `common/` edit is never
re-copied, so its mtime stays behind and the per-shader gate still reports it
stale — fixing that needs a content stamp from the compile scripts
themselves (upstream in ANTfrastructure), not a smarter mtime comparison here.

## The manifest is data, in one place

The shader list (source file, entry point, stage, targets), the combined-WGSL
copy map, and the depth-texture patch table live in
`Resources/ShadersSlang/shader-manifest.json` — the single source of truth
read by BOTH compile scripts (PS via `ConvertFrom-Json`, bash via `python3`,
which is required and fails loud when missing — `jq` is *not* in the
ANTfrastructure Linux image). Add or retarget a shader by
editing the JSON, never by editing the scripts; the two scripts stayed in
sync only by luck before this (they had drifted four ways, including Linux
silently skipping compilation when slangc was absent). `_comment` fields in
the JSON carry the rationale that used to be code comments.

## The combined WGSL emit needs slangc ≥ 2026.8

WGSL requires every non-builtin member of an inter-stage (varying) struct to
carry `@location(N)`. **slangc `2026.1-52-gc8ddf20bb`** — the build shipped by
Vulkan SDK 1.4.341.1, i.e. what the ANTfrastructure Linux image
(`:latest-cross`, `VULKAN_VERSION=1.4.341.1`) puts on `PATH` — drops that
attribute in the **combined** emit (compiled without `-entry`/`-stage`, which
is exactly how every `wgslMap` file is produced), turning

```wgsl
    @location(0) uv_0 : vec2<f32>,
```

into a bare `uv_0 : vec2<f32>,` that wgpu/naga rejects. The **per-entry**
emits from the *same binary* are correct, so this is a whole-module-emit bug,
not a layout-assignment one, and none of it comes from our scripts: they copy
slangc's output verbatim apart from the `depthTexturePatches` regexes (and
`bloom.wgsl`, one of the affected files, has no patches at all).

It is **not** a Windows-vs-Linux divergence, which is what it looked like
while the Windows host ran a newer SDK than the container. Verified
2026-08-02: slangc **2026.8** produces `@location` correctly and its output on
Linux is **byte-for-byte identical** to the checked-in (Windows-generated)
files — the only Windows/Linux difference in the emit is CRLF vs LF, which
`.gitattributes` normalises away.

Two guards, in `shader-manifest.json`'s `minSlangcVersionForWgsl` (`2026.8`)
and in both compile scripts:

1. **Below the floor the combined emit is skipped entirely** with a loud
   warning. SPIR-V still compiles, and the checked-in WGSL is left untouched
   instead of being overwritten with output naga rejects. If you edit a
   `.slang` that feeds `wgslMap` you must regenerate on a toolchain at or
   above the floor —
   `BuildIntegrity.CheckedInWgslIsNotOlderThanItsSlangSource` fails if that
   regeneration is skipped and forgotten.
2. **At or above the floor the emit is verified before it is copied**: any
   struct that has at least one `@builtin`/`@location` member (i.e. an IO
   struct) but also a member with neither fails the script, names the
   offending file and lines, and is *not* copied into the Rust crate. So a
   future emit regression can never be committed silently either.

`BuildIntegrity.CheckedInWgslVaryingStructsCarryLocations` applies the same
rule to the checked-in files on every CI platform, and
`BuildIntegrity.ShaderManifestPinsAMinimumSlangcVersionForWgsl` keeps the
floor from being deleted. Raise the floor rather than hand-editing generated
WGSL; bumping `VULKAN_VERSION` in the ANTfrastructure image is what re-enables
WGSL regeneration on Linux.

## Fast shader iteration

Edit a `.slang` file, then recompile locally:

```pwsh
pwsh -NoProfile -ExecutionPolicy Bypass -File .\scripts\windows\Build-SlangShaders.ps1
```

Or on Linux:

```bash
bash scripts/linux/compile-slang-shaders.sh
```

The C++ engine loads the `.spv` files at startup via `File::readCharSequence`,
so no rebuild of the C++ binary is needed for a shader-only change — just
recompile the shaders and run.

## Historical note: the retired GLSL/glslc pipeline (until the Slang migration)

Before the Slang migration, shaders were hand-written GLSL under a
`Resources/Shaders/` tree, compiled per-file by `compile-shaders.ps1` via
`glslc`, with staleness checked independently by `ShaderHelper::compileShader`
at runtime. That tree and script no longer exist — `Resources/` now holds only
`ShadersSlang/`, `Models/`, and `Textures/`, and the two build scripts named
under "How it works" above are the only compile path. The staleness and
failure-visibility lessons from that era are still worth keeping, because the
Slang scripts were written to avoid repeating them:

- **Silent reuse of a stale output is the worst failure mode.** The GLSL
  pipeline once reused a `.spv` whenever the file merely existed, without
  comparing timestamps — a shader edit could go unexecuted indefinitely while
  the GPU kept running the old binary, which cost real debugging time on a
  shadow-rendering issue that turned out to be stale SPIR-V, not a logic bug.
  The Slang staleness rule above (reuse only when newer than source **and**
  every `.slang` file) exists because of that incident.
- **A compiler warning nobody acts on is indistinguishable from success.** A
  `glslc` include-path bug once silently failed to compile the main rasterizer
  fragment shader on every build (only a `Write-Warning`, previous `.spv` left
  on disk, build reported success) and was misdiagnosed as an intentional
  legacy limitation. `BuildIntegrity.EveryShaderSourceHasCompiledBinary`
  exists so a silently-skipped shader source fails a test instead of aging
  invisibly.

## Known gaps

- `Resources/ShadersSlang/build/` (compiled `.spv`/`.wgsl` output) is
  gitignored, not committed — a fresh clone must run the compile script
  before the engine has anything to load. The staleness checks under
  "Staleness rules" above only protect an existing checkout with stale
  outputs; they do not substitute for the initial compile.
