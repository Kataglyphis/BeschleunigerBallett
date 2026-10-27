#requires -Version 7.0
# This checks the MSIX wiring and license payload, not pixels in a built MSIX.
Import-Module (Join-Path $PSScriptRoot '..\..\..\third_party\ANTfrastructure\windows\scripts\tests\TestHarness.psm1') `
  -Function ConvertTo-TestScriptAst, Get-TestAstCommands, Get-TestCommandArguments, Get-ScriptFunctionDefinition, `
  Assert-Throws, New-TestZipFile, New-TestDir, Remove-TestDir -ErrorAction Stop
Import-Module (Join-Path $PSScriptRoot '..\..\..\third_party\ANTfrastructure\windows\scripts\modules\WindowsMsiLicense.Common.psm1') -ErrorAction Stop

function Assert-MsixSource([System.Management.Automation.Language.Ast]$Ast) {
  $commands = @{}
  foreach ($name in 'Invoke-MsixPackage', 'Invoke-BuildStep', 'Assert-MsixLicenseContents') {
    $commands[$name] = @(Get-TestAstCommands -Ast $Ast -Name $name)
  }
  $calls = $commands['Invoke-MsixPackage']
  if ($calls.Count -ne 1) { throw 'Expected one MSIX packaging call' }
  $bindings = Get-TestCommandArguments -Command $calls[0]
  if ($bindings.ContainsKey('GenerateTransparentLogos')) { throw 'MSIX logo must not be transparent' }
  if ($bindings.LogoPath -ne "(Join-Path `$workspacePath 'images\logo.png')") { throw 'Expected the project logo' }
  $steps = @($commands['Invoke-BuildStep'] | Where-Object { $_.Extent.Text.Contains("-StepName 'MSIX packaging'") })
  if ($steps.Count -ne 1 -or -not (Get-TestCommandArguments -Command $steps[0]).ContainsKey('Critical')) { throw 'MSIX packaging must be a critical build step' }
  $packages = $calls
  $checks = $commands['Assert-MsixLicenseContents']
  if ($packages.Count -ne 1 -or $checks.Count -ne 1 -or $checks[0].Extent.StartOffset -le $packages[0].Extent.EndOffset) { throw 'MSIX must be checked after packaging' }
  $bindings = Get-TestCommandArguments -Command $checks[0]
  if ($bindings.Path -ne '$msixOutPath' -or $bindings.WorkspacePath -ne '$workspacePath' -or $bindings.BuildPath -ne '$buildPathClangRelease') {
    throw 'MSIX license check must use the finished artifact, project sources and release build'
  }
}

Describe 'MSIX project logo wiring' {
  $source = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\Build-Windows.ps1') -Raw

  It 'passes the project logo to the shared packager' { Assert-MsixSource (ConvertTo-TestScriptAst -Text $source) }

  It 'rejects the former transparent logo option' {
    $mutant = $source.Replace("-LogoPath (Join-Path `$workspacePath 'images\logo.png')", '-GenerateTransparentLogos')
    if ($mutant -eq $source) { throw 'transparent logo mutation did not apply' }
    Assert-Throws -Body { Assert-MsixSource (ConvertTo-TestScriptAst -Text $mutant) } -MessagePattern 'transparent'
  }

  It 'requires a critical step and checks the finished MSIX' {
    Assert-MsixSource (ConvertTo-TestScriptAst -Text $source)
    $call = 'Assert-MsixLicenseContents -Path $msixOutPath -WorkspacePath $workspacePath -BuildPath $buildPathClangRelease'
    Assert-Throws -Body { Assert-MsixSource (ConvertTo-TestScriptAst -Text ($source.Replace($call, "Write-Output 'removed'"))) } -MessagePattern 'MSIX must be checked'
    $step = "Invoke-BuildStep -Context `$context -StepName 'MSIX packaging' -Critical -Script"
    Assert-Throws -Body { Assert-MsixSource (ConvertTo-TestScriptAst -Text ($source.Replace($step, "Invoke-BuildOptional -Context `$context -Name 'MSIX packaging' -Script"))) } -MessagePattern 'critical build step'
  }

  It 'rejects each missing or altered original license in a real ZIP-shaped MSIX' {
    $workspace = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
    . (Get-ScriptFunctionDefinition -ScriptPath (Join-Path $PSScriptRoot '..\Build-Windows.ps1') -FunctionName 'Assert-MsixLicenseContents')
    $dir = New-TestDir -Prefix 'ballett-msix-license-'
    $build = Join-Path $dir 'build'
    $licenses = 'share/licenses/GraphicsEngine'
    $members = [ordered]@{
      "$licenses/LICENSE" = (Join-Path $workspace 'LICENSE')
      'Resources/Models/crytek-sponza/CC-BY-3.0.txt' = (Join-Path $workspace 'licenses/CC-BY-3.0.txt')
      'Resources/Models/crytek-sponza/crytek-sponza.NOTICE' = (Join-Path $workspace 'licenses/crytek-sponza.NOTICE')
      "$licenses/third_party/gsl/LICENSE" = (Join-Path $build '_deps/gsl-src/LICENSE')
      "$licenses/third_party/abseil/LICENSE" = (Join-Path $build '_deps/abseil-cpp-src/LICENSE')
      "$licenses/cargo/manifest.json" = (Join-Path $build 'cargo-licenses/manifest.json')
    }
    foreach ($line in @(Get-Content -LiteralPath (Join-Path $workspace 'licenses/third-party-licenses.txt') | Where-Object { $_ })) {
      $installed, $text = $line -split '=', 2
      $members["$licenses/third_party/$installed"] = Join-Path $workspace $text
    }
    try {
      foreach ($generated in 'gsl', 'abseil', 'cargo') {
        $path = @($members.Values | Where-Object { $_.StartsWith($build) -and $_.Contains($generated) })[0]
        New-Item -ItemType Directory -Force -Path (Split-Path $path -Parent) | Out-Null
        [IO.File]::WriteAllText($path, "$generated text")
      }
      $full = Join-Path $dir 'full.msix'
      New-TestZipFile -Path $full -Member $members -FileValues
      Assert-MsixLicenseContents -Path $full -WorkspacePath $workspace -BuildPath $build
      $altered = Join-Path $dir 'altered.txt'
      [IO.File]::WriteAllText($altered, 'incorrect text')
      $sampled = @($members.Keys)[0..5] + @("$licenses/third_party/imgui/LICENSE.txt", "$licenses/third_party/imgui-fonts/OFL-1.1.txt")
      foreach ($name in $sampled) {
        $partial = [ordered]@{}
        foreach ($key in $members.Keys) { if ($key -ne $name) { $partial[$key] = $members[$key] } }
        $broken = Join-Path $dir ([guid]::NewGuid().ToString() + '.msix')
        New-TestZipFile -Path $broken -Member $partial -FileValues
        Assert-Throws -Body { Assert-MsixLicenseContents -Path $broken -WorkspacePath $workspace -BuildPath $build } -MessagePattern 'package license verification failed'
        $partial[$name] = $altered
        $changed = Join-Path $dir ([guid]::NewGuid().ToString() + '.msix')
        New-TestZipFile -Path $changed -Member $partial -FileValues
        Assert-Throws -Body { Assert-MsixLicenseContents -Path $changed -WorkspacePath $workspace -BuildPath $build } -MessagePattern 'package license verification failed'
      }
    } finally { Remove-TestDir -Path $dir }
  }
}
