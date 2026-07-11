function Test-DeployedAppLayout
{
    param(
        [Parameter(Mandatory = $true)]
        [string] $AppDirectory
    )

    $qtCoreDll       = Join-Path $AppDirectory "Qt6Core.dll"
    $windowsPlatform = Join-Path $AppDirectory "platforms\qwindows.dll"

    return (Test-Path -LiteralPath $qtCoreDll -PathType Leaf) -and
        (Test-Path -LiteralPath $windowsPlatform -PathType Leaf)
}

function Test-PortableLauncherLayout
{
    param(
        [Parameter(Mandatory = $true)]
        [string] $LauncherDirectory
    )

    $runtimeDirectory = Join-Path $LauncherDirectory "vnm_terminal_runtime"
    $runtimeExe       = Join-Path $runtimeDirectory "vnm_terminal.exe"

    return (Test-Path -LiteralPath $runtimeExe -PathType Leaf) -and
        (Test-DeployedAppLayout -AppDirectory $runtimeDirectory)
}

function Resolve-DefaultTerminalExe
{
    $repoRoot = Split-Path -Parent $PSScriptRoot
    $candidates = @(
        (Join-Path $repoRoot "build\Release\vnm_terminal.exe")
        (Join-Path $repoRoot "build\Debug\vnm_terminal.exe")
        (Join-Path $repoRoot "build\vnm_terminal.exe")
        (Join-Path $repoRoot "build_codex_transcript_on\Release\vnm_terminal.exe")
        (Join-Path $repoRoot "build_codex_transcript_on\vnm_terminal.exe")
        (Join-Path $repoRoot "dist\portable_candidate\vnm_terminal.exe")
    )

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }

    throw "Pass -TerminalExe, or build the vnm_terminal target so build\Release\vnm_terminal.exe exists."
}

function Resolve-TerminalExe
{
    param(
        [Parameter(Mandatory = $true)]
        [string] $ConfiguredTerminalExe
    )

    $candidate = $ConfiguredTerminalExe
    if ([string]::IsNullOrWhiteSpace($candidate)) {
        $candidate = Resolve-DefaultTerminalExe
    }

    return (Resolve-Path -LiteralPath $candidate).ProviderPath
}

function Assert-TerminalDeployment
{
    param(
        [Parameter(Mandatory = $true)]
        [string] $ResolvedTerminalExe
    )

    $terminalDirectory = Split-Path -Parent $ResolvedTerminalExe
    if ((Test-DeployedAppLayout -AppDirectory $terminalDirectory) -or
        (Test-PortableLauncherLayout -LauncherDirectory $terminalDirectory))
    {
        return
    }

    throw @"
The selected vnm_terminal.exe does not look deployed.

Expected either:
  - Qt6Core.dll and platforms\qwindows.dll beside vnm_terminal.exe, or
  - vnm_terminal_runtime\vnm_terminal.exe with that same deployed layout.

Build the vnm_terminal target first so the post-build deployment step copies
the Qt runtime DLLs and platform plugin, or pass a deployed portable launcher
with -TerminalExe.
"@
}
