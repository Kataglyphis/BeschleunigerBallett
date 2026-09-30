#requires -Version 7.0

# Runs the real script against temp fixtures, never the checked-in baseline. Pester 3.4.0: `Should Be`, not `Should -Be`.

Describe 'Compare-PerfBaseline' {

  BeforeAll {
    $script:scriptPath = (Resolve-Path (Join-Path $PSScriptRoot '..\Compare-PerfBaseline.ps1')).Path
    $tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP ('perf-baseline-test-' + (Get-Random))) -Force
    $script:tmpDir = $tmp.FullName
  }

  AfterAll {
    Remove-Item -LiteralPath $script:tmpDir -Recurse -Force -ErrorAction SilentlyContinue
  }

  function New-BenchmarkJson {
    param([string]$Path, [array]$Benchmarks)
    $doc = @{ benchmarks = $Benchmarks }
    ($doc | ConvertTo-Json -Depth 5) | Set-Content -Path $Path
  }

  function Invoke-Compare {
    param([string]$BaselinePath, [string]$CandidatePath, [double]$ToleranceFraction)
    $scriptArgs = @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $script:scriptPath)
    if ($BaselinePath) { $scriptArgs += @('-BaselinePath', $BaselinePath) }
    if ($CandidatePath) { $scriptArgs += @('-CandidatePath', $CandidatePath) }
    if ($PSBoundParameters.ContainsKey('ToleranceFraction')) { $scriptArgs += @('-ToleranceFraction', $ToleranceFraction) }
    & pwsh @scriptArgs | Out-Null
    return $LASTEXITCODE
  }

  It 'exits 0 when baseline and candidate are identical' {
    $basePath = Join-Path $script:tmpDir 'identical-base.json'
    $candPath = Join-Path $script:tmpDir 'identical-cand.json'
    New-BenchmarkJson -Path $basePath -Benchmarks @(@{ name = 'BM_A'; real_time = 100.0; time_unit = 'ns' })
    New-BenchmarkJson -Path $candPath -Benchmarks @(@{ name = 'BM_A'; real_time = 100.0; time_unit = 'ns' })

    $exitCode = Invoke-Compare -BaselinePath $basePath -CandidatePath $candPath
    $exitCode | Should Be 0
  }

  It 'exits non-zero when a benchmark is 50% slower with the default tolerance' {
    $basePath = Join-Path $script:tmpDir 'regressed-base.json'
    $candPath = Join-Path $script:tmpDir 'regressed-cand.json'
    New-BenchmarkJson -Path $basePath -Benchmarks @(@{ name = 'BM_A'; real_time = 100.0; time_unit = 'ns' })
    New-BenchmarkJson -Path $candPath -Benchmarks @(@{ name = 'BM_A'; real_time = 150.0; time_unit = 'ns' })

    $exitCode = Invoke-Compare -BaselinePath $basePath -CandidatePath $candPath
    $exitCode | Should Not Be 0
  }

  It 'exits 0 for the same 50% regression when -ToleranceFraction 1.0' {
    $basePath = Join-Path $script:tmpDir 'tolerant-base.json'
    $candPath = Join-Path $script:tmpDir 'tolerant-cand.json'
    New-BenchmarkJson -Path $basePath -Benchmarks @(@{ name = 'BM_A'; real_time = 100.0; time_unit = 'ns' })
    New-BenchmarkJson -Path $candPath -Benchmarks @(@{ name = 'BM_A'; real_time = 150.0; time_unit = 'ns' })

    $exitCode = Invoke-Compare -BaselinePath $basePath -CandidatePath $candPath -ToleranceFraction 1.0
    $exitCode | Should Be 0
  }

  It 'exits 0 when a benchmark exists only in the candidate' {
    $basePath = Join-Path $script:tmpDir 'only-cand-base.json'
    $candPath = Join-Path $script:tmpDir 'only-cand-cand.json'
    New-BenchmarkJson -Path $basePath -Benchmarks @(@{ name = 'BM_A'; real_time = 100.0; time_unit = 'ns' })
    New-BenchmarkJson -Path $candPath -Benchmarks @(
      @{ name = 'BM_A'; real_time = 100.0; time_unit = 'ns' },
      @{ name = 'BM_New'; real_time = 999.0; time_unit = 'ns' }
    )

    $exitCode = Invoke-Compare -BaselinePath $basePath -CandidatePath $candPath
    $exitCode | Should Be 0
  }

  It 'exits 0 when a benchmark exists only in the baseline' {
    $basePath = Join-Path $script:tmpDir 'only-base-base.json'
    $candPath = Join-Path $script:tmpDir 'only-base-cand.json'
    New-BenchmarkJson -Path $basePath -Benchmarks @(
      @{ name = 'BM_A'; real_time = 100.0; time_unit = 'ns' },
      @{ name = 'BM_Removed'; real_time = 999.0; time_unit = 'ns' }
    )
    New-BenchmarkJson -Path $candPath -Benchmarks @(@{ name = 'BM_A'; real_time = 100.0; time_unit = 'ns' })

    $exitCode = Invoke-Compare -BaselinePath $basePath -CandidatePath $candPath
    $exitCode | Should Be 0
  }

  It 'treats 1000 ns baseline and 1 us candidate as a 0% delta' {
    $basePath = Join-Path $script:tmpDir 'unit-base.json'
    $candPath = Join-Path $script:tmpDir 'unit-cand.json'
    New-BenchmarkJson -Path $basePath -Benchmarks @(@{ name = 'BM_A'; real_time = 1000.0; time_unit = 'ns' })
    New-BenchmarkJson -Path $candPath -Benchmarks @(@{ name = 'BM_A'; real_time = 1.0; time_unit = 'us' })

    $exitCode = Invoke-Compare -BaselinePath $basePath -CandidatePath $candPath
    $exitCode | Should Be 0
  }

  It 'exits non-zero when -CandidatePath is missing' {
    $basePath = Join-Path $script:tmpDir 'missing-candidate-base.json'
    New-BenchmarkJson -Path $basePath -Benchmarks @(@{ name = 'BM_A'; real_time = 100.0; time_unit = 'ns' })

    $exitCode = Invoke-Compare -BaselinePath $basePath -CandidatePath $null
    $exitCode | Should Not Be 0
  }
}
