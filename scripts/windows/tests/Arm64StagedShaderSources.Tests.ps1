#requires -Version 7.0

# This runs the source-copy loop; it does not cross-compile or execute the staged binaries.
Import-Module (Join-Path $PSScriptRoot '..\..\..\third_party\ANTfrastructure\windows\scripts\tests\TestHarness.psm1') -Function Get-TestSourceCopyLoopText, Invoke-TestSourceCopyLoop, Get-TestScriptCommands, Get-ScriptFunctionDefinition -ErrorAction Stop

function Assert-WgslEmissionWiring {
    param([Parameter(Mandatory)][string]$ScriptPath)
    foreach ($check in @(
        @{ Command = 'Get-SlangShaderManifest'; Text = 'manifestPath' }
        @{ Command = 'Join-Path'; Text = 'combined_$name' }
        @{ Command = 'Remove-Item'; Text = 'old -Force' }
        @{ Command = 'Copy-SlangPinnedWgslSnapshot'; Text = 'SnapshotRoot $snapshotRoot' }
        @{ Command = 'Assert-SlangWgslOutputsMatch'; Text = 'DestinationRoot $snapshotRoot' }
        @{ Command = 'Remove-Item'; Text = 'snapshotRoot -Recurse -Force' }
        @{ Command = 'New-Item'; Text = 'generated-wgsl' }
        @{ Command = 'Copy-Item'; Text = 'proof.Generated' }
    )) {
        $selected = @(Get-TestScriptCommands -ScriptPath $ScriptPath -Name $check.Command | Where-Object {
            $_.Extent.Text.Contains($check.Text)
        })
        if ($selected.Count -ne 1) { throw "Expected one $($check.Command) call with $($check.Text), found $($selected.Count)" }
    }
    $precompile = (Get-ScriptFunctionDefinition -ScriptPath $ScriptPath -FunctionName Invoke-SlangShaderPrecompile).ToString()
    $snapshotAt = $precompile.IndexOf('Copy-SlangPinnedWgslSnapshot -Mappings')
    $compilerAt = $precompile.IndexOf('& $compileSlangScript')
    $assertAt = $precompile.IndexOf('Assert-SlangWgslOutputsMatch -Mappings')
    if ($snapshotAt -lt 0 -or $compilerAt -le $snapshotAt -or $assertAt -le $compilerAt) {
        throw 'Expected the pinned WGSL snapshot before compilation and parity after compilation'
    }
}

function Invoke-StageSourceCopyFixture {
    param([Parameter(Mandatory)][string]$Loop, [Parameter(Mandatory)][string]$Root)
    Invoke-TestSourceCopyLoop -Loop $Loop -Root $Root -SourceVariableName workspacePath -DestinationVariableName tests -Files @{
        'Resources\ShadersSlang\build\spirv\fixture.spv' = 'compiled'
        'Resources\ShadersSlang\rasterizer\rasterizer.slang' = 'local'
        'Resources\Models\Dinosaurs\fixture.txt' = 'model'
        'Resources\Models\GltfTest\fixture.txt' = 'model'
        'Resources\Models\ShadowTest\fixture.txt' = 'model'
        'Resources\Models\VikingRoom\fixture.txt' = 'model'
        'Resources\Models\crytek-sponza\fixture.txt' = 'model'
        'third_party\OxidANT\shaders\forward\forward.slang' = 'owned'
    }
}

Describe 'ARM64 staged shader source closure' {
    $copyLoop = Get-TestSourceCopyLoopText -ScriptPath (Join-Path $PSScriptRoot '..\Build-Windows.ps1') -LoopVariable rel -ConditionContains 'Resources\ShadersSlang'

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

    # This wiring check cannot prove slangc ran; the shared parity fixture and CI compiler do.
    It 'requires fresh WGSL parity before staging and rejects a removed proof' {
        $driver = Join-Path $PSScriptRoot '..\Build-Windows.ps1'
        Assert-WgslEmissionWiring -ScriptPath $driver
        $source = Get-Content -LiteralPath $driver -Raw
        $mutant = $source.Replace('Assert-SlangWgslOutputsMatch -Mappings', 'Write-Output -Mappings')
        ($mutant -ne $source) | Should Be $true
        $path = Join-Path $TestDrive 'without-wgsl-proof.ps1'
        [IO.File]::WriteAllText($path, $mutant)
        $failure = $null
        try { Assert-WgslEmissionWiring -ScriptPath $path } catch { $failure = $_.Exception.Message }
        $failure | Should Match 'Expected one Assert-SlangWgslOutputsMatch call'
        $mutant = $source.Replace('"combined_$name"', '"combined_*"')
        ($mutant -ne $source) | Should Be $true
        [IO.File]::WriteAllText($path, $mutant)
        $failure = $null
        try { Assert-WgslEmissionWiring -ScriptPath $path } catch { $failure = $_.Exception.Message }
        $failure | Should Match 'Expected one Join-Path call'

        $mutant = $source.Replace('-CombinedOutputDir $combinedOutputDir -DestinationRoot $snapshotRoot',
            '-CombinedOutputDir $combinedOutputDir -DestinationRoot $repoRoot')
        ($mutant -ne $source) | Should Be $true
        [IO.File]::WriteAllText($path, $mutant)
        $failure = $null
        try { Assert-WgslEmissionWiring -ScriptPath $path } catch { $failure = $_.Exception.Message }
        $failure | Should Match 'Expected one Assert-SlangWgslOutputsMatch call'
    }
}
