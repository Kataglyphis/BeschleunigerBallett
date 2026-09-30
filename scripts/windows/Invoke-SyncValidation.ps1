#requires -Version 7.0
<#
.SYNOPSIS
Runs the GPU suites with Vulkan synchronization validation and fails on any SYNC-HAZARD.
.DESCRIPTION
Wraps ANTfrastructure's WindowsVulkanValidation.Common. Local only: the GPU suites
skip on CI runners, so a CI gate would be vacuous.
.PARAMETER ExecutablePath
commitTestSuite.exe; defaults to the repo root, then build-clangcl-debug\.
.PARAMETER GtestFilter
Passed as --gtest_filter; defaults to the two GPU suites.
.PARAMETER VulkanSdkBin
Directory holding the Khronos validation layer.
.PARAMETER LogDir
Where each run's timestamped log is written.
.PARAMETER LogFixturePath
Test-only: evaluate this log instead of running the suite, so Pester needs no GPU.
#>

param(
    [string]$ExecutablePath,
    [string]$GtestFilter = 'GoldenRender.*:Integration.*',
    [string]$VulkanSdkBin = 'C:\VulkanSDK\1.4.350.0\Bin',
    [string]$LogDir = (Join-Path $PSScriptRoot '..\..\logs\sync-validation'),
    [string]$LogFixturePath
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'Resolve-BuildModule.ps1')
Import-BuildModule @('WindowsVulkanValidation.Common')

# Test-only: keeps the pass/fail contract verifiable without a GPU.
if ($LogFixturePath) {
    if (Test-VulkanValidationLog -LogPath $LogFixturePath) { exit 0 }
    exit 1
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$settingsSource = (Resolve-Path (Join-Path $PSScriptRoot '..\vk_layer_settings.txt')).Path

if (-not $ExecutablePath) {
    $candidates = @(
        (Join-Path $repoRoot 'commitTestSuite.exe'),
        (Join-Path $repoRoot 'build-clangcl-debug\commitTestSuite.exe')
    )
    $ExecutablePath = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $ExecutablePath) {
        throw ("commitTestSuite.exe not found at the repo root or in build-clangcl-debug\. Build it with " +
            "'pwsh -ExecutionPolicy Bypass -File .\scripts\windows\Build-Windows-Container.ps1 " +
            "-Configurations clangcl-debug -SkipTests' and docker cp it out (see AGENTS.md), " +
            "or pass -ExecutablePath explicitly.")
    }
} elseif (-not (Test-Path $ExecutablePath)) {
    throw "Executable not found at '$ExecutablePath'."
}
$ExecutablePath = (Resolve-Path $ExecutablePath).Path

if (-not (Test-Path $VulkanSdkBin)) {
    throw "Vulkan SDK Bin directory not found at '$VulkanSdkBin'. Pass -VulkanSdkBin pointing at an installed SDK."
}

if (-not (Test-Path $LogDir)) {
    New-Item -ItemType Directory -Path $LogDir -Force | Out-Null
}
$timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$logPath = Join-Path $LogDir "$timestamp.log"

# Resources/ loads CWD-relative, so run from the repo root; see docs/gpu-golden-testing.md.
Invoke-VulkanValidationRun -ExecutablePath $ExecutablePath `
    -Arguments @("--gtest_filter=$GtestFilter") `
    -WorkingDirectory $repoRoot `
    -LogPath $logPath `
    -LayerPath $VulkanSdkBin `
    -SettingsPath $settingsSource
$exitCode = $LASTEXITCODE

$hazards = @(Get-VulkanValidationHazard -LogPath $logPath)
if ($hazards.Count -gt 0) {
    Write-VulkanValidationReport -Hazard $hazards -LogPath $logPath
    exit 1
}

Write-Host ''
if ($exitCode -ne 0) {
    Write-Host "Test executable exited with code $exitCode (no SYNC-HAZARD lines found, but the run itself failed - check $logPath)." -ForegroundColor Yellow
    exit $exitCode
}

Write-Host '=== NO SYNCHRONIZATION HAZARDS DETECTED ===' -ForegroundColor Green
exit 0
