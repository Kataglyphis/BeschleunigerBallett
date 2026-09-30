Set-StrictMode -Version Latest
#requires -Version 7.0

# Project-local on purpose: the Dinosaurs scene and this repo's crate layout are not ANTfrastructure material.

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

Export-ModuleMember -Function Convert-DinosaursObjToGltf
