#requires -Version 7.0
# Compare-PerfBaseline.ps1
#
# Diffs a fresh Google-Benchmark JSON run (perfTestSuite.exe --benchmark_out=...)
# against a checked-in per-machine baseline, flagging regressions. Turns the
# "Measured baseline" table in BACKLOG.md from prose someone has to eyeball
# into an executable check.
#
# Project wrapper around ANTfrastructure's generic comparator: the JSON parsing,
# the time_unit normalisation, the matching, the tolerance check and the report
# live in WindowsPerfBaseline.Common (nothing in them is specific to this
# engine); only this repo's baseline path, tolerance and baseline-refresh
# policy live here.
#
# Run (from the repo root, after a clangcl-profile build):
#   build-clangcl-profile\perfTestSuite.exe --benchmark_out=perf.json --benchmark_out_format=json
#   pwsh -ExecutionPolicy Bypass -File .\scripts\Compare-PerfBaseline.ps1 -CandidatePath perf.json
#
# Baselines are per-machine (thread count, clock speed, thermal state all leak
# into wall-clock numbers - see "Regression tracking" under Performance testing
# in BACKLOG.md), so this script is NOT wired into CI. The checked-in baseline
# for this repo's Windows dev machine is
# Test/perf/baselines/win-9070xt-32core.json. There is deliberately no
# "capture" mode here: to refresh a baseline, run the suite, eyeball the
# result, and copy the JSON over by hand - a bad run should never be able to
# silently become the new baseline. The shared module has no capture mode
# either, and must not grow one for this consumer.
#
# Exit code: non-zero if any matched benchmark regressed beyond
# -ToleranceFraction. Benchmarks present in only one file are reported but
# never fatal - the suite grows over time and that alone should not fail a
# comparison.

[CmdletBinding()]
param(
    [string]$BaselinePath = (Join-Path $PSScriptRoot '..\Test\perf\baselines\win-9070xt-32core.json'),
    [Parameter(Mandatory)] [string]$CandidatePath,
    # Fraction over baseline real_time before a benchmark is flagged as a
    # regression. Default +25% - generous on purpose; wall-clock noise on a
    # desktop is real (see BACKLOG.md "Regression tracking").
    [double]$ToleranceFraction = 0.25
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'Windows\Resolve-BuildModule.ps1')
Import-BuildModule @('WindowsPerfBaseline.Common')

# The module formats every number with the invariant culture itself, so no
# thread-culture fiddling is needed here for "N1"/percent output to stay
# unambiguous on a comma-decimal host.
exit (Invoke-BenchmarkBaselineComparison -BaselinePath $BaselinePath `
        -CandidatePath $CandidatePath -ToleranceFraction $ToleranceFraction)
