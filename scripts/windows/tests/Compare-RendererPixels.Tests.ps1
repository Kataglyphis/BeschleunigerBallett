#requires -Version 7.0

# Runs the real script against generated fixture PNGs.

Describe 'Compare-RendererPixels' {

  BeforeAll {
    $script:scriptPath = (Resolve-Path (Join-Path $PSScriptRoot '..\Compare-RendererPixels.ps1')).Path
    $tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP ('renderer-pixels-test-' + (Get-Random))) -Force
    $script:tmpDir = $tmp.FullName

    Add-Type -AssemblyName System.Drawing.Common
    . (Join-Path $PSScriptRoot '..\Resolve-BuildModule.ps1')
    Import-BuildModule 'Compare-Renderer.Common'

    function New-FlatPng {
      # Flat colour: stddev 0 and one bucket, so it must fail as uniform.
      param([string]$Path, [int]$W = 64, [int]$H = 64,
        [System.Drawing.Color]$Color = [System.Drawing.Color]::FromArgb(255, 128, 128, 128))
      $bmp = [System.Drawing.Bitmap]::new($W, $H)
      $g = [System.Drawing.Graphics]::FromImage($bmp)
      try {
        $g.Clear($Color)
      } finally {
        $g.Dispose()
      }
      $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
      $bmp.Dispose()
    }

    function New-StructuredPng {
      # Diagonal gradient: many buckets, high stddev, mostly lit, so it must pass.
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
    function New-GreyRampPng {
      # One column per grey level, 0 to 255.
      param([string]$Path)
      $bmp = [System.Drawing.Bitmap]::new(256, 2)
      try {
        for ($x = 0; $x -lt 256; $x++) {
          for ($y = 0; $y -lt 2; $y++) { $bmp.SetPixel($x, $y, [System.Drawing.Color]::FromArgb(255, $x, $x, $x)) }
        }
        $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
      } finally {
        $bmp.Dispose()
      }
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
  }

  AfterAll {
    Remove-Item -LiteralPath $script:tmpDir -Recurse -Force -ErrorAction SilentlyContinue
  }

  It 'rejects a flat/uniform frame with exit 1 and mentions stddev' {
    $outDir = Join-Path $script:tmpDir 'flat'
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null
    New-FlatPng -Path (Join-Path $outDir 'cpp-vulkan.png')

    $result = Invoke-ComparePixels -OutDir $outDir -SkipCpp -SkipRust
    $result.ExitCode | Should -Be 1
    ($result.Output -match 'stddev') | Should -Be $true
  }

  It 'passes structural checks for a frame with real luminance variety' {
    $outDir = Join-Path $script:tmpDir 'structured'
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null
    New-StructuredPng -Path (Join-Path $outDir 'cpp-vulkan.png')

    $result = Invoke-ComparePixels -OutDir $outDir -SkipCpp -SkipRust
    $result.ExitCode | Should -Be 0
  }

  It 'checks uniformity, darkness and bucket count for both labeled renderers' {
    $outDir = Join-Path $script:tmpDir 'both-dark'
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null
    New-FlatPng -Path (Join-Path $outDir 'cpp-vulkan.png') -Color ([System.Drawing.Color]::Black)
    New-FlatPng -Path (Join-Path $outDir 'rust-webgpu.png') -Color ([System.Drawing.Color]::Black)

    $result = Invoke-ComparePixels -OutDir $outDir -SkipCpp -SkipRust
    $result.ExitCode | Should -Be 1
    foreach ($label in @('C\+\+', 'Rust')) {
      ($result.Output -match "FAIL \[$label\] Frame is essentially uniform") | Should -Be $true
      ($result.Output -match "FAIL \[$label\] Frame is too dark") | Should -Be $true
      ($result.Output -match "FAIL \[$label\] Too few luminance buckets") | Should -Be $true
    }
  }

  It 'validates each renderer independently when only Rust is uniform' {
    $outDir = Join-Path $script:tmpDir 'mixed-structure'
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null
    New-StructuredPng -Path (Join-Path $outDir 'cpp-vulkan.png')
    New-FlatPng -Path (Join-Path $outDir 'rust-webgpu.png')

    $result = Invoke-ComparePixels -OutDir $outDir -SkipCpp -SkipRust
    $result.ExitCode | Should -Be 1
    ($result.Output -match 'OK   \[C\+\+\] Frame has structure') | Should -Be $true
    ($result.Output -match 'FAIL \[C\+\+\]') | Should -Be $false
    ($result.Output -match 'FAIL \[Rust\] Frame is essentially uniform') | Should -Be $true
    ($result.Output -match 'FAIL \[Rust\] Too few luminance buckets') | Should -Be $true
  }

  It 'does not throw when computing metrics for a captured frame (regression: use-after-dispose)' {
    $outDir = Join-Path $script:tmpDir 'no-throw'
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null
    New-StructuredPng -Path (Join-Path $outDir 'cpp-vulkan.png')

    $result = Invoke-ComparePixels -OutDir $outDir -SkipCpp -SkipRust
    ($result.Output -match 'ObjectDisposedException') | Should -Be $false
  }

  # GoldenMetrics.LuminanceWeighsTheChannelsByRec709 pins the C++ side; Rec. 601 reads 149.685.
  It 'weighs the channels by Rec. 709, as GoldenMetrics.hpp does' {
    $png = Join-Path $script:tmpDir 'green.png'
    New-FlatPng -Path $png -Color ([System.Drawing.Color]::FromArgb(255, 0, 255, 0))

    $metrics = Get-FrameMetrics -Path $png
    $metrics.mean | Should -BeGreaterThan 182.375
    $metrics.mean | Should -BeLessThan 182.377
    $metrics.lit_fraction | Should -Be 1.0
  }

  # GoldenMetrics.FractionAboveLeavesAPixelAtTheThresholdOut's strict level: grey 8 reads exactly 8, which is dark.
  It 'counts a pixel at the lit level as dark' {
    $at = Join-Path $script:tmpDir 'grey-8.png'
    $above = Join-Path $script:tmpDir 'grey-9.png'
    New-FlatPng -Path $at -Color ([System.Drawing.Color]::FromArgb(255, 8, 8, 8))
    New-FlatPng -Path $above -Color ([System.Drawing.Color]::FromArgb(255, 9, 9, 9))

    (Get-FrameMetrics -Path $at).lit_fraction | Should -Be 0.0
    (Get-FrameMetrics -Path $above).lit_fraction | Should -Be 1.0
  }

  # The one bucket rule, GoldenMetrics.EveryGreyLevelIsItsOwnBucket's: a floor of the luma counts 203 here.
  It 'puts every grey level in its own bucket' {
    $png = Join-Path $script:tmpDir 'grey-ramp.png'
    New-GreyRampPng -Path $png

    $metrics = Get-FrameMetrics -Path $png
    $metrics.buckets | Should -Be 256
    $metrics.mean | Should -BeGreaterThan 127.499
    $metrics.mean | Should -BeLessThan 127.501
    # Divided by the pixel count, as luminance_stddev divides.
    $metrics.stddev | Should -BeGreaterThan 73.900
    $metrics.stddev | Should -BeLessThan 73.901
  }

  It 'exits 2 rather than PASSED when -ValidationOnly finds no frames at all' {
    $outDir = Join-Path $script:tmpDir 'empty-validation'
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null

    $result = Invoke-ComparePixels -OutDir $outDir -ValidationOnly
    $result.ExitCode | Should -Be 2
    ($result.Output -match 'PIXEL COMPARISON PASSED') | Should -Be $false
  }

  It '-ValidationOnly validates a frame it actually finds on disk' {
    $outDir = Join-Path $script:tmpDir 'validation-hit'
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null
    # DISABLED_DumpsFrameToPng's name, not the capture's cpp-vulkan.png: validation takes either.
    New-StructuredPng -Path (Join-Path $outDir 'cpp-vulkan-shadows-on.png')

    $result = Invoke-ComparePixels -OutDir $outDir -ValidationOnly
    $result.ExitCode | Should -Be 0
    ($result.Output -match 'Found 1 existing frame') | Should -Be $true
  }
}
