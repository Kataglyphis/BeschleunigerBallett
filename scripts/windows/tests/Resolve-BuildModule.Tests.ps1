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
      # WindowsTesting.Common and WindowsClang.Common were vendored here until
      # 2026-08-11, when the two-consumer test moved them into ANTfrastructure
      # (OmniAccelerANT needed the same ASan-runtime discovery). The vendored
      # copies were then deleted -- and nothing else had to change, because the
      # preference order below picks up the upstream copy automatically. That
      # automatic pickup is the property this asserts.
      $expectedRoot = Join-Path $script:repoRoot 'third_party\ANTfrastructure\windows\scripts\modules'
      foreach ($moduleName in @('WindowsTesting.Common', 'WindowsClang.Common')) {
        Resolve-BuildModulePath -Name $moduleName | Should Be (Join-Path $expectedRoot "$moduleName.psm1")
      }
    }
  }

  Context 'Resolve-BuildModulePath failure mode' {
    It 'throws and names both searched locations when a module exists nowhere' {
      # Explicit try/catch instead of 'Should Throw': Pester 3.4.0 (in-box
      # Windows version) fails to observe the exception from this module call.
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
    It 'vendors nothing: every module this repo uses now lives upstream' {
      # The rule this guards is "no consumer copy of anything that exists
      # upstream". It is deliberately an EMPTY-set assertion rather than a
      # deleted test: a new .psm1 appearing here should have to justify itself
      # by failing this, not slip in unnoticed.
      $vendoredDir = Join-Path $PSScriptRoot '..\modules'
      $actual = @(Get-ChildItem -Path $vendoredDir -Filter '*.psm1' -ErrorAction SilentlyContinue |
        Select-Object -ExpandProperty Name | Sort-Object)
      $actual.Count | Should Be 0
    }
  }

  Context 'Import-BuildModule Shared guarantee' {
    It 'exposes WindowsScripts.Shared exports even when the caller does not name it' {
      # NOT a shadowing problem (the old comment here said so): a nested
      # Import-Module inside a .psm1 binds into THAT module's private scope and
      # never reaches the importing session. So importing WindowsBuild.Common
      # alone yields Write-BuildLog but not Resolve-WorkspacePath. The template's
      # unconditional Shared import is what closes that gap.
      Import-BuildModule @('WindowsBuild.Common')
      (Get-Command Resolve-WorkspacePath -ErrorAction SilentlyContinue) | Should Not Be $null
      (Get-Command Add-DirectoriesToPath -ErrorAction SilentlyContinue) | Should Not Be $null
    }
  }
}
