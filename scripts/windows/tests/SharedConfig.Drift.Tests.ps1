#requires -Version 7.0

# The PowerShell mirror of the lint-gates drift check, not the gate itself; see shared/config/README.md upstream. Pester 3.4.0.

Describe 'Shared tool config' {

    $repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
    $syncScript = Join-Path $repoRoot 'third_party\ANTfrastructure\shared\config\Sync-SharedConfig.ps1'

    It 'has the ANTfrastructure sync script available' {
        # Without this, a missing submodule makes the next test pass by never running.
        Test-Path $syncScript | Should Be $true
    }

    It 'matches the canonical copies in ANTfrastructure' {
        $output = & pwsh -NoProfile -File $syncScript -RepoRoot $repoRoot -Check 2>&1
        $exitCode = $LASTEXITCODE

        if ($exitCode -ne 0) {
            Write-Host 'Shared tool config has drifted from ANTfrastructure:'
            $output | ForEach-Object { Write-Host "  $_" }
        }

        $exitCode | Should Be 0
    }
}
