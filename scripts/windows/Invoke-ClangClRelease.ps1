<#
.SYNOPSIS
Starts the compiled executable inside the release directory and sets necessary environment variables.
#>

#requires -Version 7.0

param (
    [string]$ExeName = "GraphicsEngine.exe",
    [Parameter(ValueFromRemainingArguments=$true)]
    [string[]]$ExeArgs
)

$ErrorActionPreference = "Stop"

$ProjectRoot = (Resolve-Path "$PSScriptRoot\..\..").Path

. (Join-Path $PSScriptRoot 'Resolve-BuildModule.ps1')
Import-BuildModule @('WindowsAppRunner.Common')

# Locate the built executable in the build directory created by the CMake preset
$ReleaseDir = Join-Path $ProjectRoot "build-clangcl-release"

# The project root is the working directory so images/ and Resources/ resolve.
Invoke-AppRun -BuildRoot $ReleaseDir `
    -ExecutableName $ExeName `
    -Configurations @('Release') `
    -WorkingDirectory $ProjectRoot `
    -ExeArgs $ExeArgs `
    -Label 'release' `
    -NotFoundHint 'Build it first: Build-Windows.ps1 -Configurations clangcl-release (or Build-Windows-Container.ps1).'

exit $LASTEXITCODE
