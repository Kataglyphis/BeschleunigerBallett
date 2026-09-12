#requires -Version 7.0
<#
.SYNOPSIS
  Compiles Slang shaders to SPIR-V (Vulkan/C++) and WGSL (Rust/WebGPU).

.DESCRIPTION
  Slang is the single source of truth for shaders shared between the C++
  Vulkan renderer and the Rust WebGPU renderer. This script compiles each
  Slang entry point to the targets its consumer needs:
    - spirv  -> .spv  (loaded by the C++ Vulkan renderer)
    - wgsl   -> .wgsl (consumed by the Rust WebGPU renderer)

  Everything generic — resolving slangc, expanding the -I include paths,
  reading the manifest, compiling each (file, entry, target) with staleness
  checking, the combined WGSL emit with its post-emit patch table, the
  minSlangcVersionForWgsl floor and the WGSL varying-location validator — lives
  upstream in ANTfrastructure's WindowsSlang.Common module (twin of
  linux/scripts/lib/slang-compile.sh). This script keeps only this project's
  paths.

  The project data (entry points, targets, the combined-WGSL map and the
  depth-texture patch table) is single-sourced from
  Resources/ShadersSlang/shader-manifest.json, shared with
  scripts/linux/compile-slang-shaders.sh. Schema notes live in that file's
  "_comment" fields.

  Math-only modules (e.g. common/aces.slang) have no entry point and are
  never emitted directly: they are `import`ed by entry-point shaders and
  linked by slangc. See docs/shader-sharing.md.

  Staleness: an output is reused only when it
  is newer than its source AND every .slang file under the Slang tree AND
  the manifest file itself (conservative — an import or manifest edit
  rebuilds every dependent).

.NOTES
  slangc is resolved from VULKAN_SDK\Bin, then PATH. The Vulkan SDK ships
  slangc (verified: VulkanSDK 1.4.350.0). A missing slangc is a hard failure,
  never a silent skip. Container availability of slangc is an open item — see
  docs/shader-sharing.md.
#>

param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$scriptRoot = Resolve-Path (Join-Path $PSScriptRoot '..\..') | Select-Object -ExpandProperty Path
$slangRoot = Join-Path $scriptRoot 'Resources\ShadersSlang'
$buildRoot = Join-Path $slangRoot 'build'

# The driver lives upstream; Resolve-BuildModule fails loudly when the
# ANTfrastructure submodule is not checked out.
. (Join-Path $PSScriptRoot 'Resolve-BuildModule.ps1')
Import-BuildModule @('WindowsSlang.Common')

Invoke-SlangShaderCompile `
    -ManifestPath (Join-Path $slangRoot 'shader-manifest.json') `
    -SourceRoot $slangRoot `
    -SpirvOutputRoot (Join-Path $buildRoot 'spirv') `
    -WgslOutputRoot (Join-Path $buildRoot 'wgsl') `
    -CombinedOutputDir $buildRoot `
    -DestinationRoot $scriptRoot
