<#
.SYNOPSIS
Launches vnm_terminal with a deterministic synchronized-output scroll payload.

.DESCRIPTION
The default mode is a launcher. It starts vnm_terminal.exe with an explicit
--synchronized-output-scroll-policy value, then runs this same script in
-PayloadOnly mode inside the terminal as the child process.

The launcher validates the deployment shape before starting the app. It accepts
either a raw app binary whose Qt DLLs and platforms\qwindows.dll plugin are
beside vnm_terminal.exe, or a portable launcher whose vnm_terminal_runtime
directory contains that deployed app layout.

-PayloadOnly is intended for the launcher. It writes the deterministic DEC
synchronized-output payload to the current console and does not start the app.
#>

[CmdletBinding(PositionalBinding = $false)]
param(
    [string] $TerminalExe = "",

    [ValidateSet("defer", "immediate-public")]
    [string] $Policy = "immediate-public",

    [string] $CaptureTranscript = "",

    [switch] $WheelTrace,

    [switch] $TranscriptSnapshotDiagnostics,

    [ValidateRange(1, 100000)]
    [int] $PublicRows = 80,

    [ValidateRange(1, 3600)]
    [int] $HoldSeconds = 12,

    [switch] $PayloadOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "terminal_repro_common.ps1")

function Resolve-OptionalOutputPath
{
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return ""
    }

    $parent = Split-Path -Parent $Path
    if (![string]::IsNullOrWhiteSpace($parent) -and
        !(Test-Path -LiteralPath $parent -PathType Container))
    {
        throw "The capture transcript directory does not exist: $parent"
    }

    return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
}

function Write-SynchronizedOutputPayload
{
    param(
        [Parameter(Mandatory = $true)]
        [int] $PublicRows,

        [Parameter(Mandatory = $true)]
        [int] $HoldSeconds
    )

    $escape = [char] 27
    $bell   = [char] 7

    [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)

    for ($row = 0; $row -lt $PublicRows; ++$row) {
        [Console]::Out.WriteLine("public-prefix-{0:D3}" -f $row)
    }

    [Console]::Out.WriteLine("public-prefix-ready: scroll during the synchronized-output hold")
    [Console]::Out.Write("${escape}[?2026h")
    [Console]::Out.Flush()

    try {
        [Console]::Out.Write("HIDDEN-ROW-SENTINEL`r`n")
        [Console]::Out.Write("${escape}[31mHIDDEN-STYLE-SENTINEL${escape}[0m`r`n")
        [Console]::Out.Write(
            "${escape}]8;;https://hidden.invalid${bell}HIDDEN-HYPERLINK-SENTINEL${escape}]8;;${bell}`r`n")
        [Console]::Out.Write("${escape}[s${escape}[3;7HHIDDEN-CURSOR-POSITION-SENTINEL${escape}[u")
        [Console]::Out.Write("${escape}[5 qHIDDEN-CURSOR-SHAPE-SENTINEL`r`n")
        [Console]::Out.Write("${escape}[?25lHIDDEN-MODE-CURSOR-VISIBILITY-SENTINEL`r`n")
        [Console]::Out.Write("${escape}[?7lHIDDEN-MODE-AUTOWRAP-SENTINEL`r`n")
        [Console]::Out.Flush()

        Start-Sleep -Seconds $HoldSeconds
    }
    finally {
        [Console]::Out.Write("${escape}[?7h")
        [Console]::Out.Write("${escape}[?25h")
        [Console]::Out.Write("${escape}[0 q")
        [Console]::Out.Write("${escape}[?2026l")
        [Console]::Out.WriteLine("public-suffix-after-release")
        [Console]::Out.Flush()
    }
}

if ($PayloadOnly) {
    Write-SynchronizedOutputPayload -PublicRows $PublicRows -HoldSeconds $HoldSeconds
    exit 0
}

if (($WheelTrace -or $TranscriptSnapshotDiagnostics) -and
    [string]::IsNullOrWhiteSpace($CaptureTranscript))
{
    throw "-WheelTrace and -TranscriptSnapshotDiagnostics require -CaptureTranscript."
}

$resolvedTerminalExe = Resolve-TerminalExe -ConfiguredTerminalExe $TerminalExe
Assert-TerminalDeployment -ResolvedTerminalExe $resolvedTerminalExe

$scriptPath = (Resolve-Path -LiteralPath $PSCommandPath).ProviderPath
$captureTranscriptPath = Resolve-OptionalOutputPath -Path $CaptureTranscript

$terminalArguments = @("--synchronized-output-scroll-policy", $Policy)
if (![string]::IsNullOrWhiteSpace($captureTranscriptPath)) {
    $terminalArguments += @("--capture-transcript", $captureTranscriptPath)
}
if ($WheelTrace) {
    $terminalArguments += "--wheel-trace"
}
if ($TranscriptSnapshotDiagnostics) {
    $terminalArguments += "--transcript-snapshot-diagnostics"
}

$payloadArguments = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", $scriptPath,
    "-PayloadOnly",
    "-PublicRows", $PublicRows.ToString([System.Globalization.CultureInfo]::InvariantCulture),
    "-HoldSeconds", $HoldSeconds.ToString([System.Globalization.CultureInfo]::InvariantCulture)
)

$terminalArguments += @("--", "powershell.exe") + $payloadArguments

& $resolvedTerminalExe @terminalArguments
exit $LASTEXITCODE
