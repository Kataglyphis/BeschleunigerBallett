#requires -Version 7.0
<#
.SYNOPSIS
Runs the local clang-cl debug test flow, optional fuzz tests, and then launches the app.
#>

param (
    [string]$ExeName = "GraphicsEngine.exe",
    [switch]$SkipTests,
    [switch]$SkipFuzzTests,
    [switch]$RunVulkanIntegrationTest,
    [switch]$SkipAppLaunch,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ExeArgs
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

# Own flow rather than Invoke-AppRun: tests and fuzz executables run before the launch.
. (Join-Path $PSScriptRoot 'Resolve-BuildModule.ps1')
Import-BuildModule @('WindowsScripts.Shared', 'WindowsBuild.Common', 'WindowsCMake.Common',
                     'WindowsAppRunner.Common', 'WindowsTesting.Common')

$ProjectRoot = (Resolve-Path "$PSScriptRoot\..\..").Path
$DebugDir = Join-Path $ProjectRoot 'build-clangcl-debug'

# The Debug build requests VK_LAYER_KHRONOS_validation and aborts without it: a VK_LAYER_PATH holding it wins, then the SDK.
function Resolve-ValidationLayerDirectory {
    $candidates = @($env:VK_LAYER_PATH, $(if ($env:VULKAN_SDK) { Join-Path $env:VULKAN_SDK 'Bin' }))
    if (Test-Path -LiteralPath 'C:\VulkanSDK') {
        $byVersion = { $version = $null; if ([version]::TryParse($_.Name, [ref]$version)) { $version } else { [version]'0.0' } }
        $candidates += @(Get-ChildItem -LiteralPath 'C:\VulkanSDK' -Directory | Sort-Object $byVersion -Descending |
                ForEach-Object { Join-Path $_.FullName 'Bin' })
    }
    foreach ($dir in $candidates) {
        if ($dir -and (Test-Path -LiteralPath (Join-Path $dir 'VkLayer_khronos_validation.json'))) { return $dir }
    }
    return $null
}
$FuzzDir = $DebugDir

# -Required throws naming the missing tool here, not later on a $null path.
$cmakeExePath = Get-PreferredToolPath -Required -CommandName 'cmake.exe' -CandidatePaths @(
    'C:\Program Files\CMake\bin\cmake.exe'
)
$ctestExePath = Get-PreferredToolPath -Required -CommandName 'ctest.exe' -CandidatePaths @(
    'C:\Program Files\CMake\bin\ctest.exe'
)
$clangClExePath = Get-PreferredToolPath -Required -CommandName 'clang-cl.exe' -CandidatePaths @(
    'C:\Program Files\LLVM\bin\clang-cl.exe',
    (Join-Path $env:USERPROFILE 'scoop\apps\llvm\current\bin\clang-cl.exe')
)

Add-DirectoriesToPath @(
    $DebugDir,
    (Join-Path $DebugDir 'bin'),
    $FuzzDir,
    (Join-Path $FuzzDir 'bin'),
    (Join-Path $ProjectRoot 'bin')
)

$context = New-BuildContext -Workspace $ProjectRoot -LogDir 'logs\windows' -StopOnError
$currentCMakeShareDir = Get-CMakeShareDir -CMakeExePath $cmakeExePath

if (-not $ctestExePath) {
    throw "ctest.exe not found. Install CMake or add it to PATH before running this script."
}

if (-not $clangClExePath) {
    Write-BuildLogWarning -Context $context -Message 'clang-cl.exe was not found on PATH or in the default LLVM install locations. Clang ASan runtime discovery may fail for fuzz tests.'
}

$WorkDir = $ProjectRoot
$script:appExitCode = 0

Open-BuildLog -Context $context

try {
    Set-Location -Path $WorkDir

    if (-not $SkipTests) {
        Write-BuildLog -Context $context -Message "Running tests via CTest in $DebugDir..."
        Update-CTestMetadataPaths -BuildRoot $DebugDir -WorkspaceRoot $ProjectRoot -CMakeShareDir $currentCMakeShareDir
        $excludeRegex = @()
        if (-not $RunVulkanIntegrationTest) {
            $excludeRegex += '^Integration\.VulkanEngine$'
            Write-BuildLogWarning -Context $context -Message 'Skipping Integration.VulkanEngine in the local host runner. Use -RunVulkanIntegrationTest to include it explicitly.'
        }

        Invoke-CtestDiscoveredTests -Context $context -BuildRoot $DebugDir -Configuration 'Debug' -ExcludeRegex $excludeRegex -RuntimeFlavor 'Clang'

        if (-not $SkipFuzzTests) {
            if (Test-Path $FuzzDir) {
                Write-BuildLog -Context $context -Message "Running local fuzz executables in $FuzzDir..."

                foreach ($fuzzExecutable in @('first_fuzz_test.exe', 'example_fuzz_test.exe', 'obj_parsing_fuzz_test.exe', 'gltf_parsing_fuzz_test.exe', 'scene_config_fuzz_test.exe', 'shader_file_reader_fuzz_test.exe', 'texture_loading_fuzz_test.exe')) {
                    $resolvedFuzzExecutable = Resolve-TestExecutable -BuildRoot $FuzzDir -ExecutableName $fuzzExecutable
                    if (-not $resolvedFuzzExecutable) {
                        throw "Expected fuzz executable '$fuzzExecutable' was not found inside $FuzzDir."
                    }

                    $ranFuzzExecutable = Invoke-ManualTestExecutable -Context $context -BuildRoot $FuzzDir -ExecutableName $fuzzExecutable -RuntimeFlavor 'Clang'
                    if (-not $ranFuzzExecutable) {
                        throw "Fuzz executable '$fuzzExecutable' did not start successfully. Check the Clang ASan runtime setup."
                    }
                }
            } else {
                Write-BuildLogWarning -Context $context -Message "Fuzz build directory '$FuzzDir' does not exist. Skipping fuzz executable run."
            }
        }
    }

    if ($SkipAppLaunch) {
        Write-BuildLog -Context $context -Message 'Skipping application launch because -SkipAppLaunch was requested.'
        return
    }

    $ExePath = Resolve-AppExecutablePath -BuildRoot $DebugDir -ExecutableName $ExeName -Configurations @('Debug')
    if (-not $ExePath) {
        throw "Executable '$ExeName' not found inside $DebugDir. Run 'Build-Windows.ps1 -Configurations clangcl-debug' first."
    }

    Write-BuildLog -Context $context -Message "Starting $ExePath..."
    Write-BuildLog -Context $context -Message "Working Directory: $WorkDir"

    # Force Vulkan to use the proprietary AMD driver instead of the AMDVLK open-source driver.
    $proprietaryDriver = 'C:\WINDOWS\System32\DriverStore\FileRepository\u0198974.inf_amd64_dcac9659486b668a\B025819\amd-vulkan64.json'
    if (Test-Path $proprietaryDriver) {
        $env:VK_ICD_FILENAMES = $proprietaryDriver
    }

    # Clearing VK_LAYER_PATH used to hide the SDK's layer, so every launch crashed and "run again" could not help.
    $layerDir = Resolve-ValidationLayerDirectory
    if ($layerDir) {
        $env:VK_LAYER_PATH = $layerDir
        Write-BuildLog -Context $context -Message "Validation layer: $layerDir"
    } else {
        Write-BuildLogWarning -Context $context -Message 'No VkLayer_khronos_validation.json in VK_LAYER_PATH, VULKAN_SDK\Bin or C:\VulkanSDK\*\Bin; the Debug build will abort at instance creation (install the SDK: winget install KhronosGroup.VulkanSDK).'
    }
    $env:VK_INSTANCE_LAYERS = ''

    # Unlike the test runs: silences GUI/driver global-init and RTL-allocator noise.
    Invoke-WithAsanOptions -Options 'log_path=logs/asan.log:report_globals=0:windows_hook_rtl_allocators=false' -Script {
        if ($ExeArgs) {
            & $ExePath $ExeArgs
        } else {
            & $ExePath
        }

        $exitCode = $LASTEXITCODE
        if ($exitCode -eq -1073740791) {
            Write-Error "The app aborted at startup (0xC0000409), as when the validation layers are missing; VK_LAYER_PATH was '$env:VK_LAYER_PATH'."
        } elseif ($exitCode -ne 0) {
            Write-BuildLogWarning -Context $context -Message "Process failed with exit code $exitCode"
        }

        $script:appExitCode = $exitCode
    }
} finally {
    Close-BuildLog -Context $context
}

exit $script:appExitCode

