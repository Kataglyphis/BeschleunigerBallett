#requires -Version 7.0

# -LogFixturePath exercises the real exit-code logic without a GPU.

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
    $LASTEXITCODE | Should -Not -Be 0
  }

  It 'exits non-zero when the log contains a VUID' {
    $logPath = Join-Path $script:tmpDir 'vuid.log'
    Set-Content -Path $logPath -Value @(
      '[ RUN      ] GoldenRender.ShadowsDarkenSomePixels'
      'Validation Error: [ VUID-vkCmdDraw-None-08600 ] pipeline layout does not match the bound descriptor sets'
      '[       OK ] GoldenRender.ShadowsDarkenSomePixels'
    )

    & pwsh -NoProfile -ExecutionPolicy Bypass -File $script:scriptPath -LogFixturePath $logPath | Out-Null
    $LASTEXITCODE | Should -Not -Be 0
  }

  It 'exits zero when the log has neither a SYNC-HAZARD nor a VUID' {
    $logPath = Join-Path $script:tmpDir 'clean.log'
    Set-Content -Path $logPath -Value @(
      '[ RUN      ] GoldenRender.ShadowsDarkenSomePixels'
      '[       OK ] GoldenRender.ShadowsDarkenSomePixels'
    )

    & pwsh -NoProfile -ExecutionPolicy Bypass -File $script:scriptPath -LogFixturePath $logPath | Out-Null
    $LASTEXITCODE | Should -Be 0
  }

  # A stand-in suite that reports whether the wrapper staged GPU-AV beside it; needs cmd, so Windows only.
  function Invoke-GpuAvProbe([string]$Name, [string[]]$Switch) {
    $probe = Join-Path $script:tmpDir 'gpuav-probe.cmd'
    Set-Content -Path $probe -Value @(
      '@echo off'
      'findstr /c:"gpuav_enable = true" "%~dp0vk_layer_settings.txt" >nul && echo GPUAV_ON || echo GPUAV_OFF'
      'exit /b 0'
    )
    $logDir = Join-Path $script:tmpDir $Name
    & pwsh -NoProfile -ExecutionPolicy Bypass -File $script:scriptPath -ExecutablePath $probe `
      -VulkanSdkBin $script:tmpDir -LogDir $logDir @Switch | Out-Null
    return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Log = (Get-Content -Path (Join-Path $logDir '*.log') -Raw) }
  }

  It 'stages GPU-AV beside the suite with -GpuAssisted' {
    $run = Invoke-GpuAvProbe -Name 'gpuav-on' -Switch @('-GpuAssisted')
    $run.ExitCode | Should Be 0
    $run.Log | Should Match 'GPUAV_ON'
  }

  It 'leaves GPU-AV off without -GpuAssisted' {
    $run = Invoke-GpuAvProbe -Name 'gpuav-off' -Switch @()
    $run.ExitCode | Should Be 0
    $run.Log | Should Match 'GPUAV_OFF'
  }

  It 'throws when the fixture log does not exist' {
    $missingPath = Join-Path $script:tmpDir 'does-not-exist.log'
    & pwsh -NoProfile -ExecutionPolicy Bypass -File $script:scriptPath -LogFixturePath $missingPath | Out-Null
    $LASTEXITCODE | Should -Not -Be 0
  }
}
