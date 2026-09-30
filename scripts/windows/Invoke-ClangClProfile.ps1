<#
.SYNOPSIS
Starts the compiled executable inside the profile directory and sets necessary environment variables.
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
$ProfileDir = Join-Path $ProjectRoot "build-clangcl-profile"

# The project root is the working directory so images/ and Resources/ resolve.
Invoke-AppRun -BuildRoot $ProfileDir `
    -ExecutableName $ExeName `
    -Configurations @('Profile', 'RelWithDebInfo') `
    -WorkingDirectory $ProjectRoot `
    -ExeArgs $ExeArgs `
    -Label 'profile' `
    -NotFoundHint 'Please build the profile preset first.' `
    -EnvHook {
        # Force Vulkan to use the Proprietary AMD Driver to prevent AMD open-source driver access violations
        $ProprietaryDriver = "C:\WINDOWS\System32\DriverStore\FileRepository\u0198974.inf_amd64_dcac9659486b668a\B025819\amd-vulkan64.json"
        if (Test-Path $ProprietaryDriver) {
            $env:VK_ICD_FILENAMES = $ProprietaryDriver
        }
    }

exit $LASTEXITCODE
