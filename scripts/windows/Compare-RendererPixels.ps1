#requires -Version 7.0
# Compare-RendererPixels.ps1 - structural, not pixel-exact: the pipelines and color spaces differ. Exit 2 = nothing checked.

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

# Get-FrameMetrics and Test-FrameStructure: GoldenMetrics.hpp's verdict, which compare-renderer-pixels.sh applies too.
. (Join-Path $PSScriptRoot 'Resolve-BuildModule.ps1')
Import-BuildModule 'Compare-Renderer.Common'

# Target paths (validation mode may override them)
$cppPng = Join-Path $OutDir 'cpp-vulkan.png'
$rustPng = Join-Path $OutDir 'rust-webgpu.png'
$cppMetrics = $null
$rustMetrics = $null

# Phase 0: validation-only mode (CI, no GPU)
if ($ValidationOnly) {
    Write-Host '== Validation-only mode (CI) ==' -ForegroundColor Cyan
    Write-Host 'Skipping GPU-dependent capture. Checking for pre-existing frames...' -ForegroundColor Yellow
    # An array even for one frame: Resolve-BuildModule.ps1 left strict mode on, where a lone file has no Count.
    $existingPngs = @(Get-ChildItem (Join-Path $OutDir '*.png') -ErrorAction SilentlyContinue)
    if ($existingPngs.Count -gt 0) {
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

# Phase 1: C++/Vulkan frame capture
if (-not $SkipCpp -and -not $ValidationOnly) {
    Write-Host '== C++/Vulkan (golden harness, RendersNonBlankFrame) ==' -ForegroundColor Cyan

    $suite = Join-Path $RepoRoot 'build-clangcl-debug\commitTestSuite.exe'
    if (-not (Test-Path $suite)) {
        Write-Host 'WARNING: commitTestSuite.exe not found. Build clangcl-debug first.' -ForegroundColor Yellow
        Write-Host 'Skipping C++ capture.' -ForegroundColor Yellow
    } else {
        # The golden writes the frame it graded; a stale one would pass as this run's.
        Remove-Item -LiteralPath $cppPng -Force -ErrorAction SilentlyContinue
        $env:KATAGLYPHIS_GRADED_FRAME_PNG = $cppPng
        Push-Location $RepoRoot
        try {
            & $suite --gtest_filter=GoldenRender.RendersNonBlankFrame 2>&1 | Out-Null
            if ($LASTEXITCODE -ne 0) {
                Write-Host "WARNING: Frame capture test exited with code $LASTEXITCODE" -ForegroundColor Yellow
            }
        } finally {
            Pop-Location
            $env:KATAGLYPHIS_GRADED_FRAME_PNG = ''
        }

        if (Test-Path $cppPng) {
            Write-Host "C++ captured: $cppPng" -ForegroundColor Green
        } else {
            Write-Host 'C++ captured no frame (likely no GPU).' -ForegroundColor Yellow
        }
    }
} else {
    Write-Host '== C++/Vulkan (skipped) ==' -ForegroundColor Yellow
}

# Phase 2: Rust/WebGPU frame capture
if (-not $SkipRust -and -not $ValidationOnly) {
    Write-Host '== Rust/WebGPU (headless_render, same scene, 1200x768) ==' -ForegroundColor Cyan

    # Convert Dinosaurs OBJ -> glTF (shared helper, reuses cached version)
    $dinoGltf = Join-Path $OutDir 'dinosaurs.gltf'
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

# Phase 3: metrics
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

# Never report success on an empty run.
if (-not $cppMetrics -and -not $rustMetrics) {
    Write-Host ''
    Write-Host 'No frames were captured or found - nothing was checked.' -ForegroundColor Red
    Write-Host '=== PIXEL COMPARISON: NOTHING CHECKED (exit code 2) ===' -ForegroundColor Red
    exit 2
}

# Phase 4: structural assertions
Write-Host ''
Write-Host '=== Assertions ===' -ForegroundColor Cyan

if ($cppMetrics -and -not (Test-FrameStructure -Label 'C++' -Metrics $cppMetrics)) { $exitCode = 1 }
if ($rustMetrics -and -not (Test-FrameStructure -Label 'Rust' -Metrics $rustMetrics)) { $exitCode = 1 }

# Cross-renderer comparison (only when both frames are available)
if ($cppMetrics -and $rustMetrics) {
    Write-Host ''
    Write-Host '=== Cross-Renderer Comparison ===' -ForegroundColor Cyan

    $meanDiff = [Math]::Abs($cppMetrics.mean - $rustMetrics.mean)
    $stddevDiff = [Math]::Abs($cppMetrics.stddev - $rustMetrics.stddev)

    Write-Host "Mean luminance diff: $($meanDiff.ToString('N2')) (C++: $($cppMetrics.mean.ToString('N2')), Rust: $($rustMetrics.mean.ToString('N2')))"
    Write-Host "StdDev diff:         $($stddevDiff.ToString('N2'))"

    # Generous: pipelines differ and Rust is sRGB (~1.48x brighter); a pass only rules out degenerate frames.
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
