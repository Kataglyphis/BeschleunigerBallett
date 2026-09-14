#requires -Version 7.0
<#
.SYNOPSIS
  Agentic loop for BeschleunigerBallett (Windows): planner adds tasks to
  BACKLOG.md, executor drains the queue.
  Uses the WindowsAgenticLoop.Common module from ANTfrastructure, so
  this wrapper stays thin: it resolves the module, loads the config, and calls
  Invoke-AgenticLoop. Build configurations come from the config's buildMatrix
  and the planner/executor task prompts default to ANTfrastructure's
  shared/agentic-loop/prompts/*.md - do NOT hard-code prompt text here, that
  is how the two platforms drifted apart once already.

  Engines are selected by the config's .engine key (or -Engine /
  $env:AGENTIC_ENGINE); models are configured per engine in the config.
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

# Resolve module from ANTfrastructure or vendored fallback
$modulePath = $null
foreach ($c in @((Join-Path $repoRoot 'third_party\ANTfrastructure\windows\scripts\modules\WindowsAgenticLoop.Common.psm1'),
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

# Build configs and planner/executor task prompts come from the module:
# configs from the config's buildMatrix (legacy buildConfigurations fallback),
# prompts from ANTfrastructure's shared/agentic-loop/prompts/*.md defaults.
try {
    Invoke-AgenticLoop -Config $config -Engine $Engine -RepoRoot $repoRoot `
        -MaxIterations:$MaxIterations -SkipBuild:$SkipBuild -SkipTests:$SkipTests `
        -SkipQuality:$SkipQuality -PlannerOnly:$PlannerOnly -ExecutorOnly:$ExecutorOnly
} finally {
    Complete-AgenticLoop
}
