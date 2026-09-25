#requires -Version 7.0
param(

  [string[]]$Configurations = @('all'),
  [switch]$SkipFormat,
  # Rewrite sources with clang-format/cmake-format instead of only reporting
  # drift. Off by default: see the formatting block below for why.
  [switch]$ApplyFormat,
  [switch]$SkipTidy,
  [switch]$SkipTests,
  [switch]$SkipPerfTests,
  [switch]$SkipMsix,
  [switch]$SkipBuild,
  [switch]$VerboseBuild,
  [int]$ParallelJobs = 0,
  [switch]$DisableSccache,
  [switch]$DisableIntegrationTestsMsvcDebug,
  # WebDAV: prefer explicit script parameters (these match CLI flags), fall back to env vars
  [string]$WebDavHostname,
  [string]$WebDavUsername,
  [string]$WebDavPassword,
  [string]$RemoteBasePath,
  [string]$LocalAssetsFolder,
  # amd64 (alias x64) or arm64. Empty: the image's WINDOWS_TARGET_ARCH, else amd64,
  # resolved by the hub's Get-WindowsTargetArch, which throws on anything else.
  # arm64 is the cross build of windows-arm64-cross.yml: clangcl-release only,
  # packaged to dist\windows-arm64 (third_party/ANTfrastructure/docs/windows-cross-builds.md).
  [string]$TargetArch = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Config helpers moved to WindowsConfig.Common.psm1

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

# Modules come from the ANTfrastructure submodule when available (preferred, so
# reusable scripts live upstream); modules its refactor removed are vendored
# in scripts/windows/modules. See Resolve-BuildModule.ps1.
. (Join-Path $PSScriptRoot 'Resolve-BuildModule.ps1')

# WindowsBuild.Common contains the logging primitives (formerly a separate
# WindowsLogging.Common module — now folded in upstream).
Import-BuildModule @(
  'WindowsScripts.Shared',
  'WindowsBuild.Common',
  'WindowsToolchain.Common',
  'WindowsUv.Common',
  'WindowsCodeQL.Common',
  'WindowsConfig.Common',
  'WindowsClang.Common',
  'WindowsWebDav.Common',
  'WindowsMsix.Common',
  'WindowsMsix.Signing',
  'WindowsCMake.Common',
  'WindowsFormatting.Common',
  'WindowsTesting.Common',
  'WindowsTargetArch.Common'
)
# The cross lanes' configure arguments, package arch and DLL closure. A hub pin older than
# the cross lanes lacks the module and says which commit it needs.
try { Import-BuildModule @('WindowsCrossBundle.Common') } catch {
  throw "This build needs ANTfrastructure's WindowsCrossBundle.Common (hub commit of 2026-09-25, third_party/ANTfrastructure/docs/windows-cross-builds.md); move third_party/ANTfrastructure to it or later. ($($_.Exception.Message))"
}
$TargetArch = Get-WindowsTargetArch -Arch $TargetArch
$isCross = Test-WindowsCrossTarget -Arch $TargetArch
$packageArch = Get-WindowsPackageArch -Arch $TargetArch

$defaultConfigPath = Join-Path $PSScriptRoot 'Build-Windows.config.psd1'
$configPath = Get-OrDefault $env:BUILD_WINDOWS_CONFIG $defaultConfigPath
if (-not (Test-Path $configPath)) {
  throw "Build config not found: $configPath"
}
$config = Import-PowerShellDataFile -Path $configPath

$workspaceRootEnvVar = Get-OrDefault $env:WORKSPACE_ROOT_ENV (Get-ConfigValue -Config $config -Path 'Build.WorkspaceRootEnv')
$workspaceEnvItem = Get-Item -Path "Env:$workspaceRootEnvVar" -ErrorAction SilentlyContinue
$workspaceRootFromEnv = if ($null -ne $workspaceEnvItem) { $workspaceEnvItem.Value } else { $null }
$workspaceRoot = Get-OrDefault $workspaceRootFromEnv $repoRoot
$workspacePath = Resolve-WorkspacePath -Path $workspaceRoot

$logDir = Get-OrDefault $env:BUILD_LOG_DIR (Get-ConfigValue -Config $config -Path 'Build.LogDir')

function Get-EnvironmentVariableValue {
  param([Parameter(Mandatory)][string]$Name)

  $envItem = Get-Item -Path "Env:$Name" -ErrorAction SilentlyContinue
  if ($null -eq $envItem) {
    return $null
  }

  return $envItem.Value
}

function Get-BuildConfigurationSpec {
  param(
    [Parameter(Mandatory)][string]$Name,
    [Parameter(Mandatory)]$Config,
    [Parameter(Mandatory)][string]$WorkspacePath
  )

  $configuration = Get-ConfigValue -Config $Config -Path "Build.Configurations.$Name"
  if ($null -eq $configuration) {
    throw "Build configuration '$Name' not found in $configPath"
  }

  $buildDir = Get-OrDefault (Get-EnvironmentVariableValue -Name $configuration['BuildDirEnv']) $configuration['BuildDir']
  $preset = Get-OrDefault (Get-EnvironmentVariableValue -Name $configuration['PresetEnv']) $configuration['Preset']

  return @{
    BuildPath = Join-Path $WorkspacePath $buildDir
    Preset = $preset
  }
}

$availableConfigurations = @('msvc-debug', 'msvc-release', 'clangcl-debug', 'clangcl-profile', 'clangcl-release')
$buildConfigurationSpecs = @{}
foreach ($configurationName in $availableConfigurations) {
  $buildConfigurationSpecs[$configurationName] = Get-BuildConfigurationSpec -Name $configurationName -Config $config -WorkspacePath $workspacePath
}

$buildPathMsvcDebug = $buildConfigurationSpecs['msvc-debug']['BuildPath']
$presetMsvcDebug = $buildConfigurationSpecs['msvc-debug']['Preset']
$buildPathMsvcRelease = $buildConfigurationSpecs['msvc-release']['BuildPath']
$presetMsvcRelease = $buildConfigurationSpecs['msvc-release']['Preset']
$buildPathClangDebug = $buildConfigurationSpecs['clangcl-debug']['BuildPath']
$presetClangDebug = $buildConfigurationSpecs['clangcl-debug']['Preset']
$buildPathClangProfile = $buildConfigurationSpecs['clangcl-profile']['BuildPath']
$presetClangProfile = $buildConfigurationSpecs['clangcl-profile']['Preset']
$buildPathClangRelease = $buildConfigurationSpecs['clangcl-release']['BuildPath']
$presetClangRelease = $buildConfigurationSpecs['clangcl-release']['Preset']

$selectedConfigurations = Get-SelectedConfigurations -Configurations $Configurations -AvailableConfigurations $availableConfigurations

# A cross build is clangcl-release only: Debug links an ASan runtime the bundle has no
# aarch64 copy of and runs FuzzTest's grammar generator during the build, Profile runs
# benchmarks, and the MSVC presets pin x64. Its tree sits beside the host's.
if ($isCross) {
  $notCross = @($selectedConfigurations | Where-Object { $_ -ne 'clangcl-release' })
  if ($notCross.Count -gt 0) {
    throw "-TargetArch $TargetArch builds clangcl-release only, not $($notCross -join ', ') (third_party/ANTfrastructure/docs/windows-cross-builds.md)."
  }
  $buildPathClangRelease = "$buildPathClangRelease-$TargetArch"
}
$distArch = Join-Path $workspacePath "dist\windows-$TargetArch"

# If SkipBuild is requested, clear any selected build configurations so
# configuration-specific configure/build steps are not executed. This keeps
# non-build steps such as formatting running.
if ($SkipBuild) {
  $selectedConfigurations.Clear()
}

$script:clangClVersionLogged = $false

function Invoke-ConfiguredBuild {
  param(
    [Parameter(Mandatory)][string]$BuildPath,
    [Parameter(Mandatory)][string]$Preset,
    [Parameter(Mandatory)][string]$Configuration,
    [string[]]$ConfigureExtraArgs = @()
  )

  Invoke-CmakeConfigureAndBuild -Context $context -BuildPath $BuildPath -Preset $Preset -Configuration $Configuration -CleanBuildRoot -ParallelJobs $ParallelJobs -VerboseOutput:$VerboseBuild -DisableSccache:$DisableSccache -ConfigureExtraArgs $ConfigureExtraArgs
}


function Invoke-SlangShaderPrecompile {
  param([Parameter(Mandatory)][string]$BuildLabel)

  $compileSlangScript = Join-Path $PSScriptRoot 'Build-SlangShaders.ps1'
  if (-not (Test-Path $compileSlangScript)) {
    Write-BuildLogWarning -Context $context -Message "Slang shader compile script not found: $compileSlangScript"
    return
  }

  Write-BuildLog -Context $context -Message "Precompiling Slang shaders ($BuildLabel)"
  & $compileSlangScript
}

function Assert-ClangClAvailable {
  if ($script:clangClVersionLogged) {
    return
  }

  $clangClCommand = Get-Command 'clang-cl.exe' -ErrorAction SilentlyContinue
  if (-not $clangClCommand) {
    throw 'clang-cl.exe not found on PATH. Install LLVM/Visual Studio Clang tools and run from a Developer PowerShell.'
  }

  Invoke-BuildExternal -Context $context -File $clangClCommand.Source -Parameters @('--version') | Out-Null
  $script:clangClVersionLogged = $true
}

$context = New-BuildContext -Workspace $workspacePath -LogDir $logDir -StopOnError

# Ensure no running instance of the application or crash handler is blocking the build output
Stop-Process -Name "GraphicsEngine", "WerFault" -Force -ErrorAction SilentlyContinue

try {
  Open-BuildLog -Context $context
  # If WebDAV parameters are supplied via environment variables, attempt an
  # early download of .pfx files before other build steps. This is optional
  # and will not fail the orchestration if it errors.
  try {
    # Prefer explicit script parameters passed on the command-line, fall back to
    # environment variables for CI compatibility.
    $webdavHost = if (-not [string]::IsNullOrWhiteSpace($WebDavHostname)) { $WebDavHostname } elseif (-not [string]::IsNullOrWhiteSpace($env:WEBDAV_HOSTNAME)) { $env:WEBDAV_HOSTNAME } else { $env:WEB_DAV_HOSTNAME }
    $webdavUser = if (-not [string]::IsNullOrWhiteSpace($WebDavUsername)) { $WebDavUsername } elseif (-not [string]::IsNullOrWhiteSpace($env:WEBDAV_USERNAME)) { $env:WEBDAV_USERNAME } else { $env:WEB_DAV_USERNAME }
    $webdavPass = if (-not [string]::IsNullOrWhiteSpace($WebDavPassword)) { $WebDavPassword } elseif (-not [string]::IsNullOrWhiteSpace($env:WEBDAV_PASSWORD)) { $env:WEBDAV_PASSWORD } else { $env:WEB_DAV_PASSWORD }
    $webdavRemote = if (-not [string]::IsNullOrWhiteSpace($RemoteBasePath)) { $RemoteBasePath } elseif (-not [string]::IsNullOrWhiteSpace($env:REMOTE_BASE_PATH)) { $env:REMOTE_BASE_PATH } else { $env:WEB_DAV_REMOTE_BASE_PATH }
    $webdavLocal = if (-not [string]::IsNullOrWhiteSpace($LocalAssetsFolder)) { $LocalAssetsFolder } elseif (-not [string]::IsNullOrWhiteSpace($env:WEBDAV_LOCAL_BASE_PATH)) { $env:WEBDAV_LOCAL_BASE_PATH } elseif (-not [string]::IsNullOrWhiteSpace($env:WEB_DAV_LOCAL_BASE_PATH)) { $env:WEB_DAV_LOCAL_BASE_PATH } else { $workspacePath }

    if (-not [string]::IsNullOrWhiteSpace($webdavHost) -and -not [string]::IsNullOrWhiteSpace($webdavUser) -and -not [string]::IsNullOrWhiteSpace($webdavPass) -and -not [string]::IsNullOrWhiteSpace($webdavRemote)) {
      Invoke-BuildOptional -Context $context -Name 'Early WebDAV .pfx download' -Script {
        Invoke-EarlyWebDavDownload -Context $context -WorkspacePath $workspacePath -WebDavHost $webdavHost -WebDavUser $webdavUser -WebDavPass $webdavPass -WebDavRemote $webdavRemote -WebDavLocal $webdavLocal
      }
    } else {
      Write-BuildLog -Context $context -Message 'DEBUG: Early WebDAV step skipped: missing WebDAV parameters.'
    }
  } catch {
    Write-BuildLogWarning -Context $context -Message "Early WebDAV invocation failed: $($_.Exception.Message)"
  }
  if ($SkipBuild) {
    Write-BuildLogWarning -Context $context -Message 'Skipping all configure/build steps due to -SkipBuild.'
  }
  Write-BuildLog -Context $context -Message "Workspace: $workspacePath"
  Write-BuildLog -Context $context -Message "Configurations=$(([string[]]$selectedConfigurations -join ', '))"
  Write-BuildLog -Context $context -Message "DEBUG: ParallelJobs=$ParallelJobs (0=all cores)"
  Write-BuildLog -Context $context -Message "DEBUG: VerboseBuild=$VerboseBuild"
  Write-BuildLog -Context $context -Message "DEBUG: DisableSccache=$DisableSccache"
  Write-BuildLog -Context $context -Message "DEBUG: System ProcessorCount=$([Environment]::ProcessorCount)"

  Invoke-BuildStep -Context $context -StepName 'Tool versions' -Critical -Script {
    Invoke-ToolchainChecks -Context $context -ToolArguments @{
      'cmake' = @('--version')
      'ninja' = @('--version')
    } -RequiredTools @('cmake', 'ninja') -FailOnMissingRequiredTools
  } | Out-Null

  # Formatting REPORTS by default and only rewrites with -ApplyFormat.
  #
  # Until 2026-07-20 Get-ProjectCppFiles had an inverted _deps filter and
  # returned zero project files, so this step formatted nothing at all. Fixing
  # that armed a 77-file rewrite for anyone running the default build - a
  # sweep that collides with everything in flight and wants a deliberate
  # moment plus a .git-blame-ignore-revs entry. So the default is now the
  # non-destructive check, and applying the sweep is an explicit act.
  #
  # THE CMAKE-FORMAT VENV IS THE HUB'S PINNED ONE, not this repo's docs stack.
  # -RequirementsPath is new in 604294e2 and closes a real gap: without it
  # Initialize-UvVenvPython installs <workspace>/requirements.txt - sphinx,
  # sphinx-book-theme, myst-parser, breathe, exhale, sphinx_design, pre-commit,
  # junit2html - to obtain one formatter, and takes `cmake-format` UNPINNED, so
  # a floating release could move a verdict with no commit to blame. The hub's
  # linux/scripts/cmake-format.requirements.txt is the pinned pair
  # (cmake-format==0.6.13 + pyyaml==6.0.3) that run-static-analysis-format.sh
  # now installs by the library's own default, so both lanes format with the
  # SAME cmake-format.
  #
  # The other two new parameters are deliberately NOT passed, each measured
  # 2026-09-15 rather than assumed:
  #   -ExcludePattern  would add nothing here. The 18 tracked CMake files this
  #                    repo owns all sit under CMakeLists.txt, Src/, Test/ and
  #                    cmake/; the module's built-in build*/, third_party/,
  #                    _deps/ and vcpkg_installed/ filters already cover this
  #                    repo's whole CODE_QUALITY_CMAKE_EXCLUDE_PATHS list, and
  #                    .venv is filtered in the only branch where it can bite.
  #   -Check           would turn this -Critical step into a gate that fails on
  #                    unformatted input. Measured against the pinned formatter,
  #                    17 of those 18 files are not cmake-format clean, so it
  #                    would make the DEFAULT build red and arm exactly the
  #                    unrequested sweep the paragraph above defers. Adopting a
  #                    parameter is not the moment to change a policy.
  if (-not $SkipFormat) {
    $cmakeFormatRequirements = Join-Path $workspacePath 'third_party\ANTfrastructure\linux\scripts\cmake-format.requirements.txt'
    if ($ApplyFormat) {
      Invoke-BuildStep -Context $context -StepName 'Python tooling + cmake-format (REWRITING)' -Critical -Script {
        Invoke-CmakeFormatStep -Context $context -WorkspacePath $workspacePath -RequirementsPath $cmakeFormatRequirements
      } | Out-Null

      Invoke-BuildStep -Context $context -StepName 'clang-format (C/C++) (REWRITING)' -Critical -Script {
        Invoke-ClangFormatStep -Context $context -WorkspacePath $workspacePath
      } | Out-Null
    } else {
      Invoke-BuildStep -Context $context -StepName 'clang-format check (report only)' -Critical -Script {
        Invoke-ClangFormatCheck -Context $context -WorkspacePath $workspacePath
      } | Out-Null
    }
  }

  if (Test-ConfigurationSelected -Name 'msvc-debug' -SelectedConfigurations $selectedConfigurations) {
    # Make MSVC debug configure/build optional so failures here don't fail the whole orchestration
    Invoke-BuildOptional -Context $context -Name "Configure/Build: $presetMsvcDebug (MSVC Debug - optional)" -Script {
      Invoke-CmakeConfigureAndBuild -Context $context -BuildPath $buildPathMsvcDebug -Preset $presetMsvcDebug -Configuration 'Debug' -CleanBuildRoot -ParallelJobs $ParallelJobs -VerboseOutput:$VerboseBuild -DisableSccache:$DisableSccache
    }

    if (-not $SkipTests) {
      # Make MSVC debug tests optional as well
      Invoke-BuildOptional -Context $context -Name 'Test: MSVC Debug (optional)' -Script {
        $excludeRegex = @()
        if ($DisableIntegrationTestsMsvcDebug) {
          Write-BuildLogWarning -Context $context -Message 'MSVC Debug integration tests disabled via -DisableIntegrationTestsMsvcDebug.'
          $excludeRegex += '^Integration\.'
        }

        Invoke-CtestDiscoveredTests -Context $context -BuildRoot $buildPathMsvcDebug -Configuration 'Debug' -ExcludeRegex $excludeRegex -RuntimeFlavor 'Msvc'
      }
    }
  }

  if (Test-ConfigurationSelected -Name 'msvc-release' -SelectedConfigurations $selectedConfigurations) {
    # Make MSVC release configure/build optional so failures here don't fail the whole orchestration
    Invoke-BuildOptional -Context $context -Name "Configure/Build: $presetMsvcRelease (MSVC Release - optional)" -Script {
      Invoke-SlangShaderPrecompile -BuildLabel 'MSVC Release'
      Invoke-ConfiguredBuild -BuildPath $buildPathMsvcRelease -Preset $presetMsvcRelease -Configuration 'Release'
    }
  }

  if (Test-ConfigurationSelected -Name 'clangcl-debug' -SelectedConfigurations $selectedConfigurations) {
    Invoke-BuildStep -Context $context -StepName "Configure/Build: $presetClangDebug" -Critical -Script {
      Invoke-SlangShaderPrecompile -BuildLabel 'ClangCL Debug'
      Invoke-ConfiguredBuild -BuildPath $buildPathClangDebug -Preset $presetClangDebug -Configuration 'Debug'
    } | Out-Null

    if (-not $SkipTidy) {
      Invoke-BuildStep -Context $context -StepName 'clang-tidy --fix (Src)' -Critical -Script {
        Invoke-ClangTidyFixStep -Context $context -WorkspacePath $workspacePath -BuildRoot $buildPathClangDebug
      } | Out-Null
    }

    if (-not $SkipTests) {
      Invoke-BuildStep -Context $context -StepName 'Test: Clang Debug' -Critical -Script {
        Invoke-CtestDiscoveredTests -Context $context -BuildRoot $buildPathClangDebug -Configuration 'Debug' -RuntimeFlavor 'Clang'
      } | Out-Null
    }
  }

  if (Test-ConfigurationSelected -Name 'clangcl-profile' -SelectedConfigurations $selectedConfigurations) {
    Invoke-BuildStep -Context $context -StepName "Configure/Build: $presetClangProfile" -Critical -Script {
      Invoke-ConfiguredBuild -BuildPath $buildPathClangProfile -Preset $presetClangProfile -Configuration 'RelWithDebInfo'
    } | Out-Null

    if (-not $SkipPerfTests) {
      Invoke-BuildStep -Context $context -StepName 'Benchmarks' -Critical -Script {
        Push-Location $buildPathClangProfile
        try {
          $benchmarkExe = Resolve-TestExecutable -BuildRoot $buildPathClangProfile -ExecutableName 'perfTestSuite.exe'
          if (-not $benchmarkExe) {
            Write-BuildLog -Context $context -Message 'Benchmark executable not found. Skipping benchmark run.'
            return
          }

          Invoke-BuildExternal -Context $context -File $benchmarkExe -Parameters @(
            '--benchmark_out=results.json',
            '--benchmark_out_format=json'
          ) | Out-Null
        } finally {
          Pop-Location
        }
      } | Out-Null
    }
  }

  if (Test-ConfigurationSelected -Name 'clangcl-release' -SelectedConfigurations $selectedConfigurations) {
    Invoke-BuildStep -Context $context -StepName "Release build/package: $presetClangRelease$(if ($isCross) { " (cross, $TargetArch)" })" -Critical -Script {
      if ($SkipBuild) {
        # When -SkipBuild is requested, skip configure/build but still run
        # the packaging step (assumes a previous Release build exists).
        Write-BuildLog -Context $context -Message 'Skipping Clang Release build due to -SkipBuild.'
      } else {
        Invoke-SlangShaderPrecompile -BuildLabel 'ClangCL Release'
        Invoke-ConfiguredBuild -BuildPath $buildPathClangRelease -Preset $presetClangRelease -Configuration 'Release' `
          -ConfigureExtraArgs @(Get-CrossConfigureArgs -Arch $TargetArch -Corrosion -Vulkan)
      }

      # Always attempt packaging when clangcl-release is selected; packaging
      # should not be skipped by -SkipBuild.
      $packageArgs = @(
        '--build', $buildPathClangRelease,
        '--target', 'package',
        '--config', 'Release'
      )
      if ($ParallelJobs -gt 0) {
        $packageArgs += @('--parallel', $ParallelJobs.ToString())
        Write-BuildLog -Context $context -Message "DEBUG: Package build using --parallel $ParallelJobs"
      } else {
        $packageArgs += @('--parallel')
        Write-BuildLog -Context $context -Message "DEBUG: Package build using --parallel (all cores)"
      }
      Write-BuildLog -Context $context -Message "DEBUG: Package command: cmake $($packageArgs -join ' ')"
      Invoke-BuildExternal -Context $context -File 'cmake' -Parameters $packageArgs | Out-Null
    } | Out-Null

    # The cross lane's product (windows-arm64-cross.yml): the install tree the installers
    # pack, plus every DLL its binaries import from the image, so it runs on a clean arm64
    # device (Copy-PeImportClosure). vulkan-1.dll is the device's GPU driver's, never
    # shipped. The MSI and ZIP travel beside it; NSIS's installer stub is x86 by design,
    # and the arch gate walks this tree.
    if ($isCross) {
      Invoke-BuildStep -Context $context -StepName "Portable bundle ($TargetArch)" -Critical -Script {
        $bundle = Join-Path $distArch 'bundle'
        if (Test-Path $bundle) { Remove-BuildRoot -Context $context -Path $bundle | Out-Null }
        Invoke-BuildExternal -Context $context -File 'cmake' -Parameters @('--install', $buildPathClangRelease, '--config', 'Release', '--prefix', $bundle) | Out-Null
        $bundleBin = Join-Path $bundle 'bin'
        $seeds = @(Get-ChildItem -LiteralPath $bundleBin -File | Where-Object { $_.Extension -in '.exe', '.dll' } | ForEach-Object FullName)
        $copied = @(Copy-PeImportClosure -Path $seeds -SearchDirectory @('C:\runtime\bin') -Destination $bundleBin -Arch $TargetArch)
        $packages = Join-Path $distArch 'packages'
        if (Test-Path $packages) { Remove-BuildRoot -Context $context -Path $packages | Out-Null }
        New-Item -ItemType Directory -Force -Path $packages | Out-Null
        Get-ChildItem -LiteralPath $buildPathClangRelease -File | Where-Object { $_.Extension -in '.msi', '.zip' } |
          Copy-Item -Destination $packages
        Write-BuildLog -Context $context -Message "Portable bundle $bundle; DLL closure: $(@($copied | ForEach-Object { Split-Path $_ -Leaf }) -join ', ')"
      } | Out-Null
    }
  }

  # MSIX packaging.
  #
  # STAGING IS THIS SCRIPT'S; EVERYTHING AFTER IT IS THE HUB'S. What goes into
  # the package is the one part that genuinely differs between the three
  # consumers that had each written this out, and here it is a `cmake --install`
  # of the clangcl-release tree. Invoke-MsixPackage (WindowsMsix.Common, reached
  # through Import-BuildModule above) owns the rest - the four logo assets, the
  # manifest expansion, the pack, and the assertion that a package actually
  # appeared. See third_party/ANTfrastructure/docs/windows-builds.md.
  #
  # Two things the hand-rolled block this replaces did not do:
  #   * assert the OUTPUT. makeappx has been seen to report success and produce
  #     no file; the step then went green and the artifact upload found nothing.
  #   * write Wide310x150Logo.png. The hub writes all four names an AppxManifest
  #     references by convention; this manifest names three, and the fourth
  #     costs nothing and stops being a surprise the day the manifest grows.
  #
  # -Sign -SigningRoot $workspacePath: the hub signs with the first *.pfx at the
  # repository root (gitignored) and MSIX_PFX_PASSWORD, then verifies; with no
  # .pfx it warns and the package stays unsigned. This script called
  # Invoke-MsixSign itself until the hub's -Sign stopped searching the staging
  # directory's parent (hub, 2026-09-25).
  if ((-not $SkipMsix) -and (Test-ConfigurationSelected -Name 'clangcl-release' -SelectedConfigurations $selectedConfigurations)) {
    Invoke-BuildOptional -Context $context -Name 'MSIX packaging' -Script {
      $makeappxPath = Resolve-WindowsSdkToolPath -ToolName 'makeappx.exe' -OverridePath $null
      if (-not $makeappxPath) {
        throw 'makeappx.exe not found. Install Windows SDK or add it to PATH.'
      }

      $msixName = Get-OrDefault $env:MSIX_PACKAGE_NAME (Get-ConfigValue -Config $config -Path 'Msix.PackageNameDefault')
      $msixPublisher = Get-OrDefault $env:MSIX_PUBLISHER (Get-ConfigValue -Config $config -Path 'Msix.Publisher')

      # VERSION.txt is the ONLY source of the package version - there is no
      # config fallback and no MSIX_VERSION override any more. The old
      # Test-Path/else pair fell back to a `Version` key in
      # Build-Windows.config.psd1 that had been frozen at 1.5.0.0 since it was
      # written, so a missing or misspelled VERSION.txt did not fail the build:
      # it shipped an installer stamped with a version the repo left behind.
      #
      # The PRESENCE check stays here and stays fatal; only the PARSE is the
      # hub's. Get-PackageVersion falls back to 0.0.1.0 when it finds no version
      # file, which is right for a repo that has none and wrong for this one -
      # missing must surface before the artifact is published. What it buys is
      # the padding: `.Trim() + '.0'` assumed exactly three components, and
      # makeappx rejects a three-component version outright.
      $versionFile = Join-Path $workspacePath 'VERSION.txt'
      if (-not (Test-Path $versionFile)) {
        throw "VERSION.txt not found at $versionFile - it is the source of the MSIX package version."
      }
      $msixVersion = Get-PackageVersion -WorkspacePath $workspacePath -Components 4

      $msixMinVersion = Get-OrDefault $env:MSIX_MIN_VERSION (Get-ConfigValue -Config $config -Path 'Msix.MinVersion')

      $msixStaging = Join-Path $buildPathClangRelease 'msix-staging'
      if (Test-Path $msixStaging) {
        Remove-BuildRoot -Context $context -Path $msixStaging | Out-Null
      }

      # A cross package stages the portable bundle, whose bin\ already carries the DLL
      # closure an arm64 device lacks; the host package installs the tree as before.
      if ($isCross) {
        Copy-Item -Path (Join-Path $distArch 'bundle') -Destination $msixStaging -Recurse
      } else {
        Invoke-BuildExternal -Context $context -File 'cmake' -Parameters @(
          '--install', $buildPathClangRelease,
          '--config', 'Release',
          '--prefix', $msixStaging
        ) | Out-Null
      }

      $manifestTemplateRel = Get-ConfigValue -Config $config -Path 'Msix.ManifestTemplate'
      $manifestTemplatePath = if ([System.IO.Path]::IsPathRooted($manifestTemplateRel)) { $manifestTemplateRel } else { Join-Path $workspacePath $manifestTemplateRel }
      if (-not (Test-Path $manifestTemplatePath)) {
        throw "MSIX manifest template not found: $manifestTemplatePath"
      }

      # The staging assertion stays in front of the hub call: what `cmake
      # --install` did or did not lay down is a caller question, and the hub
      # would otherwise pack a manifest that names an executable nobody put there.
      $exeRelPath = "bin/$msixName.exe"
      if (-not (Test-Path (Join-Path $msixStaging $exeRelPath))) {
        throw "Expected executable not found in MSIX staging: $exeRelPath"
      }

      $msixOutPath = if ($isCross) { Join-Path $distArch "msix\${msixName}_$packageArch.msix" } else { Join-Path $buildPathClangRelease "$msixName.msix" }
      Invoke-MsixPackage -Context $context `
        -StagingDir $msixStaging `
        -ManifestTemplatePath $manifestTemplatePath `
        -TokenMap @{
          '__MSIX_NAME__' = $msixName
          '__MSIX_PUBLISHER__' = $msixPublisher
          '__MSIX_VERSION__' = $msixVersion
          '__MSIX_ARCH__' = $packageArch
          '__MSIX_MIN_VERSION__' = $msixMinVersion
          '__EXE_REL_PATH__' = $exeRelPath
          '__STORE_LOGO_REL__' = 'Assets/StoreLogo.png'
          '__LOGO150_REL__' = 'Assets/Square150x150Logo.png'
          '__LOGO44_REL__' = 'Assets/Square44x44Logo.png'
        } `
        -OutputPath $msixOutPath `
        -GenerateTransparentLogos `
        -MakeAppxPath $makeappxPath `
        -Sign -SigningRoot $workspacePath | Out-Null
    }
  } elseif (-not $SkipMsix) {
    Write-BuildLog -Context $context -Message 'DEBUG: MSIX packaging skipped because clangcl-release was not selected.'
  }

  Write-BuildLogSuccess -Context $context -Message 'Windows build orchestration completed.'
} finally {
  Write-BuildSummary -Context $context
  Close-BuildLog -Context $context
}

if ($context.Results.Failed.Count -gt 0) {
  throw "Windows build completed with failures ($($context.Results.Failed.Count) steps failed)."
}

