#requires -Version 7.0
<#
.SYNOPSIS
  Preflight for the agentic loop: fail loudly when a prompt overlay the config
  DECLARES does not actually reach the agent.

.DESCRIPTION
  The overlays are the only prompt text this repo owns, and every way of losing
  them is silent:

    * ANTfrastructure's Resolve-AgenticEngine reads plannerPromptOverlayFile /
      executorPromptOverlayFile from engines.<engine> only. Moving the keys to
      a top-level promptOverlays block therefore made both overlays vanish -
      no warning, no log line, a loop running on the bare shared prompt.
    * New-AgenticComposedPrompt downgrades a missing overlay FILE to a WARN and
      falls back to the shared prompt alone.
    * The Bash half (linux/scripts/lib/agentic-engines.sh) does not implement
      the overlay shape at all; it reads only the older full-override
      plannerPromptFile / executorPromptFile keys.

  Each of those leaves a loop that looks healthy and is not, so this asserts the
  delivery instead of assuming it: it resolves the config exactly the way the
  loop will, then requires the resolved prompt the agent is actually handed to
  CONTAIN the overlay text. A declared overlay that no reader picks up is an
  error here rather than silence downstream.

  A MODULE, not a dot-sourced script, and that is not a style choice: a script's
  param block lands in the DOT-SOURCER's scope, and PowerShell variable names
  are case-insensitive. The first cut of this file carried
  `param([string]$RepoRoot = '', [string]$Engine = '')`, which emptied
  Invoke-AgenticLoop.ps1's own $repoRoot and swallowed its -Engine override the
  moment it was dot-sourced - and in the Pester suite it made every check pass
  vacuously over an empty repo root. Import-Module has its own scope and cannot
  do that.

  Check a config by hand:
    Import-Module ./scripts/agentic-loop/AgenticPromptOverlay.psm1 -Force
    $cfg = Get-Content ./scripts/agentic-loop/AgenticLoop.config.json -Raw | ConvertFrom-Json
    Assert-AgenticPromptOverlay -Config $cfg -RepoRoot (Get-Location).Path -Engine claude
#>

Set-StrictMode -Version Latest

function Get-OverlayConfigValue {
    <#
      .SYNOPSIS
        StrictMode-safe property lookup over both hashtables (tests) and the
        PSCustomObjects ConvertFrom-Json produces. A local copy of the module's
        Get-AgenticConfigValue so the declaration and mirror checks below work
        on a config alone, with no ANTfrastructure checkout.
    #>
    param($Object, [string]$Name, $Default = $null)
    if ($null -eq $Object) { return $Default }
    if ($Object -is [hashtable]) {
        if ($Object.ContainsKey($Name) -and $null -ne $Object[$Name]) { return $Object[$Name] }
        return $Default
    }
    $prop = $Object.PSObject.Properties[$Name]
    if ($prop -and $null -ne $prop.Value) { return $prop.Value }
    return $Default
}

function Get-AgenticOverlayDeclaration {
    <#
      .SYNOPSIS
        Both declared locations of one role's overlay: the top-level
        promptOverlays block (the shape the hub is moving to) and the
        engines.<engine> mirror (the shape the pinned hub reads).
      .OUTPUTS
        Hashtable: Role, Key, TopLevel, Engine, Effective (TopLevel preferred).
    #>
    param(
        $Config,
        [Parameter(Mandatory)][ValidateSet('planner', 'executor')][string]$Role,
        [Parameter(Mandatory)][string]$Engine
    )
    $key = "${Role}PromptOverlayFile"
    $topLevel = Get-OverlayConfigValue (Get-OverlayConfigValue $Config 'promptOverlays' $null) $key $null
    $engines = Get-OverlayConfigValue $Config 'engines' $null
    $engineValue = Get-OverlayConfigValue (Get-OverlayConfigValue $engines $Engine $null) $key $null
    return @{
        Role      = $Role
        Key       = $key
        TopLevel  = $topLevel
        Engine    = $engineValue
        Effective = if ($topLevel) { $topLevel } else { $engineValue }
    }
}

function Assert-AgenticPromptOverlay {
    <#
      .SYNOPSIS
        Throw unless every declared overlay is (a) declared consistently in both
        shapes, (b) present on disk, and (c) present in the prompt text the
        selected engine actually delivers to the agent.
      .PARAMETER Config
        Parsed loop config (ConvertFrom-Json output or a hashtable).
      .PARAMETER RepoRoot
        Repo root the config's repo-relative paths resolve against.
      .PARAMETER Engine
        claude | opencode. Decides which delivery path is checked.
    #>
    param(
        [Parameter(Mandatory)]$Config,
        [Parameter(Mandatory)][string]$RepoRoot,
        [Parameter(Mandatory)][ValidateSet('claude', 'opencode')][string]$Engine
    )

    foreach ($role in @('planner', 'executor')) {
        $decl = Get-AgenticOverlayDeclaration -Config $Config -Role $role -Engine $Engine

        # Nothing declared anywhere: this repo has no overlay for the role and
        # the shared prompt alone is the intended configuration. Not an error -
        # the failure this guards is a DECLARED overlay going unread.
        if (-not $decl.Effective) { continue }

        # The mirror exists only because two readers disagree on where the key
        # lives. Two copies of a path drift, and the drift would show up as the
        # loop quietly using whichever copy its reader happens to consult.
        if ($decl.TopLevel -and $decl.Engine -and $decl.TopLevel -ne $decl.Engine) {
            throw ("Prompt overlay declared twice with different values: promptOverlays.$($decl.Key) = '$($decl.TopLevel)' " +
                   "but engines.$Engine.$($decl.Key) = '$($decl.Engine)'. They are a compatibility mirror of ONE setting " +
                   'and different readers pick different copies; make them identical.')
        }

        $overlayPath = $decl.Effective
        if (-not [System.IO.Path]::IsPathRooted($overlayPath)) {
            $overlayPath = Join-Path $RepoRoot $overlayPath
        }
        if (-not (Test-Path -LiteralPath $overlayPath)) {
            throw ("Prompt overlay declared but missing on disk: $($decl.Key) = '$($decl.Effective)' resolves to " +
                   "'$overlayPath'. ANTfrastructure's New-AgenticComposedPrompt downgrades this to a WARN and runs on the " +
                   'shared prompt alone, so it is caught here instead: restore the file or drop the declaration.')
        }

        $overlayText = ((Get-Content -LiteralPath $overlayPath -Raw) -replace "`r`n", "`n").Trim()
        if (-not $overlayText) {
            throw "Prompt overlay is empty: $overlayPath. An empty overlay is indistinguishable from a lost one; delete the declaration instead."
        }

        # (c) Delivery. Resolved the way the loop resolves it, then read back.
        $delivered = Get-AgenticDeliveredPrompt -Config $Config -RepoRoot $RepoRoot -Engine $Engine -Role $role
        if (-not $delivered.Path) {
            throw ("Prompt overlay declared but NOT read by the '$Engine' reader: $($decl.Key) = '$($decl.Effective)'. " +
                   $delivered.How + ' A declared overlay that no reader picks up is an error, not a silent fallback to the ' +
                   'shared prompt: mirror the key where this reader looks, or teach the reader the shape.')
        }
        if (-not (Test-Path -LiteralPath $delivered.Path)) {
            throw "Prompt for role '$role' resolves to a path that does not exist: $($delivered.Path) ($($delivered.How))"
        }
        # .Contains, not -like: the overlay is prose full of [ ] * ? characters,
        # every one of which -like would read as a wildcard.
        $deliveredText = (Get-Content -LiteralPath $delivered.Path -Raw) -replace "`r`n", "`n"
        if (-not $deliveredText.Contains($overlayText)) {
            throw ("Prompt overlay declared but ABSENT from what the agent is handed: '$overlayPath' is not contained in " +
                   "$($delivered.Path) ($($delivered.How)). The '$role' agent would run without this repo's project rules.")
        }
    }
}

function Get-AgenticDeliveredPrompt {
    <#
      .SYNOPSIS
        The file whose text the engine actually gives the role, plus a sentence
        naming the mechanism (used verbatim in the failure messages).
      .OUTPUTS
        Hashtable: Path ($null when the engine has no delivery path at all) and
        How (the mechanism, or why there is none).
    #>
    param(
        [Parameter(Mandatory)]$Config,
        [Parameter(Mandatory)][string]$RepoRoot,
        [Parameter(Mandatory)][string]$Engine,
        [Parameter(Mandatory)][string]$Role
    )

    if ($Engine -eq 'opencode') {
        # opencode takes no prompt file on its command line: `opencode run
        # --agent <role>` resolves .opencode/agents/<role>.md out of the
        # checkout, so that file IS the delivery path.
        $agentFile = Join-Path $RepoRoot ".opencode/agents/$Role.md"
        return @{
            Path = if (Test-Path -LiteralPath $agentFile) { $agentFile } else { $null }
            How  = "opencode resolves .opencode/agents/$Role.md from the checkout; that file is missing."
        }
    }

    # claude: ANTfrastructure composes shared prompt + overlay into a temp file and
    # passes it to --append-system-prompt-file. Ask the module rather than
    # reimplementing it - the point is to observe the reader that will run.
    if (-not (Get-Command Resolve-AgenticEngine -ErrorAction SilentlyContinue)) {
        throw ('Resolve-AgenticEngine is not available: import ANTfrastructure''s WindowsAgenticLoop.Common module before ' +
               'calling Assert-AgenticPromptOverlay, or the overlay delivery cannot be observed at all.')
    }
    $settings = Resolve-AgenticEngine -Config $Config -RepoRoot $RepoRoot -EngineOverride $Engine
    $path = if ($Role -eq 'planner') { $settings.PlannerPromptFile } else { $settings.ExecutorPromptFile }
    return @{
        Path = if ($path) { $path } else { $null }
        How  = ("Resolve-AgenticEngine returned no $Role prompt file: it reads ${Role}PromptOverlayFile from " +
                "engines.$Engine only, so a declaration that lives solely in the top-level promptOverlays block is invisible to it.")
    }
}

Export-ModuleMember -Function @(
    'Get-OverlayConfigValue',
    'Get-AgenticOverlayDeclaration',
    'Get-AgenticDeliveredPrompt',
    'Assert-AgenticPromptOverlay'
)
