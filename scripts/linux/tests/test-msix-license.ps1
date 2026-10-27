#requires -Version 7.0
# Runs the shared MSIX suite without Pester; it does not build the full application.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$harness = Join-Path $root 'third_party/ANTfrastructure/windows/scripts/tests/TestHarness.psm1'
Import-Module $harness -Function Describe, It, Reset-TestState, Get-TestResult -ErrorAction Stop
Reset-TestState
. (Join-Path $root 'scripts/windows/tests/MsixLogoWiring.Tests.ps1')
$results = @(Get-TestResult)
if ($results.Count -lt 4 -or @($results | Where-Object { -not $_.Ok }).Count -gt 0) { throw 'MSIX license suite failed or ran too few cases' }
