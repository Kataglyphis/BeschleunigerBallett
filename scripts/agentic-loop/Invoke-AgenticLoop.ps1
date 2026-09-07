#requires -Version 7.0
<#
.SYNOPSIS
  Agentic loop: planner adds tasks to BACKLOG.md, executor drains the queue.
  Uses WindowsAgenticLoop.Common module from ContainerHub.

  Engines (config .engine, or -Engine / $env:AGENTIC_ENGINE):
    claude   — planner: Opus 5 (fallback Fable 5), executor: Sonnet
    opencode — planner: GLM 5.2, executor: DeepSeek v4 Flash
.PARAMETER Engine  Engine override: claude | opencode (default: config .engine).
.PARAMETER DryRun  Print actions without executing.
.PARAMETER MaxIterations  Override max iterations (0 = unlimited).
.PARAMETER PlannerOnly  Run planner once and exit.
.PARAMETER ExecutorOnly  Drain the queue and exit.
#>
param([string]$Engine = '', [switch]$DryRun, [int]$MaxIterations = -1, [switch]$SkipBuild,
      [switch]$SkipTests, [switch]$SkipQuality, [switch]$PlannerOnly, [switch]$ExecutorOnly)

$ErrorActionPreference = 'Stop'; Set-StrictMode -Version Latest
$scriptRoot = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $scriptRoot '..\..')).Path

# Resolve module from ContainerHub or vendored fallback
$modulePath = $null
foreach ($c in @((Join-Path $repoRoot 'third_party\ContainerHub\windows\scripts\modules\WindowsAgenticLoop.Common.psm1'),
                 (Join-Path $scriptRoot 'modules\WindowsAgenticLoop.Common.psm1'))) {
    if (Test-Path $c) { $modulePath = (Resolve-Path $c).Path; break }
}
if (-not $modulePath) { Write-Host "FATAL: Module not found" -ForegroundColor Red; exit 1 }
Import-Module $modulePath -Force

# Config
$configPath = Join-Path $scriptRoot 'AgenticLoop.config.json'
if (-not (Test-Path $configPath)) { Write-Host "FATAL: Config not found: $configPath" -ForegroundColor Red; exit 1 }
$config = Get-Content $configPath -Raw | ConvertFrom-Json
if (-not $config) { Write-Host "FATAL: Invalid JSON" -ForegroundColor Red; exit 1 }

Initialize-AgenticLoop -ConfigPath $configPath -RepoRoot $repoRoot -DryRun:$DryRun

# Preflight: a prompt overlay the config DECLARES and no reader picks up is an
# error, not a silent fallback to the bare shared prompt. Both ways of losing
# one are quiet upstream - Resolve-AgenticEngine reads the overlay keys from
# engines.<engine> only, and New-AgenticComposedPrompt downgrades a missing
# overlay file to a WARN - so the loop would run without this repo's project
# rules and report nothing. Asserted here, before the first agent invocation,
# so the failure costs a second instead of a whole session.
Import-Module (Join-Path $scriptRoot 'AgenticPromptOverlay.psm1') -Force
$overlayEngine = if ($Engine) { $Engine }
                 elseif ($env:AGENTIC_ENGINE) { $env:AGENTIC_ENGINE }
                 else { Get-AgenticConfigValue $config 'engine' 'opencode' }
Assert-AgenticPromptOverlay -Config $config -RepoRoot $repoRoot -Engine $overlayEngine

# Build configs and planner/executor task prompts come from the module:
# configs from the config's buildMatrix (legacy buildConfigurations fallback),
# prompts from ContainerHub's shared/agentic-loop/prompts/*.md defaults.
try {
    Invoke-AgenticLoop -Config $config -Engine $Engine -RepoRoot $repoRoot `
        -MaxIterations:$MaxIterations -SkipBuild:$SkipBuild -SkipTests:$SkipTests `
        -SkipQuality:$SkipQuality -PlannerOnly:$PlannerOnly -ExecutorOnly:$ExecutorOnly
} finally {
    Complete-AgenticLoop
}
