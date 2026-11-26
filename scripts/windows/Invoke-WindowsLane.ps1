#requires -Version 7.0
<#
.SYNOPSIS
  The Windows x64 lane's container half, run by windows-x64.yml and by a local container run.
.DESCRIPTION
  Builds clangcl-debug, clangcl-profile and clangcl-release, then runs the CPU suites, the compile suite,
  the perf suite, fuzz seed corpora and renderer comparisons, and starts the portable bundle and the
  installed MSI from a foreign folder with --check_resources; every check runs and the lane fails if any
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
$engineExe = 'BeschleunigerBallett.exe'

# A shortcut starts the program in any folder (1.7.0's MSI: bin\, and no shader found), so the check runs from %TEMP%.
function Test-InstalledTree {
  param([Parameter(Mandatory)][string]$Root, [Parameter(Mandatory)][string]$Label)
  $exe = Join-Path $Root "bin\$engineExe"
  if (-not (Test-Path -LiteralPath $exe)) { return "$Label has no $exe" }
  $out = Join-Path $env:TEMP "check-resources-$Label.txt"
  # Start-Process -Wait: the Release program is a GUI one, which PowerShell would not wait for.
  $p = Start-Process -FilePath $exe -ArgumentList '--check_resources' -WorkingDirectory $env:TEMP -Wait -PassThru -NoNewWindow -RedirectStandardOutput $out
  Get-Content -LiteralPath $out | Write-Host
  if ($p.ExitCode -ne 0) { return "${Label}: $engineExe --check_resources exited $($p.ExitCode)" }
  return $null
}

# The MSI as a user gets it, installed and started from elsewhere, then removed again.
function Test-MsiInstall {
  param([Parameter(Mandatory)][string]$DistDir)
  $msi = @(Get-ChildItem -Path (Join-Path $DistDir 'packages') -Filter '*.msi' -File -ErrorAction SilentlyContinue)
  if ($msi.Count -ne 1) { return "expected one .msi in $DistDir\packages, found $($msi.Count)" }
  $dir = Join-Path $env:TEMP 'msi-install'
  $log = Join-Path $env:TEMP 'msi-install.log'
  $p = Start-Process msiexec.exe -ArgumentList '/i', "`"$($msi[0].FullName)`"", '/qn', '/norestart', "INSTALL_ROOT=`"$dir`"", '/l*v', "`"$log`"" -Wait -PassThru
  if ($p.ExitCode -notin 0, 3010) { return "msiexec /i $($msi[0].Name) exited $($p.ExitCode); see $log" }
  try { return Test-InstalledTree -Root $dir -Label 'msi' }
  finally { $null = Start-Process msiexec.exe -ArgumentList '/x', "`"$($msi[0].FullName)`"", '/qn', '/norestart' -Wait -PassThru }
}

# -SkipPerfTests: the perf suite runs below and fails on a missing exe. No -SkipFormat: this lane gates clang-format.
# -WarningCensus: a red census fails the build script after every build and package, so the suites below do not run.
$buildArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'Build-Windows.ps1'),
  '-Configurations', 'clangcl-debug,clangcl-profile,clangcl-release', '-SkipTests', '-SkipPerfTests', '-StageTests', '-WarningCensus')
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
foreach ($t in @('first_fuzz_test','example_fuzz_test','scene_config_fuzz_test','shader_file_reader_fuzz_test','texture_loading_fuzz_test')) {
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

$distDir = Join-Path $workspace 'dist\windows-x64'
foreach ($problem in @((Test-InstalledTree -Root (Join-Path $distDir 'bundle') -Label 'bundle'), (Test-MsiInstall -DistDir $distDir))) {
  if ($problem) { $failed.Add($problem) }
}

if ($failed.Count -gt 0) {
  foreach ($f in $failed) { Write-Host "::error::$f" }
  exit 1
}
Write-Host 'Windows x64 lane: build, CPU, compile and perf suites, fuzz seeds, renderer comparisons and the installed trees passed.'
