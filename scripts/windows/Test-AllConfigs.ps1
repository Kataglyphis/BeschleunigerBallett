#requires -Version 7.0
# Test-AllConfigs.ps1 - the Windows container builds plus, where Linux containers run, the TSan build; non-zero if any failed.

[CmdletBinding()]
param(

    # Comma-separated list of Windows configurations to build.
    [string]$WindowsConfigurations = 'clangcl-debug,clangcl-profile,clangcl-release',
    # Linux CMake preset.
    [string]$LinuxPreset = 'linux-debug-tsan-clang',
    # Passed through to Build-Windows-Container.ps1.
    [switch]$FreshContainer,
    # Also run the test suite after each build (Windows only).
    [switch]$RunTests,
    # Skip the Linux TSan build entirely.
    [switch]$SkipLinux
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

. (Join-Path $PSScriptRoot 'Resolve-BuildModule.ps1')
Import-BuildModule 'WindowsContainerImage.Common', 'WindowsBuildSweep.Common'

# From versions.env, never a literal, so a fleet-wide tag bump moves this sweep too.
$linuxImage = Get-CiImageReference
$linuxBuildDir = 'build-linux-tsan'

$results = @()

# Windows container builds
$winScript = Join-Path $PSScriptRoot 'Build-Windows-Container.ps1'
$winArgs = @('-Configurations', $WindowsConfigurations, '-SkipPerfTests')
if (-not $RunTests) { $winArgs += '-SkipTests' }
if ($FreshContainer) { $winArgs += '-FreshContainer' }

$results += Invoke-SweepStep -Name "Windows container builds ($WindowsConfigurations)" `
    -Skip:(-not (Test-Path $winScript)) `
    -SkipReason "Build script not found at $winScript." `
    -Action { & $winScript @winArgs }

# Linux TSan build
$linuxScript = Join-Path $PSScriptRoot '..\linux\cmake-configure-build.sh'
$linuxSkipReason = ''
if ($SkipLinux) {
    $linuxSkipReason = 'Requested with -SkipLinux.'
} elseif (-not (Test-Path $linuxScript)) {
    $linuxSkipReason = "Build script not found at $linuxScript."
} elseif (-not (Test-LinuxContainerSupport)) {
    $linuxSkipReason = 'Linux containers not available on this host - install Rancher Desktop or Docker Desktop with Linux container support.'
}

$results += Invoke-SweepStep -Name "Linux TSan build ($LinuxPreset)" `
    -Skip:([bool]$linuxSkipReason) `
    -SkipReason $linuxSkipReason `
    -Action {
    Invoke-InLinuxContainerBuild -RepoRoot $repoRoot -Image $linuxImage -Command @"
bash /workspace/scripts/linux/cmake-configure-build.sh \
    --preset $LinuxPreset \
    --build-dir $linuxBuildDir \
    --skip-configure false
"@
}

exit (Write-SweepSummary -Result $results)
