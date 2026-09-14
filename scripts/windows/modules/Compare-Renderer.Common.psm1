Set-StrictMode -Version Latest
#requires -Version 7.0

# Shared helpers for the cross-renderer comparison scripts
# (Compare-RendererPixels.ps1 / Compare-RendererTimings.ps1). Deliberately
# project-local, not ANTfrastructure material: Dinosaurs scene, this repo's
# Rust crate layout.

# Converts the Dinosaurs OBJ scene to glTF via the Rust crate's obj2gltf
# example, reusing a cached output unless the OBJ is newer. Both comparison
# scripts previously carried this block verbatim.
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
