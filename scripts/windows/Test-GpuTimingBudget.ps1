#requires -Version 7.0
# Per-host GPU pass budgets over a KATAGLYPHIS_GPU_TIMING_JSON dump; per machine, never a CI gate (timings are not gated).

[CmdletBinding()]
param(
    # The dump to check; unset runs -SuitePath's RendersNonBlankFrame to write one.
    [string]$TimingPath,
    # A host entry under "hosts" in the budget file.
    [Parameter(Mandatory)] [string]$HostProfile,
    [string]$BudgetPath = [IO.Path]::Combine($PSScriptRoot, '..', '..', 'Test', 'perf', 'baselines', 'gpu-timing-budgets.json'),
    [string]$SuitePath = [IO.Path]::Combine($PSScriptRoot, '..', '..', 'build-clangcl-profile', 'commitTestSuite.exe')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Read-JsonFile {
    param([Parameter(Mandatory)] [string]$Path, [Parameter(Mandatory)] [string]$Label)
    if (-not (Test-Path -LiteralPath $Path)) { throw "$Label not found: $Path" }
    return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json -AsHashtable
}

# An absent budget section is no budget; a pass named in one must be in the dump, or the budget checks nothing.
function Test-PassBudget {
    param(
        [Parameter(Mandatory)] [hashtable]$Measured,
        [hashtable]$Limits,
        [Parameter(Mandatory)] [string]$Unit
    )
    $failures = 0
    $checked = 0
    if ($null -eq $Limits) { return @{ Failures = 0; Checked = 0 } }
    foreach ($pass in ($Limits.Keys | Sort-Object)) {
        $limit = [double]$Limits[$pass]
        if (-not $Measured.ContainsKey($pass)) {
            Write-Host ("FAIL {0,-16} budgeted ({1}) but absent from the dump" -f $pass, $Unit) -ForegroundColor Red
            $failures++
            continue
        }
        $value = [double]$Measured[$pass]
        $checked++
        if ($value -gt $limit) {
            Write-Host ("FAIL {0,-16} {1,10:N4} {2} > budget {3:N4}" -f $pass, $value, $Unit, $limit) -ForegroundColor Red
            $failures++
        } else {
            Write-Host ("OK   {0,-16} {1,10:N4} {2} <= budget {3:N4}" -f $pass, $value, $Unit, $limit)
        }
    }
    return @{ Failures = $failures; Checked = $checked }
}

$budget = Read-JsonFile -Path $BudgetPath -Label 'Budget file'
if (-not $budget.ContainsKey('hosts') -or -not $budget.hosts.ContainsKey($HostProfile)) {
    $known = if ($budget.ContainsKey('hosts')) { ($budget.hosts.Keys | Sort-Object) -join ', ' } else { '(none)' }
    Write-Host "FAIL host profile '$HostProfile' is not in $BudgetPath; known: $known" -ForegroundColor Red
    exit 1
}
$hostBudget = $budget.hosts[$HostProfile]

if (-not $TimingPath) {
    if (-not (Test-Path -LiteralPath $SuitePath)) { throw "No -TimingPath, and no suite at $SuitePath to write one." }
    $TimingPath = Join-Path ([IO.Path]::GetTempPath()) ('kataglyphis-gpu-budget-' + [guid]::NewGuid().ToString('N') + '.json')
    $env:KATAGLYPHIS_GPU_TIMING_JSON = $TimingPath
    try {
        & $SuitePath --gtest_filter=GoldenRender.RendersNonBlankFrame *> $null
        if ($LASTEXITCODE -ne 0) {
            Write-Host "FAIL $SuitePath exited with code $LASTEXITCODE" -ForegroundColor Red
            exit 1
        }
    } finally {
        $env:KATAGLYPHIS_GPU_TIMING_JSON = ''
    }
}

$dump = Read-JsonFile -Path $TimingPath -Label 'Timing dump'
# Like Compare-RendererTimings.ps1: a run that measured nothing is exit 2, never a pass.
if (-not $dump.timestamps_supported -or [int64]$dump.frames_measured -le 0 -or $dump.passes.Count -eq 0) {
    Write-Host "NOTHING CHECKED: $TimingPath holds no measured frames (timestamps supported: $($dump.timestamps_supported))." -ForegroundColor Red
    exit 2
}

$passes = @{}
foreach ($name in $dump.passes.Keys) { $passes[$name] = [double]$dump.passes[$name] }
# A nested scope (Forward inside Main) is already part of its parent's time, so the frame adds only the top level.
$nested = if ($dump.ContainsKey('nested_in') -and $null -ne $dump.nested_in) { @($dump.nested_in.Keys) } else { @() }
$frameMs = ($passes.Keys | Where-Object { $_ -notin $nested } | ForEach-Object { $passes[$_] } | Measure-Object -Sum).Sum
$shares = @{}
if ($frameMs -gt 0.0) {
    foreach ($name in $passes.Keys) { $shares[$name] = $passes[$name] / $frameMs }
}

Write-Host ("Host profile '{0}', {1} frames measured, {2:N4} ms over the top-level timed passes" -f $HostProfile, $dump.frames_measured, $frameMs)
# A Debug+ASan budget passes a Profile dump without proving anything, so the reader must see which build it fits.
Write-Host "Budgeted build: $($hostBudget['build'])"
$absolute = Test-PassBudget -Measured $passes -Limits $hostBudget['max_ms'] -Unit 'ms'
$relative = Test-PassBudget -Measured $shares -Limits $hostBudget['max_share'] -Unit 'share'

$failures = $absolute.Failures + $relative.Failures
if ($failures -gt 0) {
    Write-Host "=== GPU TIMING BUDGET FAILED ($failures over budget or missing) ===" -ForegroundColor Red
    exit 1
}
if ($absolute.Checked + $relative.Checked -eq 0) {
    Write-Host "=== GPU TIMING BUDGET: NOTHING CHECKED (host profile '$HostProfile' budgets no pass) ===" -ForegroundColor Red
    exit 2
}
Write-Host '=== GPU TIMING BUDGET PASSED ===' -ForegroundColor Green
exit 0
