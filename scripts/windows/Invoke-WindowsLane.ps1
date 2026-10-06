#requires -Version 7.0
<#
.SYNOPSIS
  The Windows x64 lane's container half, run by windows-x64.yml and by a local container run.
.DESCRIPTION
  Builds clangcl-debug, clangcl-profile and clangcl-release, then runs the CPU suites, the compile suite,
  the perf suite, fuzz seed corpora and renderer comparisons; every check runs and the lane fails if any
  did. Secrets arrive as environment. The GPU suites run after it, on the runner host, from the Release
  suite -StageTests leaves in dist\windows-x64-tests (windows-x64.yml's host-command).
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$workspace = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location -LiteralPath $workspace
$debugDir = Join-Path $workspace 'build-clangcl-debug'
$profileDir = Join-Path $workspace 'build-clangcl-profile'
$failed = [System.Collections.Generic.List[string]]::new()

# -SkipPerfTests: the perf suite runs below and fails on a missing exe. No -SkipFormat: this lane gates clang-format.
$buildArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'Build-Windows.ps1'),
  '-Configurations', 'clangcl-debug,clangcl-profile,clangcl-release', '-SkipTests', '-SkipPerfTests', '-StageTests')
& pwsh @buildArgs
if ($LASTEXITCODE -ne 0) { Write-Host "::error::Build-Windows.ps1 exited $LASTEXITCODE"; exit 1 }

# Excluded by name: the image has a Vulkan loader but no device, so these abort; the host step runs them. BuildIntegrity parses this.
$gpuOnlySuites = @(
  'GoldenRender.*'
  'Integration.*'
) -join ':'
$suite = Join-Path $debugDir 'commitTestSuite.exe'
if (-not (Test-Path -LiteralPath $suite)) { $failed.Add("missing $suite") } else {
  Write-Host "running $suite ($((Get-Item -LiteralPath $suite).Length) bytes), excluding $gpuOnlySuites"
  & $suite "--gtest_filter=-$gpuOnlySuites"
  if ($LASTEXITCODE -ne 0) { $failed.Add("commitTestSuite.exe exited $LASTEXITCODE") }
}

$compileSuite = Join-Path $debugDir 'compileTestSuite.exe'
if (-not (Test-Path -LiteralPath $compileSuite)) { $failed.Add("missing $compileSuite") } else {
  & $compileSuite
  if ($LASTEXITCODE -ne 0) { $failed.Add("compileTestSuite.exe exited $LASTEXITCODE") }
}

# Functional, not timing: every benchmark must run without error, from the repo root as ctest's perf_suite_runs does.
$perfSuite = Join-Path $profileDir 'perfTestSuite.exe'
$perfJson = Join-Path $profileDir 'perf-results.json'
if (-not (Test-Path -LiteralPath $perfSuite)) { $failed.Add("missing $perfSuite") } else {
  & $perfSuite --benchmark_min_time=0.05s "--benchmark_out=$perfJson" --benchmark_out_format=json
  if ($LASTEXITCODE -ne 0) { $failed.Add("perfTestSuite.exe exited $LASTEXITCODE") } else {
    $runs = @((Get-Content -LiteralPath $perfJson -Raw | ConvertFrom-Json).benchmarks)
    if ($runs.Count -eq 0) { $failed.Add('perfTestSuite.exe ran no benchmark') }
    foreach ($run in @($runs | Where-Object { $_.PSObject.Properties['error_occurred'] -and $_.error_occurred })) {
      $failed.Add("benchmark $($run.name) failed: $($run.error_message)")
    }
  }
}

# Seed corpora (CPU-only); BuildIntegrity parses this one-line list; ten minutes makes a hang fail, not stall.
foreach ($t in @('first_fuzz_test','example_fuzz_test','obj_parsing_fuzz_test','gltf_parsing_fuzz_test','scene_config_fuzz_test','shader_file_reader_fuzz_test','texture_loading_fuzz_test')) {
  $exe = Join-Path $debugDir "$t.exe"
  if (-not (Test-Path -LiteralPath $exe)) { $failed.Add("missing $exe (fuzz targets are Debug + clang-cl only)"); continue }
  Write-Host "--- $t"
  $p = Start-Process -FilePath $exe -NoNewWindow -PassThru
  # Read once now: without it ExitCode stays empty for a process started this way.
  $null = $p.Handle
  if (-not $p.WaitForExit(600000)) { $p.Kill($true); $failed.Add("fuzz target $t still ran after ten minutes"); continue }
  if ($p.ExitCode -ne 0) { $failed.Add("fuzz target $t exited $($p.ExitCode)") } else { Write-Host "--- $t OK" }
}

# Hosted runners have no GPU: schema and pass names only, unless KATAGLYPHIS_CI_HAS_GPU=1.
$validation = @(if ($env:KATAGLYPHIS_CI_HAS_GPU -ne '1') { '-ValidationOnly' })
Write-Host "GPU available: $($validation.Count -eq 0)"
# Exit 2 means nothing was checked: a warning without a GPU, never a pass, and fatal with one.
foreach ($comparison in 'Compare-RendererTimings', 'Compare-RendererPixels') {
  & pwsh -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "$comparison.ps1") -RepoRoot $workspace @validation
  $code = $LASTEXITCODE
  if ($code -eq 2 -and $validation.Count -gt 0) {
    Write-Host "::warning::$comparison checked NOTHING - this runner has no GPU, so nothing was captured (script exit 2). That is not a pass, only an absence of evidence."
  } elseif ($code -ne 0) {
    $failed.Add("$comparison.ps1 exited $code")
  }
}

if ($failed.Count -gt 0) {
  foreach ($f in $failed) { Write-Host "::error::$f" }
  exit 1
}
Write-Host 'Windows x64 lane: build, CPU, compile and perf suites, fuzz seeds and renderer comparisons passed.'
