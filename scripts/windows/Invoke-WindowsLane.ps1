#requires -Version 7.0
<#
.SYNOPSIS
  The Windows x64 lane's container half, run by windows-x64.yml and by a local container run.
.DESCRIPTION
  Builds clangcl-debug and clangcl-release, then runs the CPU suites, fuzz seed corpora and renderer
  comparisons; every check runs and the lane fails if any did. Secrets arrive as environment.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$workspace = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location -LiteralPath $workspace
$debugDir = Join-Path $workspace 'build-clangcl-debug'
$failed = [System.Collections.Generic.List[string]]::new()

# clangcl-profile is not built: nothing here consumes it, and -SkipPerfTests skips its one purpose.
$buildArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'Build-Windows.ps1'),
  '-Configurations', 'clangcl-debug,clangcl-release', '-SkipFormat', '-SkipTidy', '-SkipTests', '-SkipPerfTests')
& pwsh @buildArgs
if ($LASTEXITCODE -ne 0) { Write-Host "::error::Build-Windows.ps1 exited $LASTEXITCODE"; exit 1 }

# Excluded by name: a GPU-less runner still has the Vulkan loader and aborts creating a device. BuildIntegrity parses this.
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
& pwsh -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Compare-RendererTimings.ps1') -RepoRoot $workspace @validation
if ($LASTEXITCODE -ne 0) { $failed.Add("Compare-RendererTimings.ps1 exited $LASTEXITCODE") }
# Exit 2 means nothing was checked: a warning without a GPU, never a pass, and fatal with one.
& pwsh -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Compare-RendererPixels.ps1') -RepoRoot $workspace @validation
$pixels = $LASTEXITCODE
if ($pixels -eq 2 -and $validation.Count -gt 0) {
  Write-Host '::warning::Pixel comparison checked NOTHING - this runner has no GPU, so no frames were captured (script exit 2). That is not a pass, only an absence of evidence.'
} elseif ($pixels -ne 0) {
  $failed.Add("Compare-RendererPixels.ps1 exited $pixels")
}

if ($failed.Count -gt 0) {
  foreach ($f in $failed) { Write-Host "::error::$f" }
  exit 1
}
Write-Host 'Windows x64 lane: build, CPU suites, fuzz seeds and renderer comparisons passed.'
