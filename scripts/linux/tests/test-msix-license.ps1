#requires -Version 7.0
# Runs the shared MSIX suite under the image's Pester, which its BeforeAll blocks need; it does not build the full application.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
Import-Module Pester -MinimumVersion 5.0.0 -ErrorAction Stop
$config = New-PesterConfiguration
$config.Run.Path = Join-Path $root 'scripts/windows/tests/MsixLogoWiring.Tests.ps1'
$config.Run.PassThru = $true
$result = Invoke-Pester -Configuration $config
if ($result.FailedCount -gt 0 -or $result.FailedContainersCount -gt 0 -or $result.PassedCount -lt 4) {
    throw "MSIX license suite failed or ran too few cases: $($result.PassedCount) passed, $($result.FailedCount) failed, $($result.FailedContainersCount) container(s) failed"
}
