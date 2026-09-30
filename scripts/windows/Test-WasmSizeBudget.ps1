#requires -Version 7.0
# Test-WasmSizeBudget.ps1 - local twin of scripts/linux/wasm-size-budget.sh; needs rustup's wasm32 target.

[CmdletBinding()]
param(

    # 12 MiB, matching scripts/linux/wasm-size-budget.sh, the CI-enforced twin.
    [int]$BudgetBytes = 12582912,
    # Path to the Rust project template (kataglyphis_webgpu_renderer lives here).
    [string]$RustProjectDir = (Join-Path $PSScriptRoot '..\..\third_party\OxidANT'),
    # Skip the cargo build step (useful for re-checking an existing build).
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$exitCode = 0

# Resolve paths
$RustProjectDir = (Resolve-Path $RustProjectDir).Path
$wasmFile = Join-Path $RustProjectDir 'target\wasm32-unknown-unknown\release\kataglyphis_webgpu_renderer.wasm'

Write-Host '=== Wasm Size Budget Test ===' -ForegroundColor Cyan
Write-Host "Budget: $BudgetBytes bytes ($([math]::Round($BudgetBytes / 1MB, 2)) MiB)" -ForegroundColor Cyan
Write-Host "Rust project: $RustProjectDir" -ForegroundColor Cyan
Write-Host ''

# Prerequisites, reported separately with rustup's stderr, so a broken toolchain never reads as a missing target.
if (-not (Get-Command rustup -ErrorAction SilentlyContinue)) {
    Write-Host 'rustup is not on PATH, so the wasm target cannot be checked or built.' -ForegroundColor Yellow
    Write-Host 'Install Rust from https://rustup.rs, then: rustup target add wasm32-unknown-unknown' -ForegroundColor Yellow
    exit 1
}

$installed = rustup target list --installed
if ($LASTEXITCODE -ne 0) {
    Write-Host "rustup target list --installed failed (exit $LASTEXITCODE). Its output is above." -ForegroundColor Red
    Write-Host 'This is a broken Rust toolchain, NOT a missing wasm target.' -ForegroundColor Red
    exit 1
}

if (-not ($installed -match 'wasm32-unknown-unknown')) {
    Write-Host 'Wasm target wasm32-unknown-unknown not installed.' -ForegroundColor Yellow
    Write-Host 'Install with: rustup target add wasm32-unknown-unknown' -ForegroundColor Yellow
    exit 1
}

# Bootstrap the pinned binaryen rather than fail, from the same versions.env pin as the Linux twin.
. (Join-Path $PSScriptRoot 'Resolve-BuildModule.ps1')
Import-BuildModule @('WindowsWasmOpt.Common')
$null = Install-WasmOpt

# Build
if (-not $SkipBuild) {
    Write-Host '== Building for wasm32-unknown-unknown (release) ==' -ForegroundColor Cyan
    Push-Location $RustProjectDir
    try {
        cargo build --target wasm32-unknown-unknown --release -p kataglyphis_webgpu_renderer
        if ($LASTEXITCODE -ne 0) {
            throw "cargo build failed (exit code $LASTEXITCODE)."
        }
    } finally {
        Pop-Location
    }
} else {
    Write-Host '== Skipping build (-SkipBuild) ==' -ForegroundColor Yellow
}

# Check file exists
if (-not (Test-Path $wasmFile)) {
    Write-Host "FAIL: Wasm file not found at $wasmFile" -ForegroundColor Red
    Write-Host 'Build the crate first, or omit -SkipBuild.' -ForegroundColor Yellow
    exit 1
}

# Measure pre-opt size
$preSize = (Get-Item $wasmFile).Length
Write-Host "Pre-opt size: $preSize bytes ($([math]::Round($preSize / 1KB, 1)) KiB)" -ForegroundColor Cyan

# Optimise with wasm-opt
Write-Host '== Running wasm-opt -Oz ==' -ForegroundColor Cyan
$optFile = $wasmFile -replace '\.wasm$', '.opt.wasm'
# Invoke-WasmOpt adds the feature flags wgpu/naga output needs and the --all-features retry.
Invoke-WasmOpt -InputPath $wasmFile -OutputPath $optFile -OptimizationLevel '-Oz'

$optSize = (Get-Item $optFile).Length
Write-Host "Post-opt size: $optSize bytes ($([math]::Round($optSize / 1KB, 1)) KiB)" -ForegroundColor Cyan
Write-Host "Saved: $($preSize - $optSize) bytes ($([math]::Round(($preSize - $optSize) / 1KB, 1)) KiB)" -ForegroundColor Cyan

# Replace original with optimised
Move-Item -Force $optFile $wasmFile

# Check against budget
$finalSize = (Get-Item $wasmFile).Length
$ratio = [math]::Round($finalSize * 100.0 / $BudgetBytes, 1)

Write-Host ''
Write-Host "Final size: $finalSize bytes ($([math]::Round($finalSize / 1KB, 1)) KiB)" -ForegroundColor Cyan
Write-Host "Budget:     $BudgetBytes bytes ($([math]::Round($BudgetBytes / 1KB, 1)) KiB)" -ForegroundColor Cyan
Write-Host "Ratio:      $ratio%" -ForegroundColor Cyan

if ($finalSize -gt $BudgetBytes) {
    $over = $finalSize - $BudgetBytes
    Write-Host "FAIL: Wasm size $finalSize bytes exceeds budget of $BudgetBytes bytes ($over bytes over)!" -ForegroundColor Red
    Write-Host 'Consider: feature flags, LTO, dead-code elimination, or raising the budget.' -ForegroundColor Yellow
    $exitCode = 1
} else {
    $under = $BudgetBytes - $finalSize
    Write-Host "PASS: Wasm size $finalSize bytes is within budget ($under bytes to spare)." -ForegroundColor Green
}

Write-Host ''
if ($exitCode -eq 0) {
    Write-Host '=== WASM SIZE BUDGET TEST PASSED ===' -ForegroundColor Green
} else {
    Write-Host '=== WASM SIZE BUDGET TEST FAILED ===' -ForegroundColor Red
}
exit $exitCode

