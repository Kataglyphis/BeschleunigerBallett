Set-StrictMode -Version Latest
#requires -Version 7.0

# Project-local on purpose: the Dinosaurs scene, this repo's crate layout and the goldens' verdict are not ANTfrastructure material.

# Reuses the cached glTF unless the OBJ is newer.
function Convert-DinosaursObjToGltf {
  param(
    [Parameter(Mandatory)]
    [string]$RepoRoot,
    [Parameter(Mandatory)]
    [string]$OutputGltf
  )

  $dinoObj = Join-Path $RepoRoot 'Resources\Models\Dinosaurs\dinosaurs.obj'
  Push-Location (Join-Path $RepoRoot 'third_party\OxidANT')
  try {
    if (-not (Test-Path $OutputGltf) -or
        (Get-Item $dinoObj).LastWriteTime -gt (Get-Item $OutputGltf).LastWriteTime) {
      Write-Host '== Converting Dinosaurs OBJ -> glTF ==' -ForegroundColor Cyan
      cargo run -p kataglyphis_webgpu_renderer --example obj2gltf --quiet -- $dinoObj $OutputGltf 2>$null | Out-Null
      if ($LASTEXITCODE -ne 0) { throw "obj2gltf failed." }
    }
  } finally {
    Pop-Location
  }
}

# GoldenMetrics.hpp's frame_structure: Rec. 709 luma of the stored bytes, each bucket the nearest level.
function Get-FrameMetrics {
  param([Parameter(Mandatory)][string]$Path)

  if (-not (Test-Path $Path)) { return $null }

  Add-Type -AssemblyName System.Drawing.Common
  $img = [System.Drawing.Bitmap]::new($Path)
  try {
    $w = $img.Width
    $h = $img.Height
    $rect = [System.Drawing.Rectangle]::new(0, 0, $w, $h)
    $data = $img.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    try {
      # Read Stride while locked; after UnlockBits it only works by accident.
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

  $lumas = [double[]]::new($total)
  $seen = [bool[]]::new(256)
  $sum = 0.0
  $lit = 0

  # LockBits with Format32bppArgb gives BGRA byte order per pixel.
  for ($y = 0; $y -lt $h; $y++) {
    $rowStart = $y * $stride
    for ($x = 0; $x -lt $w; $x++) {
      $i = $rowStart + ($x * 4)
      # luminance_of's order of terms, so both sides round alike.
      $lum = 0.2126 * $bytes[$i + 2] + 0.7152 * $bytes[$i + 1] + 0.0722 * $bytes[$i]
      $lumas[$y * $w + $x] = $lum
      $sum += $lum
      $seen[[int][Math]::Floor([Math]::Clamp($lum, 0.0, 255.0) + 0.5)] = $true
      if ($lum -gt 8.0) { $lit++ }
    }
  }

  $mean = $sum / $total
  $variance = 0.0
  foreach ($lum in $lumas) {
    $delta = $lum - $mean
    $variance += $delta * $delta
  }

  return @{
    mean         = $mean
    stddev       = [Math]::Sqrt($variance / $total)
    buckets      = @($seen | Where-Object { $_ }).Count
    lit_fraction = $lit / $total
  }
}

# GoldenMetrics.hpp's kFrameMinStddev, kFrameMinLitFraction and kFrameMinBuckets, each strict.
function Test-FrameStructure {
  param([Parameter(Mandatory)][string]$Label, [Parameter(Mandatory)][hashtable]$Metrics)

  $passed = $true
  if ($Metrics.stddev -le 2.0) {
    Write-Host "FAIL [$Label] Frame is essentially uniform (stddev $($Metrics.stddev))." -ForegroundColor Red
    $passed = $false
  } else {
    Write-Host "OK   [$Label] Frame has structure (stddev $($Metrics.stddev))." -ForegroundColor Green
  }
  if ($Metrics.lit_fraction -le 0.05) {
    Write-Host "FAIL [$Label] Frame is too dark (lit $($Metrics.lit_fraction * 100)% of pixels)." -ForegroundColor Red
    $passed = $false
  } else {
    Write-Host "OK   [$Label] Frame is well-lit (lit $($Metrics.lit_fraction * 100)%)." -ForegroundColor Green
  }
  if ($Metrics.buckets -le 8) {
    Write-Host "FAIL [$Label] Too few luminance buckets ($($Metrics.buckets))." -ForegroundColor Red
    $passed = $false
  } else {
    Write-Host "OK   [$Label] Good luminance variety ($($Metrics.buckets) buckets)." -ForegroundColor Green
  }
  return $passed
}

Export-ModuleMember -Function Convert-DinosaursObjToGltf, Get-FrameMetrics, Test-FrameStructure
