<#
.SYNOPSIS
Runs a deterministic Codex resume and windowed-width resize profiling scenario.

.DESCRIPTION
Launches a directly deployed vnm_terminal.exe with an explicit ConPTY child
command: codex resume <SessionId>. After the terminal window appears, the
driver pins that exact HWND, sets the explicit windowed size from -WindowSize,
and holds it for -InitialWindowedHoldMs. It then increases the width by
-HorizontalResizeDeltaPx, restores the original width, and holds the settled
windowed state for -FinalWindowedHoldMs so delayed Codex output is included in
the capture before ending. It records
every request and observation in a machine-readable manifest, requires output,
keeps the app open after the child exits, and then sends Ctrl+C through the
terminal window before closing the app. Because the app is kept open after the
child exits, the driver sends one Ctrl+C by default; additional attempts are
opt-in with -CtrlCAttempts.

The SessionId is intentionally not optional at run time. This driver never
substitutes codex --last, because that would make the workload non-reproducible.

The output directory must be new or empty. The driver does not remove or
overwrite pre-existing files. It writes the app profile, final metrics,
metrics timeline, redirected stdout/stderr, the window manifest, and optionally
one per-run WPR CPU.verbose ETL file. Successful metrics must use the v3 schema,
come from a compiled profiling build, observe output, and report a scripted
child interruption (process_exit_reason INTERRUPTED and process_exit_code 130)
with no backend errors.

The driver also requires the resolved Codex executable to appear as a direct
child of vnm_terminal.exe. It records that PID and path, waits on that child
after each Ctrl+C, and closes the keep-open terminal only after the child exits.

The optional -KernelTrace switch requires the invoking PowerShell process to
already be elevated, starts WPR directly with CPU.verbose in file mode, and
stops the same named trace in finally, retrying one failed stop. It does not
request elevation or display a UAC prompt. If both stop attempts fail, it
performs one bounded -cancel for the same instance and records whether WPR's
success exit code proved cancellation. Without -KernelTrace, kernel tracing is
not part of this driver; run WPR separately if a different WPR profile is
required.

.EXAMPLE
.\tools\codex_resume_resize_profile.ps1 -SessionId 01234567-89ab-cdef-0123-456789abcdef

.EXAMPLE
.\tools\codex_resume_resize_profile.ps1 `
    -SessionId 01234567-89ab-cdef-0123-456789abcdef `
    -TerminalExe C:\build\Debug\vnm_terminal.exe `
    -OutputDirectory C:\captures\codex-resize-01 `
    -WindowSize 900x600 -HorizontalResizeDeltaPx 50 `
    -FinalWindowedHoldMs 10000 -KernelTrace

.NOTES
The default working directory is C:\plms\varinomics\logonomic. The default
window size is the terminal application's explicit 900x600 logical-pixel
default; pass -WindowSize to choose another starting size. The application's
custom chrome remains the default; pass -NativeTitlebar to opt into the
platform titlebar. The driver pins the initially discovered top-level HWND and
fails if it disappears or changes. It validates the final v3 metrics contract:
compiled profiling, requested profile text, observed output, child exit,
zero backend errors, and the scripted INTERRUPTED child exit with code 130.

Run help without starting a terminal or Codex:
.\tools\codex_resume_resize_profile.ps1 -Help
#>

[CmdletBinding(PositionalBinding = $false)]
param(
    [string] $SessionId = "",

    [string] $TerminalExe = "",

    [string] $CodexCommand = "codex",

    [string] $CodexAccountHome = "C:\Users\imak\.codex-dev",

    [string] $WorkingDirectory = "C:\plms\varinomics\logonomic",

    [string] $OutputDirectory = "",

    [string] $WindowSize = "900x600",

    [ValidateRange(0, 600000)]
    [int] $InitialWindowedHoldMs = 10000,

    [ValidateRange(0, 600000)]
    [int] $ResizedWindowHoldMs = 10000,

    [ValidateRange(1, 1000)]
    [int] $HorizontalResizeDeltaPx = 50,

    [ValidateRange(0, 600000)]
    [int] $FinalWindowedHoldMs = 10000,

    [ValidateRange(100, 120000)]
    [int] $WindowDiscoveryTimeoutMs = 30000,

    [ValidateRange(100, 120000)]
    [int] $CodexChildDiscoveryTimeoutMs = 30000,

    [ValidateRange(100, 120000)]
    [int] $TransitionTimeoutMs = 15000,

    [ValidateRange(10, 2000)]
    [int] $PollIntervalMs = 50,

    [ValidateRange(1, 3)]
    [int] $CtrlCAttempts = 1,

    [ValidateRange(100, 10000)]
    [int] $CtrlCRepeatDelayMs = 1000,

    [ValidateRange(1000, 300000)]
    [int] $ExitTimeoutMs = 30000,

    [ValidateRange(100, 60000)]
    [int] $MetricsTimelineIntervalMs = 1000,

    [ValidateRange(1000, 600000)]
    [int] $WprCommandTimeoutMs = 120000,

    [switch] $NativeTitlebar,

    [switch] $CaptureOutput,

    [switch] $KernelTrace,

    [switch] $Help
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$codexCimOperationTimeoutSec = 5

if ($Help) {
    @"
codex_resume_resize_profile.ps1

Required:
  -SessionId <exact-id>       Resume this exact Codex session. --last is never used.

Common options:
  -TerminalExe <path>         Direct deployed vnm_terminal.exe; default uses
                              terminal_repro_common.ps1, but portable launchers are rejected.
                              Release uses Qt6Core.dll/qwindows.dll; Debug uses
                              Qt6Cored.dll/qwindowsd.dll beside the executable.
  -CodexCommand <name|path>   Bare name or explicit .exe; resolved to an actual
                              executable, hashed, and passed after the terminal's --;
                              default codex.
  -CodexAccountHome <path>    Existing Codex account-home directory; default:
                              C:\Users\imak\.codex-dev. Its resolved path is passed
                              as CODEX_HOME to the direct Codex child.
  -WorkingDirectory <path>    App and child working directory; default:
                              C:\plms\varinomics\logonomic
  -OutputDirectory <path>     New or empty directory for all run artifacts.
  -WindowSize <WxH>           Initial logical window size; default 900x600.
  -InitialWindowedHoldMs <n>
                              Hold the discovered windowed state before the
                              first width increase; default 10000.
  -HorizontalResizeDeltaPx <n>
                              Increase the explicit width by this many pixels;
                              default 50.
  -ResizedWindowHoldMs <n>    Hold the wider windowed state; default 10000.
  -FinalWindowedHoldMs <n>    After restoring the original width, hold the
                              final windowed state before ending; default 10000.
  -NativeTitlebar             Use the platform titlebar instead of custom chrome.
  -CaptureOutput               Retain the bounded backend-output capture in the
                              run output directory for workload analysis.

Bounded timing:
  -WindowDiscoveryTimeoutMs   Window discovery deadline; default 30000.
  -CodexChildDiscoveryTimeoutMs
                              Direct Codex-child discovery deadline; default 30000.
  Codex CIM operation timeout   Fixed Get-CimInstance bound; 5 seconds (not configurable).
  -TransitionTimeoutMs        Per-transition state/rect deadline; default 15000.
  -PollIntervalMs             Window observation interval; default 50.
  -CtrlCAttempts <n>          Maximum automated Ctrl+C sends; default 1.
                              Additional attempts are opt-in.
  -CtrlCRepeatDelayMs         Delay between Ctrl+C sends; default 1000.
  -ExitTimeoutMs               Codex-child and graceful app exit deadline; default 30000.
  -MetricsTimelineIntervalMs  App metrics sampling interval; default 1000.
  -WprCommandTimeoutMs         Bounded wait for each direct WPR command; default 120000.

Kernel tracing:
  -KernelTrace                Start WPR CPU.verbose file-mode tracing directly.
                              The invoking PowerShell must already be elevated;
                              this switch never requests UAC elevation.
                              The exact per-run ETL is kernel_cpu_verbose.etl in
                              the output directory. WPR is stopped in finally.
                              A failed or timed-out stop is retried once and recorded;
                              a timed-out start also receives bounded cleanup attempts.
                              If both stops fail, one bounded -cancel is attempted;
                              failure remains fatal and the result is recorded.

The driver passes these app options:
  --cwd, --window-size, --profile-text, --metrics-json,
  --metrics-timeline-jsonl, --metrics-timeline-interval-ms,
  --require-output, --keep-open-after-process-exits, and then:
  -- <CodexCommand> resume <SessionId>

The final metrics contract requires process_exit_reason INTERRUPTED with
process_exit_code 130; ineffective scripted Ctrl+C does not pass validation.

Transition order is sequence 1, explicit base windowed geometry with the
InitialWindowedHoldMs hold; sequence 2 increases only the width by
HorizontalResizeDeltaPx and holds it for ResizedWindowHoldMs; sequence 3
restores the base geometry and holds it for FinalWindowedHoldMs before ending.
No maximize or fullscreen request is made.

The child environment contains VNM_TERMINAL_SETTINGS_NO_PERSIST=1 and the
resolved -CodexAccountHome as CODEX_HOME. No real terminal or Codex process is
started by -Help.
"@ | Write-Host
    return
}

if ($env:OS -ne "Windows_NT") {
    throw "This driver is Windows-only."
}

. (Join-Path $PSScriptRoot "terminal_repro_common.ps1")

function Get-UtcTimestamp
{
    return [DateTime]::UtcNow.ToString("o", [Globalization.CultureInfo]::InvariantCulture)
}

function ConvertTo-WindowsCommandLineArgument
{
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string] $Argument
    )

    if ($Argument.Length -gt 0 -and $Argument -notmatch '[\s"]') {
        return $Argument
    }

    $builder = [Text.StringBuilder]::new()
    [void] $builder.Append('"')
    $backslashes = 0

    foreach ($character in $Argument.ToCharArray()) {
        if ($character -eq '\') {
            ++$backslashes
            continue
        }

        if ($character -eq '"') {
            if ($backslashes -gt 0) {
                [void] $builder.Append(('\' * ($backslashes * 2)))
            }
            [void] $builder.Append('\')
            [void] $builder.Append('"')
            $backslashes = 0
            continue
        }

        if ($backslashes -gt 0) {
            [void] $builder.Append(('\' * $backslashes))
            $backslashes = 0
        }
        [void] $builder.Append($character)
    }

    if ($backslashes -gt 0) {
        [void] $builder.Append(('\' * ($backslashes * 2)))
    }
    [void] $builder.Append('"')
    return $builder.ToString()
}

function Join-WindowsCommandLine
{
    param(
        [Parameter(Mandatory = $true)]
        [string[]] $Arguments
    )

    return (($Arguments | ForEach-Object {
        ConvertTo-WindowsCommandLineArgument -Argument $_
    }) -join " ")
}

function Resolve-OutputDirectory
{
    param(
        [Parameter(Mandatory = $true)]
        [string] $ConfiguredOutputDirectory
    )

    if (![string]::IsNullOrWhiteSpace($ConfiguredOutputDirectory)) {
        $candidate = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath(
            $ConfiguredOutputDirectory)
        if (Test-Path -LiteralPath $candidate) {
            if (!(Test-Path -LiteralPath $candidate -PathType Container)) {
                throw "The output path is not a directory: $candidate"
            }
            if (@(Get-ChildItem -LiteralPath $candidate -Force).Count -ne 0) {
                throw "The output directory must be empty: $candidate"
            }
        }
        else {
            [IO.Directory]::CreateDirectory($candidate) | Out-Null
        }
        return (Resolve-Path -LiteralPath $candidate).ProviderPath
    }

    $temporaryRoot = [IO.Path]::GetTempPath()
    for ($attempt = 0; $attempt -lt 10; ++$attempt) {
        $name = "vnm_terminal_codex_resume_resize_{0}_{1}" -f @(
            (Get-Date -Format "yyyyMMdd_HHmmss_fff"),
            ([Guid]::NewGuid().ToString("N")))
        $candidate = Join-Path $temporaryRoot $name
        try {
            [IO.Directory]::CreateDirectory($candidate) | Out-Null
            return (Resolve-Path -LiteralPath $candidate).ProviderPath
        }
        catch [IO.IOException] {
            if ($attempt -eq 9) {
                throw
            }
        }
    }

    throw "Could not create a unique output directory under $temporaryRoot"
}

function Assert-SessionId
{
    param(
        [Parameter(Mandatory = $true)]
        [string] $Value
    )

    if ([string]::IsNullOrWhiteSpace($Value)) {
        throw "-SessionId is required and must name one exact Codex session; --last is not supported."
    }
    if ($Value -match '^[\s-]' -or $Value -match '[\r\n\x00-\x1f\x7f]') {
        throw "-SessionId must be one non-option, single-line session identifier."
    }
    if ($Value -ieq "--last") {
        throw "-SessionId must be an exact session identifier; --last is intentionally rejected."
    }
}

function Assert-WindowSize
{
    param(
        [Parameter(Mandatory = $true)]
        [string] $Value
    )

    $match = [Regex]::Match($Value, '^([0-9]+)x([0-9]+)$')
    if (!$match.Success) {
        throw "-WindowSize must use positive <width>x<height> values: $Value"
    }

    [int] $width = 0
    [int] $height = 0
    if (![int]::TryParse($match.Groups[1].Value, [Globalization.NumberStyles]::None,
            [Globalization.CultureInfo]::InvariantCulture, [ref] $width) -or
        ![int]::TryParse($match.Groups[2].Value, [Globalization.NumberStyles]::None,
            [Globalization.CultureInfo]::InvariantCulture, [ref] $height) -or
        $width -lt 100 -or $height -lt 100 -or $width -gt 8192 -or $height -gt 8192)
    {
        throw "-WindowSize axes must each be in the range 100..8192: $Value"
    }
}

function Convert-WindowSizeToDimensions
{
    param(
        [Parameter(Mandatory = $true)]
        [string] $Value
    )

    $match = [Regex]::Match($Value, '^([0-9]+)x([0-9]+)$')
    if (!$match.Success) {
        throw "Cannot convert invalid window size: $Value"
    }

    return [ordered]@{
        width  = [int] $match.Groups[1].Value
        height = [int] $match.Groups[2].Value
    }
}

function Resolve-CodexExecutable
{
    param(
        [Parameter(Mandatory = $true)]
        [string] $ConfiguredCodexCommand
    )

    if ([string]::IsNullOrWhiteSpace($ConfiguredCodexCommand)) {
        throw "-CodexCommand must name a Codex .exe or a bare executable name."
    }

    $hasDirectorySeparator = $ConfiguredCodexCommand.IndexOfAny([char[]] "\/") -ge 0
    $candidate = $ConfiguredCodexCommand
    if ($hasDirectorySeparator) {
        $candidate = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath(
            $ConfiguredCodexCommand)
    }
    else {
        $extension = [IO.Path]::GetExtension($ConfiguredCodexCommand)
        if (![string]::IsNullOrWhiteSpace($extension) -and $extension -ine ".exe") {
            throw "-CodexCommand must resolve to an .exe; non-executable shims are rejected: $ConfiguredCodexCommand"
        }

        $lookupName = if ([string]::IsNullOrWhiteSpace($extension)) {
            "$ConfiguredCodexCommand.exe"
        }
        else {
            $ConfiguredCodexCommand
        }
        $command = Get-Command -Name $lookupName -CommandType Application `
            -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -eq $command) {
            throw "Could not resolve Codex executable '$lookupName' through PATH."
        }
        $candidate = $command.Source
    }

    if ([IO.Path]::GetExtension($candidate) -ine ".exe") {
        throw "The resolved Codex command is not an .exe: $candidate"
    }
    if (!(Test-Path -LiteralPath $candidate -PathType Leaf)) {
        throw "The resolved Codex executable does not exist: $candidate"
    }

    return (Resolve-Path -LiteralPath $candidate).ProviderPath
}

function Resolve-CodexAccountHome
{
    param(
        [Parameter(Mandatory = $true)]
        [string] $ConfiguredPath
    )

    if ([string]::IsNullOrWhiteSpace($ConfiguredPath)) {
        throw "-CodexAccountHome must name an existing directory."
    }

    $unresolvedPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath(
        $ConfiguredPath)
    if (!(Test-Path -LiteralPath $unresolvedPath -PathType Container)) {
        throw "The Codex account-home directory does not exist: $unresolvedPath"
    }

    return (Resolve-Path -LiteralPath $unresolvedPath).ProviderPath
}

function Assert-DirectTerminalDeployment
{
    param(
        [Parameter(Mandatory = $true)]
        [string] $ResolvedTerminalExe
    )

    $terminalDirectory = Split-Path -Parent $ResolvedTerminalExe
    $terminalName = [IO.Path]::GetFileName($ResolvedTerminalExe)
    if ($terminalName -ine "vnm_terminal.exe") {
        throw "This HWND-based driver requires the direct vnm_terminal.exe binary: $ResolvedTerminalExe"
    }
    if ((Split-Path -Leaf $terminalDirectory) -ieq "vnm_terminal_runtime") {
        throw "Portable launcher runtime layouts are not supported by this HWND-based driver: $ResolvedTerminalExe"
    }

    $portableRuntimeExe = Join-Path $terminalDirectory "vnm_terminal_runtime\vnm_terminal.exe"
    if (Test-Path -LiteralPath $portableRuntimeExe -PathType Leaf) {
        throw "Portable launcher layouts are not supported by this HWND-based driver: $ResolvedTerminalExe"
    }

    $releaseLayout =
        (Test-Path -LiteralPath (Join-Path $terminalDirectory "Qt6Core.dll") -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-Path $terminalDirectory "platforms\qwindows.dll") -PathType Leaf)
    $debugLayout =
        (Test-Path -LiteralPath (Join-Path $terminalDirectory "Qt6Cored.dll") -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-Path $terminalDirectory "platforms\qwindowsd.dll") -PathType Leaf)
    if (!$releaseLayout -and !$debugLayout) {
        throw @"
The selected vnm_terminal.exe is not a directly deployed app.

Expected one complete Qt deployment beside:
  Qt6Core.dll and platforms\qwindows.dll (release or other non-debug), or
  Qt6Cored.dll and platforms\qwindowsd.dll (Debug)

The files must be beside:
  $ResolvedTerminalExe

Portable launchers are intentionally rejected because this driver pins the
launched process PID and its top-level HWND.
"@
    }
}

function Assert-InputPaths
{
    param(
        [Parameter(Mandatory = $true)]
        [string] $ConfiguredTerminalExe,

        [Parameter(Mandatory = $true)]
        [string] $ConfiguredWorkingDirectory,

        [Parameter(Mandatory = $true)]
        [string] $ConfiguredCodexCommand,

        [Parameter(Mandatory = $true)]
        [string] $ConfiguredCodexAccountHome
    )

    $resolvedExe = Resolve-TerminalExe -ConfiguredTerminalExe $ConfiguredTerminalExe
    Assert-DirectTerminalDeployment -ResolvedTerminalExe $resolvedExe

    $resolvedWorkingDirectory = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath(
        $ConfiguredWorkingDirectory)
    if (!(Test-Path -LiteralPath $resolvedWorkingDirectory -PathType Container)) {
        throw "The working directory does not exist: $resolvedWorkingDirectory"
    }

    $resolvedCodexCommand = Resolve-CodexExecutable -ConfiguredCodexCommand $ConfiguredCodexCommand
    $resolvedCodexAccountHome = Resolve-CodexAccountHome `
        -ConfiguredPath $ConfiguredCodexAccountHome

    return [ordered]@{
        terminal_exe        = $resolvedExe
        working_directory   = (Resolve-Path -LiteralPath $resolvedWorkingDirectory).ProviderPath
        codex_exe           = $resolvedCodexCommand
        codex_account_home  = $resolvedCodexAccountHome
    }
}

if ($SessionId -match '\s') {
    throw "-SessionId must be one exact, whitespace-free session identifier."
}
Assert-SessionId -Value $SessionId
Assert-WindowSize -Value $WindowSize
$windowDimensions = Convert-WindowSizeToDimensions -Value $WindowSize
$expandedWindowWidth = $windowDimensions.width + $HorizontalResizeDeltaPx
if ($expandedWindowWidth -gt 8192) {
    throw (
        "-WindowSize width plus -HorizontalResizeDeltaPx must be no greater than 8192: " +
        "{0}+{1}" -f @($windowDimensions.width, $HorizontalResizeDeltaPx))
}

$inputPaths = Assert-InputPaths `
    -ConfiguredTerminalExe $TerminalExe `
    -ConfiguredWorkingDirectory $WorkingDirectory `
    -ConfiguredCodexCommand $CodexCommand `
    -ConfiguredCodexAccountHome $CodexAccountHome

$resolvedOutputDirectory = Resolve-OutputDirectory -ConfiguredOutputDirectory $OutputDirectory
$manifestPath = Join-Path $resolvedOutputDirectory "manifest.json"
$profilePath = Join-Path $resolvedOutputDirectory "terminal_profile.txt"
$metricsPath = Join-Path $resolvedOutputDirectory "terminal_metrics.json"
$metricsTimelinePath = Join-Path $resolvedOutputDirectory "terminal_metrics.jsonl"
$captureOutputBasePath = Join-Path $resolvedOutputDirectory "backend_output"
$stdoutPath = Join-Path $resolvedOutputDirectory "terminal_stdout.log"
$stderrPath = Join-Path $resolvedOutputDirectory "terminal_stderr.log"
$etlPath = Join-Path $resolvedOutputDirectory "kernel_cpu_verbose.etl"

$terminalArguments = @(
    "--cwd", $inputPaths.working_directory,
    "--window-size", $WindowSize,
    "--profile-text", $profilePath,
    "--metrics-json", $metricsPath,
    "--metrics-timeline-jsonl", $metricsTimelinePath,
    "--metrics-timeline-interval-ms",
    $MetricsTimelineIntervalMs.ToString([Globalization.CultureInfo]::InvariantCulture),
    "--require-output",
    "--keep-open-after-process-exits"
)
if ($CaptureOutput) {
    $terminalArguments += @("--capture-output", $captureOutputBasePath)
}
if ($NativeTitlebar) {
    $terminalArguments += "--native-titlebar"
}
$terminalArguments += @(
    "--",
    $inputPaths.codex_exe,
    "resume",
    $SessionId
)

$runStopwatch = [Diagnostics.Stopwatch]::StartNew()
$manifest = [ordered]@{
    schema                  = "vnm_terminal_codex_resume_resize_manifest_v1"
    status                  = "starting"
    started_at_utc          = Get-UtcTimestamp
    finished_at_utc         = $null
    elapsed_ms              = $null
    session_id              = $SessionId
    terminal_exe            = $inputPaths.terminal_exe
    terminal_exe_sha256     = (Get-FileHash -LiteralPath $inputPaths.terminal_exe -Algorithm SHA256).Hash
    terminal_deployment     = "direct"
    codex_exe               = $inputPaths.codex_exe
    codex_exe_sha256        = (Get-FileHash -LiteralPath $inputPaths.codex_exe -Algorithm SHA256).Hash
    codex_account_home      = $inputPaths.codex_account_home
    child_command           = @($inputPaths.codex_exe, "resume", $SessionId)
    child_environment       = [ordered]@{
        CODEX_HOME                       = $inputPaths.codex_account_home
        VNM_TERMINAL_SETTINGS_NO_PERSIST = "1"
    }
    codex_process            = [ordered]@{
        pid                  = $null
        path                 = $null
        parent_pid           = $null
        discovered_at_utc    = $null
        exited               = $false
        exit_observed_at_utc = $null
    }
    codex_child_observations = New-Object System.Collections.ArrayList
    working_directory       = $inputPaths.working_directory
    window_size_argument    = $WindowSize
    base_window_width_px    = $windowDimensions.width
    base_window_height_px   = $windowDimensions.height
    expanded_window_width_px = $expandedWindowWidth
    expanded_window_height_px = $windowDimensions.height
    initial_windowed_hold_ms = $InitialWindowedHoldMs
    resized_window_hold_ms  = $ResizedWindowHoldMs
    horizontal_resize_delta_px = $HorizontalResizeDeltaPx
    final_windowed_hold_ms  = $FinalWindowedHoldMs
    window_discovery_timeout_ms = $WindowDiscoveryTimeoutMs
    codex_child_discovery_timeout_ms = $CodexChildDiscoveryTimeoutMs
    codex_cim_operation_timeout_sec = $codexCimOperationTimeoutSec
    transition_timeout_ms   = $TransitionTimeoutMs
    poll_interval_ms        = $PollIntervalMs
    ctrl_c_attempts         = $CtrlCAttempts
    ctrl_c_repeat_delay_ms  = $CtrlCRepeatDelayMs
    exit_timeout_ms         = $ExitTimeoutMs
    metrics_timeline_interval_ms = $MetricsTimelineIntervalMs
    wpr_command_timeout_ms  = $WprCommandTimeoutMs
    native_titlebar         = [bool] $NativeTitlebar
    capture_output_requested = [bool] $CaptureOutput
    capture_output_base_path = if ($CaptureOutput) { $captureOutputBasePath } else { $null }
    kernel_trace_requested  = [bool] $KernelTrace
    kernel_trace_elevation_required = [bool] $KernelTrace
    kernel_trace_elevation_verified = $false
    kernel_trace_instance   = $null
    kernel_trace_start_attempted = $false
    kernel_trace_started_at_utc = $null
    kernel_trace_stopped_at_utc = $null
    kernel_trace_command_records = New-Object System.Collections.ArrayList
    kernel_trace_stop_attempts = New-Object System.Collections.ArrayList
    kernel_trace_cancel_attempts = New-Object System.Collections.ArrayList
    kernel_trace_stop_succeeded = $false
    kernel_trace_cancel_attempted = $false
    kernel_trace_cancel_succeeded = $false
    kernel_trace_cancel_proven = $false
    kernel_trace_cancel_error = $null
    terminal_arguments       = $terminalArguments
    metrics_validation       = [ordered]@{
        schema                  = "vnm_terminal_runtime_metrics_v3"
        profiling_compiled     = $true
        profile_text_requested = $true
        output_seen            = $true
        process_exited         = $true
        backend_error_count    = 0
        timeout_expired        = $false
        process_exit_reason     = "INTERRUPTED"
        process_exit_code       = 130
    }
    artifacts               = [ordered]@{
        manifest           = $manifestPath
        profile_text       = $profilePath
        metrics_json       = $metricsPath
        metrics_timeline   = $metricsTimelinePath
        stdout             = $stdoutPath
        stderr             = $stderrPath
        kernel_etl         = if ($KernelTrace) { $etlPath } else { $null }
    }
    process                = [ordered]@{
        pid              = $null
        exit_code        = $null
        exited           = $false
        exit_observed_at_utc = $null
    }
    forced_tree_termination = $false
    shutdown_sequence_attempted = $false
    initial_window_observation = $null
    pinned_window_handle     = $null
    transitions             = New-Object System.Collections.ArrayList
    exit_sequence           = New-Object System.Collections.ArrayList
    error                   = $null
    inventory_excludes_manifest = $true
    files                   = @()
}

function Write-RunManifest
{
    $manifestJson = $script:manifest | ConvertTo-Json -Depth 12
    [IO.File]::WriteAllText(
        $script:manifestPath,
        $manifestJson,
        [Text.UTF8Encoding]::new($false))
}

function Get-RunElapsedMilliseconds
{
    return [Math]::Round($script:runStopwatch.Elapsed.TotalMilliseconds, 3)
}

if ($null -eq ("VnmCodexResumeResizeNativeMethods" -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public sealed class VnmCodexWindowSnapshot
{
    public IntPtr Handle { get; set; }
    public string HandleHex { get; set; }
    public int OwningProcessId { get; set; }
    public bool Exists { get; set; }
    public bool Visible { get; set; }
    public bool Maximized { get; set; }
    public bool Minimized { get; set; }
    public int Left { get; set; }
    public int Top { get; set; }
    public int Right { get; set; }
    public int Bottom { get; set; }
    public int Width { get { return Right - Left; } }
    public int Height { get { return Bottom - Top; } }
    public string State
    {
        get
        {
            if (!Exists) return "gone";
            if (!Visible) return "hidden";
            if (Minimized) return "minimized";
            if (Maximized) return "maximized";
            return "windowed";
        }
    }
}

public static class VnmCodexResumeResizeNativeMethods
{
    private const uint GW_OWNER = 4;
    private const int SW_RESTORE = 9;
    private const int SW_MAXIMIZE = 3;
    private const uint WM_KEYDOWN = 0x0100;
    private const uint WM_KEYUP = 0x0101;
    private const uint VK_CONTROL = 0x11;
    private const uint VK_C = 0x43;
    private const uint INPUT_KEYBOARD = 1;
    private const uint KEYEVENTF_KEYUP = 0x0002;
    private const uint SWP_NOZORDER = 0x0004;
    private const uint SWP_NOACTIVATE = 0x0010;

    [StructLayout(LayoutKind.Sequential)]
    private struct RECT
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct KEYBDINPUT
    {
        public ushort wVk;
        public ushort wScan;
        public uint dwFlags;
        public uint time;
        public UIntPtr dwExtraInfo;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct INPUT
    {
        public uint type;
        public KEYBDINPUT ki;
    }

    [DllImport("user32.dll")]
    private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lParam);

    private delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);

    [DllImport("user32.dll")]
    private static extern bool IsWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern IntPtr GetWindow(IntPtr hWnd, uint command);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool SetWindowPos(
        IntPtr hWnd,
        IntPtr hWndInsertAfter,
        int x,
        int y,
        int cx,
        int cy,
        uint flags);

    [DllImport("user32.dll")]
    private static extern bool IsZoomed(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern bool IsIconic(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern bool ShowWindowAsync(IntPtr hWnd, int command);

    [DllImport("user32.dll")]
    private static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern bool BringWindowToTop(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint SendInput(uint numberOfInputs, INPUT[] inputs, int sizeOfInput);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool PostMessage(IntPtr hWnd, uint message, UIntPtr wParam, IntPtr lParam);

    public static IntPtr[] FindVisibleTopLevelWindows(int processId)
    {
        var handles = new List<IntPtr>();
        EnumWindows((hWnd, lParam) =>
        {
            uint candidateProcessId;
            GetWindowThreadProcessId(hWnd, out candidateProcessId);
            if (candidateProcessId == (uint) processId &&
                IsWindowVisible(hWnd) &&
                GetWindow(hWnd, GW_OWNER) == IntPtr.Zero)
            {
                handles.Add(hWnd);
            }
            return true;
        }, IntPtr.Zero);
        return handles.ToArray();
    }

    public static VnmCodexWindowSnapshot ReadSnapshot(IntPtr hWnd)
    {
        if (hWnd == IntPtr.Zero || !IsWindow(hWnd)) return null;
        RECT rect;
        if (!GetWindowRect(hWnd, out rect)) return null;
        uint owningProcessId;
        GetWindowThreadProcessId(hWnd, out owningProcessId);
        return new VnmCodexWindowSnapshot
        {
            Handle = hWnd,
            HandleHex = "0x" + hWnd.ToInt64().ToString("X"),
            OwningProcessId = (int) owningProcessId,
            Exists = true,
            Visible = IsWindowVisible(hWnd),
            Maximized = IsZoomed(hWnd),
            Minimized = IsIconic(hWnd),
            Left = rect.Left,
            Top = rect.Top,
            Right = rect.Right,
            Bottom = rect.Bottom
        };
    }

    public static bool ShowMaximized(IntPtr hWnd)
    {
        return ShowWindowAsync(hWnd, SW_MAXIMIZE);
    }

    public static bool ShowRestored(IntPtr hWnd)
    {
        return ShowWindowAsync(hWnd, SW_RESTORE);
    }

    public static bool SetWindowSize(IntPtr hWnd, int width, int height)
    {
        if (hWnd == IntPtr.Zero || !IsWindow(hWnd)) return false;
        RECT rect;
        if (!GetWindowRect(hWnd, out rect)) return false;
        return SetWindowPos(
            hWnd,
            IntPtr.Zero,
            rect.Left,
            rect.Top,
            width,
            height,
            SWP_NOZORDER | SWP_NOACTIVATE);
    }

    public static bool Activate(IntPtr hWnd)
    {
        BringWindowToTop(hWnd);
        return SetForegroundWindow(hWnd);
    }

    public static IntPtr ForegroundWindow()
    {
        return GetForegroundWindow();
    }

    public static bool SendCtrlC()
    {
        var inputs = new INPUT[]
        {
            KeyboardInput((ushort) VK_CONTROL, 0),
            KeyboardInput((ushort) VK_C, 0),
            KeyboardInput((ushort) VK_C, KEYEVENTF_KEYUP),
            KeyboardInput((ushort) VK_CONTROL, KEYEVENTF_KEYUP)
        };
        return SendInput((uint) inputs.Length, inputs, Marshal.SizeOf(typeof(INPUT))) ==
            (uint) inputs.Length;
    }

    public static bool PostCtrlC(IntPtr hWnd)
    {
        bool first = PostMessage(hWnd, WM_KEYDOWN, (UIntPtr) VK_CONTROL, IntPtr.Zero);
        bool second = PostMessage(hWnd, WM_KEYDOWN, (UIntPtr) VK_C, IntPtr.Zero);
        bool third = PostMessage(hWnd, WM_KEYUP, (UIntPtr) VK_C, IntPtr.Zero);
        bool fourth = PostMessage(hWnd, WM_KEYUP, (UIntPtr) VK_CONTROL, IntPtr.Zero);
        return first && second && third && fourth;
    }

    private static INPUT KeyboardInput(ushort virtualKey, uint flags)
    {
        return new INPUT
        {
            type = INPUT_KEYBOARD,
            ki = new KEYBDINPUT
            {
                wVk = virtualKey,
                wScan = 0,
                dwFlags = flags,
                time = 0,
                dwExtraInfo = UIntPtr.Zero
            }
        };
    }
}
'@
}

function Convert-SnapshotToManifestObject
{
    param(
        [Parameter(Mandatory = $true)]
        $Snapshot
    )

    return [ordered]@{
        handle            = $Snapshot.HandleHex
        owning_process_id = $Snapshot.OwningProcessId
        state             = $Snapshot.State
        visible           = [bool] $Snapshot.Visible
        maximized         = [bool] $Snapshot.Maximized
        minimized         = [bool] $Snapshot.Minimized
        rect        = [ordered]@{
            left   = $Snapshot.Left
            top    = $Snapshot.Top
            right  = $Snapshot.Right
            bottom = $Snapshot.Bottom
            width  = $Snapshot.Width
            height = $Snapshot.Height
        }
    }
}

function Find-TerminalWindowSnapshot
{
    param(
        [Parameter(Mandatory = $true)]
        [int] $ProcessId
    )

    $snapshots = foreach ($handle in [VnmCodexResumeResizeNativeMethods]::FindVisibleTopLevelWindows($ProcessId)) {
        $snapshot = [VnmCodexResumeResizeNativeMethods]::ReadSnapshot($handle)
        if ($null -ne $snapshot -and $snapshot.Exists -and $snapshot.Visible) {
            $snapshot
        }
    }

    if ($null -eq $snapshots) {
        return $null
    }

    return ($snapshots | Sort-Object -Property @{ Expression = {
        [long] $_.Width * [long] $_.Height
    }; Descending = $true } | Select-Object -First 1)
}

function Read-PinnedTerminalWindowSnapshot
{
    param(
        [Parameter(Mandatory = $true)]
        [int] $ProcessId
    )

    if ($null -eq $script:pinnedWindowHandle -or
        $script:pinnedWindowHandle -eq [IntPtr]::Zero)
    {
        throw "The vnm_terminal top-level HWND has not been pinned."
    }

    $snapshot = [VnmCodexResumeResizeNativeMethods]::ReadSnapshot(
        $script:pinnedWindowHandle)
    if ($null -eq $snapshot -or !$snapshot.Exists) {
        throw "The pinned vnm_terminal HWND $($script:manifest["pinned_window_handle"]) disappeared."
    }
    if ($snapshot.Handle -ne $script:pinnedWindowHandle) {
        throw "The vnm_terminal top-level HWND changed after pinning."
    }
    if ($snapshot.OwningProcessId -ne $ProcessId) {
        throw (
            "The pinned HWND $($snapshot.HandleHex) is now owned by PID {0}, not the launched " +
            "vnm_terminal PID {1}." -f @($snapshot.OwningProcessId, $ProcessId))
    }
    return $snapshot
}

function Wait-ForTerminalWindow
{
    param(
        [Parameter(Mandatory = $true)]
        [Diagnostics.Process] $Process,

        [Parameter(Mandatory = $true)]
        [int] $TimeoutMs
    )

    $deadline = [Diagnostics.Stopwatch]::StartNew()
    while ($deadline.ElapsedMilliseconds -le $TimeoutMs) {
        if ($Process.HasExited) {
            throw "vnm_terminal exited before its top-level window appeared (exit code $($Process.ExitCode))."
        }

        $snapshot = Find-TerminalWindowSnapshot -ProcessId $Process.Id
        if ($null -ne $snapshot) {
            return $snapshot
        }
        Start-Sleep -Milliseconds $PollIntervalMs
    }

    throw "Timed out after $TimeoutMs ms waiting for the vnm_terminal top-level window."
}

function Add-WindowObservation
{
    param(
        [Parameter(Mandatory = $true)]
        [System.Collections.ArrayList] $Collection,

        [Parameter(Mandatory = $true)]
        $Snapshot
    )

    $observation = Convert-SnapshotToManifestObject -Snapshot $Snapshot
    $observation["timestamp_utc"] = Get-UtcTimestamp
    $observation["elapsed_ms"] = Get-RunElapsedMilliseconds
    [void] $Collection.Add($observation)
}

function Wait-ForRequestedWindowState
{
    param(
        [Parameter(Mandatory = $true)]
        [Diagnostics.Process] $Process,

        [Parameter(Mandatory = $true)]
        [string] $RequestedState,

        [Parameter(Mandatory = $true)]
        [int] $TimeoutMs,

        [Parameter(Mandatory = $true)]
        [System.Collections.ArrayList] $Observations
    )

    $deadline = [Diagnostics.Stopwatch]::StartNew()
    $stableSignature = $null
    $stableSamples = 0
    $latestSnapshot = $null

    while ($deadline.ElapsedMilliseconds -le $TimeoutMs) {
        if ($Process.HasExited) {
            throw "vnm_terminal exited during the $RequestedState transition (exit code $($Process.ExitCode))."
        }

        $latestSnapshot = Read-PinnedTerminalWindowSnapshot -ProcessId $Process.Id
        Add-WindowObservation -Collection (,$Observations) -Snapshot $latestSnapshot
        $signature = "{0}|{1}|{2}|{3}|{4}" -f @(
            $latestSnapshot.State,
            $latestSnapshot.Left,
            $latestSnapshot.Top,
            $latestSnapshot.Right,
            $latestSnapshot.Bottom)
        $matches = $latestSnapshot.State -eq $RequestedState
        if ($matches -and $signature -eq $stableSignature) {
            ++$stableSamples
        }
        elseif ($matches) {
            $stableSignature = $signature
            $stableSamples = 1
        }
        else {
            $stableSignature = $null
            $stableSamples = 0
        }

        if ($stableSamples -ge 2) {
            return $latestSnapshot
        }

        Start-Sleep -Milliseconds $PollIntervalMs
    }

    throw "Timed out after $TimeoutMs ms waiting for requested $RequestedState state."
}

function Invoke-WindowTransition
{
    param(
        [Parameter(Mandatory = $true)]
        [Diagnostics.Process] $Process,

        [Parameter(Mandatory = $true)]
        [int] $Cycle,

        [Parameter(Mandatory = $true)]
        [int] $Sequence,

        [Parameter(Mandatory = $true)]
        [ValidateSet("maximized", "windowed")]
        [string] $RequestedState,

        [Parameter(Mandatory = $true)]
        [int] $HoldMs
    )

    $transition = [ordered]@{
        sequence                = $Sequence
        cycle                   = $Cycle
        phase                   = if ($Cycle -eq 0) { "initial-fullscreen" } else { "cycle" }
        requested_state         = $RequestedState
        requested_at_utc        = $null
        requested_elapsed_ms    = $null
        requested_window        = $null
        handle                  = $null
        request_returned        = $false
        observations            = New-Object System.Collections.ArrayList
        observed_state          = $null
        observed_at_utc         = $null
        observed_elapsed_ms     = $null
        observed_rect           = $null
        completed               = $false
        hold_ms                 = $HoldMs
        hold_state_check_interval_ms = $PollIntervalMs
        hold_state_check_count  = 0
        hold_state_violation    = $null
        hold_final_observation  = $null
        hold_final_state        = $null
        hold_final_at_utc       = $null
        hold_final_elapsed_ms   = $null
        hold_completed_at_utc   = $null
    }
    [void] $script:manifest["transitions"].Add($transition)
    Write-RunManifest

    $currentWindow = Read-PinnedTerminalWindowSnapshot -ProcessId $Process.Id
    $transition["handle"] = $currentWindow.HandleHex

    $transition["requested_at_utc"] = Get-UtcTimestamp
    $transition["requested_elapsed_ms"] = Get-RunElapsedMilliseconds
    $requestedWindow = Convert-SnapshotToManifestObject -Snapshot $currentWindow
    $requestedWindow["timestamp_utc"] = $transition["requested_at_utc"]
    $requestedWindow["elapsed_ms"] = $transition["requested_elapsed_ms"]
    $transition["requested_window"] = $requestedWindow
    Write-RunManifest

    $transition["request_returned"] = if ($RequestedState -eq "maximized") {
        [VnmCodexResumeResizeNativeMethods]::ShowMaximized($currentWindow.Handle)
    }
    else {
        [VnmCodexResumeResizeNativeMethods]::ShowRestored($currentWindow.Handle)
    }
    Write-RunManifest

    $observed = Wait-ForRequestedWindowState `
        -Process $Process `
        -RequestedState $RequestedState `
        -TimeoutMs $TransitionTimeoutMs `
        -Observations (,$transition["observations"])
    $transition["observed_state"] = $observed.State
    $transition["observed_at_utc"] = Get-UtcTimestamp
    $transition["observed_elapsed_ms"] = Get-RunElapsedMilliseconds
    $transition["observed_rect"] = Convert-SnapshotToManifestObject -Snapshot $observed
    $transition["completed"] = $true
    Write-RunManifest

    $holdStopwatch = [Diagnostics.Stopwatch]::StartNew()
    $holdFinal = $null
    do {
        $holdSample = Read-PinnedTerminalWindowSnapshot -ProcessId $Process.Id
        ++$transition["hold_state_check_count"]
        $holdSampleAtUtc = Get-UtcTimestamp
        $holdSampleElapsedMs = Get-RunElapsedMilliseconds
        if ($holdSample.State -ne $RequestedState) {
            $holdSampleObservation = Convert-SnapshotToManifestObject -Snapshot $holdSample
            $holdSampleObservation["timestamp_utc"] = $holdSampleAtUtc
            $holdSampleObservation["elapsed_ms"] = $holdSampleElapsedMs
            $transition["hold_state_violation"] = [ordered]@{
                state         = $holdSample.State
                at_utc        = $holdSampleAtUtc
                elapsed_ms    = $holdSampleElapsedMs
                observation   = $holdSampleObservation
            }
            Write-RunManifest
            throw (
                "The window left the requested $RequestedState state during the {0} ms hold " +
                "at state check {1}; the observed state was {2}." -f @(
                    $HoldMs,
                    $transition["hold_state_check_count"],
                    $holdSample.State))
        }
        $holdFinal = $holdSample

        $remainingHoldMs = $HoldMs - [int] $holdStopwatch.ElapsedMilliseconds
        if ($remainingHoldMs -le 0) {
            break
        }
        $sleepMs = [Math]::Min($PollIntervalMs, $remainingHoldMs)
        if ($sleepMs -gt 0) {
            Start-Sleep -Milliseconds $sleepMs
        }
    } while ($true)
    $holdStopwatch.Stop()

    $transition["hold_final_at_utc"] = Get-UtcTimestamp
    $transition["hold_final_elapsed_ms"] = Get-RunElapsedMilliseconds
    $transition["hold_final_state"] = $holdFinal.State
    $holdFinalObservation = Convert-SnapshotToManifestObject -Snapshot $holdFinal
    $holdFinalObservation["timestamp_utc"] = $transition["hold_final_at_utc"]
    $holdFinalObservation["elapsed_ms"] = $transition["hold_final_elapsed_ms"]
    $transition["hold_final_observation"] = $holdFinalObservation
    if ($holdFinal.State -ne $RequestedState) {
        Write-RunManifest
        throw (
            "The window left the requested $RequestedState state during the {0} ms hold " +
            "at the final state check; the final observed state was {1}." -f @(
                $HoldMs,
                $holdFinal.State))
    }
    $transition["hold_completed_at_utc"] = Get-UtcTimestamp
    Write-RunManifest
}

function Wait-ForRequestedWindowGeometry
{
    param(
        [Parameter(Mandatory = $true)]
        [Diagnostics.Process] $Process,

        [Parameter(Mandatory = $true)]
        [int] $RequestedWidth,

        [Parameter(Mandatory = $true)]
        [int] $RequestedHeight,

        [Parameter(Mandatory = $true)]
        [int] $TimeoutMs,

        [Parameter(Mandatory = $true)]
        [System.Collections.ArrayList] $Observations
    )

    $deadline = [Diagnostics.Stopwatch]::StartNew()
    $stableSignature = $null
    $stableSamples = 0
    $latestSnapshot = $null

    while ($deadline.ElapsedMilliseconds -le $TimeoutMs) {
        if ($Process.HasExited) {
            throw (
                "vnm_terminal exited during the {0}x{1} window resize " +
                "(exit code {2})." -f @(
                    $RequestedWidth,
                    $RequestedHeight,
                    $Process.ExitCode))
        }

        $latestSnapshot = Read-PinnedTerminalWindowSnapshot -ProcessId $Process.Id
        Add-WindowObservation -Collection (,$Observations) -Snapshot $latestSnapshot
        $signature = "{0}|{1}|{2}" -f @(
            $latestSnapshot.State,
            $latestSnapshot.Width,
            $latestSnapshot.Height)
        $matches =
            $latestSnapshot.State -eq "windowed" -and
            $latestSnapshot.Width -eq $RequestedWidth -and
            $latestSnapshot.Height -eq $RequestedHeight
        if ($matches -and $signature -eq $stableSignature) {
            ++$stableSamples
        }
        elseif ($matches) {
            $stableSignature = $signature
            $stableSamples = 1
        }
        else {
            $stableSignature = $null
            $stableSamples = 0
        }

        if ($stableSamples -ge 2) {
            return $latestSnapshot
        }

        Start-Sleep -Milliseconds $PollIntervalMs
    }

    throw (
        "Timed out after {0} ms waiting for windowed geometry {1}x{2}." -f @(
            $TimeoutMs,
            $RequestedWidth,
            $RequestedHeight))
}

function Invoke-WindowGeometryTransition
{
    param(
        [Parameter(Mandatory = $true)]
        [Diagnostics.Process] $Process,

        [Parameter(Mandatory = $true)]
        [int] $Cycle,

        [Parameter(Mandatory = $true)]
        [int] $Sequence,

        [Parameter(Mandatory = $true)]
        [int] $RequestedWidth,

        [Parameter(Mandatory = $true)]
        [int] $RequestedHeight,

        [Parameter(Mandatory = $true)]
        [int] $HoldMs,

        [Parameter(Mandatory = $true)]
        [string] $Phase
    )

    $transition = [ordered]@{
        sequence                = $Sequence
        cycle                   = $Cycle
        phase                   = $Phase
        requested_state         = "windowed"
        requested_width         = $RequestedWidth
        requested_height        = $RequestedHeight
        requested_at_utc        = $null
        requested_elapsed_ms    = $null
        requested_window        = $null
        handle                  = $null
        request_returned        = $false
        observations            = New-Object System.Collections.ArrayList
        observed_state          = $null
        observed_at_utc         = $null
        observed_elapsed_ms     = $null
        observed_rect           = $null
        completed               = $false
        hold_ms                 = $HoldMs
        hold_state_check_interval_ms = $PollIntervalMs
        hold_state_check_count  = 0
        hold_state_violation    = $null
        hold_final_observation  = $null
        hold_final_state        = $null
        hold_final_at_utc       = $null
        hold_final_elapsed_ms   = $null
        hold_completed_at_utc   = $null
    }
    [void] $script:manifest["transitions"].Add($transition)
    Write-RunManifest

    $currentWindow = Read-PinnedTerminalWindowSnapshot -ProcessId $Process.Id
    if ($currentWindow.State -ne "windowed") {
        throw (
            "The window must be windowed before the {0}x{1} resize; observed " +
            "state {2}." -f @(
                $RequestedWidth,
                $RequestedHeight,
                $currentWindow.State))
    }
    $transition["handle"] = $currentWindow.HandleHex

    $transition["requested_at_utc"] = Get-UtcTimestamp
    $transition["requested_elapsed_ms"] = Get-RunElapsedMilliseconds
    $requestedWindow = Convert-SnapshotToManifestObject -Snapshot $currentWindow
    $requestedWindow["timestamp_utc"] = $transition["requested_at_utc"]
    $requestedWindow["elapsed_ms"] = $transition["requested_elapsed_ms"]
    $transition["requested_window"] = $requestedWindow
    Write-RunManifest

    $transition["request_returned"] =
        [VnmCodexResumeResizeNativeMethods]::SetWindowSize(
            $currentWindow.Handle,
            $RequestedWidth,
            $RequestedHeight)
    Write-RunManifest
    if (!$transition["request_returned"]) {
        throw (
            "SetWindowPos failed for the requested windowed geometry {0}x{1}." -f @(
                $RequestedWidth,
                $RequestedHeight))
    }

    $observed = Wait-ForRequestedWindowGeometry `
        -Process $Process `
        -RequestedWidth $RequestedWidth `
        -RequestedHeight $RequestedHeight `
        -TimeoutMs $TransitionTimeoutMs `
        -Observations (,$transition["observations"])
    $transition["observed_state"] = $observed.State
    $transition["observed_at_utc"] = Get-UtcTimestamp
    $transition["observed_elapsed_ms"] = Get-RunElapsedMilliseconds
    $transition["observed_rect"] = Convert-SnapshotToManifestObject -Snapshot $observed
    $transition["completed"] = $true
    Write-RunManifest

    $holdStopwatch = [Diagnostics.Stopwatch]::StartNew()
    $holdFinal = $null
    do {
        $holdSample = Read-PinnedTerminalWindowSnapshot -ProcessId $Process.Id
        ++$transition["hold_state_check_count"]
        $holdSampleAtUtc = Get-UtcTimestamp
        $holdSampleElapsedMs = Get-RunElapsedMilliseconds
        $holdMatches =
            $holdSample.State -eq "windowed" -and
            $holdSample.Width -eq $RequestedWidth -and
            $holdSample.Height -eq $RequestedHeight
        if (!$holdMatches) {
            $holdSampleObservation = Convert-SnapshotToManifestObject -Snapshot $holdSample
            $holdSampleObservation["timestamp_utc"] = $holdSampleAtUtc
            $holdSampleObservation["elapsed_ms"] = $holdSampleElapsedMs
            $transition["hold_state_violation"] = [ordered]@{
                requested_width  = $RequestedWidth
                requested_height = $RequestedHeight
                at_utc           = $holdSampleAtUtc
                elapsed_ms       = $holdSampleElapsedMs
                observation      = $holdSampleObservation
            }
            Write-RunManifest
            throw (
                "The window left the requested windowed geometry {0}x{1} during " +
                "the {2} ms hold at state check {3}; observed {4}x{5} in state " +
                "{6}." -f @(
                    $RequestedWidth,
                    $RequestedHeight,
                    $HoldMs,
                    $transition["hold_state_check_count"],
                    $holdSample.Width,
                    $holdSample.Height,
                    $holdSample.State))
        }
        $holdFinal = $holdSample

        $remainingHoldMs = $HoldMs - [int] $holdStopwatch.ElapsedMilliseconds
        if ($remainingHoldMs -le 0) {
            break
        }
        $sleepMs = [Math]::Min($PollIntervalMs, $remainingHoldMs)
        if ($sleepMs -gt 0) {
            Start-Sleep -Milliseconds $sleepMs
        }
    } while ($true)
    $holdStopwatch.Stop()

    $transition["hold_final_at_utc"] = Get-UtcTimestamp
    $transition["hold_final_elapsed_ms"] = Get-RunElapsedMilliseconds
    $transition["hold_final_state"] = $holdFinal.State
    $holdFinalObservation = Convert-SnapshotToManifestObject -Snapshot $holdFinal
    $holdFinalObservation["timestamp_utc"] = $transition["hold_final_at_utc"]
    $holdFinalObservation["elapsed_ms"] = $transition["hold_final_elapsed_ms"]
    $transition["hold_final_observation"] = $holdFinalObservation
    $transition["hold_completed_at_utc"] = Get-UtcTimestamp
    Write-RunManifest
}

function Invoke-NarrowTaskkillForProcess
{
    param(
        [Parameter(Mandatory = $true)]
        [Diagnostics.Process] $Process,

        [Parameter(Mandatory = $true)]
        [string] $Purpose
    )

    $taskkillPath = Join-Path $env:SystemRoot "System32\taskkill.exe"
    $arguments = @(
        "/PID", $Process.Id.ToString([Globalization.CultureInfo]::InvariantCulture),
        "/F"
    )
    $result = [ordered]@{
        purpose          = $Purpose
        target_pid       = $Process.Id
        scope            = "single-pid"
        arguments        = $arguments
        command_pid      = $null
        command_exited   = $false
        exit_code        = $null
        timed_out        = $false
        terminated        = $false
        exited_after_kill = $null
        target_exited    = $false
        error            = $null
    }
    $taskkillProcess = $null
    try {
        if (!(Test-Path -LiteralPath $taskkillPath -PathType Leaf)) {
            $result["error"] = "taskkill.exe is unavailable: $taskkillPath"
            return $result
        }

        $taskkillProcess = Start-Process `
            -FilePath $taskkillPath `
            -ArgumentList (Join-WindowsCommandLine -Arguments $arguments) `
            -WindowStyle Hidden `
            -PassThru
        $result["command_pid"] = $taskkillProcess.Id
        if (!$taskkillProcess.WaitForExit(10000)) {
            $result["timed_out"] = $true
            try {
                $taskkillProcess.Kill()
                $result["terminated"] = $true
                $result["exited_after_kill"] = [bool] $taskkillProcess.WaitForExit(5000)
            }
            catch {
                $result["error"] = "Scoped taskkill termination failed: $($_.Exception.Message)"
            }
            if (!$result["exited_after_kill"]) {
                if ([string]::IsNullOrWhiteSpace($result["error"])) {
                    $result["error"] = "Scoped taskkill command remained running after its timeout."
                }
                return $result
            }
            $result["command_exited"] = $true
            $result["error"] = "Scoped taskkill command timed out."
            return $result
        }

        $result["command_exited"] = $true
        $result["exit_code"] = $taskkillProcess.ExitCode
        try {
            $Process.Refresh()
            $result["target_exited"] = [bool] $Process.HasExited
        }
        catch {
            $result["error"] = "Could not verify target PID $($Process.Id) after scoped taskkill: $($_.Exception.Message)"
        }
        if ($result["exit_code"] -ne 0 -and
            [string]::IsNullOrWhiteSpace($result["error"]))
        {
            $result["error"] = "Scoped taskkill returned exit code $($result["exit_code"])."
        }
        elseif (!$result["target_exited"] -and
            [string]::IsNullOrWhiteSpace($result["error"]))
        {
            $result["error"] = "Scoped taskkill returned 0, but target PID $($Process.Id) remains running."
        }
        return $result
    }
    catch {
        $result["error"] = $_.Exception.Message
        return $result
    }
    finally {
        if ($null -ne $taskkillProcess) {
            $taskkillProcess.Dispose()
        }
    }
}

function Invoke-ElevatedWpr
{
    param(
        [Parameter(Mandatory = $true)]
        [string] $WprPath,

        [Parameter(Mandatory = $true)]
        [string[]] $Arguments,

        [Parameter(Mandatory = $true)]
        [ValidateSet("start", "stop", "cancel")]
        [string] $Operation
    )

    $commandRecord = [ordered]@{
        operation       = $Operation
        arguments       = $Arguments
        started_at_utc  = Get-UtcTimestamp
        finished_at_utc = $null
        pid             = $null
        exit_code       = $null
        timed_out       = $false
        terminated      = $false
        exited_after_kill = $null
        timeout_taskkill = $null
        error           = $null
    }
    [void] $script:manifest["kernel_trace_command_records"].Add($commandRecord)
    Write-RunManifest

    $wprProcess = $null
    try {
        $wprProcess = Start-Process `
            -FilePath $WprPath `
            -ArgumentList (Join-WindowsCommandLine -Arguments $Arguments) `
            -WindowStyle Hidden `
            -PassThru
        $commandRecord["pid"] = $wprProcess.Id
        Write-RunManifest

        if (!$wprProcess.WaitForExit($WprCommandTimeoutMs)) {
            $commandRecord["timed_out"] = $true
            $wprExitedAfterKill = $false
            try {
                $wprProcess.Refresh()
                if (!$wprProcess.HasExited) {
                    $wprProcess.Kill()
                    $commandRecord["terminated"] = $true
                }
                $wprExitedAfterKill = [bool] $wprProcess.WaitForExit(5000)
            }
            catch {
                if ([string]::IsNullOrWhiteSpace($commandRecord["error"])) {
                    $commandRecord["error"] = "WPR timeout termination or wait failed: $($_.Exception.Message)"
                }
            }
            $commandRecord["exited_after_kill"] = [bool] $wprExitedAfterKill
            if (!$wprExitedAfterKill) {
                $timeoutTaskkill = Invoke-NarrowTaskkillForProcess `
                    -Process $wprProcess `
                    -Purpose ("wpr-timeout-{0}" -f $Operation)
                $commandRecord["timeout_taskkill"] = $timeoutTaskkill
                if (!$timeoutTaskkill["command_exited"] -or
                    $timeoutTaskkill["exit_code"] -ne 0 -or
                    !$timeoutTaskkill["target_exited"])
                {
                    throw (
                        "WPR {0} command timed out and its PID {1} was not " +
                        "confirmed stopped by the scoped taskkill; target_exited={2}; " +
                        "taskkill_error={3}" -f @(
                            $Operation,
                            $wprProcess.Id,
                            $timeoutTaskkill["target_exited"],
                            $timeoutTaskkill["error"]))
                }
            }
            throw "WPR $Operation command timed out after $WprCommandTimeoutMs ms."
        }

        $commandRecord["exit_code"] = $wprProcess.ExitCode
        return $wprProcess.ExitCode
    }
    catch {
        if ([string]::IsNullOrWhiteSpace($commandRecord["error"])) {
            $commandRecord["error"] = $_.Exception.Message
        }
        throw
    }
    finally {
        $commandRecord["finished_at_utc"] = Get-UtcTimestamp
        try {
            Write-RunManifest
        }
        catch {
            if ([string]::IsNullOrWhiteSpace($commandRecord["error"])) {
                $commandRecord["error"] = $_.Exception.Message
            }
        }
        if ($null -ne $wprProcess) {
            $wprProcess.Dispose()
        }
    }
}

function Resolve-WprPath
{
    $programFilesX86 = ${env:ProgramFiles(x86)}
    if (![string]::IsNullOrWhiteSpace($programFilesX86)) {
        $knownPath = Join-Path $programFilesX86 "Windows Kits\10\Windows Performance Toolkit\wpr.exe"
        if (Test-Path -LiteralPath $knownPath -PathType Leaf) {
            return (Resolve-Path -LiteralPath $knownPath).ProviderPath
        }
    }

    $command = Get-Command "wpr.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $command -and (Test-Path -LiteralPath $command.Source -PathType Leaf)) {
        return (Resolve-Path -LiteralPath $command.Source).ProviderPath
    }

    throw "-KernelTrace requested, but wpr.exe was not found."
}

function Assert-InvokingPowerShellElevated
{
    try {
        $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
        $principal = [Security.Principal.WindowsPrincipal]::new($identity)
        $isElevated = $principal.IsInRole(
            [Security.Principal.WindowsBuiltInRole]::Administrator)
    }
    catch {
        throw (
            "-KernelTrace requires an already elevated PowerShell process, " +
            "but its elevation status could not be verified: {0}" -f @(
                $_.Exception.Message))
    }

    if (!$isElevated) {
        throw (
            "-KernelTrace requires the invoking PowerShell process to already " +
            "be elevated. Start PowerShell as Administrator and rerun; this " +
            "driver does not request UAC elevation.")
    }
}

function Start-KernelTrace
{
    param(
        [Parameter(Mandatory = $true)]
        [string] $WprPath,

        [Parameter(Mandatory = $true)]
        [string] $TraceInstanceName,

        [Parameter(Mandatory = $true)]
        [string] $TraceDirectory
    )

    $arguments = @(
        "-start", "CPU.verbose",
        "-filemode",
        "-recordtempto", $TraceDirectory,
        "-instancename", $TraceInstanceName
    )
    $exitCode = Invoke-ElevatedWpr `
        -WprPath $WprPath `
        -Arguments $arguments `
        -Operation "start"
    if ($exitCode -ne 0) {
        throw "WPR start failed with exit code $exitCode."
    }
}

function Stop-KernelTrace
{
    param(
        [Parameter(Mandatory = $true)]
        [string] $WprPath,

        [Parameter(Mandatory = $true)]
        [string] $TraceInstanceName,

        [Parameter(Mandatory = $true)]
        [string] $EtlPath
    )

    $arguments = @(
        "-stop", $EtlPath,
        "-instancename", $TraceInstanceName
    )
    $exitCode = Invoke-ElevatedWpr `
        -WprPath $WprPath `
        -Arguments $arguments `
        -Operation "stop"
    if ($exitCode -ne 0) {
        throw "WPR stop failed with exit code $exitCode."
    }
}

function Cancel-KernelTrace
{
    param(
        [Parameter(Mandatory = $true)]
        [string] $WprPath,

        [Parameter(Mandatory = $true)]
        [string] $TraceInstanceName
    )

    $arguments = @(
        "-cancel",
        "-instancename", $TraceInstanceName
    )
    $exitCode = Invoke-ElevatedWpr `
        -WprPath $WprPath `
        -Arguments $arguments `
        -Operation "cancel"
    if ($exitCode -ne 0) {
        throw "WPR cancel failed with exit code $exitCode."
    }
}

function Wait-ForProcessExit
{
    param(
        [Parameter(Mandatory = $true)]
        [Diagnostics.Process] $Process,

        [Parameter(Mandatory = $true)]
        [int] $TimeoutMs
    )

    if ($Process.HasExited) {
        return $true
    }
    return $Process.WaitForExit($TimeoutMs)
}

function Get-NormalizedExecutablePath
{
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path
    )

    try {
        return [IO.Path]::GetFullPath($Path)
    }
    catch {
        throw "Could not normalize executable path '$Path': $($_.Exception.Message)"
    }
}

function Find-DirectCodexProcess
{
    param(
        [Parameter(Mandatory = $true)]
        [int] $TerminalProcessId,

        [Parameter(Mandatory = $true)]
        [string] $ExpectedExecutablePath
    )

    $expectedPath = Get-NormalizedExecutablePath -Path $ExpectedExecutablePath
    $expectedName = [IO.Path]::GetFileName($expectedPath)
    $children = @(Get-CimInstance `
        -ClassName Win32_Process `
        -Filter ("ParentProcessId = {0}" -f $TerminalProcessId) `
        -OperationTimeoutSec $codexCimOperationTimeoutSec `
        -ErrorAction Stop)
    $matches = New-Object System.Collections.ArrayList

    foreach ($child in $children) {
        $childName = [string] $child.Name
        $nameMatches = $childName -ieq $expectedName
        $childPath = [string] $child.ExecutablePath
        if ([string]::IsNullOrWhiteSpace($childPath)) {
            if ($nameMatches) {
                throw (
                    "Cannot establish the executable path for direct Codex " +
                    "child PID {0}." -f @($child.ProcessId))
            }
            continue
        }

        $normalizedChildPath = Get-NormalizedExecutablePath -Path $childPath
        $pathMatches = $normalizedChildPath -ieq $expectedPath
        if ($nameMatches -and !$pathMatches) {
            throw (
                "Direct child PID {0} is named '{1}' but its executable path " +
                "does not match the resolved Codex executable: {2}" -f @(
                    $child.ProcessId,
                    $childName,
                    $childPath))
        }
        if ($pathMatches) {
            [void] $matches.Add($child)
        }
    }

    if ($matches.Count -eq 0) {
        return $null
    }
    if ($matches.Count -ne 1) {
        throw (
            "Expected exactly one direct Codex child of vnm_terminal PID {0}; " +
            "found {1}." -f @($TerminalProcessId, $matches.Count))
    }
    return $matches[0]
}

function Get-KnownDirectCodexProcess
{
    param(
        [Parameter(Mandatory = $true)]
        [int] $TerminalProcessId,

        [Parameter(Mandatory = $true)]
        [int] $CodexProcessId,

        [Parameter(Mandatory = $true)]
        [string] $ExpectedExecutablePath
    )

    $records = @(Get-CimInstance `
        -ClassName Win32_Process `
        -Filter ("ProcessId = {0}" -f $CodexProcessId) `
        -OperationTimeoutSec $codexCimOperationTimeoutSec `
        -ErrorAction Stop)
    if ($records.Count -eq 0) {
        return $null
    }
    if ($records.Count -ne 1) {
        throw "Expected exactly one CIM record for Codex PID $CodexProcessId."
    }

    $record = $records[0]
    if ([int] $record.ParentProcessId -ne $TerminalProcessId) {
        throw (
            "Codex PID {0} no longer has the launched vnm_terminal PID {1} as " +
            "its direct parent (actual parent PID {2})." -f @(
                $CodexProcessId,
                $TerminalProcessId,
                $record.ParentProcessId))
    }
    if ([string]::IsNullOrWhiteSpace([string] $record.ExecutablePath)) {
        throw "Cannot establish the executable path for Codex PID $CodexProcessId."
    }

    $expectedPath = Get-NormalizedExecutablePath -Path $ExpectedExecutablePath
    $observedPath = Get-NormalizedExecutablePath -Path ([string] $record.ExecutablePath)
    if ($observedPath -ine $expectedPath) {
        throw (
            "Codex PID {0} executable path changed or was reused: observed {1}; " +
            "expected {2}." -f @(
                $CodexProcessId,
                $record.ExecutablePath,
                $ExpectedExecutablePath))
    }
    return $record
}

function Wait-ForDirectCodexChild
{
    param(
        [Parameter(Mandatory = $true)]
        [Diagnostics.Process] $TerminalProcess,

        [Parameter(Mandatory = $true)]
        [string] $ExpectedExecutablePath,

        [Parameter(Mandatory = $true)]
        [int] $TimeoutMs
    )

    $deadline = [Diagnostics.Stopwatch]::StartNew()
    while ($deadline.ElapsedMilliseconds -le $TimeoutMs) {
        if ($TerminalProcess.HasExited) {
            throw (
                "vnm_terminal exited before its direct Codex child could be " +
                "discovered (exit code $($TerminalProcess.ExitCode)).")
        }

        $child = Find-DirectCodexProcess `
            -TerminalProcessId $TerminalProcess.Id `
            -ExpectedExecutablePath $ExpectedExecutablePath
        if ($null -ne $child) {
            return $child
        }
        Start-Sleep -Milliseconds $PollIntervalMs
    }

    throw (
        "Could not establish a direct Codex child of vnm_terminal PID {0} " +
        "within {1} ms." -f @($TerminalProcess.Id, $TimeoutMs))
}

function Wait-ForDirectCodexChildExit
{
    param(
        [Parameter(Mandatory = $true)]
        [Diagnostics.Process] $TerminalProcess,

        [Parameter(Mandatory = $true)]
        [int] $CodexProcessId,

        [Parameter(Mandatory = $true)]
        [string] $ExpectedExecutablePath,

        [Parameter(Mandatory = $true)]
        [int] $TimeoutMs,

        [Parameter(Mandatory = $true)]
        [string] $Purpose
    )

    $observation = [ordered]@{
        purpose        = $Purpose
        codex_pid      = $CodexProcessId
        started_at_utc = Get-UtcTimestamp
        finished_at_utc = $null
        timeout_ms     = $TimeoutMs
        exited         = $false
        error          = $null
    }
    $childExited = $false
    try {
        $deadline = [Diagnostics.Stopwatch]::StartNew()
        while ($deadline.ElapsedMilliseconds -le $TimeoutMs) {
            if ($TerminalProcess.HasExited) {
                throw (
                    "vnm_terminal exited before disappearance of direct Codex " +
                    "child PID $CodexProcessId could be proven.")
            }
            $child = Get-KnownDirectCodexProcess `
                -TerminalProcessId $TerminalProcess.Id `
                -CodexProcessId $CodexProcessId `
                -ExpectedExecutablePath $ExpectedExecutablePath
            if ($null -eq $child) {
                if ($TerminalProcess.HasExited) {
                    throw (
                        "vnm_terminal exited before disappearance of direct Codex " +
                        "child PID $CodexProcessId could be proven.")
                }
                $childExited = $true
                $manifest["codex_process"]["exited"] = $true
                $manifest["codex_process"]["exit_observed_at_utc"] = Get-UtcTimestamp
                break
            }
            if ($TerminalProcess.HasExited) {
                throw (
                    "vnm_terminal exited while the direct Codex child PID " +
                    "$CodexProcessId was still present.")
            }
            Start-Sleep -Milliseconds $PollIntervalMs
        }
    }
    catch {
        $observation["error"] = $_.Exception.Message
        throw
    }
    finally {
        $observation["finished_at_utc"] = Get-UtcTimestamp
        $observation["exited"] = [bool] $childExited
        [void] $manifest["codex_child_observations"].Add($observation)
        Write-RunManifest
    }
    return $childExited
}

function Invoke-TerminalProcessTreeTermination
{
    param(
        [Parameter(Mandatory = $true)]
        [Diagnostics.Process] $Process
    )

    $taskkillPath = Join-Path $env:SystemRoot "System32\taskkill.exe"
    if (!(Test-Path -LiteralPath $taskkillPath -PathType Leaf)) {
        throw "Cannot terminate the spawned vnm_terminal process tree: taskkill.exe is unavailable."
    }

    $arguments = @(
        "/PID", $Process.Id.ToString([Globalization.CultureInfo]::InvariantCulture),
        "/T",
        "/F"
    )
    for ($attempt = 1; $attempt -le 2; ++$attempt) {
        Add-ExitSequenceAction -Action "taskkill-process-tree-requested" -Details @{
            pid        = $Process.Id
            attempt    = $attempt
            executable = $taskkillPath
            arguments  = $arguments
        }

        $taskkillProcess = $null
        try {
            $taskkillProcess = Start-Process `
                -FilePath $taskkillPath `
                -ArgumentList (Join-WindowsCommandLine -Arguments $arguments) `
                -WindowStyle Hidden `
                -PassThru
            if (!$taskkillProcess.WaitForExit(10000)) {
                $taskkillExitedAfterKill = $false
                $taskkillKillError = $null
                try {
                    $taskkillProcess.Kill()
                    $taskkillExitedAfterKill = [bool] $taskkillProcess.WaitForExit(5000)
                }
                catch {
                    $taskkillKillError = $_.Exception.Message
                }
                $timeoutPidGone = Test-SpawnedTerminalProcessGone -Process $Process
                Add-ExitSequenceAction -Action "taskkill-command-timeout" -Details @{
                    pid               = $Process.Id
                    attempt           = $attempt
                    command_pid       = $taskkillProcess.Id
                    exited_after_kill = $taskkillExitedAfterKill
                    pid_gone          = [bool] $timeoutPidGone
                    error             = $taskkillKillError
                }
                if (!$taskkillExitedAfterKill) {
                    if ([string]::IsNullOrWhiteSpace($taskkillKillError)) {
                        throw "taskkill.exe command PID $($taskkillProcess.Id) remained running after its timeout termination."
                    }
                    throw "taskkill.exe timeout termination could not be verified: $taskkillKillError"
                }
                if ($timeoutPidGone) {
                    Add-ExitSequenceAction -Action "taskkill-process-tree-completed" -Details @{
                        pid            = $Process.Id
                        attempt        = $attempt
                        exit_code      = $null
                        pid_gone       = $true
                        success_reason = "target_pid_gone_after_helper_termination"
                    }
                    $script:manifest["forced_tree_termination"] = $true
                    return
                }
                if ($attempt -eq 2) {
                    throw "taskkill.exe timed out on both bounded attempts while terminating PID $($Process.Id); pid_gone=$timeoutPidGone."
                }
                continue
            }

            $taskkillExitCode = $taskkillProcess.ExitCode
            $pidGone = Test-SpawnedTerminalProcessGone -Process $Process
            Add-ExitSequenceAction -Action "taskkill-process-tree-completed" -Details @{
                pid        = $Process.Id
                attempt    = $attempt
                exit_code  = $taskkillExitCode
                pid_gone   = [bool] $pidGone
            }
            if ($taskkillExitCode -ne 0) {
                throw "taskkill.exe returned exit code $taskkillExitCode for PID $($Process.Id)."
            }
            if (!$pidGone) {
                throw "taskkill.exe returned 0, but the spawned vnm_terminal PID $($Process.Id) remains present."
            }
            $script:manifest["forced_tree_termination"] = $true
            return
        }
        catch {
            Add-ExitSequenceAction -Action "taskkill-process-tree-failed" -Details @{
                pid     = $Process.Id
                attempt = $attempt
                error   = $_.Exception.Message
            }
            throw
        }
        finally {
            if ($null -ne $taskkillProcess) {
                $taskkillProcess.Dispose()
            }
        }
    }

    throw "The bounded taskkill retry loop did not terminate the spawned vnm_terminal process tree."
}

function Test-SpawnedTerminalProcessGone
{
    param(
        [Parameter(Mandatory = $true)]
        [Diagnostics.Process] $Process
    )

    try {
        $Process.Refresh()
        if (!$Process.HasExited) {
            return $false
        }
        return $null -eq (Get-Process -Id $Process.Id -ErrorAction SilentlyContinue)
    }
    catch {
        return $false
    }
}

function Add-ExitSequenceAction
{
    param(
        [Parameter(Mandatory = $true)]
        [string] $Action,

        [Parameter(Mandatory = $true)]
        [hashtable] $Details
    )

    $record = [ordered]@{
        action       = $Action
        timestamp_utc = Get-UtcTimestamp
        elapsed_ms   = Get-RunElapsedMilliseconds
        details      = $Details
    }
    [void] $script:manifest["exit_sequence"].Add($record)
    Write-RunManifest
}

function Send-AutomatedCtrlC
{
    param(
        [Parameter(Mandatory = $true)]
        [Diagnostics.Process] $Process,

        [Parameter(Mandatory = $true)]
        [int] $CodexProcessId,

        [Parameter(Mandatory = $true)]
        [string] $ExpectedExecutablePath
    )

    $codexExited = $false
    for ($attempt = 1; $attempt -le $CtrlCAttempts; ++$attempt) {
        if ($Process.HasExited) {
            throw "vnm_terminal exited before scripted Ctrl+C attempt $attempt."
        }

        $codexBeforeAttempt = Get-KnownDirectCodexProcess `
            -TerminalProcessId $Process.Id `
            -CodexProcessId $CodexProcessId `
            -ExpectedExecutablePath $ExpectedExecutablePath
        if ($null -eq $codexBeforeAttempt) {
            if ($attempt -eq 1) {
                throw "The direct Codex child exited before the scripted Ctrl+C sequence."
            }
            $codexExited = $true
            $manifest["codex_process"]["exited"] = $true
            $manifest["codex_process"]["exit_observed_at_utc"] = Get-UtcTimestamp
            Write-RunManifest
            break
        }

        $snapshot = Read-PinnedTerminalWindowSnapshot -ProcessId $Process.Id
        $activationReturned = [VnmCodexResumeResizeNativeMethods]::Activate($snapshot.Handle)
        $foregroundDeadline = [Diagnostics.Stopwatch]::StartNew()
        $foregroundReady = $false
        while ($foregroundDeadline.ElapsedMilliseconds -le $TransitionTimeoutMs) {
            if ([VnmCodexResumeResizeNativeMethods]::ForegroundWindow() -eq $snapshot.Handle) {
                $foregroundReady = $true
                break
            }
            Start-Sleep -Milliseconds $PollIntervalMs
        }

        $method = "post-message-fallback"
        $sendReturned = $false
        if ($foregroundReady) {
            $method = "send-input"
            $sendReturned = [VnmCodexResumeResizeNativeMethods]::SendCtrlC()
        }
        if (!$sendReturned) {
            $sendReturned = [VnmCodexResumeResizeNativeMethods]::PostCtrlC($snapshot.Handle)
        }

        Add-ExitSequenceAction -Action "ctrl-c" -Details @{
            attempt             = $attempt
            handle               = $snapshot.HandleHex
            activation_returned  = [bool] $activationReturned
            foreground_ready     = [bool] $foregroundReady
            method               = $method
            send_returned        = [bool] $sendReturned
        }
        if (!$sendReturned) {
            throw "The automated Ctrl+C input could not be sent on attempt $attempt."
        }

        $codexExited = Wait-ForDirectCodexChildExit `
            -TerminalProcess $Process `
            -CodexProcessId $CodexProcessId `
            -ExpectedExecutablePath $ExpectedExecutablePath `
            -TimeoutMs $CtrlCRepeatDelayMs `
            -Purpose ("after-ctrl-c-{0}" -f $attempt)
        if ($codexExited) {
            break
        }
    }

    if (!$codexExited) {
        $codexExited = Wait-ForDirectCodexChildExit `
            -TerminalProcess $Process `
            -CodexProcessId $CodexProcessId `
            -ExpectedExecutablePath $ExpectedExecutablePath `
            -TimeoutMs $ExitTimeoutMs `
            -Purpose "after-ctrl-c-attempts"
    }
    if (!$codexExited) {
        throw (
            "The direct Codex child PID {0} remained alive after the scripted " +
            "Ctrl+C attempts and the {1} ms exit deadline; the keep-open " +
            "terminal will not be closed." -f @($CodexProcessId, $ExitTimeoutMs))
    }

    $closeReturned = $Process.CloseMainWindow()
    Add-ExitSequenceAction -Action "close-main-window-after-interrupt" -Details @{
        close_returned = [bool] $closeReturned
    }
    if (!(Wait-ForProcessExit -Process $Process -TimeoutMs $ExitTimeoutMs)) {
        throw "vnm_terminal did not exit after the automated Ctrl+C sequence and graceful window close. PID $($Process.Id) remains running."
    }
}

function Start-TerminalProcess
{
    param(
        [Parameter(Mandatory = $true)]
        [string] $Executable,

        [Parameter(Mandatory = $true)]
        [string[]] $Arguments,

        [Parameter(Mandatory = $true)]
        [string] $ProcessWorkingDirectory,

        [Parameter(Mandatory = $true)]
        [string] $CodexAccountHome
    )

    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $Executable
    $startInfo.WorkingDirectory = $ProcessWorkingDirectory
    $startInfo.UseShellExecute = $false
    # Keep the GUI terminal out of the driver's console process group.  The
    # baseline terminal exits its inherited console when the interrupted child
    # terminates, which can otherwise deliver a console-close event to this
    # driver before it flushes the run artifacts.
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.EnvironmentVariables["CODEX_HOME"] = $CodexAccountHome
    $startInfo.EnvironmentVariables["VNM_TERMINAL_SETTINGS_NO_PERSIST"] = "1"

    $argumentListProperty = $startInfo.GetType().GetProperty("ArgumentList")
    if ($null -ne $argumentListProperty) {
        foreach ($argument in $Arguments) {
            [void] $startInfo.ArgumentList.Add($argument)
        }
    }
    else {
        $startInfo.Arguments = Join-WindowsCommandLine -Arguments $Arguments
    }

    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    try {
        if (!$process.Start()) {
            throw "Could not start vnm_terminal.exe: $Executable"
        }
    }
    catch {
        $process.Dispose()
        throw
    }
    return $process
}

function Complete-RedirectedOutput
{
    param(
        [Parameter(Mandatory = $true)]
        $StandardOutputTask,

        [Parameter(Mandatory = $true)]
        $StandardErrorTask
    )

    $standardOutput = $StandardOutputTask.GetAwaiter().GetResult()
    $standardError = $StandardErrorTask.GetAwaiter().GetResult()
    [IO.File]::WriteAllText($stdoutPath, $standardOutput, [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText($stderrPath, $standardError, [Text.UTF8Encoding]::new($false))
}

function Test-JsonNumber
{
    param(
        [Parameter(Mandatory = $false)]
        $Value
    )

    if ($null -eq $Value -or
        $Value -is [bool] -or
        $Value -is [string])
    {
        return $false
    }
    if ($Value -is [byte] -or
        $Value -is [sbyte] -or
        $Value -is [int16] -or
        $Value -is [uint16] -or
        $Value -is [int32] -or
        $Value -is [uint32] -or
        $Value -is [int64] -or
        $Value -is [uint64] -or
        $Value -is [decimal])
    {
        return $true
    }
    if ($Value -is [single]) {
        return ![single]::IsNaN($Value) -and
            ![single]::IsInfinity($Value)
    }
    if ($Value -is [double]) {
        return ![double]::IsNaN($Value) -and
            ![double]::IsInfinity($Value)
    }
    return $false
}

function Test-IntegralJsonNumber
{
    param(
        [Parameter(Mandatory = $false)]
        $Value
    )

    if (!(Test-JsonNumber -Value $Value)) {
        return $false
    }
    if ($Value -is [byte] -or
        $Value -is [sbyte] -or
        $Value -is [int16] -or
        $Value -is [uint16] -or
        $Value -is [int32] -or
        $Value -is [uint32] -or
        $Value -is [int64] -or
        $Value -is [uint64])
    {
        return $true
    }
    if ($Value -is [decimal]) {
        return [decimal]::Truncate($Value) -eq $Value
    }
    return [double] $Value -eq [Math]::Truncate([double] $Value)
}

function Assert-OutputArtifacts
{
    param(
        [Parameter(Mandatory = $true)]
        [int] $ExitCode
    )

    $requiredArtifacts = @(
        @{ Name = "profile text"; Path = $profilePath; RequireContent = $true },
        @{ Name = "metrics JSON"; Path = $metricsPath; RequireContent = $true },
        @{ Name = "metrics timeline JSONL"; Path = $metricsTimelinePath; RequireContent = $true },
        @{ Name = "stdout log"; Path = $stdoutPath; RequireContent = $false },
        @{ Name = "stderr log"; Path = $stderrPath; RequireContent = $false }
    )
    if ($KernelTrace) {
        $requiredArtifacts += @{ Name = "kernel ETL"; Path = $etlPath; RequireContent = $true }
    }

    foreach ($artifact in $requiredArtifacts) {
        if (!(Test-Path -LiteralPath $artifact.Path -PathType Leaf)) {
            throw "Required $($artifact.Name) artifact is missing: $($artifact.Path)"
        }
        if ($artifact.RequireContent -and (Get-Item -LiteralPath $artifact.Path).Length -eq 0) {
            throw "Required $($artifact.Name) artifact is empty: $($artifact.Path)"
        }
    }
    if ($ExitCode -ne 0) {
        throw "vnm_terminal exited with status $ExitCode."
    }

    $metrics = Get-Content -Raw -LiteralPath $metricsPath | ConvertFrom-Json
    if ($null -eq $metrics -or
        $metrics -is [Array] -or
        $metrics -isnot [PSCustomObject])
    {
        throw "Final metrics must be a JSON object: $metricsPath"
    }

    $schemaProperty = $metrics.PSObject.Properties["schema"]
    if ($null -eq $schemaProperty -or
        $schemaProperty.Value -isnot [string] -or
        $schemaProperty.Value -ne "vnm_terminal_runtime_metrics_v3")
    {
        throw "Final metrics must use vnm_terminal_runtime_metrics_v3: $metricsPath"
    }

    $profilingProperty = $metrics.PSObject.Properties["profiling"]
    if ($null -eq $profilingProperty -or
        $profilingProperty.Value -is [Array] -or
        $profilingProperty.Value -isnot [PSCustomObject])
    {
        throw "Final metrics are missing the profiling object: $metricsPath"
    }
    $profiling = $profilingProperty.Value

    $requiredBooleanProperties = @(
        @{ Object = $profiling; Name = "compiled"; Expected = $true },
        @{ Object = $profiling; Name = "profile_text_requested"; Expected = $true },
        @{ Object = $metrics; Name = "output_seen"; Expected = $true },
        @{ Object = $metrics; Name = "process_exited"; Expected = $true },
        @{ Object = $metrics; Name = "timeout_expired"; Expected = $false }
    )
    foreach ($requiredBoolean in $requiredBooleanProperties) {
        $property = $requiredBoolean.Object.PSObject.Properties[$requiredBoolean.Name]
        if ($null -eq $property -or
            $property.Value -isnot [bool] -or
            $property.Value -ne $requiredBoolean.Expected)
        {
            throw (
                "Final metrics property '{0}' must be boolean {1}: {2}" -f @(
                    $requiredBoolean.Name,
                    $requiredBoolean.Expected,
                    $metricsPath))
        }
    }

    $backendErrorProperty = $metrics.PSObject.Properties["backend_error_count"]
    if ($null -eq $backendErrorProperty) {
        throw "Final metrics are missing backend_error_count: $metricsPath"
    }
    if (!(Test-IntegralJsonNumber -Value $backendErrorProperty.Value)) {
        throw "Final metrics backend_error_count must be an integral JSON number: $metricsPath"
    }
    if ($backendErrorProperty.Value -ne 0) {
        throw "Final metrics backend_error_count must be 0: $metricsPath"
    }

    $exitReasonProperty = $metrics.PSObject.Properties["process_exit_reason"]
    $acceptableExitReasons = @("INTERRUPTED")
    if ($null -eq $exitReasonProperty -or
        $exitReasonProperty.Value -isnot [string] -or
        $acceptableExitReasons -notcontains $exitReasonProperty.Value)
    {
        throw (
            "Final metrics process_exit_reason must be one of {0}: {1}" -f @(
                ($acceptableExitReasons -join ", "),
                $metricsPath))
    }

    $exitCodeProperty = $metrics.PSObject.Properties["process_exit_code"]
    if ($null -eq $exitCodeProperty) {
        throw "Final metrics are missing process_exit_code: $metricsPath"
    }
    if (!(Test-IntegralJsonNumber -Value $exitCodeProperty.Value)) {
        throw "Final metrics process_exit_code must be an integral JSON number: $metricsPath"
    }
    if ($exitCodeProperty.Value -ne 130) {
        throw "Final metrics process_exit_code must be 130 for a scripted Ctrl+C interruption: $metricsPath"
    }

    $ctrlCActionCount = @(
        $script:manifest["exit_sequence"] |
            Where-Object { $_.action -eq "ctrl-c" }
    ).Count
    if ($ctrlCActionCount -eq 0) {
        throw "The final metrics claim interruption, but the manifest contains no scripted Ctrl+C action."
    }

    $elapsedProperty = $metrics.PSObject.Properties["elapsed_ms"]
    if ($null -eq $elapsedProperty) {
        throw "Final metrics are missing elapsed_ms: $metricsPath"
    }
    if (!(Test-JsonNumber -Value $elapsedProperty.Value)) {
        throw "Final metrics elapsed_ms must be a JSON number: $metricsPath"
    }
    if ($elapsedProperty.Value -lt 0) {
        throw "Final metrics elapsed_ms must be nonnegative: $metricsPath"
    }

    $timelineLines = @(Get-Content -LiteralPath $metricsTimelinePath)
    if ($timelineLines.Count -eq 0) {
        throw "Metrics timeline contains no samples: $metricsTimelinePath"
    }
    $timelineObjects = New-Object System.Collections.ArrayList
    foreach ($line in $timelineLines) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            throw "Metrics timeline contains an empty line: $metricsTimelinePath"
        }
        $timelineObject = $line | ConvertFrom-Json
        if ($null -eq $timelineObject -or
            $timelineObject -is [Array] -or
            $timelineObject -isnot [PSCustomObject])
        {
            throw "Metrics timeline lines must be JSON objects: $metricsTimelinePath"
        }

        $timelineSchemaProperty = $timelineObject.PSObject.Properties["schema"]
        if ($null -eq $timelineSchemaProperty -or
            $timelineSchemaProperty.Value -isnot [string] -or
            $timelineSchemaProperty.Value -ne "vnm_terminal_metrics_timeline_sample_v1")
        {
            throw "Metrics timeline lines must use vnm_terminal_metrics_timeline_sample_v1: $metricsTimelinePath"
        }

        $kindProperty = $timelineObject.PSObject.Properties["kind"]
        if ($null -eq $kindProperty -or
            $kindProperty.Value -isnot [string] -or
            @("periodic", "final") -notcontains $kindProperty.Value)
        {
            throw "Metrics timeline line kind must be periodic or final: $metricsTimelinePath"
        }

        $runtimeMetricsProperty = $timelineObject.PSObject.Properties["runtime_metrics"]
        if ($null -eq $runtimeMetricsProperty -or
            $runtimeMetricsProperty.Value -is [Array] -or
            $runtimeMetricsProperty.Value -isnot [PSCustomObject])
        {
            throw "Metrics timeline lines must contain a runtime_metrics object: $metricsTimelinePath"
        }
        $runtimeMetrics = $runtimeMetricsProperty.Value
        $runtimeSchemaProperty = $runtimeMetrics.PSObject.Properties["schema"]
        if ($null -eq $runtimeSchemaProperty -or
            $runtimeSchemaProperty.Value -isnot [string] -or
            $runtimeSchemaProperty.Value -ne "vnm_terminal_runtime_metrics_v3")
        {
            throw "Metrics timeline runtime_metrics must use vnm_terminal_runtime_metrics_v3: $metricsTimelinePath"
        }
        [void] $timelineObjects.Add($timelineObject)
    }
    if ($timelineObjects.Count -eq 0 -or $null -eq $timelineObjects[$timelineObjects.Count - 1]) {
        throw "Metrics timeline has no valid final JSON object: $metricsTimelinePath"
    }

    $finalTimelineObject = $timelineObjects[$timelineObjects.Count - 1]
    $finalKindProperty = $finalTimelineObject.PSObject.Properties["kind"]
    if ($null -eq $finalKindProperty -or
        $finalKindProperty.Value -isnot [string] -or
        $finalKindProperty.Value -ne "final")
    {
        throw "The final metrics timeline line must have kind final: $metricsTimelinePath"
    }

    $finalRuntimeMetrics = $finalTimelineObject.PSObject.Properties["runtime_metrics"].Value
    $finalProfilingProperty = $finalRuntimeMetrics.PSObject.Properties["profiling"]
    if ($null -eq $finalProfilingProperty -or
        $finalProfilingProperty.Value -is [Array] -or
        $finalProfilingProperty.Value -isnot [PSCustomObject])
    {
        throw "The final metrics timeline runtime_metrics must contain a profiling object: $metricsTimelinePath"
    }
    $finalProfiling = $finalProfilingProperty.Value
    foreach ($profilingName in @("compiled", "profile_text_requested")) {
        $profilingProperty = $finalProfiling.PSObject.Properties[$profilingName]
        if ($null -eq $profilingProperty -or
            $profilingProperty.Value -isnot [bool] -or
            $profilingProperty.Value -ne $true)
        {
            throw (
                "The final metrics timeline profiling property '{0}' must be boolean true: {1}" -f @(
                    $profilingName,
                    $metricsTimelinePath))
        }
    }

    $finalOutputSeenProperty = $finalRuntimeMetrics.PSObject.Properties["output_seen"]
    if ($null -eq $finalOutputSeenProperty -or
        $finalOutputSeenProperty.Value -isnot [bool] -or
        $finalOutputSeenProperty.Value -ne $true)
    {
        throw "The final metrics timeline output_seen must be boolean true: $metricsTimelinePath"
    }

    $finalTimeoutProperty = $finalRuntimeMetrics.PSObject.Properties["timeout_expired"]
    if ($null -eq $finalTimeoutProperty -or
        $finalTimeoutProperty.Value -isnot [bool] -or
        $finalTimeoutProperty.Value -ne $false)
    {
        throw "The final metrics timeline timeout_expired must be boolean false: $metricsTimelinePath"
    }

    $finalBackendErrorProperty = $finalRuntimeMetrics.PSObject.Properties["backend_error_count"]
    if ($null -eq $finalBackendErrorProperty) {
        throw "The final metrics timeline is missing backend_error_count: $metricsTimelinePath"
    }
    if (!(Test-IntegralJsonNumber -Value $finalBackendErrorProperty.Value)) {
        throw "The final metrics timeline backend_error_count must be an integral JSON number: $metricsTimelinePath"
    }
    if ($finalBackendErrorProperty.Value -ne 0) {
        throw "The final metrics timeline backend_error_count must be 0: $metricsTimelinePath"
    }

    $finalReasonProperty = $finalRuntimeMetrics.PSObject.Properties["process_exit_reason"]
    if ($null -eq $finalReasonProperty -or
        $finalReasonProperty.Value -isnot [string] -or
        $finalReasonProperty.Value -ne "INTERRUPTED")
    {
        throw "The final metrics timeline runtime_metrics must report INTERRUPTED: $metricsTimelinePath"
    }

    $finalExitCodeProperty = $finalRuntimeMetrics.PSObject.Properties["process_exit_code"]
    if ($null -eq $finalExitCodeProperty) {
        throw "The final metrics timeline is missing process_exit_code: $metricsTimelinePath"
    }
    if (!(Test-IntegralJsonNumber -Value $finalExitCodeProperty.Value)) {
        throw "The final metrics timeline process_exit_code must be an integral JSON number: $metricsTimelinePath"
    }
    if ($finalExitCodeProperty.Value -ne 130) {
        throw "The final metrics timeline process_exit_code must be 130: $metricsTimelinePath"
    }

    $finalExitedProperty = $finalRuntimeMetrics.PSObject.Properties["process_exited"]
    if ($null -eq $finalExitedProperty -or
        $finalExitedProperty.Value -isnot [bool] -or
        $finalExitedProperty.Value -ne $true)
    {
        throw "The final metrics timeline process_exited must be boolean true: $metricsTimelinePath"
    }
}

function Update-ManifestFileInventory
{
    $manifestName = [IO.Path]::GetFileName($manifestPath)
    $script:manifest["files"] = @(
        Get-ChildItem -LiteralPath $resolvedOutputDirectory -Force -File |
            Where-Object { $_.Name -ne $manifestName } |
            Sort-Object -Property Name |
            ForEach-Object {
                $hash = Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256
                [ordered]@{
                    name   = $_.Name
                    length = $_.Length
                    sha256 = $hash.Hash
                }
            })
}

$terminalProcess = $null
$standardOutputTask = $null
$standardErrorTask = $null
$kernelTraceStarted = $false
$kernelTraceStartAttempted = $false
$wprPath = $null
$traceInstanceName = $null
$codexProcessId = $null
$script:pinnedWindowHandle = [IntPtr]::Zero
$primaryFailure = $null
$cleanupFailure = $null
$traceFailure = $null
$shutdownSequenceAttempted = $false

try {
    Write-RunManifest

    if ($KernelTrace) {
        Assert-InvokingPowerShellElevated
        $manifest["kernel_trace_elevation_verified"] = $true
        Write-RunManifest
        $wprPath = Resolve-WprPath
        $traceInstanceName = "VnmCodexResize_{0}" -f ([Guid]::NewGuid().ToString("N"))
        $manifest["kernel_trace_instance"] = $traceInstanceName
        Write-RunManifest
        $kernelTraceStartAttempted = $true
        $manifest["kernel_trace_start_attempted"] = $true
        Write-RunManifest
        Start-KernelTrace `
            -WprPath $wprPath `
            -TraceInstanceName $traceInstanceName `
            -TraceDirectory $resolvedOutputDirectory
        $kernelTraceStarted = $true
        $manifest["kernel_trace_started_at_utc"] = Get-UtcTimestamp
        $manifest["status"] = "kernel-trace-started"
        Write-RunManifest
    }

    $terminalProcess = Start-TerminalProcess `
        -Executable $inputPaths.terminal_exe `
        -Arguments $terminalArguments `
        -ProcessWorkingDirectory $inputPaths.working_directory `
        -CodexAccountHome $inputPaths.codex_account_home
    $standardOutputTask = $terminalProcess.StandardOutput.ReadToEndAsync()
    $standardErrorTask = $terminalProcess.StandardError.ReadToEndAsync()
    $manifest["process"]["pid"] = $terminalProcess.Id
    $manifest["status"] = "running"
    Write-RunManifest

    $codexChild = Wait-ForDirectCodexChild `
        -TerminalProcess $terminalProcess `
        -ExpectedExecutablePath $inputPaths.codex_exe `
        -TimeoutMs $CodexChildDiscoveryTimeoutMs
    $codexProcessId = [int] $codexChild.ProcessId
    $manifest["codex_process"]["pid"] = $codexProcessId
    $manifest["codex_process"]["path"] = [string] $codexChild.ExecutablePath
    $manifest["codex_process"]["parent_pid"] = [int] $codexChild.ParentProcessId
    $manifest["codex_process"]["discovered_at_utc"] = Get-UtcTimestamp
    $manifest["status"] = "codex-child-ready"
    Write-RunManifest

    $initialWindow = Wait-ForTerminalWindow `
        -Process $terminalProcess `
        -TimeoutMs $WindowDiscoveryTimeoutMs
    $script:pinnedWindowHandle = $initialWindow.Handle
    $manifest["pinned_window_handle"] = $initialWindow.HandleHex
    $initialObservation = Convert-SnapshotToManifestObject -Snapshot $initialWindow
    $initialObservation["timestamp_utc"] = Get-UtcTimestamp
    $initialObservation["elapsed_ms"] = Get-RunElapsedMilliseconds
    $manifest["initial_window_observation"] = $initialObservation
    $manifest["status"] = "window-ready"
    Write-RunManifest

    $sequence = 0
    ++$sequence
    Invoke-WindowGeometryTransition `
        -Process $terminalProcess `
        -Cycle 0 `
        -Sequence $sequence `
        -RequestedWidth $windowDimensions.width `
        -RequestedHeight $windowDimensions.height `
        -HoldMs $InitialWindowedHoldMs `
        -Phase "initial-windowed-base"

    ++$sequence
    Invoke-WindowGeometryTransition `
        -Process $terminalProcess `
        -Cycle 1 `
        -Sequence $sequence `
        -RequestedWidth $expandedWindowWidth `
        -RequestedHeight $windowDimensions.height `
        -HoldMs $ResizedWindowHoldMs `
        -Phase "expanded-windowed-width"

    ++$sequence
    Invoke-WindowGeometryTransition `
        -Process $terminalProcess `
        -Cycle 2 `
        -Sequence $sequence `
        -RequestedWidth $windowDimensions.width `
        -RequestedHeight $windowDimensions.height `
        -HoldMs $FinalWindowedHoldMs `
        -Phase "restored-windowed-base"

    $manifest["status"] = "exiting"
    $shutdownSequenceAttempted = $true
    $manifest["shutdown_sequence_attempted"] = $true
    Write-RunManifest
    Send-AutomatedCtrlC `
        -Process $terminalProcess `
        -CodexProcessId $codexProcessId `
        -ExpectedExecutablePath $inputPaths.codex_exe
}
catch {
    $primaryFailure = $_
    $manifest["status"] = "failed"
    $manifest["error"] = $_.Exception.Message
    try {
        Write-RunManifest
    }
    catch {
        $cleanupFailure = $_
    }
}
finally {
    $terminalExited = $false
    $terminalExitCode = $null

    if ($null -ne $terminalProcess) {
        try {
            if (!$terminalProcess.HasExited -and !$shutdownSequenceAttempted) {
                $shutdownSequenceAttempted = $true
                $manifest["shutdown_sequence_attempted"] = $true
                Write-RunManifest
                if ($null -eq $codexProcessId) {
                    throw "Cannot send automated Ctrl+C because the direct Codex child was never established."
                }
                Send-AutomatedCtrlC `
                    -Process $terminalProcess `
                    -CodexProcessId $codexProcessId `
                    -ExpectedExecutablePath $inputPaths.codex_exe
            }
        }
        catch {
            if ($null -eq $cleanupFailure) {
                $cleanupFailure = $_
            }
        }

        try {
            if (!$terminalProcess.HasExited) {
                $closeReturned = $terminalProcess.CloseMainWindow()
                Add-ExitSequenceAction -Action "cleanup-close-main-window-retry" -Details @{
                    close_returned = [bool] $closeReturned
                }
                [void] (Wait-ForProcessExit `
                    -Process $terminalProcess `
                    -TimeoutMs $ExitTimeoutMs)
            }
        }
        catch {
            if ($null -eq $cleanupFailure) {
                $cleanupFailure = $_
            }
        }

        try {
            if (!$terminalProcess.HasExited) {
                Invoke-TerminalProcessTreeTermination -Process $terminalProcess
                [void] (Wait-ForProcessExit `
                    -Process $terminalProcess `
                    -TimeoutMs ([Math]::Min($ExitTimeoutMs, 10000)))
                if (!$terminalProcess.HasExited) {
                    throw "taskkill did not terminate the spawned vnm_terminal process tree. PID $($terminalProcess.Id) remains running."
                }
            }
        }
        catch {
            if ($null -eq $cleanupFailure) {
                $cleanupFailure = $_
            }
        }

        try {
            $terminalExited = [bool] $terminalProcess.HasExited
            if ($terminalExited) {
                $terminalExitCode = $terminalProcess.ExitCode
                $manifest["process"]["exit_code"] = $terminalExitCode
                $manifest["process"]["exit_observed_at_utc"] = Get-UtcTimestamp
            }
            $manifest["process"]["exited"] = $terminalExited
        }
        catch {
            if ($null -eq $cleanupFailure) {
                $cleanupFailure = $_
            }
        }

        try {
            if ($terminalExited -and
                $null -ne $standardOutputTask -and
                $null -ne $standardErrorTask)
            {
                Complete-RedirectedOutput `
                    -StandardOutputTask $standardOutputTask `
                    -StandardErrorTask $standardErrorTask
            }
        }
        catch {
            if ($null -eq $cleanupFailure) {
                $cleanupFailure = $_
            }
        }
    }

    if ($kernelTraceStarted -or $kernelTraceStartAttempted) {
        $manifest["status"] = "stopping-kernel-trace"
        for ($attempt = 1;
            $attempt -le 2 -and ($kernelTraceStarted -or $kernelTraceStartAttempted);
            ++$attempt)
        {
            $stopRecord = [ordered]@{
                attempt       = $attempt
                started_at_utc = Get-UtcTimestamp
                succeeded     = $false
                error         = $null
            }
            try {
                Stop-KernelTrace `
                    -WprPath $wprPath `
                    -TraceInstanceName $traceInstanceName `
                    -EtlPath $etlPath
                $stopRecord["succeeded"] = $true
                $kernelTraceStarted = $false
                $kernelTraceStartAttempted = $false
                $manifest["kernel_trace_stop_succeeded"] = $true
                $manifest["kernel_trace_stopped_at_utc"] = Get-UtcTimestamp
                $traceFailure = $null
            }
            catch {
                $stopRecord["error"] = $_.Exception.Message
                $traceFailure = $_
                if ($attempt -eq 1) {
                    Start-Sleep -Milliseconds 250
                }
            }
            $stopRecord["finished_at_utc"] = Get-UtcTimestamp
            [void] $manifest["kernel_trace_stop_attempts"].Add($stopRecord)
            try {
                Write-RunManifest
            }
            catch {
                if ($null -eq $cleanupFailure) {
                    $cleanupFailure = $_
                }
            }
        }
    }

    if ($kernelTraceStarted -or $kernelTraceStartAttempted) {
        $manifest["status"] = "canceling-kernel-trace"
        $manifest["kernel_trace_cancel_attempted"] = $true
        $cancelRecord = [ordered]@{
            attempt        = 1
            operation      = "cancel"
            started_at_utc = Get-UtcTimestamp
            finished_at_utc = $null
            succeeded      = $false
            proven_stopped = $false
            proof          = $null
            exit_code      = $null
            error          = $null
        }
        try {
            $cancelExitCode = Cancel-KernelTrace `
                -WprPath $wprPath `
                -TraceInstanceName $traceInstanceName
            $cancelRecord["exit_code"] = $cancelExitCode
            $cancelRecord["succeeded"] = $true
            $cancelRecord["proven_stopped"] = $true
            $cancelRecord["proof"] = "wpr_exit_code_0"
            $manifest["kernel_trace_cancel_succeeded"] = $true
            $manifest["kernel_trace_cancel_proven"] = $true
            $kernelTraceStarted = $false
            $kernelTraceStartAttempted = $false
        }
        catch {
            $cancelRecord["error"] = $_.Exception.Message
            $manifest["kernel_trace_cancel_error"] = $_.Exception.Message
            # Keep the stop failure in traceFailure so cancellation cannot make the run succeed.
        }
        $cancelRecord["finished_at_utc"] = Get-UtcTimestamp
        [void] $manifest["kernel_trace_cancel_attempts"].Add($cancelRecord)
        try {
            Write-RunManifest
        }
        catch {
            if ($null -eq $cleanupFailure) {
                $cleanupFailure = $_
            }
        }
    }

    try {
        Update-ManifestFileInventory
        $manifest["finished_at_utc"] = Get-UtcTimestamp
        $manifest["elapsed_ms"] = Get-RunElapsedMilliseconds
        Write-RunManifest
    }
    catch {
        if ($null -eq $cleanupFailure) {
            $cleanupFailure = $_
        }
    }

    if ($null -ne $terminalProcess) {
        try {
            $terminalProcess.Dispose()
        }
        catch {
            if ($null -eq $cleanupFailure) {
                $cleanupFailure = $_
            }
        }
    }
}

if ($null -eq $primaryFailure -and $null -eq $cleanupFailure -and $null -eq $traceFailure) {
    try {
        if ($null -eq $terminalProcess -or !$terminalExited) {
            throw "vnm_terminal did not complete a clean exit."
        }
        Assert-OutputArtifacts -ExitCode $terminalExitCode
    }
    catch {
        $primaryFailure = $_
    }
}

if ($null -ne $primaryFailure -or $null -ne $cleanupFailure -or $null -ne $traceFailure) {
    $failureMessages = @(
        $primaryFailure,
        $cleanupFailure,
        $traceFailure
    ) | Where-Object { $null -ne $_ } | ForEach-Object { $_.Exception.Message }
    $manifest["status"] = "failed"
    $manifest["error"] = $failureMessages -join " | "
    try {
        Update-ManifestFileInventory
        $manifest["finished_at_utc"] = Get-UtcTimestamp
        $manifest["elapsed_ms"] = Get-RunElapsedMilliseconds
        Write-RunManifest
    }
    catch {
        $failureMessages += $_.Exception.Message
    }
    throw ($failureMessages -join " | ")
}

$manifest["status"] = "completed"
$manifest["finished_at_utc"] = Get-UtcTimestamp
$manifest["elapsed_ms"] = Get-RunElapsedMilliseconds
Update-ManifestFileInventory
Write-RunManifest

Write-Host "Completed deterministic Codex resume/resize profile."
Write-Host "Output directory: $resolvedOutputDirectory"
Write-Host "Manifest: $manifestPath"
exit 0
