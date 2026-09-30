#requires -Version 7.0

# .gitignore never untracks an indexed file, so generated output can linger. Pester 3.4.0: no BeforeAll outside Describe.

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
        # Catches artifacts committed before any ignore rule, which the check above cannot see.
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
