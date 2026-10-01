#requires -Version 7.0
<#
.SYNOPSIS
  Runs commitTestSuite tests on Mesa's lavapipe, one process per test, on a Windows host without a GPU driver.
.DESCRIPTION
  Provisions a pinned, SHA256-checked lavapipe and Khronos loader for the host's arch, registers the ICD, and runs every
  test -Filter selects from -RepoRoot, one process each as ctest runs them on Linux. Prints one
  `TESTS: passed=<n> failed=<n> skipped=<n>` line; a failure, a skip or no pass at all exits 1. The Windows x64 lane's
  host step and the arm64 lane's GPU job call it (AGENTS.md § What CI runs, docs/gpu-golden-testing.md).
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

. (Join-Path $PSScriptRoot 'Resolve-BuildModule.ps1')
Import-BuildModule @('WindowsScripts.Shared')

# One Mesa for both arches (mmozeiko/build-mesa, the only lavapipe built for arm64 Windows) and LunarG's loader beside it.
$pins = @{
  AMD64 = @{
    Driver = 'https://github.com/mmozeiko/build-mesa/releases/download/26.2.3/mesa-lavapipe-x64-26.2.3.7z'
    DriverSha256 = '316831c064a4b627d63bec066822b01b139bb13a009cbcf8fd4a745ca21e83d0'
    Icd = 'lvp_icd.x86_64.json'
    Loader = 'https://sdk.lunarg.com/sdk/download/1.4.357.0/windows/VulkanRT-X64-1.4.357.0-Components.zip'
    LoaderSha256 = 'a14672efed15aafc7f5a16572d35cd3a3416eadf670aeee3cdf50ee32d5fbf83'
    LoaderDir = 'VulkanRT-X64-1.4.357.0-Components\x64'
  }
  ARM64 = @{
    Driver = 'https://github.com/mmozeiko/build-mesa/releases/download/26.2.3/mesa-lavapipe-arm64-26.2.3.7z'
    DriverSha256 = '1aa5f0454c1f9d47f1f15a001e7f2a0da01774cfe4b8e1f00c21478de6cd1529'
    Icd = 'lvp_icd.aarch64.json'
    Loader = 'https://sdk.lunarg.com/sdk/download/1.4.357.0/warm/VulkanRT-ARM64-1.4.357.0-Components.zip'
    LoaderSha256 = '0a51a619525e0c7a156125c4f80c4f591c494cef9ff59dc4481735779a9a280c'
    LoaderDir = 'VulkanRT-ARM64-1.4.357.0-Components'
  }
}
$pin = $pins[$env:PROCESSOR_ARCHITECTURE]
if (-not $pin) { throw "no lavapipe pin for $env:PROCESSOR_ARCHITECTURE" }
$suitePath = (Resolve-Path -LiteralPath $Suite).Path

function Install-Lavapipe {
  # Downloads and unpacks both pins; returns the ICD manifest and the loader directory.
  $lavapipeDir = Join-Path $ToolDir 'lavapipe'
  $archive = Join-Path $ToolDir (Split-Path $pin.Driver -Leaf)
  Invoke-DownloadWithRetry -Url $pin.Driver -DestinationPath $archive -ExpectedSha256 $pin.DriverSha256 -Description 'lavapipe'
  $sevenZip = @((Get-Command 7z -ErrorAction SilentlyContinue | ForEach-Object Source), "$env:ProgramFiles\7-Zip\7z.exe") |
    Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
  if (-not $sevenZip) { throw '7-Zip is needed to unpack lavapipe, and neither 7z on PATH nor Program Files\7-Zip has it' }
  & $sevenZip x -y "-o$lavapipeDir" $archive | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "7-Zip exited $LASTEXITCODE unpacking $archive" }

  $zip = Join-Path $ToolDir (Split-Path $pin.Loader -Leaf)
  Invoke-DownloadWithRetry -Url $pin.Loader -DestinationPath $zip -ExpectedSha256 $pin.LoaderSha256 -Description 'Vulkan loader' -ExpectSignature PK
  Expand-Archive -LiteralPath $zip -DestinationPath $ToolDir -Force
  return @{ Icd = (Join-Path $lavapipeDir $pin.Icd); LoaderDir = (Join-Path $ToolDir $pin.LoaderDir) }
}

function Register-Lavapipe([string]$Icd) {
  # The loader ignores VK_DRIVER_FILES in an elevated process, which a hosted runner is; HKLM is what it reads then.
  $env:VK_DRIVER_FILES = $Icd
  $env:VK_LOADER_DRIVERS_SELECT = '*lvp_icd*'
  if (Test-Elevated) {
    $key = 'HKLM:\SOFTWARE\Khronos\Vulkan\Drivers'
    if (-not (Test-Path -LiteralPath $key)) { New-Item -Path $key -Force | Out-Null }
    New-ItemProperty -LiteralPath $key -Name $Icd -Value 0 -PropertyType DWord -Force | Out-Null
    # A crashing test must fail now, not wait out the timeout behind a WER dialog nobody clicks.
    New-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting' -Name DontShowUI -Value 1 -PropertyType DWord -Force | Out-Null
  }
  # The BVH sort needs 8-lane subgroups; arm64's native 128 bits give 4 (docs/gpu-golden-testing.md).
  $env:LP_NATIVE_VECTOR_WIDTH = $env:LP_NATIVE_VECTOR_WIDTH ?? '256'
}

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

$tools = Install-Lavapipe
Register-Lavapipe -Icd $tools.Icd
# Beside the suite, so it wins over any vulkan-1.dll the image put in System32.
Copy-Item -LiteralPath (Join-Path $tools.LoaderDir 'vulkan-1.dll') -Destination (Split-Path $suitePath) -Force

# GLFW keeps a window inside the desktop, and the goldens open 1200x768; a hosted runner's desktop starts at 1024x768.
Add-Type -AssemblyName System.Windows.Forms
$bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
if (($bounds.Width -lt 1280 -or $bounds.Height -lt 900) -and (Get-Command Set-DisplayResolution -ErrorAction SilentlyContinue)) {
  try { Set-DisplayResolution -Width 1920 -Height 1080 -Force } catch { Write-Warning "Set-DisplayResolution: $($_.Exception.Message)" }
}
Write-Host "desktop: $([System.Windows.Forms.Screen]::PrimaryScreen.Bounds.Size)"

$summary = @(& (Join-Path $tools.LoaderDir 'vulkaninfo.exe') --summary 2>&1 | ForEach-Object { "$_" })
$summary | Write-Host
if (-not ($summary -match 'llvmpipe')) { throw 'vulkaninfo lists no llvmpipe device; the ICD registration did not take' }

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
    $process = Start-Process -FilePath $suitePath -ArgumentList $testArgs -NoNewWindow -PassThru
    # Read once now: without it ExitCode stays empty for a process started this way.
    $null = $process.Handle
    if ($process.WaitForExit($TimeoutSeconds * 1000)) {
      $outcome = Get-TestOutcome -Json $json -ExitCode $process.ExitCode
    } else {
      $process.Kill($true)
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
