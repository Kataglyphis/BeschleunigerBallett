#requires -Version 7.0

# This runs the source-copy loop; it does not cross-compile or execute the staged binaries.
Import-Module (Join-Path $PSScriptRoot '..\..\..\third_party\ANTfrastructure\windows\scripts\tests\TestHarness.psm1') -Function Get-TestScriptAst, Write-TestFileTree -ErrorAction Stop

function Get-StageSourceCopyLoop {
    param([Parameter(Mandatory)][string]$ScriptPath)
    $ast = Get-TestScriptAst -ScriptPath $ScriptPath
    $loops = @($ast.FindAll({
        param($node)
        $node -is [Management.Automation.Language.ForEachStatementAst] -and
        $node.Variable.VariablePath.UserPath -eq 'rel' -and
        $node.Condition.Extent.Text.Contains('Resources\ShadersSlang')
    }, $true))
    if ($loops.Count -ne 1) { throw "Expected one source-copy loop, found $($loops.Count)" }
    return $loops[0].Extent.Text
}

function Invoke-StageSourceCopyFixture {
    param([Parameter(Mandatory)][string]$Loop, [Parameter(Mandatory)][string]$Root)
    $workspacePath = Join-Path $Root 'source'
    $tests = Join-Path $Root 'stage'
    Write-TestFileTree -Root $workspacePath -Files @{
        'Resources\ShadersSlang\build\spirv\fixture.spv' = 'compiled'
        'Resources\ShadersSlang\rasterizer\rasterizer.slang' = 'local'
        'Resources\Models\Dinosaurs\fixture.txt' = 'model'
        'Resources\Models\GltfTest\fixture.txt' = 'model'
        'Resources\Models\ShadowTest\fixture.txt' = 'model'
        'Resources\Models\VikingRoom\fixture.txt' = 'model'
        'Resources\Models\crytek-sponza\fixture.txt' = 'model'
        'third_party\OxidANT\shaders\forward\forward.slang' = 'owned'
    }
    $null = New-Item -ItemType Directory -Path $tests -Force
    & ([scriptblock]::Create($Loop))
    return $tests
}

Describe 'ARM64 staged shader source closure' {
    $copyLoop = Get-StageSourceCopyLoop -ScriptPath (Join-Path $PSScriptRoot '..\Build-Windows.ps1')

    It 'keeps local and pinned Slang sources at their canonical paths' {
        $owner = Join-Path $PSScriptRoot '..\..\..\third_party\OxidANT\shaders\forward\forward.slang'
        (Test-Path -LiteralPath $owner -PathType Leaf) | Should Be $true
        (Test-Path -LiteralPath (Join-Path $PSScriptRoot '..\..\..\Resources\ShadersSlang\rasterizer\rasterizer.slang') -PathType Leaf) | Should Be $true
        $stage = Invoke-StageSourceCopyFixture -Loop $copyLoop -Root (Join-Path $TestDrive 'control')
        $owned = Join-Path $stage 'third_party\OxidANT\shaders\forward\forward.slang'
        (Get-Content -LiteralPath $owned -Raw) | Should Be 'owned'
        (Get-Content -LiteralPath (Join-Path $stage 'Resources\ShadersSlang\rasterizer\rasterizer.slang') -Raw) | Should Be 'local'
        (Get-Content -LiteralPath (Join-Path $stage 'Resources\ShadersSlang\build\spirv\fixture.spv') -Raw) | Should Be 'compiled'
        (Test-Path -LiteralPath (Join-Path $stage 'Resources\ShadersSlang\forward\forward.slang')) | Should Be $false
    }

    It 'fails the source closure when the owner path is removed' {
        $mutant = $copyLoop.Replace(", 'third_party\OxidANT\shaders'", '')
        ($mutant -ne $copyLoop) | Should Be $true
        $stage = Invoke-StageSourceCopyFixture -Loop $mutant -Root (Join-Path $TestDrive 'mutant')
        (Test-Path -LiteralPath (Join-Path $stage 'third_party\OxidANT\shaders\forward\forward.slang')) | Should Be $false
    }

    It 'fails the source closure when only compiled Vulkan shaders are staged' {
        $mutant = $copyLoop.Replace("'Resources\ShadersSlang'", "'Resources\ShadersSlang\build'")
        ($mutant -ne $copyLoop) | Should Be $true
        $stage = Invoke-StageSourceCopyFixture -Loop $mutant -Root (Join-Path $TestDrive 'compiled-only')
        (Test-Path -LiteralPath (Join-Path $stage 'Resources\ShadersSlang\rasterizer\rasterizer.slang')) | Should Be $false
    }
}
