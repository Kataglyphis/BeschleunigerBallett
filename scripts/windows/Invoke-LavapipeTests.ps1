#requires -Version 7.0
<#
.SYNOPSIS
  Runs commitTestSuite tests on Mesa's lavapipe, one process per test, on a Windows host without a GPU driver.
.DESCRIPTION
  Provisions a pinned, SHA256-checked lavapipe and Khronos loader for the host's arch, registers the ICD, and runs every
  test -Filter selects from -RepoRoot, one process each as ctest runs them on Linux. Prints one
  `TESTS: passed=<n> failed=<n> skipped=<n>` line; a failure, a skip or no pass at all exits 1. With the validation layer
  -StageTests puts in vulkan-layers\ beside the suite (x64), the Release suite validates, sync included, and a test that
  logs a VUID or SYNC-HAZARD fails. The Windows x64 lane's host step and the arm64 lane's GPU job call it (AGENTS.md
  § What CI runs, docs/gpu-golden-testing.md).
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory)][string]$Suite,
  [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
  # The GUI input sweep is excluded for time, as on Linux.
  [string]$Filter = 'GoldenRender.*:Integration.*:-GoldenRender.GuiInputSweepNeverCrashesOrLosesTheDevice',
  [string]$ToolDir = (Join-Path ($env:RUNNER_TEMP ?? [IO.Path]::GetTempPath()) 'lavapipe'),
  # ctest's per-test limit on Linux.
  [int]$TimeoutSeconds = 1500
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# Exit codes are read below; a failing test must be counted, not thrown.
$PSNativeCommandUseErrorActionPreference = $false

# The pins, the downloads and the ICD registration have one owner: the hub's host provisioner.
$installer = Join-Path $RepoRoot 'third_party\ANTfrastructure\windows\scripts\host\Install-LavapipeHost.ps1'
if (-not (Test-Path -LiteralPath $installer)) { throw "Required script not found: $installer (run: git submodule update --init --recursive third_party/ANTfrastructure)" }
$tools = & $installer -ToolDir $ToolDir
$suitePath = (Resolve-Path -LiteralPath $Suite).Path

function Get-TestName([string[]]$Listing) {
  # --gtest_list_tests prints "Suite." then indented names; DISABLED_ tests are listed but never run.
  $suiteName = ''
  foreach ($line in $Listing) {
    $text = ($line -replace '#.*$', '').TrimEnd()
    if ($text -match '^(\S+)\.$') { $suiteName = $Matches[1]; continue }
    if ($text -match '^\s+(\S+)$' -and $suiteName -and -not $Matches[1].StartsWith('DISABLED_')) { "$suiteName.$($Matches[1])" }
  }
}

function Get-TestOutcome([string]$Json, [int]$ExitCode) {
  # Passed, failed or skipped from one test's gtest JSON; a crash leaves no JSON and a non-zero exit.
  if ($ExitCode -ne 0) { return 'failed' }
  if (-not (Test-Path -LiteralPath $Json)) { return 'failed' }
  $case = (Get-Content -LiteralPath $Json -Raw | ConvertFrom-Json).testsuites[0].testsuite[0]
  if ($case.PSObject.Properties['failures'] -and @($case.failures).Count -gt 0) { return 'failed' }
  if ($case.result -eq 'SKIPPED') { return 'skipped' }
  return 'passed'
}

# Beside the suite, so it wins over any vulkan-1.dll the image put in System32.
Copy-Item -LiteralPath (Join-Path $tools.LoaderDir 'vulkan-1.dll') -Destination (Split-Path $suitePath) -Force

# Windows clamps a new window to the desktop, and the goldens frame a 1200x768 view; a hosted runner's desktop is 1024x768.
Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;
public static class KataglyphisDesktop {
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  struct DevMode {
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string DeviceName;
    public short SpecVersion, DriverVersion, Size, DriverExtra;
    public int Fields, PositionX, PositionY, Orientation, FixedOutput;
    public short Color, Duplex, YResolution, TTOption, Collate;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string FormName;
    public short LogPixels;
    public int BitsPerPel, PelsWidth, PelsHeight, DisplayFlags, DisplayFrequency, IcmMethod, IcmIntent, MediaType, DitherType, Reserved1, Reserved2, PanningWidth, PanningHeight;
  }
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern bool EnumDisplaySettings(string device, int mode, ref DevMode devMode);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int ChangeDisplaySettings(ref DevMode devMode, int flags);
  [DllImport("user32.dll")] static extern int GetSystemMetrics(int index);
  [DllImport("user32.dll")] static extern bool SetProcessDPIAware();
  static KataglyphisDesktop() { SetProcessDPIAware(); }
  public static string Current() { return GetSystemMetrics(0) + "x" + GetSystemMetrics(1); }
  public static bool Fits(int width, int height) { return GetSystemMetrics(0) >= width && GetSystemMetrics(1) >= height; }
  public static int Resize(int width, int height) {
    var mode = new DevMode();
    mode.Size = (short)Marshal.SizeOf(typeof(DevMode));
    if (!EnumDisplaySettings(null, -1, ref mode)) { return -100; }
    mode.PelsWidth = width;
    mode.PelsHeight = height;
    mode.Fields = 0x80000 | 0x100000;
    return ChangeDisplaySettings(ref mode, 0);
  }
}
'@
Write-Host "desktop: $([KataglyphisDesktop]::Current())"
# Only on a CI runner: on a workstation this would change the owner's display mode.
foreach ($size in @(1920, 1080), @(1600, 900), @(1280, 1024)) {
  if ($env:GITHUB_ACTIONS -ne 'true' -or [KataglyphisDesktop]::Fits(1280, 900)) { break }
  Write-Host "ChangeDisplaySettings $($size -join 'x'): $([KataglyphisDesktop]::Resize($size[0], $size[1])) -> $([KataglyphisDesktop]::Current())"
}
if (-not [KataglyphisDesktop]::Fits(1280, 900)) { Write-Warning "the desktop stays $([KataglyphisDesktop]::Current()); windows will be clamped and framing-sensitive goldens may fail" }

$summary = @(& (Join-Path $tools.LoaderDir 'vulkaninfo.exe') --summary 2>&1 | ForEach-Object { "$_" })
$summary | Write-Host
if (-not ($summary -match 'llvmpipe')) { throw 'vulkaninfo lists no llvmpipe device; the ICD registration did not take' }

# The Release suite validates only with the layer Build-Windows.ps1 -StageTests puts beside it, sync validation included.
$layerDir = Join-Path (Split-Path $suitePath) 'vulkan-layers'
$validating = Test-Path -LiteralPath (Join-Path $layerDir 'VkLayer_khronos_validation.json')
if ($validating) {
  $env:VK_ADD_LAYER_PATH = $layerDir
  $env:KATAGLYPHIS_VULKAN_VALIDATION = '1'
  $env:VK_KHRONOS_VALIDATION_VALIDATE_SYNC = '1'
}
Write-Host "validation layer: $(if ($validating) { "$layerDir, sync validation on" } else { 'none staged; the suites run unvalidated' })"

$listing = @(& $suitePath --gtest_list_tests "--gtest_filter=$Filter" 2>&1 | ForEach-Object { "$_" })
if ($LASTEXITCODE -ne 0) { $listing | Write-Host; throw "listing the tests exited $LASTEXITCODE" }
$tests = @(Get-TestName -Listing $listing)
if ($tests.Count -eq 0) { throw "-Filter $Filter selects no test" }

$results = Join-Path $ToolDir 'results'
New-Item -ItemType Directory -Force -Path $results | Out-Null
$counts = @{ passed = 0; failed = 0; skipped = 0 }
$notPassed = [System.Collections.Generic.List[string]]::new()
Push-Location -LiteralPath $RepoRoot
try {
  foreach ($test in $tests) {
    $fileName = ($test -replace '[\\/:*?<>|]', '_') + '.json'
    $json = Join-Path $results $fileName
    Write-Host "=== $test"
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $testArgs = @("--gtest_filter=$test", "--gtest_output=json:`"$json`"")
    $stdout = [IO.Path]::ChangeExtension($json, '.out.txt')
    $stderr = [IO.Path]::ChangeExtension($json, '.err.txt')
    $process = Start-Process -FilePath $suitePath -ArgumentList $testArgs -NoNewWindow -PassThru `
      -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    # Read once now: without it ExitCode stays empty for a process started this way.
    $null = $process.Handle
    if ($process.WaitForExit($TimeoutSeconds * 1000)) {
      $outcome = Get-TestOutcome -Json $json -ExitCode $process.ExitCode
    } else {
      $process.Kill($true)
      $outcome = 'failed'
    }
    $output = @(foreach ($log in $stdout, $stderr) { if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log } })
    $output | Write-Host
    # Most tests ignore validation messages, so the captured output is where a VUID or a sync hazard surfaces.
    $findings = @($output | Where-Object { $_ -match 'VUID-|SYNC-HAZARD' })
    if ($findings.Count -gt 0 -and $outcome -eq 'passed') {
      Write-Host "::error::$test logged $($findings.Count) validation message(s), first: $($findings[0])"
      $outcome = 'failed'
    }
    $counts[$outcome]++
    Write-Host "=== $test $outcome ($([int]$clock.Elapsed.TotalSeconds) s)"
    if ($outcome -ne 'passed') { $notPassed.Add("$test $outcome") }
  }
} finally {
  Pop-Location
}

$line = "TESTS: passed=$($counts.passed) failed=$($counts.failed) skipped=$($counts.skipped)"
Write-Output $line
if ($env:GITHUB_STEP_SUMMARY) {
  "### $(Split-Path $suitePath -Leaf) on lavapipe ($env:PROCESSOR_ARCHITECTURE)`n`n$line`n`n$(@($notPassed | ForEach-Object { "- $_" }) -join "`n")" |
    Out-File -Append -Encoding utf8 -FilePath $env:GITHUB_STEP_SUMMARY
}
foreach ($entry in $notPassed) { Write-Host "::error::$entry" }
# A skip is a test that did not run, which is what this job exists to prevent.
if ($counts.failed -gt 0 -or $counts.skipped -gt 0 -or $counts.passed -eq 0) { exit 1 }
exit 0
