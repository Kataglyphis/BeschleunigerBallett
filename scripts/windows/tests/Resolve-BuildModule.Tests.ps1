#requires -Version 7.0

Describe 'Resolve-BuildModule' {
  BeforeAll {
    . (Join-Path $PSScriptRoot '..\Resolve-BuildModule.ps1')
    $script:repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
  }

  Context 'Resolve-BuildModulePath preference order' {
    It 'prefers the ANTfrastructure upstream copy for a module that lives there' {
      $resolved = Resolve-BuildModulePath -Name 'WindowsScripts.Shared'
      $expectedRoot = Join-Path $script:repoRoot 'third_party\ANTfrastructure\windows\scripts\modules'
      $resolved | Should Be (Join-Path $expectedRoot 'WindowsScripts.Shared.psm1')
    }

    It 'resolves the once-vendored modules upstream now that they were moved there' {
      # Asserts the preference order picks up a moved module upstream with no other change.
      $expectedRoot = Join-Path $script:repoRoot 'third_party\ANTfrastructure\windows\scripts\modules'
      foreach ($moduleName in @('WindowsTesting.Common', 'WindowsClang.Common')) {
        Resolve-BuildModulePath -Name $moduleName | Should Be (Join-Path $expectedRoot "$moduleName.psm1")
      }
    }
  }

  Context 'Resolve-BuildModulePath failure mode' {
    It 'throws and names both searched locations when a module exists nowhere' {
      # Not 'Should Throw': Pester 3.4.0 misses the exception from this module call.
      $threw = $false
      $message = $null
      try {
        Resolve-BuildModulePath -Name 'NoSuchModule' | Out-Null
      } catch {
        $threw = $true
        $message = $_.Exception.Message
      }

      $threw | Should Be $true
      $message | Should Match ([regex]::Escape('third_party\ANTfrastructure\windows\scripts\modules\NoSuchModule.psm1'))
      $message | Should Match ([regex]::Escape('scripts\windows\modules\NoSuchModule.psm1'))
    }
  }

  Context 'Vendored fallback directory contents' {
    It 'holds no local module that also exists upstream' {
      # A local name ANTfrastructure also ships is a vendored copy; only project modules belong here.
      $vendoredDir = Join-Path $PSScriptRoot '..\modules'
      $upstreamDir = Join-Path $PSScriptRoot '..\..\..\third_party\ANTfrastructure\windows\scripts\modules'
      $local = @(Get-ChildItem -Path $vendoredDir -Filter '*.psm1' -ErrorAction SilentlyContinue |
        Select-Object -ExpandProperty Name)
      $upstream = @(Get-ChildItem -Path $upstreamDir -Filter '*.psm1' -ErrorAction SilentlyContinue |
        Select-Object -ExpandProperty Name)
      $shadowed = @($local | Where-Object { $upstream -contains $_ })
      $shadowed.Count | Should Be 0
    }
  }

  Context 'Import-BuildModule Shared guarantee' {
    It 'exposes WindowsScripts.Shared exports even when the caller does not name it' {
      # A nested Import-Module stays module-private; the template's unconditional Shared import closes that gap.
      Import-BuildModule @('WindowsBuild.Common')
      (Get-Command Resolve-WorkspacePath -ErrorAction SilentlyContinue) | Should Not Be $null
      (Get-Command Add-DirectoriesToPath -ErrorAction SilentlyContinue) | Should Not Be $null
    }
  }
}
