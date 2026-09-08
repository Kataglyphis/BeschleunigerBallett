#requires -Version 7.0
# Build the project inside the ContainerHub Windows developer image using
# Stevedore's docker.exe (see third_party/ContainerHub/docs/windows-builds.md
# for why nerdctl is not an option on Windows).
#
# This is a thin project wrapper: the transport decision (tar pipe vs bind
# mount), the reusable container, the incremental streaming and the artifact
# verification all live in ContainerHub's WindowsContainerBuild.Reuse module
# (Invoke-ContainerBuild), because none of that is specific to this engine.
# Rationale + measurements: ContainerHub docs/windows-container-build-performance.md
# and docs/container-build-caching.md.

param(

  # Comma-separated Build-Windows.ps1 configurations to build.
  [string]$Configurations = 'clangcl-debug,clangcl-profile,clangcl-release',
  # EMPTY means "ask ContainerHub", which is what you want unless you are
  # testing an image that is not the family one. It cannot default to the
  # resolved value here: a param default is evaluated at bind time, before the
  # module that answers the question has been imported. Resolved below.
  [string]$Image = '',
  # Explicit docker.exe path; falls back to $env:DOCKER_EXE, the Stevedore
  # install locations, then 'docker' on PATH.
  [string]$DockerExe,
  # Process isolation exposes all host CPUs; Hyper-V isolation defaults to 2.
  [ValidateSet('process', 'hyperv')]
  [string]$Isolation = 'process',
  # Only applied under Hyper-V isolation (process isolation shares the host).
  [int]$CpuCount = 0,
  [int]$MemoryGb = 16,
  [switch]$RunTests,
  [int]$ParallelJobs = 0,
  # Opt into the bind-mount transport. Off by default because it is MEASURED
  # SLOWER on this Dev Drive host - see Invoke-ContainerBuild.
  [switch]$UseBindMount,
  # Discard the reusable build container and start from a clean one.
  [switch]$FreshContainer
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

# Preflight: Build-Windows.ps1 resolves modules from ContainerHub first, then
# the vendored fallback (scripts/windows/modules). Fail fast if a module that
# only exists vendored (deleted upstream in ContainerHub b391a1d) is missing.
. (Join-Path $PSScriptRoot 'Resolve-BuildModule.ps1')
$null = Resolve-BuildModulePath -Name 'WindowsBuild.Common'

# Reusable build-container helpers live upstream in ContainerHub - they apply to
# any project built in that image, not just this engine. Must load before first
# use (Resolve-DockerExe below).
Import-Module (Resolve-BuildModulePath -Name 'WindowsContainerBuild.Reuse') -Force -Global

# The Windows developer image, resolved from ContainerHub's
# linux/scripts/01-core/versions.env (CI_IMAGE_WINDOWS_TAG) rather than named
# here. That file is the fleet's one owner of the two CI image tags -
# .github/workflows/Windows.yml already inherits it as the container action's
# `image:` default, and Get-CiImageReference is the PowerShell twin of
# scripts/linux/ci-image-ref.sh, gated against it by ContainerHub's
# verify_ci_image_refs.py. Before this, a tag bump upstream left this local
# entry point building in the previous image with nothing to say so.
#
# Imported EXPLICITLY and not relied upon via WindowsContainerBuild.Reuse: a
# nested Import-Module inside a .psm1 binds into that module's private scope and
# never reaches this script (see Resolve-BuildModule.ps1's header).
#
# THE else BRANCH IS A PIN FALLBACK, not a second source of truth.
# Get-CiImageReference landed in ContainerHub after the commit
# third_party/ContainerHub currently pins, and this script WORKS today; a hard
# dependency would break a working local entry point to gain nothing until the
# gitlink moves. Get-Command is a capability probe, not a swallowed error.
#
# TO RETIRE: in the same commit that bumps third_party/ContainerHub to a hub
# commit exporting Get-CiImageReference, collapse this to
# `if (-not $Image) { $Image = Get-CiImageReference -Windows }` and delete this
# note.
Import-Module (Resolve-BuildModulePath -Name 'WindowsContainerImage.Common') -Force -Global
if (-not $Image) {
  $Image = if (Get-Command Get-CiImageReference -ErrorAction SilentlyContinue) {
    Get-CiImageReference -Windows
  } else {
    'ghcr.io/kataglyphis/kataglyphis_beschleuniger:winamd64'
  }
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
    '-SkipTidy', '-SkipPerfTests', '-SkipMsix'
  )
  if (-not $RunTests) { $psArgs += '-SkipTests' }
  if ($ParallelJobs -gt 0) { $psArgs += @('-ParallelJobs', "$ParallelJobs") }
  return $psArgs
}.GetNewClosure()

$cacheEnv = Get-SccacheContainerEnv
# Build trees are streamed in for incremental builds - do not wipe them.
# KATAGLYPHIS_KEEP_BUILD_ROOT is the established contract with the image.
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

  # Anchor the build-tree excludes to the repo root (./...): unanchored
  # patterns match at every path depth in bsdtar and would strip nested files
  # like third_party/ContainerHub/windows/build.ps1. The host-side
  # cargo target tree is excluded too: the container builds its own Rust
  # artifacts under the build dirs, and a stale incremental cache streamed in
  # once wedged every later transfer ("Can't unlink already-existing object:
  # Permission denied", observed 2026-08-02).
  InboundExclude = @('.git', './logs', './build', './build-*', './build_*',
    './third_party/OxidANT/target')

  IncrementalDirs    = $buildDirs
  # Cargo's cxxbridge output nests deep enough to blow past the Windows path
  # limit inside the container, which failed the whole transfer.
  IncrementalExclude = @('cargo')

  OutputDirs      = (@('logs') + $buildDirs)
  VerifyDirs      = $buildDirs
  OutboundExclude = @('*/CMakeFiles', '*/_deps', '*/cargo', '*.obj', '*.lib', '*.ilk', '*.pcm', '*/corrosion')

  UseBindMount   = $UseBindMount
  FreshContainer = $FreshContainer
}
$null = Invoke-ContainerBuild @build

Write-Host 'Container build finished successfully.'
