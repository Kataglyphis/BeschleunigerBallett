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
  [string]$LocalAssetsFolder
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
  'WindowsTesting.Common'
)

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
    [Parameter(Mandatory)][string]$Configuration
  )

  Invoke-CmakeConfigureAndBuild -Context $context -BuildPath $BuildPath -Preset $Preset -Configuration $Configuration -CleanBuildRoot -ParallelJobs $ParallelJobs -VerboseOutput:$VerboseBuild -DisableSccache:$DisableSccache
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
  if (-not $SkipFormat) {
    if ($ApplyFormat) {
      Invoke-BuildStep -Context $context -StepName 'Python tooling + cmake-format (REWRITING)' -Critical -Script {
        Invoke-CmakeFormatStep -Context $context -WorkspacePath $workspacePath
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
    Invoke-BuildStep -Context $context -StepName "Release build/package: $presetClangRelease" -Critical -Script {
      if ($SkipBuild) {
        # When -SkipBuild is requested, skip configure/build but still run
        # the packaging step (assumes a previous Release build exists).
        Write-BuildLog -Context $context -Message 'Skipping Clang Release build due to -SkipBuild.'
      } else {
        Invoke-SlangShaderPrecompile -BuildLabel 'ClangCL Release'
        Invoke-ConfiguredBuild -BuildPath $buildPathClangRelease -Preset $presetClangRelease -Configuration 'Release'
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
  }

  if ((-not $SkipMsix) -and (Test-ConfigurationSelected -Name 'clangcl-release' -SelectedConfigurations $selectedConfigurations)) {
    Invoke-BuildOptional -Context $context -Name 'MSIX packaging' -Script {
      $makeappxPath = Resolve-WindowsSdkToolPath -ToolName 'makeappx.exe' -OverridePath $null
      if (-not $makeappxPath) {
        throw 'makeappx.exe not found. Install Windows SDK or add it to PATH.'
      }

      $msixName = Get-OrDefault $env:MSIX_PACKAGE_NAME (Get-ConfigValue -Config $config -Path 'Msix.PackageNameDefault')
      $msixPublisher = Get-OrDefault $env:MSIX_PUBLISHER (Get-ConfigValue -Config $config -Path 'Msix.Publisher')
      
      $versionFile = Join-Path $workspacePath 'version.txt'
      if (Test-Path $versionFile) {
        $msixVersion = (Get-Content -Path $versionFile).Trim() + '.0'
      } else {
        $msixVersion = Get-OrDefault $env:MSIX_VERSION (Get-ConfigValue -Config $config -Path 'Msix.Version')
      }
      
      $msixMinVersion = Get-OrDefault $env:MSIX_MIN_VERSION (Get-ConfigValue -Config $config -Path 'Msix.MinVersion')

      $msixStaging = Join-Path $buildPathClangRelease 'msix-staging'
      $assetsDir = Join-Path $msixStaging 'Assets'
      if (Test-Path $msixStaging) {
        Remove-BuildRoot -Context $context -Path $msixStaging | Out-Null
      }

      Invoke-BuildExternal -Context $context -File 'cmake' -Parameters @(
        '--install', $buildPathClangRelease,
        '--config', 'Release',
        '--prefix', $msixStaging
      ) | Out-Null

      Resolve-DirectoryPath -Path $assetsDir | Out-Null

      $manifestTemplateRel = Get-ConfigValue -Config $config -Path 'Msix.ManifestTemplate'
      $manifestTemplatePath = if ([System.IO.Path]::IsPathRooted($manifestTemplateRel)) { $manifestTemplateRel } else { Join-Path $workspacePath $manifestTemplateRel }
      if (-not (Test-Path $manifestTemplatePath)) {
        throw "MSIX manifest template not found: $manifestTemplatePath"
      }

      $exeRelPath = "bin/$msixName.exe"
      $template = Get-Content -Path $manifestTemplatePath -Raw -Encoding UTF8
      $manifestXml = Expand-XmlTemplateTokens -Template $template -TokenMap @{
        '__MSIX_NAME__' = $msixName
        '__MSIX_PUBLISHER__' = $msixPublisher
        '__MSIX_VERSION__' = $msixVersion
        '__MSIX_MIN_VERSION__' = $msixMinVersion
        '__EXE_REL_PATH__' = $exeRelPath
        '__STORE_LOGO_REL__' = 'Assets/StoreLogo.png'
        '__LOGO150_REL__' = 'Assets/Square150x150Logo.png'
        '__LOGO44_REL__' = 'Assets/Square44x44Logo.png'
      }

      Set-Content -Path (Join-Path $msixStaging 'AppxManifest.xml') -Value $manifestXml -Encoding UTF8
      New-TransparentPng -Path (Join-Path $msixStaging 'Assets\StoreLogo.png') -Width 50 -Height 50
      New-TransparentPng -Path (Join-Path $msixStaging 'Assets\Square150x150Logo.png') -Width 150 -Height 150
      New-TransparentPng -Path (Join-Path $msixStaging 'Assets\Square44x44Logo.png') -Width 44 -Height 44

      if (-not (Test-Path (Join-Path $msixStaging $exeRelPath))) {
        throw "Expected executable not found in MSIX staging: $exeRelPath"
      }

      $msixOutPath = Join-Path $buildPathClangRelease "$msixName.msix"
      Invoke-BuildExternal -Context $context -File $makeappxPath -Parameters @('pack', '/d', $msixStaging, '/p', $msixOutPath, '/o') | Out-Null

      # Attempt to sign the generated MSIX using the signing helper module.
      Invoke-MsixSign -Context $context -WorkspacePath $workspacePath -MsixOutPath $msixOutPath
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

