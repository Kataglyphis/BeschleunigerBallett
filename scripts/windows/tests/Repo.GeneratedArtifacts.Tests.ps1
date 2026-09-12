#requires -Version 7.0

# Guards against generated artifacts sneaking into this repo's git index.
#
# The check is generic and was upstreamed on 2026-08-07 to ANTfrastructure's
# WindowsRepoHygiene.Common (Get-TrackedIgnoredFile, with its own suite). Only
# this repo's root stays here.
#
# Why it exists: `docs/build/` was excluded by .gitignore (twice, in fact - a
# duplicate rule at both line 11 and line 85) yet 21 files under it plus a stray
# __pycache__/*.pyc were already tracked, because .gitignore only stops NEW
# files from being added - it does nothing once a path is already indexed. The
# result: git kept serving a two-month-stale docs/build/html/ that CI
# regenerates from scratch on every run.
#
# Deliberately the general gate, not a docs/build-specific one: any future
# generated artifact that lands in the index while also being gitignored fails
# it too.
#
# NOTE: written for Pester 3.4.0 (what the Windows lane pins) - no BeforeAll
# outside Describe, and the dash-less assertion syntax.

Describe 'Repo generated artifacts' {

    . (Join-Path $PSScriptRoot '..\Resolve-BuildModule.ps1')
    Import-BuildModule 'WindowsRepoHygiene.Common'

    $repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path

    It 'has no tracked file that is also gitignored' {
        $tracked = @(Get-TrackedIgnoredFile -RepoRoot $repoRoot)

        if ($tracked.Count -gt 0) {
            Write-Host 'Tracked files that .gitignore also excludes (generated artifacts committed by mistake):'
            $tracked | ForEach-Object { Write-Host "  $_" }
            Write-Host 'Fix with: git rm --cached <path> for each file listed above - do not relax .gitignore.'
        }

        $tracked.Count | Should Be 0
    }

    It 'has no tracked file under a known generated-output path' {
        # The check above only sees files that are tracked AND ignored. An
        # artifact committed before anyone added the ignore rule is tracked and
        # NOT ignored, so it is invisible to it - which is exactly how
        # Testing/TAG, Testing/Temporary/CTestCostData.txt and a per-run
        # Test.xml stayed in this index (untracked and ignored 2026-08-07).
        #
        # These are git pathspecs, and the list is deliberately explicit: what
        # counts as generated is a property of this build, not something the
        # shared module can infer.
        $generated = @(
            'Testing/'          # CTest run output
            'docs/build/'       # Sphinx output
            '**/__pycache__/'
            '*.profraw'         # llvm coverage
            'logs/'
            'pipeline_cache/'
        )
        $tracked = @(Get-TrackedGeneratedArtifact -RepoRoot $repoRoot -Pattern $generated)

        if ($tracked.Count -gt 0) {
            Write-Host 'Tracked files under a generated-output path:'
            $tracked | ForEach-Object { Write-Host "  $_" }
            Write-Host 'Fix with: git rm -r --cached <path>, then add the path to .gitignore.'
        }

        $tracked.Count | Should Be 0
    }
}
