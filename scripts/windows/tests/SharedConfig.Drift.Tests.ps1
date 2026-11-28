#requires -Version 7.0

# The PowerShell mirror of the lint-gates drift check, not the gate itself; see shared/config/README.md upstream.

Describe 'Shared tool config' {
    BeforeAll {
        $script:repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
        $script:syncScript = Join-Path $script:repoRoot 'third_party\ANTfrastructure\shared\config\Sync-SharedConfig.ps1'
    }

    It 'has the ANTfrastructure sync script available' {
        # Without this, a missing submodule makes the next test pass by never running.
        Test-Path $script:syncScript | Should -Be $true
    }

    It 'matches the canonical copies in ANTfrastructure' {
        $output = & pwsh -NoProfile -File $script:syncScript -RepoRoot $script:repoRoot -Check 2>&1
        $exitCode = $LASTEXITCODE

        if ($exitCode -ne 0) {
            Write-Host 'Shared tool config has drifted from ANTfrastructure:'
            $output | ForEach-Object { Write-Host "  $_" }
        }

        $exitCode | Should -Be 0
    }
}
