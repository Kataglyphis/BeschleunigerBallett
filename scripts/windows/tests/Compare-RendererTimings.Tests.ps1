#requires -Version 7.0

# Pester coverage for scripts/Compare-RendererTimings.ps1's expected-pass
# derivation and gate logic - the script previously hard-coded a Rust pass
# named 'Post' that TimedPass has never had (a guaranteed FAIL every run)
# while silently never checking three real passes (Bloom, Histogram,
# ExposureReduce). Invokes the real script as a child process, the same
# pattern Compare-PerfBaseline.Tests.ps1 uses, plus small fixture source
# files for Get-ExpectedPassNames so the parser is tested against known input
# rather than the live (and evolving) engine sources.
#
# NOTE: written for Pester 3.4.0 (the version installed here) - dash-less
# assertion syntax (`Should Be 0`, never Pester 5's `Should -Be 0`).

Describe 'Compare-RendererTimings' {

  BeforeAll {
    $script:scriptPath = (Resolve-Path (Join-Path $PSScriptRoot '..\..\Compare-RendererTimings.ps1')).Path
    $tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP ('renderer-timings-test-' + (Get-Random))) -Force
    $script:tmpDir = $tmp.FullName

    $script:cppFixturePath = Join-Path $script:tmpDir 'GUIRendererSharedVars.ixx'
    @'
inline constexpr const char *GPU_TIMED_PASS_EXPORT_NAMES[GPU_TIMED_PASS_COUNT] = {
    "Clouds",
    "ShadowCascades",
    "Main",
    "Sky",
    "Post",
};
'@ | Set-Content -Path $script:cppFixturePath

    # std::array form (post GPU_TIMED_PASS_COUNT-table-derivation refactor) -
    # the parser must accept both spellings.
    $script:cppArrayFixturePath = Join-Path $script:tmpDir 'GUIRendererSharedVarsArray.ixx'
    @'
inline constexpr std::array GPU_TIMED_PASS_EXPORT_NAMES = std::to_array<const char *>({
  "Clouds",
  "ShadowCascades",
  "Main",
  "Sky",
  "Post",
});
'@ | Set-Content -Path $script:cppArrayFixturePath

    $script:rustFixturePath = Join-Path $script:tmpDir 'gpu_timing.rs'
    @'
impl TimedPass {
    pub fn name(self) -> &'static str {
        match self {
            TimedPass::ShadowCascades => "ShadowCascades",
            TimedPass::Forward => "Forward",
            TimedPass::OcclusionCull => "OcclusionCull",
            TimedPass::Bloom => "Bloom",
            TimedPass::Ssao => "Ssao",
            TimedPass::Histogram => "Histogram",
            TimedPass::ExposureReduce => "ExposureReduce",
            TimedPass::Tonemap => "Tonemap",
        }
    }
}
'@ | Set-Content -Path $script:rustFixturePath

    $script:emptyFixturePath = Join-Path $script:tmpDir 'empty.rs'
    'this file has no TimedPass::name() in it at all' | Set-Content -Path $script:emptyFixturePath
  }

  AfterAll {
    Remove-Item -LiteralPath $script:tmpDir -Recurse -Force -ErrorAction SilentlyContinue
  }

  function Invoke-PrintExpectedPasses {
    param([string]$CppPassSourcePath, [string]$RustPassSourcePath)
    $scriptArgs = @(
      '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $script:scriptPath,
      '-PrintExpectedPasses',
      '-CppPassSourcePath', $CppPassSourcePath,
      '-RustPassSourcePath', $RustPassSourcePath
    )
    $out = & pwsh @scriptArgs 2>&1
    return @{ ExitCode = $LASTEXITCODE; Output = ($out -join "`n") }
  }

  It 'derives the five C++ pass names from GPU_TIMED_PASS_EXPORT_NAMES' {
    $result = Invoke-PrintExpectedPasses -CppPassSourcePath $script:cppFixturePath -RustPassSourcePath $script:rustFixturePath
    $result.ExitCode | Should Be 0
    $json = $result.Output | ConvertFrom-Json
    ($json.Cpp -join ',') | Should Be 'Clouds,ShadowCascades,Main,Sky,Post'
  }

  It 'derives the five C++ pass names from the std::array GPU_TIMED_PASS_EXPORT_NAMES form' {
    $result = Invoke-PrintExpectedPasses -CppPassSourcePath $script:cppArrayFixturePath -RustPassSourcePath $script:rustFixturePath
    $result.ExitCode | Should Be 0
    $json = $result.Output | ConvertFrom-Json
    ($json.Cpp -join ',') | Should Be 'Clouds,ShadowCascades,Main,Sky,Post'
  }

  It 'derives Rust pass names that contain Tonemap and do not contain Post' {
    $result = Invoke-PrintExpectedPasses -CppPassSourcePath $script:cppFixturePath -RustPassSourcePath $script:rustFixturePath
    $result.ExitCode | Should Be 0
    $json = $result.Output | ConvertFrom-Json
    ($json.Rust -contains 'Tonemap') | Should Be $true
    ($json.Rust -contains 'Post') | Should Be $false
  }

  It 'excludes OcclusionCull from the derived Rust required list' {
    $result = Invoke-PrintExpectedPasses -CppPassSourcePath $script:cppFixturePath -RustPassSourcePath $script:rustFixturePath
    $result.ExitCode | Should Be 0
    $json = $result.Output | ConvertFrom-Json
    ($json.Rust -contains 'OcclusionCull') | Should Be $false
  }

  It 'exits non-zero when a source file parses to zero pass names' {
    $result = Invoke-PrintExpectedPasses -CppPassSourcePath $script:cppFixturePath -RustPassSourcePath $script:emptyFixturePath
    $result.ExitCode | Should Not Be 0
  }

  It 'passes -ValidationOnly when JSON is missing an optional pass but has every required one' {
    $outDir = Join-Path $script:tmpDir 'validation-ok'
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null
    # Every required Rust pass present, OcclusionCull (optional) absent.
    @'
{
  "frames_measured": 96,
  "timestamps_supported": true,
  "passes": {
    "ShadowCascades": 0.1,
    "Forward": 0.2,
    "Bloom": 0.3,
    "Ssao": 0.4,
    "Histogram": 0.5,
    "ExposureReduce": 0.6,
    "Tonemap": 0.7
  }
}
'@ | Set-Content -Path (Join-Path $outDir 'gpu-timings-rust.json')

    $scriptArgs = @(
      '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $script:scriptPath,
      '-ValidationOnly', '-OutDir', $outDir,
      '-CppPassSourcePath', $script:cppFixturePath,
      '-RustPassSourcePath', $script:rustFixturePath
    )
    & pwsh @scriptArgs | Out-Null
    $LASTEXITCODE | Should Be 0
  }

  It 'fails -ValidationOnly when JSON is missing a required pass' {
    $outDir = Join-Path $script:tmpDir 'validation-fail'
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null
    # Missing "Bloom", a required pass.
    @'
{
  "frames_measured": 96,
  "timestamps_supported": true,
  "passes": {
    "ShadowCascades": 0.1,
    "Forward": 0.2,
    "Ssao": 0.4,
    "Histogram": 0.5,
    "ExposureReduce": 0.6,
    "Tonemap": 0.7
  }
}
'@ | Set-Content -Path (Join-Path $outDir 'gpu-timings-rust.json')

    $scriptArgs = @(
      '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $script:scriptPath,
      '-ValidationOnly', '-OutDir', $outDir,
      '-CppPassSourcePath', $script:cppFixturePath,
      '-RustPassSourcePath', $script:rustFixturePath
    )
    & pwsh @scriptArgs | Out-Null
    $LASTEXITCODE | Should Not Be 0
  }
}
