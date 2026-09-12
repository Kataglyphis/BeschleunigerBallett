#requires -Version 7.0

# Guards this repo's copies of the shared tool configs against drift.
#
# .clang-format, .clang-tidy, gcovr.cfg and .pre-commit-config.yaml are owned by
# ANTfrastructure (shared/config/). They are COPIED here rather than referenced
# because clang-format, clang-tidy and pre-commit find their config by walking
# UP from the file being processed - a config inside the submodule is never
# found, and deleting the local copy would silently stop format-on-save in every
# editor while CI kept passing. See shared/config/README.md upstream.
#
# So the copies stay and this makes drift impossible instead of unnoticed:
# edit upstream, then re-run the sync script with -Write.
#
# NOTE: written for Pester 3.4.0 (what the Windows lane pins) - no BeforeAll
# outside Describe, and the dash-less assertion syntax.

Describe 'Shared tool config' {

    $repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
    $syncScript = Join-Path $repoRoot 'third_party\ANTfrastructure\shared\config\Sync-SharedConfig.ps1'

    It 'has the ANTfrastructure sync script available' {
        # A missing script means the submodule is not checked out; without this
        # the next test would "pass" by never running the check.
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
