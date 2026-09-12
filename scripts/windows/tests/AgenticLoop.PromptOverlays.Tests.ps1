#requires -Version 7.0

# Guards the agentic loop's prompt overlays - the only prompt text this repo
# owns - against the two ways they have already been lost, both silent:
#
#   1. The config declared them somewhere the reader does not look. Moving
#      plannerPromptOverlayFile / executorPromptOverlayFile out of
#      engines.claude into a top-level promptOverlays block did exactly that:
#      ANTfrastructure's Resolve-AgenticEngine reads them from engines.<engine>
#      and nowhere else, so both overlays became unread with no warning and the
#      loop ran on the bare shared prompt.
#   2. .opencode/agents/<role>.md was deleted and gitignored on the premise
#      that the loop regenerates it. Nothing in the pinned submodule writes
#      those files - `opencode run --agent <role>` resolves them out of the
#      checkout - so a fresh clone on `engine: opencode` had no role prompt at
#      all.
#
# Both are asserted here rather than left to be noticed in an agent's output.
#
# NOTE: written for Pester 3.4.0 (what the Windows lane pins) - no BeforeAll
# outside Describe, and the dash-less assertion syntax.

Describe 'Agentic loop prompt overlays' {

    $repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
    $loopDir = Join-Path $repoRoot 'scripts\agentic-loop'
    $configPath = Join-Path $loopDir 'AgenticLoop.config.json'
    $modulePath = Join-Path $repoRoot 'third_party\ANTfrastructure\windows\scripts\modules\WindowsAgenticLoop.Common.psm1'

    Import-Module (Join-Path $loopDir 'AgenticPromptOverlay.psm1') -Force
    $config = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json

    $roles = @('planner', 'executor')

    It 'declares both overlays in the top-level promptOverlays block' {
        foreach ($role in $roles) {
            $decl = Get-AgenticOverlayDeclaration -Config $config -Role $role -Engine 'claude'
            $decl.TopLevel | Should Not BeNullOrEmpty
        }
    }

    It 'mirrors the overlay keys under engines.claude, byte-identical' {
        # The mirror is not a second setting: it exists only because the pinned
        # ANTfrastructure reader looks the keys up under engines.<engine>. Dropping
        # it is the regression this file is named after; letting the two copies
        # differ is the same bug wearing a hat.
        foreach ($role in $roles) {
            $decl = Get-AgenticOverlayDeclaration -Config $config -Role $role -Engine 'claude'
            $decl.Engine | Should Not BeNullOrEmpty
            $decl.Engine | Should Be $decl.TopLevel
        }
    }

    It 'points both overlay declarations at a non-empty file' {
        foreach ($role in $roles) {
            $decl = Get-AgenticOverlayDeclaration -Config $config -Role $role -Engine 'claude'
            $path = Join-Path $repoRoot $decl.Effective
            Test-Path -LiteralPath $path | Should Be $true
            (Get-Item -LiteralPath $path).Length -gt 0 | Should Be $true
        }
    }

    It 'keeps the opencode role prompts tracked, so a fresh clone has them' {
        # `opencode run --agent <role>` reads .opencode/agents/<role>.md out of
        # the checkout and nothing generates it. Untracked means a fresh clone
        # runs both roles with no role prompt.
        $tracked = @(& git -C $repoRoot ls-files '.opencode/agents')
        foreach ($role in $roles) {
            ($tracked -contains ".opencode/agents/$role.md") | Should Be $true
        }
    }

    It 'does not gitignore the opencode role prompts' {
        # A tracked-and-ignored path is the state Repo.GeneratedArtifacts.Tests
        # already rejects, and it is how these files got deleted in the first
        # place. Checked here too so the reason is stated where it applies.
        foreach ($role in $roles) {
            & git -C $repoRoot check-ignore -q ".opencode/agents/$role.md"
            $LASTEXITCODE | Should Be 1
        }
    }

    It 'composes each opencode role prompt from the shared prompt plus the overlay' {
        # The drift that motivated deleting these files was real: they had
        # forked into a stale copy of the shared prompt. This is the gate that
        # makes forking impossible while keeping them tracked - substring
        # containment, so it does not depend on the exact wrapper format a
        # generator would use.
        foreach ($role in $roles) {
            $agentFile = Join-Path $repoRoot ".opencode\agents\$role.md"
            Test-Path -LiteralPath $agentFile | Should Be $true

            $shared = Join-Path $repoRoot "third_party\ANTfrastructure\shared\agentic-loop\system-prompts\$role.md"
            Test-Path -LiteralPath $shared | Should Be $true
            $overlay = Join-Path $repoRoot "scripts\agentic-loop\prompts\$role-overlay.md"

            $agentText = (Get-Content -LiteralPath $agentFile -Raw) -replace "`r`n", "`n"
            $sharedText = ((Get-Content -LiteralPath $shared -Raw) -replace "`r`n", "`n").Trim()
            $overlayText = ((Get-Content -LiteralPath $overlay -Raw) -replace "`r`n", "`n").Trim()

            $agentText.Contains($sharedText) | Should Be $true
            $agentText.Contains($overlayText) | Should Be $true
        }
    }

    # The four cases below call the assertion through this helper instead of
    # `Should Throw`. Pester 3.4.0's Should Throw does not work on PowerShell 7 -
    # `{ throw 'boom' } | Should Throw` FAILS - so every negative case here would
    # have reported "expected an exception" no matter what the code did, and a
    # positive case dressed as `Should Not Throw` would have passed for the same
    # wrong reason. Measured, not assumed: the first cut of this suite failed
    # exactly that way. Returning the message also lets each case assert WHICH
    # failure it got rather than merely that something went wrong.
    function Invoke-OverlayAssertion {
        param($TestConfig, [string]$TestEngine)
        try {
            Assert-AgenticPromptOverlay -Config $TestConfig -RepoRoot $repoRoot -Engine $TestEngine
            return ''
        } catch {
            return $_.Exception.Message
        }
    }

    It 'accepts the real config for the opencode engine' {
        Invoke-OverlayAssertion -TestConfig $config -TestEngine 'opencode' | Should Be ''
    }

    It 'rejects a config whose overlay declaration the reader cannot see' {
        # The B1 regression, reproduced: overlays declared ONLY at the top level.
        # Resolve-AgenticEngine returns no prompt file, the loop would run the
        # bare shared prompt, and this must be an error rather than silence.
        Test-Path -LiteralPath $modulePath | Should Be $true
        Import-Module $modulePath -Force

        $broken = @{
            engine         = 'claude'
            promptOverlays = @{
                plannerPromptOverlayFile  = 'scripts/agentic-loop/prompts/planner-overlay.md'
                executorPromptOverlayFile = 'scripts/agentic-loop/prompts/executor-overlay.md'
            }
            engines        = @{
                claude = @{ plannerModel = 'claude-opus-5'; executorModel = 'claude-sonnet-5' }
            }
        }
        Invoke-OverlayAssertion -TestConfig $broken -TestEngine 'claude' | Should Match 'declared but NOT read'
    }

    It 'accepts the same config once the engine mirror is restored' {
        # Same config plus the mirror: the module composes shared + overlay and
        # the overlay text is present in what claude is handed. Without this the
        # case above would pass for any reason at all, including a check that
        # rejects every config it is given.
        Import-Module $modulePath -Force

        $fixed = @{
            engine         = 'claude'
            promptOverlays = @{
                plannerPromptOverlayFile  = 'scripts/agentic-loop/prompts/planner-overlay.md'
                executorPromptOverlayFile = 'scripts/agentic-loop/prompts/executor-overlay.md'
            }
            engines        = @{
                claude = @{
                    plannerModel              = 'claude-opus-5'
                    executorModel             = 'claude-sonnet-5'
                    plannerPromptOverlayFile  = 'scripts/agentic-loop/prompts/planner-overlay.md'
                    executorPromptOverlayFile = 'scripts/agentic-loop/prompts/executor-overlay.md'
                }
            }
        }
        Invoke-OverlayAssertion -TestConfig $fixed -TestEngine 'claude' | Should Be ''
    }

    It 'rejects a mirror that has drifted from the top-level declaration' {
        $drifted = @{
            engine         = 'opencode'
            promptOverlays = @{ plannerPromptOverlayFile = 'scripts/agentic-loop/prompts/planner-overlay.md' }
            engines        = @{
                opencode = @{ plannerPromptOverlayFile = 'scripts/agentic-loop/prompts/planner-overlay-OLD.md' }
            }
        }
        Invoke-OverlayAssertion -TestConfig $drifted -TestEngine 'opencode' | Should Match 'declared twice with different values'
    }

    It 'rejects a declared overlay whose file is missing' {
        $missing = @{
            engine         = 'opencode'
            promptOverlays = @{ plannerPromptOverlayFile = 'scripts/agentic-loop/prompts/does-not-exist.md' }
        }
        Invoke-OverlayAssertion -TestConfig $missing -TestEngine 'opencode' | Should Match 'declared but missing on disk'
    }
}
