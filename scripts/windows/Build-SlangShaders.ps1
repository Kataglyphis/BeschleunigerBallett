#requires -Version 7.0
<#
.SYNOPSIS
  Compiles Slang shaders to SPIR-V (C++ Vulkan) and WGSL (Rust WebGPU).
.DESCRIPTION
  This project's paths over ANTfrastructure's WindowsSlang.Common; the data lives in
  Resources/ShadersSlang/shader-manifest.json, shared with compile-slang-shaders.sh.
#>

param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$scriptRoot = Resolve-Path (Join-Path $PSScriptRoot '..\..') | Select-Object -ExpandProperty Path
$slangRoot = Join-Path $scriptRoot 'Resources\ShadersSlang'
$buildRoot = Join-Path $slangRoot 'build'

. (Join-Path $PSScriptRoot 'Resolve-BuildModule.ps1')
Import-BuildModule @('WindowsSlang.Common')

Invoke-SlangShaderCompile `
    -ManifestPath (Join-Path $slangRoot 'shader-manifest.json') `
    -SourceRoot $slangRoot `
    -SpirvOutputRoot (Join-Path $buildRoot 'spirv') `
    -WgslOutputRoot (Join-Path $buildRoot 'wgsl') `
    -CombinedOutputDir $buildRoot `
    -DestinationRoot $scriptRoot
