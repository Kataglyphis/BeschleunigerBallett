#requires -Version 7.0

# Runs the real script against temp dumps and budgets; one case reads the checked-in budget file. Pester 3.4.0: `Should Be`.
Import-Module (Join-Path $PSScriptRoot '..\..\..\third_party\ANTfrastructure\windows\scripts\tests\TestHarness.psm1') -Function New-TestDir, Remove-TestDir -ErrorAction Stop

Describe 'Test-GpuTimingBudget' {

  BeforeAll {
    $script:scriptPath = (Resolve-Path (Join-Path $PSScriptRoot '..\Test-GpuTimingBudget.ps1')).Path
    $script:repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
    $script:tmpDir = New-TestDir -Prefix 'gpu-timing-budget-test-'
  }

  AfterAll {
    Remove-TestDir -Path $script:tmpDir
  }

  function New-JsonFile {
    param([string]$Name, [hashtable]$Content)
    $path = Join-Path $script:tmpDir $Name
    ($Content | ConvertTo-Json -Depth 6) | Set-Content -Path $path
    return $path
  }

  function New-Dump {
    param([string]$Name, [hashtable]$Passes, [bool]$Supported = $true, [int]$Frames = 30, [hashtable]$NestedIn)
    $content = @{ frames_measured = $Frames; timestamps_supported = $Supported; passes = $Passes }
    if ($NestedIn) { $content.nested_in = $NestedIn }
    return New-JsonFile -Name $Name -Content $content
  }

  function Invoke-Budget {
    param([string]$TimingPath, [string]$BudgetPath, [string]$HostProfile = 'test-host')
    $scriptArgs = @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $script:scriptPath,
      '-TimingPath', $TimingPath, '-BudgetPath', $BudgetPath, '-HostProfile', $HostProfile)
    & pwsh @scriptArgs | Out-Null
    return $LASTEXITCODE
  }

  # Main 8 of 10 ms (0.8), Post 2 (0.2).
  $passes = @{ Main = 8.0; Post = 2.0 }

  It 'exits 0 when every budgeted pass is within its ms and share budget' {
    $dump = New-Dump -Name 'within-dump.json' -Passes $passes
    $budget = New-JsonFile -Name 'within-budget.json' -Content @{ hosts = @{ 'test-host' = @{
          max_ms = @{ Main = 9.0; Post = 2.5 }; max_share = @{ Main = 0.85; Post = 0.25 } } } }
    Invoke-Budget -TimingPath $dump -BudgetPath $budget | Should Be 0
  }

  It 'exits 1 when a pass exceeds its absolute budget' {
    $dump = New-Dump -Name 'slow-dump.json' -Passes $passes
    $budget = New-JsonFile -Name 'slow-budget.json' -Content @{ hosts = @{ 'test-host' = @{ max_ms = @{ Main = 7.0 } } } }
    Invoke-Budget -TimingPath $dump -BudgetPath $budget | Should Be 1
  }

  It 'exits 1 when a pass takes more of the frame than its share, whatever the host clock' {
    $dump = New-Dump -Name 'share-dump.json' -Passes $passes
    $budget = New-JsonFile -Name 'share-budget.json' -Content @{ hosts = @{ 'test-host' = @{ max_share = @{ Post = 0.15 } } } }
    Invoke-Budget -TimingPath $dump -BudgetPath $budget | Should Be 1
  }

  It 'divides a share by the top-level passes only, so a nested scope is not counted twice' {
    # Main 8 encloses Forward 7, and Post 2: Post takes 2 of 10 ms (0.2), not 2 of 17 (0.12).
    $dump = New-Dump -Name 'nested-dump.json' -Passes @{ Main = 8.0; Forward = 7.0; Post = 2.0 } -NestedIn @{ Forward = 'Main' }
    $budget = New-JsonFile -Name 'nested-budget.json' -Content @{ hosts = @{ 'test-host' = @{ max_share = @{ Post = 0.15 } } } }
    Invoke-Budget -TimingPath $dump -BudgetPath $budget | Should Be 1
    $roomy = New-JsonFile -Name 'nested-roomy-budget.json' -Content @{ hosts = @{ 'test-host' = @{ max_share = @{ Post = 0.25; Forward = 0.75 } } } }
    Invoke-Budget -TimingPath $dump -BudgetPath $roomy | Should Be 0
  }

  It 'exits 1 when a budgeted pass is missing from the dump' {
    $dump = New-Dump -Name 'missing-dump.json' -Passes $passes
    $budget = New-JsonFile -Name 'missing-budget.json' -Content @{ hosts = @{ 'test-host' = @{ max_ms = @{ Forward = 1.0 } } } }
    Invoke-Budget -TimingPath $dump -BudgetPath $budget | Should Be 1
  }

  It 'exits 1 for a host profile the budget file does not have' {
    $dump = New-Dump -Name 'unknown-host-dump.json' -Passes $passes
    $budget = New-JsonFile -Name 'unknown-host-budget.json' -Content @{ hosts = @{ 'other-host' = @{ max_ms = @{ Main = 9.0 } } } }
    Invoke-Budget -TimingPath $dump -BudgetPath $budget | Should Be 1
  }

  It 'exits 2 when the device measured nothing' {
    $dump = New-Dump -Name 'unsupported-dump.json' -Passes @{} -Supported $false -Frames 0
    $budget = New-JsonFile -Name 'unsupported-budget.json' -Content @{ hosts = @{ 'test-host' = @{ max_ms = @{ Main = 9.0 } } } }
    Invoke-Budget -TimingPath $dump -BudgetPath $budget | Should Be 2
  }

  It 'exits 2 when the host profile budgets no pass' {
    $dump = New-Dump -Name 'empty-profile-dump.json' -Passes $passes
    $budget = New-JsonFile -Name 'empty-profile-budget.json' -Content @{ hosts = @{ 'test-host' = @{ device = 'nothing budgeted' } } }
    Invoke-Budget -TimingPath $dump -BudgetPath $budget | Should Be 2
  }

  It 'budgets only passes the renderer exports, in every checked-in host profile' {
    $source = Get-Content -LiteralPath (Join-Path $script:repoRoot 'Src\GraphicsEngineVulkan\renderer\GUIRendererSharedVars.ixx') -Raw
    $table = [regex]::Match($source, 'GPU_TIMED_PASS_EXPORT_NAMES\s*=\s*std::to_array<[^>]*>\s*\(\{([^}]*)\}\)')
    $table.Success | Should Be $true
    $exported = @([regex]::Matches($table.Groups[1].Value, '"([^"]+)"') | ForEach-Object { $_.Groups[1].Value })
    $budget = Get-Content -LiteralPath (Join-Path $script:repoRoot 'Test\perf\baselines\gpu-timing-budgets.json') -Raw |
      ConvertFrom-Json -AsHashtable
    $budget.hosts.Count | Should BeGreaterThan 0
    $unknown = @(foreach ($hostBudget in $budget.hosts.Values) {
        foreach ($section in @('max_ms', 'max_share')) {
          if ($hostBudget.ContainsKey($section)) { $hostBudget[$section].Keys | Where-Object { $_ -notin $exported } }
        }
      })
    ($unknown -join ', ') | Should Be ''
  }
}
