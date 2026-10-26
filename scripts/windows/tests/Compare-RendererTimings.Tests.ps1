#requires -Version 7.0

# Fixture sources, not the live engine ones, so the parser sees known input. Pester 3.4.0: `Should Be`, not `Should -Be`.
Import-Module (Join-Path $PSScriptRoot '..\..\..\third_party\ANTfrastructure\windows\scripts\tests\TestHarness.psm1') -Function ConvertTo-TestScriptAst, Get-TestAstCommands -ErrorAction Stop

Describe 'Compare-RendererTimings' {

  BeforeAll {
    $script:scriptPath = (Resolve-Path (Join-Path $PSScriptRoot '..\Compare-RendererTimings.ps1')).Path
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

    # The parser must accept the std::array spelling too.
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

  function Assert-SharedTimingConversion {
    param([string]$Text)
    $ast = ConvertTo-TestScriptAst -Text $Text
    $imports = @(Get-TestAstCommands -Ast $ast -Name 'Import-BuildModule' | Where-Object { $_.Extent.Text -match "'Compare-Renderer\.Common'" })
    $converts = @(Get-TestAstCommands -Ast $ast -Name 'Convert-DinosaursObjToGltf')
    $cargoCalls = @(Get-TestAstCommands -Ast $ast -Name 'cargo')
    $timingRuns = @($cargoCalls | Where-Object { $_.Extent.Text -match 'dump_gpu_timings' })
    $inlineConversions = @($cargoCalls | Where-Object { $_.Extent.Text -match 'obj2gltf' })
    if ($imports.Count -ne 1 -or $converts.Count -ne 1 -or $timingRuns.Count -ne 1 -or $inlineConversions.Count -ne 0) {
      throw 'Timing comparison must use the shared scene converter once and keep one timing run.'
    }
    if ($imports[0].Extent.StartOffset -ge $converts[0].Extent.StartOffset -or
        $converts[0].Extent.StartOffset -ge $timingRuns[0].Extent.StartOffset) {
      throw 'The shared converter must load and run before GPU timing.'
    }
  }

  It 'uses the shared cached scene conversion before timing and rejects an omitted call' {
    $source = Get-Content -LiteralPath $script:scriptPath -Raw
    Assert-SharedTimingConversion -Text $source
    $call = 'Convert-DinosaursObjToGltf -RepoRoot $RepoRoot -OutputGltf $dinoGltf'
    $mutant = $source.Replace($call, '')
    ($mutant -ne $source) | Should Be $true
    $failed = $false
    try { Assert-SharedTimingConversion -Text $mutant } catch { $failed = $true }
    $failed | Should Be $true
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

  It 'exits 2 under -ValidationOnly when neither JSON exists, so an empty run never reads as a pass' {
    $outDir = Join-Path $script:tmpDir 'validation-empty'
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null

    $scriptArgs = @(
      '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $script:scriptPath,
      '-ValidationOnly', '-OutDir', $outDir, '-RepoRoot', $script:tmpDir,
      '-CppPassSourcePath', $script:cppFixturePath,
      '-RustPassSourcePath', $script:rustFixturePath
    )
    & pwsh @scriptArgs | Out-Null
    $LASTEXITCODE | Should Be 2
  }
}
