#requires -Version 7.0

# This checks package CLI wiring and shared argument behavior; it does not run CMake or build an installer.
Describe 'Windows CMake package reuse' {
  . (Join-Path $PSScriptRoot '..\Resolve-BuildModule.ps1')
  Import-BuildModule @('WindowsBuild.Common', 'WindowsCMake.Common')
  Import-Module (Join-Path $PSScriptRoot '..\..\..\third_party\ANTfrastructure\windows\scripts\tests\TestHarness.psm1') -Function Get-TestScriptAst -ErrorAction Stop

  It 'uses the exported hub argument builder for the one Release package step' {
    $ast = Get-TestScriptAst -ScriptPath (Join-Path $PSScriptRoot '..\Build-Windows.ps1')
    $calls = @($ast.FindAll({ param($node) $node -is [System.Management.Automation.Language.CommandAst] -and $node.GetCommandName() -eq 'Get-CmakeBuildArgs' }, $true))
    $calls.Count | Should Be 1
    ($calls[0].Extent.Text -match "-Target 'package'") | Should Be $true
    @($ast.FindAll({ param($node) $node -is [System.Management.Automation.Language.StringConstantExpressionAst] -and $node.Value -eq '--parallel' }, $true)).Count | Should Be 0
    (Get-Command Get-CmakeBuildArgs -ErrorAction Stop).ModuleName | Should Be 'WindowsCMake.Common'
  }

  It 'keeps the package target and both parallel argument modes' {
    $context = [pscustomobject]@{ LogWriter = $null; SuppressConsoleOutput = $true }
    $limited = @(Get-CmakeBuildArgs -Context $context -BuildPath 'C:\build' -Target 'package' -Configuration 'Release' -ParallelJobs 3 -CpuCount 8)
    ($limited -join ' ') | Should Be '--build C:\build --target package --config Release --parallel 3'
    $unlimited = @(Get-CmakeBuildArgs -Context $context -BuildPath 'C:\build' -Target 'package' -Configuration 'Release' -ParallelJobs 0 -CpuCount 8)
    ($unlimited -join ' ') | Should Be '--build C:\build --target package --config Release --parallel'
  }
}
