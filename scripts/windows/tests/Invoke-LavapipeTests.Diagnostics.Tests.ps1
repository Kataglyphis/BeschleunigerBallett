#requires -Version 7.0

# These fixture verdicts cannot reproduce a GPU driver crash or prove its renderer root cause.
Import-Module (Join-Path $PSScriptRoot '..\..\..\third_party\ANTfrastructure\windows\scripts\tests\TestHarness.psm1') -Function Get-ScriptFunctionDefinition -ErrorAction Stop
. (Get-ScriptFunctionDefinition -ScriptPath (Join-Path $PSScriptRoot '..\Invoke-LavapipeTests.ps1') -FunctionName Get-TestOutcome, Wait-TestProcess, Write-TestResult)

Describe 'Lavapipe process and gtest evidence' {
  BeforeAll {
    $repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
    $script:fixtureDir = Join-Path $repoRoot ('logs\tests\lavapipe-verdict-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $script:fixtureDir | Out-Null
    $script:json = Join-Path $script:fixtureDir 'result.json'
  }

  AfterAll {
    Remove-Item -LiteralPath $script:fixtureDir -Recurse -Force
  }

  It 'reports the raw process failure even without JSON' {
    $verdict = Get-TestOutcome -Json $script:json -ExitCode -1073741819
    $verdict.Status | Should Be 'failed'
    $verdict.Reason | Should Match 'process exited -1073741819'
    $verdict.JsonPresent | Should Be $false
  }

  It 'fails a zero-exit process that did not write JSON' {
    $verdict = Get-TestOutcome -Json $script:json -ExitCode 0
    $verdict.Status | Should Be 'failed'
    $verdict.Reason | Should Match 'JSON is missing'
  }

  It 'keeps a timeout distinct from a process exit' {
    $verdict = Get-TestOutcome -Json $script:json -ExitCode 1 -TimedOut $true
    $verdict.Status | Should Be 'failed'
    $verdict.Reason | Should Match 'timed out'
  }

  It 'fails malformed JSON without losing the aggregate test verdict' {
    Set-Content -LiteralPath $script:json -Value '{broken' -Encoding UTF8
    $verdict = Get-TestOutcome -Json $script:json -ExitCode 0
    $verdict.Status | Should Be 'failed'
    $verdict.Reason | Should Match 'could not be read'
    $verdict.JsonPresent | Should Be $true
  }

  It 'fails JSON without a gtest case' {
    Set-Content -LiteralPath $script:json -Value '{"testsuites":[]}' -Encoding UTF8
    $verdict = Get-TestOutcome -Json $script:json -ExitCode 0
    $verdict.Status | Should Be 'failed'
  }

  It 'recognizes an assertion failure' {
    Set-Content -LiteralPath $script:json -Value '{"testsuites":[{"testsuite":[{"result":"COMPLETED","failures":[{"failure":"pixel mismatch"}]}]}]}' -Encoding UTF8
    $verdict = Get-TestOutcome -Json $script:json -ExitCode 0
    $verdict.Status | Should Be 'failed'
    $verdict.Reason | Should Match 'assertion'
  }

  It 'recognizes a skipped case' {
    Set-Content -LiteralPath $script:json -Value '{"testsuites":[{"testsuite":[{"result":"SKIPPED"}]}]}' -Encoding UTF8
    $verdict = Get-TestOutcome -Json $script:json -ExitCode 0
    $verdict.Status | Should Be 'skipped'
  }

  It 'passes a completed case without failures' {
    Set-Content -LiteralPath $script:json -Value '{"testsuites":[{"testsuite":[{"result":"COMPLETED","failures":[]}]}]}' -Encoding UTF8
    $verdict = Get-TestOutcome -Json $script:json -ExitCode 0
    $verdict.Status | Should Be 'passed'
  }

  It 'counts validation output as a failure while retaining the process evidence' {
    $verdict = Get-TestOutcome -Json $script:json -ExitCode 0
    $counts = @{ passed = 0; failed = 0; skipped = 0 }
    $notPassed = [System.Collections.Generic.List[string]]::new()
    $clock = [Diagnostics.Stopwatch]::StartNew()
    Write-TestResult -Test 'GoldenRender.Fixture' -Verdict $verdict -Output @('VUID-fixture') -ExitCode 0 `
      -Json $script:json -Stdout 'stdout.txt' -Stderr 'stderr.txt' -Clock $clock -Counts $counts -NotPassed $notPassed 6>$null
    $counts.failed | Should Be 1
    $counts.passed | Should Be 0
    $notPassed.Count | Should Be 1
    $notPassed[0] | Should Match 'validation message in process output'
  }

  It 'kills and reaps a process after its test timeout' {
    $child = Start-Process -FilePath (Get-Process -Id $PID).Path `
      -ArgumentList @('-NoProfile', '-Command', 'Start-Sleep -Seconds 30') -NoNewWindow -PassThru
    try {
      $timedOut = Wait-TestProcess -Process $child -Test 'GoldenRender.Fixture' -TimeoutSeconds 1
      $timedOut | Should Be $true
      $child.HasExited | Should Be $true
    } finally {
      if (-not $child.HasExited) { $child.Kill($true); $null = $child.WaitForExit(10000) }
      $child.Dispose()
    }
  }
}
