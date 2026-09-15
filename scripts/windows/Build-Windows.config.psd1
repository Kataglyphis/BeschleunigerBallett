@{
  Build = @{
    WorkspaceRootEnv = 'WORKSPACE_PATH'
    LogDir = 'logs/windows'

    Configurations = @{
      'msvc-debug' = @{
        BuildDir = 'build-msvc-debug'
        BuildDirEnv = 'BUILD_DIR_MSVC'
        Preset = 'x64-MSVC-Windows-Debug'
        PresetEnv = 'PRESET_MSVC_DEBUG'
      }
      'msvc-release' = @{
        BuildDir = 'build-msvc-release'
        BuildDirEnv = 'BUILD_DIR_MSVC_RELEASE'
        Preset = 'x64-MSVC-Windows-Release'
        PresetEnv = 'PRESET_MSVC_RELEASE'
      }
      'clangcl-debug' = @{
        BuildDir = 'build-clangcl-debug'
        BuildDirEnv = 'BUILD_DIR_CLANGCL'
        Preset = 'x64-ClangCL-Windows-Debug'
        PresetEnv = 'PRESET_CLANGCL_DEBUG'
      }
      'clangcl-profile' = @{
        BuildDir = 'build-clangcl-profile'
        BuildDirEnv = 'BUILD_DIR_PROFILE'
        Preset = 'x64-ClangCL-Windows-Profile'
        PresetEnv = 'CLANG_PROFILE_PRESET'
      }
      'clangcl-release' = @{
        BuildDir = 'build-clangcl-release'
        BuildDirEnv = 'BUILD_DIR_RELEASE'
        Preset = 'x64-ClangCL-Windows-Release'
        PresetEnv = 'PRESET_CLANGCL_RELEASE'
      }
    }
  }

  Msix = @{
    PackageNameDefault = 'GraphicsEngine'
    Publisher = 'CN=Jonas Heinle'
    # No Version key: the MSIX package version comes from the repo-root
    # VERSION.txt and from nowhere else (Build-Windows.ps1, MSIX packaging).
    # The key that used to sit here was a silent fallback pinned at 1.5.0.0.
    MinVersion = '10.0.17763.0'
    ManifestTemplate = 'scripts/windows/AppxManifest.xml.template'
  }

}
