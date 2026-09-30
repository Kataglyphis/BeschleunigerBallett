#requires -Version 7.0

# Copy of ANTfrastructure's shared/windows/templates/Resolve-BuildModule.ps1; the sync gate holds it from Set-StrictMode down.

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

    # A bare name means .psm1; a .ps1 is a dot-source helper: . (Resolve-BuildModule -Name 'Initialize-CiEnvironment.ps1')
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

    # Naming both paths shows the usual cause, a submodule that is not checked out.
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
        Imports with -Force -Global in the order given, so list modules in dependency order:
        forcing a dependency after its dependents yanks it back out of the global session.
    #>
    param(
        [Parameter(Mandatory)]
        [string[]] $Name
    )

    foreach ($moduleName in $Name) {
        if ($moduleName.EndsWith('.ps1', [System.StringComparison]::OrdinalIgnoreCase)) {
            # Import-Module on a .ps1 runs it in a throwaway scope, a silent no-op.
            throw "'$moduleName' is a dot-source script, not a module. Use: . (Resolve-BuildModule -Name '$moduleName')"
        }
        Import-Module (Resolve-BuildModule -Name $moduleName) -Force -Global -DisableNameChecking
    }

    # Always import WindowsScripts.Shared: a module's nested Import-Module stays private to that module.
    Import-Module (Resolve-BuildModule -Name 'WindowsScripts.Shared') -Force -Global -DisableNameChecking
}
