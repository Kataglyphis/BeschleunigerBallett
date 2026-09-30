#requires -Version 7.0
<#
.SYNOPSIS
  Agentic loop for BeschleunigerBallett (Windows): the planner fills BACKLOG.md, the executor drains it.
.DESCRIPTION
  Thin wrapper over ANTfrastructure's WindowsAgenticLoop.Common. Prompts come from its
  shared/agentic-loop/prompts/*.md; hard-coding them here makes the platforms drift.
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

try {
    Invoke-AgenticLoop -Config $config -Engine $Engine -RepoRoot $repoRoot `
        -MaxIterations:$MaxIterations -SkipBuild:$SkipBuild -SkipTests:$SkipTests `
        -SkipQuality:$SkipQuality -PlannerOnly:$PlannerOnly -ExecutorOnly:$ExecutorOnly
} finally {
    Complete-AgenticLoop
}
