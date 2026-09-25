#requires -Version 7.0
<#
.SYNOPSIS
  The Windows x64 lane's container half: what windows-x64.yml runs through the hub's
  container-ci-windows.yml, and what a local container run executes too.
.DESCRIPTION
  Build-Windows.ps1 builds clangcl-debug (the CPU tests and the fuzz targets) and clangcl-release
  (the product, in dist\windows-x64). Then the CPU-only test suites, every fuzz target's seed
  corpus, and the renderer timing and pixel comparisons, validation-only unless
  KATAGLYPHIS_CI_HAS_GPU is 1. Every check after the build runs, and the lane fails if any did.
  The WebDAV credentials and the signing password arrive as environment (WEBDAV_*,
  REMOTE_BASE_PATH, MSIX_CERT_PASSWORD), which Build-Windows.ps1 reads itself.
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

# The CPU suites. The GPU-only ones stay out by NAME: a GPU-less runner still has the Vulkan loader,
# so SKIP_WITHOUT_GPU can believe a device exists, and device creation then aborts the process.
# BuildIntegrity.WindowsCiExcludesExactlyTheGpuSuites parses this array, one entry per line, from
# its opening line to its joining line; a comment must not quote either, the parser takes the first.
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

# Each fuzz target's seed corpus in unit-test mode: CPU-only and quick, and a seed once found a real
# terminate-on-throw bug. BuildIntegrity's fuzz-list tests parse the target list below from its one
# line; a comment must not quote that line's opening text. A target gets ten minutes: a seed once
# hung for 68 inside readTextFile (FileReader.ixx), and a hang must fail the lane, not stall it.
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

# A hosted runner has no GPU, so the comparisons check only their schema and pass names unless
# KATAGLYPHIS_CI_HAS_GPU=1 (a self-hosted runner with an adapter) asks for the real one.
$validation = @(if ($env:KATAGLYPHIS_CI_HAS_GPU -ne '1') { '-ValidationOnly' })
Write-Host "GPU available: $($validation.Count -eq 0)"
& pwsh -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Compare-RendererTimings.ps1') -RepoRoot $workspace @validation
if ($LASTEXITCODE -ne 0) { $failed.Add("Compare-RendererTimings.ps1 exited $LASTEXITCODE") }
# Exit 2 is the pixel script's "nothing was checked", certain without a GPU: a warning then, never a
# pass, and fatal when a GPU should have produced frames (Compare-RendererPixels.Tests.ps1 pins it).
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
