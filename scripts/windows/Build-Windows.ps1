#requires -Version 7.0
param(

  [string[]]$Configurations = @('all'),
  [switch]$SkipFormat,
  # Rewrite instead of only reporting drift; off by default, a repo-wide sweep collides with work in flight.
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
  # amd64 (x64) or arm64, empty takes the image's; see third_party/ANTfrastructure/docs/windows-cross-builds.md
  [string]$TargetArch = '',
  # Release also builds the commit suite and stages it in dist\windows-<arch>-tests for the arm64 run job (hub CON43).
  [switch]$StageTests
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

. (Join-Path $PSScriptRoot 'Resolve-BuildModule.ps1')

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
# A hub pin older than the cross lanes lacks this module, so fail naming the commit it needs.
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

# Cross builds clangcl-release, clangcl-debug (its ASan runtime exists for aarch64 since 2026-10-03) and clangcl-profile with -SkipPerfTests; MSVC pins x64.
if ($isCross) {
  $notCross = @($selectedConfigurations | Where-Object { $_ -notin 'clangcl-release', 'clangcl-debug' -and -not ($_ -eq 'clangcl-profile' -and $SkipPerfTests) })
  if ($notCross.Count -gt 0) {
    throw "-TargetArch $TargetArch builds clangcl-release, clangcl-debug, and clangcl-profile with -SkipPerfTests, not $($notCross -join ', ') (third_party/ANTfrastructure/docs/windows-cross-builds.md)."
  }
  $buildPathClangRelease = "$buildPathClangRelease-$TargetArch"
  $buildPathClangProfile = "$buildPathClangProfile-$TargetArch"
  $buildPathClangDebug = "$buildPathClangDebug-$TargetArch"
}
# The product both Windows lanes upload: dist\windows-<x64|arm64>, named as a package names its arch.
$distArch = Join-Path $workspacePath "dist\windows-$packageArch"

# Clearing the selection skips configure/build while non-build steps such as formatting still run.
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
  # The early .pfx download is optional: a failure must not fail the orchestration.
  try {
    # Script parameters first, then the env vars CI sets.
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

  # Report-only unless -ApplyFormat; hub-pinned cmake-format keeps both lanes equal; -Check would red the default build.
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
      Invoke-ConfiguredBuild -BuildPath $buildPathClangDebug -Preset $presetClangDebug -Configuration 'Debug' `
        -ConfigureExtraArgs @(Get-CrossConfigureArgs -Arch $TargetArch -Corrosion -Vulkan)
    } | Out-Null

    if (-not $SkipTidy) {
      Invoke-BuildStep -Context $context -StepName 'clang-tidy --fix (Src)' -Critical -Script {
        Invoke-ClangTidyFixStep -Context $context -WorkspacePath $workspacePath -BuildRoot $buildPathClangDebug
      } | Out-Null
    }

    # A cross build stages the instrumented suite for the arm64 runner instead: the container cannot execute it.
    if (-not $SkipTests -and -not $isCross) {
      Invoke-BuildStep -Context $context -StepName 'Test: Clang Debug' -Critical -Script {
        Invoke-CtestDiscoveredTests -Context $context -BuildRoot $buildPathClangDebug -Configuration 'Debug' -RuntimeFlavor 'Clang'
      } | Out-Null
    }
  }

  if (Test-ConfigurationSelected -Name 'clangcl-profile' -SelectedConfigurations $selectedConfigurations) {
    Invoke-BuildStep -Context $context -StepName "Configure/Build: $presetClangProfile" -Critical -Script {
      Invoke-ConfiguredBuild -BuildPath $buildPathClangProfile -Preset $presetClangProfile -Configuration 'RelWithDebInfo' `
        -ConfigureExtraArgs @(Get-CrossConfigureArgs -Arch $TargetArch -Corrosion -Vulkan)
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
        # Packaging still runs and assumes a previous Release build.
        Write-BuildLog -Context $context -Message 'Skipping Clang Release build due to -SkipBuild.'
      } else {
        Invoke-SlangShaderPrecompile -BuildLabel 'ClangCL Release'
        Invoke-ConfiguredBuild -BuildPath $buildPathClangRelease -Preset $presetClangRelease -Configuration 'Release' `
          -ConfigureExtraArgs (@(if ($StageTests) { '-DBUILD_TESTING=ON', '-DKATAGLYPHIS_RELEASE_COMMIT_TESTS=ON' }) +
            # A cross build has no Debug fuzz targets, so the Release one builds them in FuzzTest unit mode.
            @(if ($StageTests -and $isCross) { '-DKATAGLYPHIS_RELEASE_FUZZ_TESTS=ON' }) +
            @(Get-CrossConfigureArgs -Arch $TargetArch -Corrosion -Vulkan))
      }

      # Packaging runs even with -SkipBuild.
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

    # The install tree plus its imported DLL closure runs on a clean machine; vulkan-1.dll is the GPU driver's, never shipped.
    Invoke-BuildStep -Context $context -StepName "Portable bundle ($TargetArch)" -Critical -Script {
      $bundle = Join-Path $distArch 'bundle'
      if (Test-Path $bundle) { Remove-BuildRoot -Context $context -Path $bundle | Out-Null }
      Invoke-BuildExternal -Context $context -File 'cmake' -Parameters @('--install', $buildPathClangRelease, '--config', 'Release', '--prefix', $bundle) | Out-Null
      $bundleBin = Join-Path $bundle 'bin'
      $seeds = @(Get-ChildItem -LiteralPath $bundleBin -File | Where-Object { $_.Extension -in '.exe', '.dll' } | ForEach-Object FullName)
      $copied = @(Copy-PeImportClosure -Path $seeds -SearchDirectory @(Get-ProductDllSearchPath -Arch $TargetArch) -Destination $bundleBin -Arch $TargetArch)
      $packages = Join-Path $distArch 'packages'
      if (Test-Path $packages) { Remove-BuildRoot -Context $context -Path $packages | Out-Null }
      New-Item -ItemType Directory -Force -Path $packages | Out-Null
      $installers = @(Get-ChildItem -LiteralPath $buildPathClangRelease -File | Where-Object { $_.Extension -in '.msi', '.zip' })
      if (-not $isCross) {
        # x64 only (NSIS's stub is x86); it names its installer like the MSI, the other .exe files are tests.
        $installers += @($installers | ForEach-Object { Join-Path $buildPathClangRelease "$($_.BaseName).exe" } |
            Select-Object -Unique | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Get-Item)
      }
      $installers | Copy-Item -Destination $packages
      Write-BuildLog -Context $context -Message "Portable bundle $bundle; DLL closure: $(@($copied | ForEach-Object { Split-Path $_ -Leaf }) -join ', ')"
    } | Out-Null

    # Beside the product, not in it: the run job downloads both, and the hub grades this tree with the same arch gate.
    if ($StageTests) {
      Invoke-BuildStep -Context $context -StepName "Stage tests ($TargetArch)" -Critical -Script {
        $tests = "$distArch-tests"
        if (Test-Path $tests) { Remove-BuildRoot -Context $context -Path $tests | Out-Null }
        New-Item -ItemType Directory -Force -Path $tests | Out-Null
        $suite = Join-Path $buildPathClangRelease 'commitTestSuite.exe'
        if (-not (Test-Path -LiteralPath $suite -PathType Leaf)) { throw "The Release build made no $suite; KATAGLYPHIS_RELEASE_COMMIT_TESTS did not take" }
        # Invoke-WindowsLane.ps1's GPU suites, plus the source checks that read Src/, scripts/ and docs/, which a test tree lacks.
        $filter = '-GoldenRender.*:Integration.*:BuildIntegrity.*:RenderPassCreateHelperUnit.PostAndSkyboxPassesDeclareNoDepthAttachment'
        $manifest = [System.Collections.Generic.List[object]]::new()
        $manifest.Add(@{ exe = 'commitTestSuite.exe'; kind = 'gtest'; args = @("--gtest_filter=$filter") })
        $staged = @($suite)
        # The perf suite rides along when clangcl-profile was built; it exits non-zero when a benchmark cannot run.
        if (Test-ConfigurationSelected -Name 'clangcl-profile' -SelectedConfigurations $selectedConfigurations) {
          $perf = Join-Path $buildPathClangProfile 'perfTestSuite.exe'
          if (-not (Test-Path -LiteralPath $perf -PathType Leaf)) { throw "The Profile build made no $perf" }
          $staged += $perf
          $manifest.Add(@{ exe = 'perfTestSuite.exe'; kind = 'exitcode'; args = @('--benchmark_min_time=0.05s') })
        }
        if ($isCross) {
          $fuzz = @(Get-ChildItem -LiteralPath $buildPathClangRelease -Filter '*_fuzz_test.exe' -File | ForEach-Object FullName)
          if ($fuzz.Count -eq 0) { throw "The Release build made no *_fuzz_test.exe; KATAGLYPHIS_RELEASE_FUZZ_TESTS did not take" }
          $staged += $fuzz
          foreach ($target in $fuzz) { $manifest.Add(@{ exe = (Split-Path $target -Leaf); kind = 'gtest' }) }
        }
        Copy-Item -LiteralPath $staged -Destination $tests
        $closure = @(Copy-PeImportClosure -Path $staged -SearchDirectory @(Get-ProductDllSearchPath -Arch $TargetArch) -Destination $tests -Arch $TargetArch)
        # The Debug (ASan) suite cannot run in the container on a cross build; it rides along, runtime DLLs beside it (the runner has no redist).
        if ($isCross -and (Test-ConfigurationSelected -Name 'clangcl-debug' -SelectedConfigurations $selectedConfigurations)) {
          $asanSuite = Join-Path $buildPathClangDebug 'commitTestSuite.exe'
          if (-not (Test-Path -LiteralPath $asanSuite -PathType Leaf)) { throw "The Debug build made no $asanSuite" }
          Copy-Item -LiteralPath $asanSuite -Destination (Join-Path $tests 'commitTestSuite-debug.exe')
          $manifest.Add(@{ exe = 'commitTestSuite-debug.exe'; kind = 'gtest'; args = @("--gtest_filter=$filter") })
          & (Join-Path $workspacePath 'third_party\ANTfrastructure\windows\scripts\build\Copy-Arm64VsRuntime.ps1') -InstallDir $tests -Flat
        }
        # repoRoot() walks up to the first Resources/ShadersSlang, so the suites find the shaders and models staged here.
        foreach ($rel in 'Resources\ShadersSlang\build', 'Resources\Models\Dinosaurs', 'Resources\Models\GltfTest', 'Resources\Models\ShadowTest', 'Resources\Models\VikingRoom', 'Resources\Models\crytek-sponza') {
          Copy-Item -LiteralPath (Join-Path $workspacePath $rel) -Destination (Join-Path $tests $rel) -Recurse -Force
        }
        # The two loose models the perf suite parses.
        foreach ($rel in 'Resources\Models\plane.obj', 'Resources\Models\plane.mtl', 'Resources\Models\suzanne.obj', 'Resources\Models\suzanne.mtl') {
          Copy-Item -LiteralPath (Join-Path $workspacePath $rel) -Destination (Join-Path $tests $rel) -Force
        }
        Copy-Item -LiteralPath (Join-Path $workspacePath 'third_party\ANTfrastructure\windows\scripts\build\Invoke-StagedTests.ps1') -Destination $tests
        ConvertTo-Json -InputObject @($manifest) | Set-Content -LiteralPath (Join-Path $tests 'tests.json') -Encoding utf8
        Write-BuildLog -Context $context -Message "Staged $(@($staged | ForEach-Object { Split-Path $_ -Leaf }) -join ', ') in $tests with $($closure.Count) closure DLL(s): $(@($closure | ForEach-Object { Split-Path $_ -Leaf }) -join ', ')"
      } | Out-Null
    }
  }

  # MSIX: staging is this script's; the hub's Invoke-MsixPackage packs, asserts the output and signs with the root *.pfx.
  if ((-not $SkipMsix) -and (Test-ConfigurationSelected -Name 'clangcl-release' -SelectedConfigurations $selectedConfigurations)) {
    Invoke-BuildOptional -Context $context -Name 'MSIX packaging' -Script {
      $makeappxPath = Resolve-WindowsSdkToolPath -ToolName 'makeappx.exe' -OverridePath $null
      if (-not $makeappxPath) {
        throw 'makeappx.exe not found. Install Windows SDK or add it to PATH.'
      }

      $msixName = Get-OrDefault $env:MSIX_PACKAGE_NAME (Get-ConfigValue -Config $config -Path 'Msix.PackageNameDefault')
      $msixPublisher = Get-OrDefault $env:MSIX_PUBLISHER (Get-ConfigValue -Config $config -Path 'Msix.Publisher')

      # Fatal here: without VERSION.txt the hub's Get-PackageVersion silently falls back to 0.0.1.0.
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

      # The bundle's bin\ already carries the DLL closure a clean machine lacks.
      Copy-Item -Path (Join-Path $distArch 'bundle') -Destination $msixStaging -Recurse

      $manifestTemplateRel = Get-ConfigValue -Config $config -Path 'Msix.ManifestTemplate'
      $manifestTemplatePath = if ([System.IO.Path]::IsPathRooted($manifestTemplateRel)) { $manifestTemplateRel } else { Join-Path $workspacePath $manifestTemplateRel }
      if (-not (Test-Path $manifestTemplatePath)) {
        throw "MSIX manifest template not found: $manifestTemplatePath"
      }

      # Assert staging first, or the hub packs a manifest naming a missing executable.
      $exeRelPath = "bin/$msixName.exe"
      if (-not (Test-Path (Join-Path $msixStaging $exeRelPath))) {
        throw "Expected executable not found in MSIX staging: $exeRelPath"
      }

      $msixOutPath = Join-Path $distArch "msix\${msixName}_$packageArch.msix"
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

