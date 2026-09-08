#requires -Version 7.0

# Pester coverage for scripts/windows/Invoke-SyncValidation.ps1's pass/fail
# contract. Invokes the actual script as a child process against a canned
# log fixture via -LogFixturePath, so it exercises the real hazard-scanning
# and exit-code logic without needing a GPU or a built commitTestSuite.exe -
# the GoldenRender/Integration suites the script normally drives only run on
# a host with a GPU (see docs/gpu-golden-testing.md).
#
# NOTE: written for Pester 3.4.0 (the version installed here) - dash-less
# assertion syntax (`Should Be 0`, never Pester 5's `Should -Be 0`).

Describe 'Invoke-SyncValidation' {

  BeforeAll {
    $script:scriptPath = (Resolve-Path (Join-Path $PSScriptRoot '..\Invoke-SyncValidation.ps1')).Path
    $tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP ('sync-validation-test-' + (Get-Random))) -Force
    $script:tmpDir = $tmp.FullName
  }

  AfterAll {
    Remove-Item -LiteralPath $script:tmpDir -Recurse -Force -ErrorAction SilentlyContinue
  }

  It 'exits non-zero when the log contains a SYNC-HAZARD' {
    $logPath = Join-Path $script:tmpDir 'hazard.log'
    Set-Content -Path $logPath -Value @(
      '[ RUN      ] GoldenRender.ShadowsDarkenSomePixels'
      'VALIDATION - SYNC-HAZARD-WRITE_AFTER_WRITE(ERROR): image barrier missing'
      '[       OK ] GoldenRender.ShadowsDarkenSomePixels'
    )

    & pwsh -NoProfile -ExecutionPolicy Bypass -File $script:scriptPath -LogFixturePath $logPath | Out-Null
    $LASTEXITCODE | Should Not Be 0
  }

  It 'exits zero when the log has no SYNC-HAZARD' {
    $logPath = Join-Path $script:tmpDir 'clean.log'
    Set-Content -Path $logPath -Value @(
      '[ RUN      ] GoldenRender.ShadowsDarkenSomePixels'
      '[       OK ] GoldenRender.ShadowsDarkenSomePixels'
    )

    & pwsh -NoProfile -ExecutionPolicy Bypass -File $script:scriptPath -LogFixturePath $logPath | Out-Null
    $LASTEXITCODE | Should Be 0
  }

  It 'throws when the fixture log does not exist' {
    $missingPath = Join-Path $script:tmpDir 'does-not-exist.log'
    & pwsh -NoProfile -ExecutionPolicy Bypass -File $script:scriptPath -LogFixturePath $missingPath | Out-Null
    $LASTEXITCODE | Should Not Be 0
  }
}
