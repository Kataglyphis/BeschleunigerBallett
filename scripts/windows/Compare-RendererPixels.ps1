#requires -Version 7.0
# Compare-RendererPixels.ps1
#
# Side-by-side pixel comparison between the C++ Vulkan engine and the Rust
# WebGPU renderer.
#
# Both render the same scene (Dinosaurs) at the same resolution (1200x768).
# Because the pipelines are structurally different (C++: Clouds/Sky,
# Rust: Ssao/Bloom/Histogram) and the color spaces differ (C++ linear vs
# Rust sRGB), this script uses STRUCTURAL metrics rather than pixel-exact
# comparison — mean luminance, luminance stddev, histogram spread — that
# survive driver, GPU and rendering differences.
#
# Usage:
#   pwsh -ExecutionPolicy Bypass -File .\scripts\Compare-RendererPixels.ps1
#
# CI mode (-ValidationOnly): checks that expected PNG frames exist and have
# structural integrity (luminance variety, lit fraction). Useful on GPU-less
# runners where headless rendering is unavailable.
#
# Exit codes:
#   0 - every frame that was captured/found passed its structural checks.
#   1 - a real assertion failure (a frame was captured/found but failed a
#       structural or cross-renderer check).
#   2 - nothing was checked at all (no frame was captured and none was found
#       on disk) - distinct from 1 so a caller can tell "broken" from
#       "regressed".
#
# Prerequisites:
#   - clangcl-debug built with Build-Windows-Container.ps1
#   - Rust toolchain (for cargo build of WebGPU examples)

[CmdletBinding()]
param(

    [string]$RepoRoot = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)),
    [string]$OutDir = (Join-Path ([IO.Path]::GetTempPath()) 'kataglyphis-pixels'),
    [int]$Width = 1200,
    [int]$Height = 768,
    [switch]$SkipCpp,
    [switch]$SkipRust,
    [switch]$ValidationOnly
)

$ErrorActionPreference = 'Stop'
$exitCode = 0

New-Item -ItemType Directory -Force $OutDir | Out-Null
Write-Host '=== Side-by-Side Pixel Comparison ===' -ForegroundColor Cyan
Write-Host "Resolution: ${Width}x${Height}" -ForegroundColor Cyan
Write-Host "Output dir: $OutDir" -ForegroundColor Cyan
Write-Host ''

# ---------------------------------------------------------------------------
# Helper: compute luminance metrics from a PNG file on disk
# ---------------------------------------------------------------------------
function Get-FrameMetrics {
    param([string]$Path)

    if (-not (Test-Path $Path)) { return $null }

    Add-Type -AssemblyName System.Drawing.Common
    $img = [System.Drawing.Bitmap]::new($Path)
    try {
        $w = $img.Width
        $h = $img.Height
        $rect = [System.Drawing.Rectangle]::new(0, 0, $w, $h)
        $data = $img.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        try {
            # Read Stride while the bits are still locked - after UnlockBits/
            # Dispose it only works by accident of the detached managed object.
            $stride = [Math]::Abs($data.Stride)
            $byteCount = $stride * $h
            $bytes = [byte[]]::new($byteCount)
            [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $byteCount)
        } finally {
            $img.UnlockBits($data)
        }
    } finally {
        $img.Dispose()
    }

    $total = $w * $h
    if ($total -eq 0) { return @{ mean = 0.0; stddev = 0.0; buckets = 0; lit_fraction = 0.0 } }

    $sum = 0.0
    $hist = @{}
    $lit = 0

    # LockBits with Format32bppArgb gives BGRA byte order per pixel.
    for ($y = 0; $y -lt $h; $y++) {
        $rowStart = $y * $stride
        for ($x = 0; $x -lt $w; $x++) {
            $i = $rowStart + ($x * 4)
            $b = $bytes[$i]
            $g = $bytes[$i + 1]
            $r = $bytes[$i + 2]
            # Relative luminance (Rec. 601 luma): L = 0.299*R + 0.587*G + 0.114*B
            $lum = 0.299 * $r + 0.587 * $g + 0.114 * $b
            $sum += $lum
            $bucket = [int][Math]::Clamp([Math]::Round($lum), 0, 255)
            $hist[$bucket] = $hist.ContainsKey($bucket) ? ($hist[$bucket] + 1) : 1
            if ($lum -gt 8.0) { $lit++ }
        }
    }

    $mean = $sum / $total

    $variance = 0.0
    for ($y = 0; $y -lt $h; $y++) {
        $rowStart = $y * $stride
        for ($x = 0; $x -lt $w; $x++) {
            $i = $rowStart + ($x * 4)
            $lum = 0.299 * $bytes[$i + 2] + 0.587 * $bytes[$i + 1] + 0.114 * $bytes[$i]
            $delta = $lum - $mean
            $variance += $delta * $delta
        }
    }
    $stddev = [Math]::Sqrt($variance / $total)

    return @{
        mean         = $mean
        stddev       = $stddev
        buckets      = $hist.Count
        lit_fraction = $lit / $total
    }
}

# ---------------------------------------------------------------------------
# Resolve target paths (may be overridden by validation-mode discovery below)
# ---------------------------------------------------------------------------
$cppPng = Join-Path $OutDir 'cpp-vulkan.png'
$rustPng = Join-Path $OutDir 'rust-webgpu.png'
$cppMetrics = $null
$rustMetrics = $null

# ---------------------------------------------------------------------------
# Phase 0: Validation-only mode (CI, no GPU)
# ---------------------------------------------------------------------------
if ($ValidationOnly) {
    Write-Host '== Validation-only mode (CI) ==' -ForegroundColor Cyan
    Write-Host 'Skipping GPU-dependent capture. Checking for pre-existing frames...' -ForegroundColor Yellow
    $existingPngs = Get-ChildItem (Join-Path $OutDir '*.png') -ErrorAction SilentlyContinue
    if ($existingPngs) {
        Write-Host "Found $($existingPngs.Count) existing frame(s) to validate." -ForegroundColor Green
        $cppCandidate = $existingPngs |
            Where-Object { $_.Name -match '^cpp-vulkan' -and $_.Name -notmatch 'delta|noise|golden-order|singletap' } |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
        $rustCandidate = $existingPngs |
            Where-Object { $_.Name -match '^rust-webgpu' -and $_.Name -notmatch 'delta|noise|golden-order|singletap' } |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($cppCandidate) { $cppPng = $cppCandidate.FullName }
        if ($rustCandidate) { $rustPng = $rustCandidate.FullName }
    } else {
        Write-Host 'No existing frames found; validation will be skipped.' -ForegroundColor Yellow
    }
}

# ---------------------------------------------------------------------------
# Phase 1: C++/Vulkan frame capture
# ---------------------------------------------------------------------------
if (-not $SkipCpp -and -not $ValidationOnly) {
    Write-Host '== C++/Vulkan (golden harness, RendersNonBlankFrame) ==' -ForegroundColor Cyan

    $suite = Join-Path $RepoRoot 'build-clangcl-debug\commitTestSuite.exe'
    if (-not (Test-Path $suite)) {
        Write-Host 'WARNING: commitTestSuite.exe not found. Build clangcl-debug first.' -ForegroundColor Yellow
        Write-Host 'Skipping C++ capture.' -ForegroundColor Yellow
    } else {
        # Run a minimal harness that captures a frame and writes it to PNG.
        # The DISABLED_DumpsFrameToPng test does exactly this when env var
        # KATAGLYPHIS_FRAME_DUMP is set.
        $env:KATAGLYPHIS_FRAME_DUMP = Join-Path $OutDir 'cpp-vulkan'
        try {
            & $suite --gtest_filter=GoldenRender.DISABLED_DumpsFrameToPng --gtest_also_run_disabled_tests 2>&1 | Out-Null
            if ($LASTEXITCODE -ne 0) {
                Write-Host "WARNING: Frame capture test exited with code $LASTEXITCODE" -ForegroundColor Yellow
            }
        } finally {
            $env:KATAGLYPHIS_FRAME_DUMP = ''
        }

        # The test writes files like cpp-vulkan-shadows-on.png, etc.
        # Pick the first non-delta, non-noise render output.
        $captured = Get-ChildItem (Join-Path $OutDir 'cpp-vulkan*.png') |
            Where-Object { $_.Name -notmatch 'delta|noise|golden-order|singletap' } |
            Sort-Object LastWriteTime -Descending |
            Select-Object -First 1
        if ($captured) {
            Write-Host "C++ captured: $($captured.FullName)" -ForegroundColor Green
            $cppPng = $captured.FullName
        } else {
            Write-Host 'C++ captured no frame (likely no GPU).' -ForegroundColor Yellow
        }
    }
} else {
    Write-Host '== C++/Vulkan (skipped) ==' -ForegroundColor Yellow
}

# ---------------------------------------------------------------------------
# Phase 2: Rust/WebGPU frame capture
# ---------------------------------------------------------------------------
if (-not $SkipRust -and -not $ValidationOnly) {
    Write-Host '== Rust/WebGPU (headless_render, same scene, 1200x768) ==' -ForegroundColor Cyan

    # Convert Dinosaurs OBJ -> glTF (shared helper, reuses cached version)
    $dinoGltf = Join-Path $OutDir 'dinosaurs.gltf'
    . (Join-Path $PSScriptRoot 'Resolve-BuildModule.ps1')
    Import-BuildModule 'Compare-Renderer.Common'
    Convert-DinosaursObjToGltf -RepoRoot $RepoRoot -OutputGltf $dinoGltf
    Push-Location (Join-Path $RepoRoot 'third_party\OxidANT')
    try {
        Write-Host '  Running headless_render example...'
        cargo run -p kataglyphis_webgpu_renderer --example headless_render --quiet -- $dinoGltf $rustPng $Width $Height 2>$null | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Write-Host "WARNING: headless_render exited with code $LASTEXITCODE" -ForegroundColor Yellow
        }
    } finally {
        Pop-Location
    }

    if (Test-Path $rustPng) {
        Write-Host "Rust captured: $rustPng" -ForegroundColor Green
    } else {
        Write-Host 'Rust captured no frame.' -ForegroundColor Yellow
    }
} else {
    Write-Host '== Rust/WebGPU (skipped) ==' -ForegroundColor Yellow
}

# ---------------------------------------------------------------------------
# Phase 3: Load PNGs and compute metrics
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '=== Metrics ===' -ForegroundColor Cyan

$metricsTable = @()

$cppMetrics = Get-FrameMetrics -Path $cppPng
if ($cppMetrics) {
    $metricsTable += [PSCustomObject]@{ Renderer = 'C++/Vulkan'; Mean = $cppMetrics.mean; StdDev = $cppMetrics.stddev; Buckets = $cppMetrics.buckets; LitFraction = $cppMetrics.lit_fraction }
} else {
    Write-Host 'C++ frame not available.' -ForegroundColor Yellow
}

$rustMetrics = Get-FrameMetrics -Path $rustPng
if ($rustMetrics) {
    $metricsTable += [PSCustomObject]@{ Renderer = 'Rust/WebGPU'; Mean = $rustMetrics.mean; StdDev = $rustMetrics.stddev; Buckets = $rustMetrics.buckets; LitFraction = $rustMetrics.lit_fraction }
} else {
    Write-Host 'Rust frame not available.' -ForegroundColor Yellow
}

if ($metricsTable.Count -gt 0) {
    $metricsTable | Format-Table -Property Renderer, @{n='Mean Lum';e={'{0:N2}' -f $_.Mean}}, @{n='StdDev';e={'{0:N2}' -f $_.StdDev}}, Buckets, @{n='Lit%';e={'{0:N1}%' -f ($_.LitFraction * 100)}} -AutoSize | Out-Host
}

# ---------------------------------------------------------------------------
# Stop reporting success on an empty run.
# ---------------------------------------------------------------------------
if (-not $cppMetrics -and -not $rustMetrics) {
    Write-Host ''
    Write-Host 'No frames were captured or found - nothing was checked.' -ForegroundColor Red
    Write-Host '=== PIXEL COMPARISON: NOTHING CHECKED (exit code 2) ===' -ForegroundColor Red
    exit 2
}

# ---------------------------------------------------------------------------
# Phase 4: Structural assertions
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '=== Assertions ===' -ForegroundColor Cyan

if ($cppMetrics) {
    # These thresholds match the GoldenRender tests.
    if ($cppMetrics.stddev -le 2.0) {
        Write-Host "FAIL [C++] Frame is essentially uniform (stddev $($cppMetrics.stddev))." -ForegroundColor Red
        $exitCode = 1
    } else {
        Write-Host "OK   [C++] Frame has structure (stddev $($cppMetrics.stddev))." -ForegroundColor Green
    }
    if ($cppMetrics.lit_fraction -le 0.05) {
        Write-Host "FAIL [C++] Frame is too dark (lit $($cppMetrics.lit_fraction * 100)% of pixels)." -ForegroundColor Red
        $exitCode = 1
    } else {
        Write-Host "OK   [C++] Frame is well-lit (lit $($cppMetrics.lit_fraction * 100)%)." -ForegroundColor Green
    }
    if ($cppMetrics.buckets -le 8) {
        Write-Host "FAIL [C++] Too few luminance buckets ($($cppMetrics.buckets))." -ForegroundColor Red
        $exitCode = 1
    } else {
        Write-Host "OK   [C++] Good luminance variety ($($cppMetrics.buckets) buckets)." -ForegroundColor Green
    }
}

if ($rustMetrics) {
    if ($rustMetrics.stddev -le 2.0) {
        Write-Host "FAIL [Rust] Frame is essentially uniform (stddev $($rustMetrics.stddev))." -ForegroundColor Red
        $exitCode = 1
    } else {
        Write-Host "OK   [Rust] Frame has structure (stddev $($rustMetrics.stddev))." -ForegroundColor Green
    }
    if ($rustMetrics.lit_fraction -le 0.05) {
        Write-Host "FAIL [Rust] Frame is too dark (lit $($rustMetrics.lit_fraction * 100)% of pixels)." -ForegroundColor Red
        $exitCode = 1
    } else {
        Write-Host "OK   [Rust] Frame is well-lit (lit $($rustMetrics.lit_fraction * 100)%)." -ForegroundColor Green
    }
    if ($rustMetrics.buckets -le 8) {
        Write-Host "FAIL [Rust] Too few luminance buckets ($($rustMetrics.buckets))." -ForegroundColor Red
        $exitCode = 1
    } else {
        Write-Host "OK   [Rust] Good luminance variety ($($rustMetrics.buckets) buckets)." -ForegroundColor Green
    }
}

# Cross-renderer comparison (only when both frames are available)
if ($cppMetrics -and $rustMetrics) {
    Write-Host ''
    Write-Host '=== Cross-Renderer Comparison ===' -ForegroundColor Cyan

    $meanDiff = [Math]::Abs($cppMetrics.mean - $rustMetrics.mean)
    $stddevDiff = [Math]::Abs($cppMetrics.stddev - $rustMetrics.stddev)

    Write-Host "Mean luminance diff: $($meanDiff.ToString('N2')) (C++: $($cppMetrics.mean.ToString('N2')), Rust: $($rustMetrics.mean.ToString('N2')))"
    Write-Host "StdDev diff:         $($stddevDiff.ToString('N2'))"

    # Generous tolerance: the pipelines are structurally different (C++:
    # Clouds/Sky, Rust: Ssao/Bloom/Histogram) and the color spaces differ
    # (C++ linear, Rust sRGB - sRGB-encoded is ~1.48x brighter due to gamma 2.2).
    # A passing value means both renderers produce a recognisable image of
    # similar relative brightness — it does NOT mean they shade identically.
    # Using ratio: Rust/C++ mean should be between 0.5 and 4.0 (generous).
    $ratio = if ($cppMetrics.mean -gt 0.0) { $rustMetrics.mean / $cppMetrics.mean } else { 10.0 }
    Write-Host "Mean luminance ratio (Rust/C++): $($ratio.ToString('N2'))"
    if ($ratio -lt 0.5 -or $ratio -gt 4.0) {
        Write-Host "FAIL Luminance ratio $($ratio.ToString('N2')) out of expected range [0.5, 4.0]." -ForegroundColor Red
        Write-Host "      This may indicate one renderer producing a blank or degenerate frame." -ForegroundColor Yellow
        $exitCode = 1
    } else {
        Write-Host "OK   Luminance ratio within tolerance." -ForegroundColor Green
    }
}

if ($ValidationOnly) {
    Write-Host ''
    Write-Host '=== Validation-Only Summary ===' -ForegroundColor Cyan
    if ($exitCode -eq 0) {
        Write-Host 'All available frames passed structural checks.' -ForegroundColor Green
    } else {
        Write-Host 'Validation found issues.' -ForegroundColor Red
    }
}

Write-Host ''
if ($exitCode -eq 0) {
    Write-Host '=== PIXEL COMPARISON PASSED ===' -ForegroundColor Green
} else {
    Write-Host "=== PIXEL COMPARISON FAILED (exit code $exitCode) ===" -ForegroundColor Red
}
exit $exitCode
