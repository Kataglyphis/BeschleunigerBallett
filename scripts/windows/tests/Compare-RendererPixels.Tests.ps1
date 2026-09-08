#requires -Version 7.0

# Pester coverage for scripts/Compare-RendererPixels.ps1's structural-metric
# gate - the script previously threw an ObjectDisposedException on its own
# success path (Get-LuminanceMetrics was called with $bmp.Width/$bmp.Height
# AFTER $bmp.Dispose()), its -ValidationOnly mode checked hard-coded filenames
# the frame-dump capture never writes, and an empty run reported PASSED
# because $exitCode defaults to 0 and nothing set it otherwise. Invokes the
# real script as a child process against small fixture PNGs generated with
# System.Drawing, the same pattern Compare-RendererTimings.Tests.ps1 and
# Compare-PerfBaseline.Tests.ps1 use.
#
# NOTE: written for Pester 3.4.0 (the version installed here) - dash-less
# assertion syntax (`Should Be 0`, never Pester 5's `Should -Be 0`).

Describe 'Compare-RendererPixels' {

  BeforeAll {
    $script:scriptPath = (Resolve-Path (Join-Path $PSScriptRoot '..\..\Compare-RendererPixels.ps1')).Path
    $tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP ('renderer-pixels-test-' + (Get-Random))) -Force
    $script:tmpDir = $tmp.FullName

    Add-Type -AssemblyName System.Drawing.Common

    function New-FlatPng {
      # A single flat colour everywhere: stddev == 0, exactly one luminance
      # bucket - must be rejected as "essentially uniform".
      param([string]$Path, [int]$W = 64, [int]$H = 64)
      $bmp = [System.Drawing.Bitmap]::new($W, $H)
      $g = [System.Drawing.Graphics]::FromImage($bmp)
      try {
        $g.Clear([System.Drawing.Color]::FromArgb(255, 128, 128, 128))
      } finally {
        $g.Dispose()
      }
      $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
      $bmp.Dispose()
    }

    function New-StructuredPng {
      # A diagonal black-to-white gradient: dozens of distinct luminance
      # buckets, high stddev, well over half the pixels lit - must pass
      # every structural check.
      param([string]$Path, [int]$W = 64, [int]$H = 64)
      $bmp = [System.Drawing.Bitmap]::new($W, $H)
      $g = [System.Drawing.Graphics]::FromImage($bmp)
      try {
        $rect = [System.Drawing.Rectangle]::new(0, 0, $W, $H)
        $brush = [System.Drawing.Drawing2D.LinearGradientBrush]::new(
          $rect, [System.Drawing.Color]::Black, [System.Drawing.Color]::White, 45.0)
        try {
          $g.FillRectangle($brush, $rect)
        } finally {
          $brush.Dispose()
        }
      } finally {
        $g.Dispose()
      }
      $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
      $bmp.Dispose()
    }
  }

  AfterAll {
    Remove-Item -LiteralPath $script:tmpDir -Recurse -Force -ErrorAction SilentlyContinue
  }

  function Invoke-ComparePixels {
    param([string]$OutDir, [switch]$SkipCpp, [switch]$SkipRust, [switch]$ValidationOnly)
    $scriptArgs = @(
      '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $script:scriptPath,
      '-OutDir', $OutDir
    )
    if ($SkipCpp) { $scriptArgs += '-SkipCpp' }
    if ($SkipRust) { $scriptArgs += '-SkipRust' }
    if ($ValidationOnly) { $scriptArgs += '-ValidationOnly' }
    $out = & pwsh @scriptArgs 2>&1
    return @{ ExitCode = $LASTEXITCODE; Output = ($out -join "`n") }
  }

  It 'rejects a flat/uniform frame with exit 1 and mentions stddev' {
    $outDir = Join-Path $script:tmpDir 'flat'
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null
    New-FlatPng -Path (Join-Path $outDir 'cpp-vulkan.png')

    $result = Invoke-ComparePixels -OutDir $outDir -SkipCpp -SkipRust
    $result.ExitCode | Should Be 1
    ($result.Output -match 'stddev') | Should Be $true
  }

  It 'passes structural checks for a frame with real luminance variety' {
    $outDir = Join-Path $script:tmpDir 'structured'
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null
    New-StructuredPng -Path (Join-Path $outDir 'cpp-vulkan.png')

    $result = Invoke-ComparePixels -OutDir $outDir -SkipCpp -SkipRust
    $result.ExitCode | Should Be 0
  }

  It 'does not throw when computing metrics for a captured frame (regression: use-after-dispose)' {
    $outDir = Join-Path $script:tmpDir 'no-throw'
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null
    New-StructuredPng -Path (Join-Path $outDir 'cpp-vulkan.png')

    $result = Invoke-ComparePixels -OutDir $outDir -SkipCpp -SkipRust
    ($result.Output -match 'ObjectDisposedException') | Should Be $false
  }

  It 'exits 2 rather than PASSED when -ValidationOnly finds no frames at all' {
    $outDir = Join-Path $script:tmpDir 'empty-validation'
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null

    $result = Invoke-ComparePixels -OutDir $outDir -ValidationOnly
    $result.ExitCode | Should Be 2
    ($result.Output -match 'PIXEL COMPARISON PASSED') | Should Be $false
  }

  It '-ValidationOnly validates a frame it actually finds on disk' {
    $outDir = Join-Path $script:tmpDir 'validation-hit'
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null
    # Named the way the real DISABLED_DumpsFrameToPng capture names its
    # output, NOT the hard-coded 'cpp-vulkan.png' the old code checked for.
    New-StructuredPng -Path (Join-Path $outDir 'cpp-vulkan-shadows-on.png')

    $result = Invoke-ComparePixels -OutDir $outDir -ValidationOnly
    $result.ExitCode | Should Be 0
    ($result.Output -match 'Found 1 existing frame') | Should Be $true
  }
}
