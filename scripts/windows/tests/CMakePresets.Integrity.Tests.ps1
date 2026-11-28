#requires -Version 7.0

# A dangling configurePreset makes CMake reject the whole file.

Describe 'CMakePresets.json integrity' {
    BeforeAll {
        $repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
        $script:presets = Get-Content (Join-Path $repoRoot 'CMakePresets.json') -Raw | ConvertFrom-Json
        $script:configureNames = @($script:presets.configurePresets | ForEach-Object { $_.name })
        function Get-EffectiveBinaryDir {
            param($PresetsByName, $Name)

            $preset = $PresetsByName[$Name]
            if ($null -eq $preset) {
                return $null
            }
            if ($preset.binaryDir) {
                return $preset.binaryDir
            }
            foreach ($parent in @($preset.inherits)) {
                $inherited = Get-EffectiveBinaryDir -PresetsByName $PresetsByName -Name $parent
                if ($inherited) {
                    return $inherited
                }
            }
            return $null
        }
        $script:presetsByName = @{}
        foreach ($preset in @($script:presets.configurePresets)) {
            $script:presetsByName[$preset.name] = $preset
        }
        # Build-Windows.config.psd1 must agree with the presets, or builds land where ctest --preset never looks.
        $configPath = Join-Path $repoRoot 'scripts\windows\Build-Windows.config.psd1'
        $buildConfig = Import-PowerShellDataFile -Path $configPath
        $script:configurations = $buildConfig.Build.Configurations
    }

    It 'parses as JSON and defines configure presets' {
        $script:configureNames.Count | Should -BeGreaterThan 0
    }

    It 'has no build/test/package preset pointing at a missing configure preset' {
        $dangling = @()
        foreach ($group in @('buildPresets', 'testPresets', 'packagePresets')) {
            foreach ($preset in @($script:presets.$group)) {
                $target = $preset.configurePreset
                if ($target -and ($script:configureNames -notcontains $target)) {
                    $dangling += "$group/$($preset.name) -> $target"
                }
            }
        }
        ($dangling -join '; ') | Should -BeNullOrEmpty
    }

    It 'has no preset inheriting from a missing preset' {
        $broken = @()
        foreach ($group in @('configurePresets', 'buildPresets', 'testPresets')) {
            $names = @($script:presets.$group | ForEach-Object { $_.name })
            foreach ($preset in @($script:presets.$group)) {
                foreach ($parent in @($preset.inherits)) {
                    if ($parent -and ($names -notcontains $parent)) {
                        $broken += "$group/$($preset.name) inherits $parent"
                    }
                }
            }
        }
        ($broken -join '; ') | Should -BeNullOrEmpty
    }

    It 'keeps configurations testable via test presets' {
        # Not every preset needs one, but most configurations must be ctest --preset-able.
        $testTargets = @($script:presets.testPresets | ForEach-Object { $_.configurePreset })
        $testTargets.Count | Should -BeGreaterThan 3
    }

    It 'points every Build-Windows.config.psd1 configuration at a real configure preset' {
        $missing = @()
        foreach ($name in $script:configurations.Keys) {
            $presetName = $script:configurations[$name].Preset
            if (-not $script:presetsByName.ContainsKey($presetName)) {
                $missing += "$name -> $presetName"
            }
        }
        ($missing -join '; ') | Should -BeNullOrEmpty
    }

    It 'agrees with CMakePresets.json on each configuration''s build directory' {
        $mismatches = @()
        foreach ($name in $script:configurations.Keys) {
            $entry = $script:configurations[$name]
            $expected = "`${sourceDir}/$($entry.BuildDir)/"
            $actual = Get-EffectiveBinaryDir -PresetsByName $script:presetsByName -Name $entry.Preset
            if ($actual -ne $expected) {
                $mismatches += "$name ($($entry.Preset)): expected '$expected', got '$actual'"
            }
        }
        ($mismatches -join '; ') | Should -BeNullOrEmpty
    }

    It 'gives no two Build-Windows.config.psd1 configurations the same BuildDir' {
        $seen = @{}
        $collisions = @()
        foreach ($name in $script:configurations.Keys) {
            $dir = $script:configurations[$name].BuildDir
            if ($seen.ContainsKey($dir)) {
                $collisions += "${dir}: $($seen[$dir]), $name"
            } else {
                $seen[$dir] = $name
            }
        }
        ($collisions -join '; ') | Should -BeNullOrEmpty
    }

    It 'gives every Windows configure preset a unique binaryDir' {
        $windowsPresets = @($script:presets.configurePresets | Where-Object {
            (-not $_.hidden) -and $_.condition -and ($_.condition.rhs -eq 'Windows')
        })

        $byDir = @{}
        foreach ($preset in $windowsPresets) {
            $dir = Get-EffectiveBinaryDir -PresetsByName $script:presetsByName -Name $preset.name
            if (-not $byDir.ContainsKey($dir)) {
                $byDir[$dir] = @()
            }
            $byDir[$dir] += $preset.name
        }

        $violations = @()
        foreach ($dir in $byDir.Keys) {
            $names = $byDir[$dir]
            if ($names.Count -gt 1) {
                $violations += "$dir shared by $($names -join ', ')"
            }
        }
        ($violations -join '; ') | Should -BeNullOrEmpty
    }
}
