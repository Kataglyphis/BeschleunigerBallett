#requires -Version 7.0
# Builds in the ANTfrastructure Windows image; see third_party/ANTfrastructure/docs/windows-container-build-performance.md

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

# Preflight: fail before any container starts if the build modules cannot be resolved.
. (Join-Path $PSScriptRoot 'Resolve-BuildModule.ps1')
$null = Resolve-BuildModulePath -Name 'WindowsBuild.Common'

# Must load before its first use (Resolve-DockerExe below).
Import-Module (Resolve-BuildModulePath -Name 'WindowsContainerBuild.Reuse') -Force -Global

# Explicit: Reuse's nested import is module-private, and the tag must follow versions.env.
Import-Module (Resolve-BuildModulePath -Name 'WindowsContainerImage.Common') -Force -Global
if (-not $Image) {
  $Image = Get-CiImageReference -Windows
}

$docker = Resolve-DockerExe -Override $DockerExe
Write-Host "Using docker: $docker"
Write-Host "Image: $Image"
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

$cacheEnv = Get-SccacheContainerEnv
# Build trees are streamed in for incremental builds; this tells the image not to wipe them.
$cacheEnv['KATAGLYPHIS_KEEP_BUILD_ROOT'] = '1'

$build = @{
  DockerExe     = $docker
  Image         = $Image
  ContainerName = 'bb-build-persistent'
  RepoRoot      = $repoRoot
  BuildCommand  = $buildCommand
  IsolationArgs = (Get-ContainerIsolationArgs -Isolation $Isolation -CpuCount $CpuCount -MemoryGb $MemoryGb)
  CacheEnv      = $cacheEnv
  KeepDirs      = @('logs', 'sccache-local')

  # bsdtar matches at every depth: './build' would drop third_party/ANTinfrastructure/windows/scripts/build.
  InboundExclude = @('.git', './logs', './build-*', './build_*',
    './third_party/OxidANT/target')

  IncrementalDirs    = $buildDirs
  # cxxbridge output nests past the Windows path limit inside the container.
  IncrementalExclude = @('cargo')

  OutputDirs      = (@('logs') + $buildDirs)
  VerifyDirs      = $buildDirs
  OutboundExclude = @('*/CMakeFiles', '*/_deps', '*/cargo', '*.obj', '*.lib', '*.ilk', '*.pcm', '*/corrosion')

  UseBindMount   = $UseBindMount
  FreshContainer = $FreshContainer
}
$null = Invoke-ContainerBuild @build

Write-Host 'Container build finished successfully.'
