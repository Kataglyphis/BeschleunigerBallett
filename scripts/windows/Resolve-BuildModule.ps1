#requires -Version 7.0

# Copied verbatim from ANTfrastructure
# `shared/windows/templates/Resolve-BuildModule.ps1` — do not hand-edit; sync
# from upstream instead. This is the one build-tooling file that cannot be
# imported out of the submodule, because it is what *finds* the submodule: it
# runs before anything upstream is importable.
#
# Contract (ANTfrastructure docs/adopting-in-a-new-project.md § 1):
#
#   1. third_party/ANTfrastructure/windows/scripts/modules/<Name>.psm1
#   2. <this script's directory>/modules/<Name>.psm1   (project-specific fallback)
#   3. throw, naming BOTH probed paths
#
# That preference order is the whole point: put a module upstream and it wins
# automatically, so this repo never silently keeps building against a stale
# vendored copy. Keep ONLY genuinely project-specific modules in the local
# fallback directory.
#
# WATCHED since 2026-09-09: this copy is registry row `resolve-build-module`
# (body mode) in ANTfrastructure `shared/config/shared-assets.manifest`, and this
# repo declares it in `.antfrastructure-shared.manifest` at its root. Everything
# from `Set-StrictMode` down is compared against the canonical template; only
# this header prose and the VALUE of $script:RepoRootRelativeToHere are local.
# Check it with:
#   bash third_party/ANTfrastructure/shared/config/sync-shared-config.sh --repo-root . --check

Set-StrictMode -Version Latest

$script:RepoRootRelativeToHere = '..\..'

$script:BuildModuleSearchRoots = @(
    [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot (Join-Path $script:RepoRootRelativeToHere 'third_party\ANTfrastructure\windows\scripts\modules'))),
    [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'modules'))
)

function Get-BuildModuleSearchRoot {
    <#
    .SYNOPSIS
        The module search roots in preference order (ANTfrastructure first).
    #>
    return @($script:BuildModuleSearchRoots)
}

function Resolve-BuildModule {
    <#
    .SYNOPSIS
        Resolves a build-module name to its .psm1, ANTfrastructure first.
    .PARAMETER Name
        Module name with or without the .psm1 suffix, e.g. 'WindowsBuild.Common'.
    #>
    param(
        [Parameter(Mandatory)]
        [string] $Name
    )

    # An explicit .psm1 OR .ps1 extension is honoured; a bare name means .psm1.
    # The .ps1 arm is what makes the repo's dot-sourced helpers reachable at all:
    # ANTfrastructure ships windows/scripts/modules/Initialize-CiEnvironment.ps1
    # (New-CiSession + the Write-CiLog family), and because this resolver used to
    # append '.psm1' unconditionally, every consumer hand-rolled that CI-session
    # preamble instead. Dot-source it:
    #     . (Resolve-BuildModule -Name 'Initialize-CiEnvironment.ps1')
    $known = @('.psm1', '.ps1')
    $hasExt = $known | Where-Object { $Name.EndsWith($_, [System.StringComparison]::OrdinalIgnoreCase) }
    $fileName = if ($hasExt) { $Name } else { "$Name.psm1" }

    $probed = [System.Collections.Generic.List[string]]::new()
    foreach ($root in $script:BuildModuleSearchRoots) {
        $candidate = Join-Path $root $fileName
        $probed.Add($candidate)
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }

    # Naming both paths is part of the contract: the failure mode is almost
    # always "submodule not checked out", and the first probed path says so.
    throw ("Build module '$Name' not found. Probed:" + [Environment]::NewLine +
        '  ' + ($probed -join ([Environment]::NewLine + '  ')) + [Environment]::NewLine +
        'If the ANTfrastructure path is missing, the submodule is not checked out: ' +
        'git submodule update --init --recursive third_party/ANTfrastructure')
}

# Back-compat alias for consumers that adopted the earlier name.
function Resolve-BuildModulePath {
    param([Parameter(Mandatory)][string] $Name)
    return (Resolve-BuildModule -Name $Name)
}

function Import-BuildModule {
    <#
    .SYNOPSIS
        Resolves and imports build modules into the caller's global session.
    .DESCRIPTION
        Imports with -Force -Global, in the order given. List modules in
        DEPENDENCY ORDER (WindowsScripts.Shared and WindowsBuild.Common first):
        ANTfrastructure's modules pull their own dependencies in with a plain,
        guarded Import-Module, so one forced top-level import gives every module
        the same copy, whereas forcing a dependency *after* its dependents can
        yank it back out of the global session state — the shadowing pitfall
        WindowsCMake.Common's header warns about.
    #>
    param(
        [Parameter(Mandatory)]
        [string[]] $Name
    )

    foreach ($moduleName in $Name) {
        if ($moduleName.EndsWith('.ps1', [System.StringComparison]::OrdinalIgnoreCase)) {
            # Import-Module on a plain .ps1 runs it in a throwaway scope and defines
            # nothing for the caller -- a silent no-op. Dot-source those instead.
            throw "'$moduleName' is a dot-source script, not a module. Use: . (Resolve-BuildModule -Name '$moduleName')"
        }
        Import-Module (Resolve-BuildModule -Name $moduleName) -Force -Global -DisableNameChecking
    }

    # WindowsScripts.Shared ALWAYS, whether or not the caller named it.
    #
    # A nested `Import-Module` inside a .psm1 binds into THAT MODULE'S private
    # scope; it does not reach the importing session. So a script that imports
    # only WindowsBuild.Common gets Write-BuildLog but NOT Resolve-WorkspacePath,
    # Add-DirectoriesToPath or the other Shared exports WindowsBuild.Common
    # itself depends on — verified 2026-08-11: `Get-Command Resolve-WorkspacePath`
    # comes back empty in exactly that setup. Every consumer hit this and
    # "fixed" it by remembering to list Shared first; doing it here means they
    # cannot forget. Shared is dependency-free, so the extra import is safe in
    # any position and costs one small module load.
    Import-Module (Resolve-BuildModule -Name 'WindowsScripts.Shared') -Force -Global -DisableNameChecking
}
