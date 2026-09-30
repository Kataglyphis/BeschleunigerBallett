#requires -Version 7.0

# A dangling configurePreset makes CMake reject the whole file. Pester 3.4.0: no BeforeAll outside Describe, dash-less Should.

Describe 'CMakePresets.json integrity' {

    $repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
    $presets = Get-Content (Join-Path $repoRoot 'CMakePresets.json') -Raw | ConvertFrom-Json
    $configureNames = @($presets.configurePresets | ForEach-Object { $_.name })

    It 'parses as JSON and defines configure presets' {
        $configureNames.Count | Should BeGreaterThan 0
    }

    It 'has no build/test/package preset pointing at a missing configure preset' {
        $dangling = @()
        foreach ($group in @('buildPresets', 'testPresets', 'packagePresets')) {
            foreach ($preset in @($presets.$group)) {
                $target = $preset.configurePreset
                if ($target -and ($configureNames -notcontains $target)) {
                    $dangling += "$group/$($preset.name) -> $target"
                }
            }
        }
        ($dangling -join '; ') | Should BeNullOrEmpty
    }

    It 'has no preset inheriting from a missing preset' {
        $broken = @()
        foreach ($group in @('configurePresets', 'buildPresets', 'testPresets')) {
            $names = @($presets.$group | ForEach-Object { $_.name })
            foreach ($preset in @($presets.$group)) {
                foreach ($parent in @($preset.inherits)) {
                    if ($parent -and ($names -notcontains $parent)) {
                        $broken += "$group/$($preset.name) inherits $parent"
                    }
                }
            }
        }
        ($broken -join '; ') | Should BeNullOrEmpty
    }

    It 'keeps configurations testable via test presets' {
        # Not every preset needs one, but most configurations must be ctest --preset-able.
        $testTargets = @($presets.testPresets | ForEach-Object { $_.configurePreset })
        $testTargets.Count | Should BeGreaterThan 3
    }

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

    $presetsByName = @{}
    foreach ($preset in @($presets.configurePresets)) {
        $presetsByName[$preset.name] = $preset
    }

    # Build-Windows.config.psd1 must agree with the presets, or builds land where ctest --preset never looks.
    $configPath = Join-Path $repoRoot 'scripts\windows\Build-Windows.config.psd1'
    $buildConfig = Import-PowerShellDataFile -Path $configPath
    $configurations = $buildConfig.Build.Configurations

    It 'points every Build-Windows.config.psd1 configuration at a real configure preset' {
        $missing = @()
        foreach ($name in $configurations.Keys) {
            $presetName = $configurations[$name].Preset
            if (-not $presetsByName.ContainsKey($presetName)) {
                $missing += "$name -> $presetName"
            }
        }
        ($missing -join '; ') | Should BeNullOrEmpty
    }

    It 'agrees with CMakePresets.json on each configuration''s build directory' {
        $mismatches = @()
        foreach ($name in $configurations.Keys) {
            $entry = $configurations[$name]
            $expected = "`${sourceDir}/$($entry.BuildDir)/"
            $actual = Get-EffectiveBinaryDir -PresetsByName $presetsByName -Name $entry.Preset
            if ($actual -ne $expected) {
                $mismatches += "$name ($($entry.Preset)): expected '$expected', got '$actual'"
            }
        }
        ($mismatches -join '; ') | Should BeNullOrEmpty
    }

    It 'gives no two Build-Windows.config.psd1 configurations the same BuildDir' {
        $seen = @{}
        $collisions = @()
        foreach ($name in $configurations.Keys) {
            $dir = $configurations[$name].BuildDir
            if ($seen.ContainsKey($dir)) {
                $collisions += "${dir}: $($seen[$dir]), $name"
            } else {
                $seen[$dir] = $name
            }
        }
        ($collisions -join '; ') | Should BeNullOrEmpty
    }

    It 'gives every Windows configure preset a unique binaryDir' {
        # The one deliberate shared build_release/ pair, named so a third sharer still fails.
        $allowedSharedDir = "`${sourceDir}/build_release/"
        $allowedSharers = @('x64-Clang-Windows-Release', 'x64-ClangCL-Windows-RelWithDebInfo')

        $windowsPresets = @($presets.configurePresets | Where-Object {
            (-not $_.hidden) -and $_.condition -and ($_.condition.rhs -eq 'Windows')
        })

        $byDir = @{}
        foreach ($preset in $windowsPresets) {
            $dir = Get-EffectiveBinaryDir -PresetsByName $presetsByName -Name $preset.name
            if (-not $byDir.ContainsKey($dir)) {
                $byDir[$dir] = @()
            }
            $byDir[$dir] += $preset.name
        }

        $violations = @()
        foreach ($dir in $byDir.Keys) {
            $names = $byDir[$dir]
            if ($names.Count -le 1) {
                continue
            }
            if ($dir -eq $allowedSharedDir) {
                $unexpected = @($names | Where-Object { $allowedSharers -notcontains $_ })
                if ($unexpected.Count -gt 0) {
                    $violations += "$dir shared by $($names -join ', ') (unexpected: $($unexpected -join ', '))"
                }
            } else {
                $violations += "$dir shared by $($names -join ', ')"
            }
        }
        ($violations -join '; ') | Should BeNullOrEmpty
    }
}
