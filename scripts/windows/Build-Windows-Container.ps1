#requires -Version 7.0
# Builds in the ANTfrastructure Windows image; see third_party/ANTfrastructure/docs/windows-container-build-performance.md

# Advanced, so an unknown parameter fails binding: a plain param() took -Preset into $args and built all three configurations.
[CmdletBinding()]
param(

  # Comma-separated Build-Windows.ps1 configurations to build.
  [string]$Configurations = 'clangcl-debug,clangcl-profile,clangcl-release',
  # Empty asks ANTfrastructure below; a param default binds before that module is imported.
  [string]$Image = '',
  # Falls back to $env:DOCKER_EXE, the Stevedore install locations, then 'docker' on PATH.
  [string]$DockerExe,
  # Process isolation exposes all host CPUs; Hyper-V isolation defaults to 2.
  [ValidateSet('process', 'hyperv')]
  [string]$Isolation = 'process',
  # Only applied under Hyper-V isolation (process isolation shares the host).
  [int]$CpuCount = 0,
  [int]$MemoryGb = 16,
  [switch]$RunTests,
  [int]$ParallelJobs = 0,
  # Off by default: the bind mount measured slower than the tar pipe on a Dev Drive host.
  [switch]$UseBindMount,
  # Discard the reusable build container and start from a clean one.
  [switch]$FreshContainer
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
# The container plumbing has one owner: the hub's Invoke-RepoContainerBuild.ps1.
$runner = Join-Path $repoRoot 'third_party\ANTfrastructure\windows\scripts\build\Invoke-RepoContainerBuild.ps1'
if (-not (Test-Path -LiteralPath $runner)) { throw "Required script not found: $runner (run: git submodule update --init --recursive third_party/ANTfrastructure)" }

Write-Host "Configurations: $Configurations"
$configurationList = @($Configurations -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })

# Build directories, as named by the shared Build-Windows configuration model.
$configModel = Import-PowerShellDataFile (Join-Path $PSScriptRoot 'Build-Windows.config.psd1')
$buildDirs = @()
foreach ($name in $configurationList) {
  $spec = $configModel.Build.Configurations[$name]
  if ($spec) { $buildDirs += $spec.BuildDir }
}

# Arguments handed to the image entrypoint (VsDevCmd + ASAN runtime PATH, then %*).
$buildCommand = {
  param([string]$WorkspacePath)

  $psArgs = @(
    'pwsh', '-NoProfile', '-ExecutionPolicy', 'Bypass',
    '-File', (Join-Path $WorkspacePath 'scripts\windows\Build-Windows.ps1'),
    '-Configurations', $Configurations,
    '-SkipPerfTests', '-SkipMsix'
  )
  if (-not $RunTests) { $psArgs += '-SkipTests' }
  if ($ParallelJobs -gt 0) { $psArgs += @('-ParallelJobs', "$ParallelJobs") }
  return $psArgs
}.GetNewClosure()

# bsdtar matches at every depth, so only names unique to the root may be listed ('./build_*' hit third_party/FUZZTEST/build_defs).
& $runner -RepoRoot $repoRoot -ContainerName 'bb-build-persistent' -BuildCommand $buildCommand `
  -Image $Image -DockerExe $DockerExe -Isolation $Isolation -CpuCount $CpuCount -MemoryGb $MemoryGb `
  -KeepDirs @('logs', 'sccache-local') `
  -InboundExclude @('.git', './logs', './build-*', './third_party/OxidANT/target') `
  -IncrementalDirs $buildDirs -IncrementalExclude @('cargo') `
  -OutputDirs (@('logs') + $buildDirs) -VerifyDirs $buildDirs `
  -OutboundExclude @('*/CMakeFiles', '*/_deps', '*/cargo', '*.obj', '*.lib', '*.ilk', '*.pcm', '*/corrosion') `
  -CacheEnv @{ KATAGLYPHIS_KEEP_BUILD_ROOT = '1' } `
  -UseBindMount:$UseBindMount -FreshContainer:$FreshContainer

Write-Host 'Container build finished successfully.'
