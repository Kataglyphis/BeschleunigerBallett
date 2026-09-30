#requires -Version 7.0
# Compare-PerfBaseline.ps1 - per-machine, not CI; one-sided entries never fail; refresh baselines by hand (no capture mode).

[CmdletBinding()]
param(
    [string]$BaselinePath = (Join-Path $PSScriptRoot '..\..\Test\perf\baselines\win-9070xt-32core.json'),
    [Parameter(Mandatory)] [string]$CandidatePath,
    # Generous on purpose: wall-clock noise on a desktop is real.
    [double]$ToleranceFraction = 0.25
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'Resolve-BuildModule.ps1')
Import-BuildModule @('WindowsPerfBaseline.Common')

exit (Invoke-BenchmarkBaselineComparison -BaselinePath $BaselinePath `
        -CandidatePath $CandidatePath -ToleranceFraction $ToleranceFraction)
