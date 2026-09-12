#requires -Version 7.0
# Test all four clang-cl configurations in one session, plus (when available)
# the Linux TSan build.
#
# One-shot correctness gate: runs the three standard Windows container builds
# (debug+ASAN, profile, release) and, on hosts with Rancher Desktop / Docker
# Linux container support, the Linux ThreadSanitizer build.
#
# The sweep harness - failure aggregation, the "can this host run Linux
# containers" probe, the bind-mounted container run, the summary - was
# upstreamed on 2026-08-07 and lives in ANTfrastructure's
# WindowsBuildSweep.Common. What is left here is this project's payload: which
# configurations, which preset, which image, which build directory.
#
# Usage:
#   pwsh -ExecutionPolicy Bypass -File .\scripts\Test-AllConfigs.ps1
#
# Returns the aggregate exit code (non-zero if ANY build failed).

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
$repoRoot = Split-Path -Parent $PSScriptRoot

. (Join-Path $PSScriptRoot 'Windows\Resolve-BuildModule.ps1')
Import-BuildModule 'WindowsContainerImage.Common', 'WindowsBuildSweep.Common'

# The Linux image this project's cross builds run in, resolved from
# ANTfrastructure's linux/scripts/01-core/versions.env rather than named here.
# That file is the fleet's one owner of the two CI image tags: the container
# composite actions carry the composed ref as their `image:` default, the Linux
# workflow's fuzz-seed loop reads it through scripts/linux/ci-image-ref.sh, and
# Get-CiImageReference is the PowerShell twin of that script (ANTfrastructure
# verify_ci_image_refs.py gates that all three compose the same string). A
# fleet-wide tag bump now moves this sweep too, instead of leaving it on a
# literal that still looks right.
#
# `:latest-cross`, NOT the stale `:latest` - see ANTfrastructure
# docs/rancher-desktop-linux-containers.md, and the measured history of why the
# tag is what it is in scripts/linux/ci-image-ref.sh.
$linuxImage = Get-CiImageReference
$linuxBuildDir = 'build-linux-tsan'

$results = @()

# --- Windows container builds ---------------------------------------------
$winScript = Join-Path $PSScriptRoot 'Windows\Build-Windows-Container.ps1'
$winArgs = @('-Configurations', $WindowsConfigurations, '-SkipPerfTests')
if (-not $RunTests) { $winArgs += '-SkipTests' }
if ($FreshContainer) { $winArgs += '-FreshContainer' }

$results += Invoke-SweepStep -Name "Windows container builds ($WindowsConfigurations)" `
    -Skip:(-not (Test-Path $winScript)) `
    -SkipReason "Build script not found at $winScript." `
    -Action { & $winScript @winArgs }

# --- Linux TSan build ------------------------------------------------------
$linuxScript = Join-Path $PSScriptRoot 'Linux\cmake-configure-build.sh'
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
