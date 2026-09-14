#requires -Version 7.0
# Runs both renderers headlessly and prints their per-pass GPU timings side by
# side - the first piece of the cross-renderer comparison harness.
#
# Sources:
#   C++/Vulkan: KATAGLYPHIS_GPU_TIMING_JSON export, produced by driving the
#               golden harness for a deterministic frame count.
#   Rust/WebGPU: the dump_gpu_timings example.
#
# Both write the same schema, so one parser reads both. Pass names only
# partially overlap (the pipelines genuinely differ - the C++ engine has
# Clouds/Sky, the Rust renderer has Ssao/Bloom/Histogram); the table aligns
# by name and leaves the other column empty rather than pretending the
# renderers are structurally identical.
#
# SAME scene, SAME resolution: the C++ golden harness renders the bundled
# Dinosaurs OBJ at 1200x768; this script converts that OBJ to glTF (the Rust
# renderer's format) via the obj2gltf example and times the Rust renderer on
# the converted scene at the same 1200x768. The conversion is data-exact -
# 166563 positions / 894174 indices on both sides, matching the C++ loader's
# own log line. Remaining honest differences: the pipelines are structurally
# different (Clouds/Sky vs Ssao/Bloom/Histogram), the camera framing differs,
# and the conversion currently carries no textures - so treat per-pass numbers
# as comparable workloads, not as a shader-for-shader benchmark.

[CmdletBinding()]
param(

    [string]$RepoRoot = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)),
    [string]$OutDir = (Join-Path ([IO.Path]::GetTempPath()) 'kataglyphis-timings'),
    # Source files the expected-pass lists are derived from (see
    # Get-ExpectedPassNames below). Overridable so tests can point these at
    # fixture files instead of the real sources.
    [string]$CppPassSourcePath = (Join-Path $RepoRoot 'Src\GraphicsEngineVulkan\renderer\GUIRendererSharedVars.ixx'),
    [string]$RustPassSourcePath = (Join-Path $RepoRoot 'third_party\OxidANT\crates\webgpu_renderer\src\render\gpu_timing.rs'),
    # Expected C++/Vulkan pass names. Defaults to every name in
    # GPU_TIMED_PASS_EXPORT_NAMES (all required - the C++ engine always
    # records all five). Left unset so the derived default (below) applies;
    # pass explicitly to pin a subset.
    [string[]]$CppExpectedPasses,
    # Expected Rust/WebGPU pass names. Defaults to every TimedPass except
    # OcclusionCull, which dump_gpu_timings only emits when occlusion culling
    # is enabled - see the -Optional argument on the Rust Assert-PassesExist
    # call below. Left unset so the derived default (below) applies.
    [string[]]$RustExpectedPasses,
    # When set, skips GPU-dependent steps and checks only JSON schema / pass
    # names from previously-produced files (useful in CI with no GPU).
    [switch]$ValidationOnly,
    # Prints the derived expected-pass lists as JSON and exits without
    # running either renderer. Exists so tests can exercise
    # Get-ExpectedPassNames without a GPU or build products.
    [switch]$PrintExpectedPasses
)

$ErrorActionPreference = 'Stop'
$exitCode = 0

function Get-ExpectedPassNames {
    # Parses the pass-name list out of the actual source of truth rather than
    # relying on a hand-maintained mirror, so the two can no longer drift
    # silently (that already happened once: this script's own $RustExpectedPasses
    # named a 'Post' pass the Rust renderer has never had).
    param(
        [Parameter(Mandatory)] [ValidateSet('Cpp', 'Rust')] [string]$Engine,
        [Parameter(Mandatory)] [string]$Path
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        throw "Get-ExpectedPassNames: source file not found: $Path"
    }
    $content = Get-Content -LiteralPath $Path -Raw

    $names = switch ($Engine) {
        'Cpp' {
            # GUIRendererSharedVars.ixx: either the legacy
            # `GPU_TIMED_PASS_EXPORT_NAMES[GPU_TIMED_PASS_COUNT] = { "Name", ... };`
            # or the std::array form
            # `GPU_TIMED_PASS_EXPORT_NAMES = std::to_array<const char *>({ "Name", ... });`.
            $m = [regex]::Match($content,
              'GPU_TIMED_PASS_EXPORT_NAMES\s*(?:\[[A-Za-z_]+\])?\s*=\s*(?:std::to_array<[^>]*>\s*)?\(?\{([^}]*)\}\)?')
            if (-not $m.Success) {
                throw "Get-ExpectedPassNames: could not find GPU_TIMED_PASS_EXPORT_NAMES initializer in $Path"
            }
            [regex]::Matches($m.Groups[1].Value, '"([^"]+)"') | ForEach-Object { $_.Groups[1].Value }
        }
        'Rust' {
            # gpu_timing.rs: fn name(self) -> &'static str { match self { TimedPass::X => "X", ... } }
            $m = [regex]::Match($content, 'fn\s+name\(self\)[\s\S]*?match self\s*\{([\s\S]*?)\}\s*\}')
            if (-not $m.Success) {
                throw "Get-ExpectedPassNames: could not find name() match arms in $Path"
            }
            [regex]::Matches($m.Groups[1].Value, '=>\s*"([^"]+)"') | ForEach-Object { $_.Groups[1].Value }
        }
    }

    $names = @($names)
    if ($names.Count -eq 0) {
        # A silently-empty expected list turns the whole gate off - that is
        # the exact failure mode this function exists to prevent.
        throw "Get-ExpectedPassNames: parsed zero pass names from $Path - the expected-pass gate would silently disable itself."
    }

    return , $names
}

if (-not $PSBoundParameters.ContainsKey('CppExpectedPasses')) {
    $CppExpectedPasses = Get-ExpectedPassNames -Engine Cpp -Path $CppPassSourcePath
}
if (-not $PSBoundParameters.ContainsKey('RustExpectedPasses')) {
    $rustAllPasses = Get-ExpectedPassNames -Engine Rust -Path $RustPassSourcePath
    $RustExpectedPasses = @($rustAllPasses | Where-Object { $_ -ne 'OcclusionCull' })
}

if ($PrintExpectedPasses) {
    [PSCustomObject]@{ Cpp = @($CppExpectedPasses); Rust = @($RustExpectedPasses) } | ConvertTo-Json -Depth 3
    exit 0
}

New-Item -ItemType Directory -Force $OutDir | Out-Null

$cppJson = Join-Path $OutDir 'gpu-timings-cpp.json'
$rustJson = Join-Path $OutDir 'gpu-timings-rust.json'

function Assert-PassesExist {
    param(
        [Parameter(Mandatory)] [string]$Label,
        [Parameter(Mandatory)] $JsonObj,
        [Parameter(Mandatory)] [string[]]$Expected,
        [string[]]$Optional = @()
    )

    $missing = $Expected | Where-Object { -not $JsonObj.passes.PSObject.Properties[$_] }
    if ($missing) {
        Write-Host "FAIL [$Label] missing expected pass(es): $($missing -join ', ')" -ForegroundColor Red
        Write-Host "  Actually present: $($JsonObj.passes.PSObject.Properties.Name -join ', ')" -ForegroundColor Yellow
        $script:exitCode = 1
    } else {
        Write-Host "OK   [$Label] all expected passes present." -ForegroundColor Green
    }

    # Warn about optional passes (helpful for catching renames).
    $optionalMissing = $Optional | Where-Object { -not $JsonObj.passes.PSObject.Properties[$_] }
    if ($optionalMissing) {
        Write-Host "WARN [$Label] optional pass(es) missing: $($optionalMissing -join ', ')" -ForegroundColor Yellow
    }

    # Validate all timing values are sane.
    foreach ($prop in $JsonObj.passes.PSObject.Properties) {
        $val = $prop.Value
        if ($val -le 0.0) {
            Write-Host "WARN [$Label] pass '$($prop.Name)' has non-positive timing: $val" -ForegroundColor Yellow
        }
        if (-not ($val -is [double]) -and -not ($val -is [int]) -and -not ($val -is [float])) {
            Write-Host "FAIL [$Label] pass '$($prop.Name)' value is not a number: $val" -ForegroundColor Red
            $script:exitCode = 1
        }
    }
}

# ---------------------------------------------------------------------------
# Phase 1: C++/Vulkan
# ---------------------------------------------------------------------------
$suite = Join-Path $RepoRoot 'build-clangcl-debug\commitTestSuite.exe'
if (-not (Test-Path $suite)) {
    if ($ValidationOnly) {
        Write-Host 'ValidationOnly mode: commitTestSuite.exe not found; assuming JSON already exists.' -ForegroundColor Yellow
    } else {
        throw "commitTestSuite.exe not found at $suite - build clangcl-debug first."
    }
}

if ((-not $ValidationOnly) -and (Test-Path $suite)) {
    Write-Host '== C++/Vulkan (golden harness, RendersNonBlankFrame) ==' -ForegroundColor Cyan
    $env:KATAGLYPHIS_GPU_TIMING_JSON = $cppJson
    try {
        & $suite --gtest_filter=GoldenRender.RendersNonBlankFrame *> $null
        if ($LASTEXITCODE -ne 0) {
            Write-Host "FAIL: commitTestSuite.exe exited with code $LASTEXITCODE" -ForegroundColor Red
            $exitCode = 1
        }
    } finally {
        $env:KATAGLYPHIS_GPU_TIMING_JSON = ''
    }
}

if (Test-Path $cppJson) {
    $cpp = Get-Content $cppJson -Raw | ConvertFrom-Json
    Assert-PassesExist -Label 'C++/Vulkan' -JsonObj $cpp -Expected $CppExpectedPasses -Optional @('Tonemap')
} elseif ($ValidationOnly) {
    Write-Host "WARN: C++ JSON not found at $cppJson - skipping validation." -ForegroundColor Yellow
} else {
    Write-Host "FAIL: C++ JSON not produced at $cppJson" -ForegroundColor Red
    $exitCode = 1
}

# ---------------------------------------------------------------------------
# Phase 2: Rust/WebGPU
# ---------------------------------------------------------------------------
$dinoObj = Join-Path $RepoRoot 'Resources\Models\Dinosaurs\dinosaurs.obj'
$dinoGltf = Join-Path $OutDir 'dinosaurs.gltf'

if ((-not $ValidationOnly) -and (Test-Path $suite)) {
    Push-Location (Join-Path $RepoRoot 'third_party\OxidANT')
    try {
        if (-not (Test-Path $dinoGltf) -or
            (Get-Item $dinoObj).LastWriteTime -gt (Get-Item $dinoGltf).LastWriteTime) {
            Write-Host '== Converting Dinosaurs OBJ -> glTF ==' -ForegroundColor Cyan
            cargo run -p kataglyphis_webgpu_renderer --example obj2gltf --quiet -- $dinoObj $dinoGltf 2>$null | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "obj2gltf failed." }
        }

        Write-Host '== Rust/WebGPU (dump_gpu_timings, same scene, 1200x768) ==' -ForegroundColor Cyan
        cargo run -p kataglyphis_webgpu_renderer --example dump_gpu_timings --quiet -- $rustJson $dinoGltf 1200 768 2>$null | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Write-Host "FAIL: dump_gpu_timings exited with code $LASTEXITCODE" -ForegroundColor Red
            $exitCode = 1
        }
    } finally {
        Pop-Location
    }
}

if (Test-Path $rustJson) {
    $rust = Get-Content $rustJson -Raw | ConvertFrom-Json
    Assert-PassesExist -Label 'Rust/WebGPU' -JsonObj $rust -Expected $RustExpectedPasses -Optional @('OcclusionCull')
} elseif ($ValidationOnly) {
    Write-Host "WARN: Rust JSON not found at $rustJson - skipping validation." -ForegroundColor Yellow
} else {
    Write-Host "FAIL: Rust JSON not produced at $rustJson" -ForegroundColor Red
    $exitCode = 1
}

# ---------------------------------------------------------------------------
# Phase 3: Side-by-side table
# ---------------------------------------------------------------------------
if ((Test-Path $cppJson) -and (Test-Path $rustJson)) {
    $cpp = Get-Content $cppJson -Raw | ConvertFrom-Json
    $rust = Get-Content $rustJson -Raw | ConvertFrom-Json

    $names = [System.Collections.Generic.SortedSet[string]]::new()
    foreach ($p in $cpp.passes.PSObject.Properties.Name) { [void]$names.Add($p) }
    foreach ($p in $rust.passes.PSObject.Properties.Name) { [void]$names.Add($p) }

    Write-Host ''
    Write-Host ("{0,-16} {1,14} {2,14}" -f 'Pass', 'C++/Vulkan ms', 'Rust/WebGPU ms')
    Write-Host ('-' * 46)
    foreach ($name in $names) {
        $c = $cpp.passes.PSObject.Properties[$name]
        $r = $rust.passes.PSObject.Properties[$name]
        $cv = if ($c) { '{0:N4}' -f $c.Value } else { '-' }
        $rv = if ($r) { '{0:N4}' -f $r.Value } else { '-' }
        Write-Host ("{0,-16} {1,14} {2,14}" -f $name, $cv, $rv)
    }
    Write-Host ''
    Write-Host ("frames: C++ {0} ({1}), Rust {2} ({3})" -f `
        $cpp.frames_measured, ($cpp.timestamps_supported ? 'timestamps' : 'NO timestamps'), `
        $rust.frames_measured, ($rust.timestamps_supported ? 'timestamps' : 'NO timestamps'))
}

Write-Host ''
if ($exitCode -eq 0) {
    Write-Host '=== GPU TIMING COMPARISON PASSED ===' -ForegroundColor Green
} else {
    Write-Host "=== GPU TIMING COMPARISON FAILED (exit code $exitCode) ===" -ForegroundColor Red
}
exit $exitCode

