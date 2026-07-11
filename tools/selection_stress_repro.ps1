<#
.SYNOPSIS
Launches vnm_terminal with an interactive long-output selection stress payload.

.DESCRIPTION
The default mode is a launcher. It starts vnm_terminal.exe, then runs this same
script in -PayloadOnly mode inside the terminal as the child process.

The payload emits deterministic rows up to each configured checkpoint and pauses
after every checkpoint so text selection can be tested in the live terminal
window. The launcher writes optional captures and app metrics to a unique run
subdirectory under the configured artifact directory. The default artifact root
is outside the repository. When app metrics are enabled, the final summary
reports the Surface-provided full-width prefix-plain-ASCII retained-row estimate.

-PayloadOnly is intended for the launcher, but it can also be run manually in
another terminal to isolate payload behavior from the vnm_terminal launcher.
#>

[CmdletBinding(PositionalBinding = $false)]
param(
    [string] $TerminalExe = "",

    [string] $Checkpoints = "20000,50000,100000,200000,205000",

    [ValidateRange(1, 100000)]
    [int] $RowsPerWrite = 1000,

    [ValidateRange(0, 10000)]
    [int] $InterChunkDelayMs = 0,

    [ValidateRange(0, 10000000)]
    [int] $ScrollbackLimit = 0,


    [ValidateRange(100, 60000)]
    [int] $MetricsTimelineIntervalMs = 1000,

    [string] $WindowSize = "1100x760",

    [string] $OutputDirectory = "",

    [switch] $SelectionTrace,

    [switch] $CaptureOutput,

    [switch] $NoAppMetrics,


    [switch] $NativeTitlebar,

    [switch] $DisablePrimaryRepaintRecovery,

    [switch] $EnableMouseReporting,

    [switch] $PayloadOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

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
        Join-Path $repoRoot "build\Release\vnm_terminal.exe",
        Join-Path $repoRoot "build\Debug\vnm_terminal.exe",
        Join-Path $repoRoot "build\vnm_terminal.exe",
        Join-Path $repoRoot "build_codex_transcript_on\Release\vnm_terminal.exe",
        Join-Path $repoRoot "build_codex_transcript_on\vnm_terminal.exe",
        Join-Path $repoRoot "dist\portable\vnm_terminal.exe"
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

function Parse-CheckpointList
{
    param(
        [Parameter(Mandatory = $true)]
        [string] $Text
    )

    $values = New-Object System.Collections.Generic.List[int]
    foreach ($part in ($Text -split ",")) {
        $trimmed = $part.Trim()
        if ([string]::IsNullOrWhiteSpace($trimmed)) {
            throw "Checkpoint list contains an empty item."
        }

        $value = 0
        if (![int]::TryParse(
                $trimmed,
                [System.Globalization.NumberStyles]::Integer,
                [System.Globalization.CultureInfo]::InvariantCulture,
                [ref] $value))
        {
            throw "Checkpoint is not an integer: $trimmed"
        }
        if ($value -le 0) {
            throw "Checkpoint must be positive: $trimmed"
        }
        if ($values.Count -gt 0 -and $value -le $values[$values.Count - 1]) {
            throw "Checkpoints must be strictly increasing: $Text"
        }

        $values.Add($value)
    }

    if ($values.Count -eq 0) {
        throw "At least one checkpoint is required."
    }

    return $values.ToArray()
}

function Resolve-OutputDirectory
{
    param(
        [Parameter(Mandatory = $true)]
        [string] $ConfiguredOutputDirectory
    )

    if (![string]::IsNullOrWhiteSpace($ConfiguredOutputDirectory)) {
        $artifactRoot = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath(
            $ConfiguredOutputDirectory)
    }
    else {
        $artifactRoot = [System.IO.Path]::GetTempPath()
    }

    New-Item -ItemType Directory -Force -Path $artifactRoot | Out-Null
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
    $runName = "vnm_terminal_selection_stress_${stamp}_$([Guid]::NewGuid().ToString('N'))"
    $directory = Join-Path $artifactRoot $runName
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    return (Resolve-Path -LiteralPath $directory).ProviderPath
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

    $builder = [System.Text.StringBuilder]::new()
    [void] $builder.Append('"')

    $backslashes = 0
    foreach ($ch in $Argument.ToCharArray()) {
        if ($ch -eq '\') {
            ++$backslashes
            continue
        }
        if ($ch -eq '"') {
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
        [void] $builder.Append($ch)
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

    $quoted = foreach ($argument in $Arguments) {
        ConvertTo-WindowsCommandLineArgument -Argument $argument
    }
    return $quoted -join " "
}

function Write-RetainedHistoryEstimateSummary
{
    param(
        [Parameter(Mandatory = $true)]
        [string] $MetricsPath
    )

    if (!(Test-Path -LiteralPath $MetricsPath -PathType Leaf)) {
        Write-Warning "Final app metrics are unavailable: $MetricsPath"
        return
    }

    try {
        $metricsJson = Get-Content -Raw -LiteralPath $MetricsPath
        $metrics = $metricsJson | ConvertFrom-Json
    }
    catch {
        Write-Warning "Final app metrics could not be parsed at ${MetricsPath}: $($_.Exception.Message)"
        return
    }

    $trimmedMetricsJson = $metricsJson.TrimStart()
    if ($trimmedMetricsJson.Length -eq 0 -or
        $trimmedMetricsJson[0] -ne '{' -or
        $null -eq $metrics -or
        $metrics -is [System.Array] -or
        $metrics -isnot [System.Management.Automation.PSCustomObject])
    {
        Write-Warning "Final app metrics top-level value must be a JSON object: $MetricsPath"
        return
    }

    $schemaProperty = $metrics.PSObject.Properties["schema"]
    if ($null -eq $schemaProperty -or
        $schemaProperty.Value -isnot [string] -or
        $schemaProperty.Value -ne "vnm_terminal_runtime_metrics_v3")
    {
        $reportedSchema = if ($null -eq $schemaProperty) {
            "<missing>"
        }
        else {
            [string] $schemaProperty.Value
        }
        Write-Warning "Final app metrics use an unexpected schema '$reportedSchema': $MetricsPath"
        return
    }

    $retainedHistoryProperty = $metrics.PSObject.Properties["retained_history"]
    if ($null -eq $retainedHistoryProperty) {
        Write-Warning "Final app metrics lack the retained_history object: $MetricsPath"
        return
    }
    $retainedHistory = $retainedHistoryProperty.Value
    if ($null -eq $retainedHistory -or
        $retainedHistory -is [System.Array] -or
        $retainedHistory -isnot [System.Management.Automation.PSCustomObject])
    {
        Write-Warning "Final app metrics retained_history value must be a JSON object: $MetricsPath"
        return
    }

    $estimateProperty = $retainedHistory.PSObject.Properties["prefix_plain_ascii_estimate"]
    if ($null -eq $estimateProperty) {
        Write-Warning (
            "Final app metrics retained_history lack the prefix_plain_ascii_estimate object: " +
            $MetricsPath)
        return
    }
    $estimate = $estimateProperty.Value
    if ($null -eq $estimate -or
        $estimate -is [System.Array] -or
        $estimate -isnot [System.Management.Automation.PSCustomObject])
    {
        Write-Warning (
            "Final app metrics retained_history.prefix_plain_ascii_estimate must be a JSON object: " +
            $MetricsPath)
        return
    }

    $counterNames = @(
        "contract_version",
        "source_width_columns",
        "record_bytes",
        "retained_rows",
        "target_rows",
        "max_columns_at_target_rows"
    )
    $counters = @{}
    foreach ($counterName in $counterNames) {
        $property = $estimate.PSObject.Properties[$counterName]
        if ($null -eq $property) {
            Write-Warning (
                "Final app metrics retained_history.prefix_plain_ascii_estimate." +
                "$counterName is missing: $MetricsPath")
            return
        }
        if ($property.Value -isnot [string] -or
            $property.Value -notmatch '^[0-9]+$')
        {
            Write-Warning (
                "Final app metrics retained_history.prefix_plain_ascii_estimate." +
                "$counterName must be an unsigned decimal counter string: $MetricsPath")
            return
        }

        [uint64] $parsedCounter = 0
        if (![uint64]::TryParse(
                $property.Value,
                [System.Globalization.NumberStyles]::None,
                [System.Globalization.CultureInfo]::InvariantCulture,
                [ref] $parsedCounter))
        {
            Write-Warning (
                "Final app metrics retained_history.prefix_plain_ascii_estimate." +
                "$counterName is outside uint64 range: $MetricsPath")
            return
        }
        $counters[$counterName] = $parsedCounter
    }

    [uint64] $contractVersion = $counters["contract_version"]
    [uint64] $columns         = $counters["source_width_columns"]
    [uint64] $recordBytes     = $counters["record_bytes"]
    [uint64] $estimatedRows   = $counters["retained_rows"]
    [uint64] $targetRows      = $counters["target_rows"]
    [uint64] $maxColumns      = $counters["max_columns_at_target_rows"]

    if ($contractVersion -ne 2) {
        Write-Warning (
            "Final app metrics retained_history.prefix_plain_ascii_estimate.contract_version " +
            "must be 2: $MetricsPath")
        return
    }
    if ($columns -eq 0 -or $columns -gt 4096) {
        Write-Warning (
            "Final app metrics retained_history.prefix_plain_ascii_estimate." +
            "source_width_columns is outside 1..4096: $MetricsPath")
        return
    }
    if ($recordBytes -eq 0) {
        Write-Warning (
            "Final app metrics retained_history.prefix_plain_ascii_estimate." +
            "record_bytes must be positive: $MetricsPath")
        return
    }
    if ($estimatedRows -eq 0) {
        Write-Warning (
            "Final app metrics retained_history.prefix_plain_ascii_estimate." +
            "retained_rows must be positive: $MetricsPath")
        return
    }
    if ($targetRows -eq 0) {
        Write-Warning (
            "Final app metrics retained_history.prefix_plain_ascii_estimate." +
            "target_rows must be positive: $MetricsPath")
        return
    }
    if ($maxColumns -gt 4096) {
        Write-Warning (
            "Final app metrics retained_history.prefix_plain_ascii_estimate." +
            "max_columns_at_target_rows is outside 0..4096: $MetricsPath")
        return
    }

    Write-Host "Current terminal columns: $columns"
    Write-Host (
        ("Estimated full-width prefix-plain-ASCII retained-row capacity: {0} rows " +
        "at {1} bytes per row.") -f @(
            $estimatedRows,
            $recordBytes))
}

function Invoke-PostLaunchDiagnostic
{
    param(
        [Parameter(Mandatory = $true)]
        [scriptblock] $Action,

        [Parameter(Mandatory = $true)]
        [string] $Description
    )

    try {
        & $Action
    }
    catch {
        Write-Warning (
            "Post-launch $Description could not be reported: $($_.Exception.Message)") `
            -WarningAction Continue
    }
}

function Write-StressRows
{
    param(
        [Parameter(Mandatory = $true)]
        [int] $StartRow,

        [Parameter(Mandatory = $true)]
        [int] $Count,

        [Parameter(Mandatory = $true)]
        [int] $RowsPerChunk,

        [Parameter(Mandatory = $true)]
        [int] $Phase,

        [Parameter(Mandatory = $true)]
        [int] $TargetRows,

        [Parameter(Mandatory = $true)]
        [int] $DelayMs
    )

    $builder = [System.Text.StringBuilder]::new(160 * [Math]::Min($RowsPerChunk, 1000))
    $written = 0
    while ($written -lt $Count) {
        [void] $builder.Clear()
        $chunkRows = [Math]::Min($RowsPerChunk, $Count - $written)
        for ($offset = 0; $offset -lt $chunkRows; ++$offset) {
            $row = $StartRow + $written + $offset
            $line = (
                "vnm-selection-stress row={0:D9} phase={1:D2} target={2:D9} token={3:D4} " +
                "text=selectable-stable-output-abcdefghijklmnopqrstuvwxyz") -f @(
                    $row,
                    $Phase,
                    $TargetRows,
                    ($row % 10000))
            [void] $builder.AppendLine($line)
        }

        [Console]::Out.Write($builder.ToString())
        [Console]::Out.Flush()
        $written += $chunkRows

        if ($DelayMs -gt 0) {
            Start-Sleep -Milliseconds $DelayMs
        }
    }
}

function Write-InteractivePayload
{
    param(
        [Parameter(Mandatory = $true)]
        [int[]] $CheckpointValues,

        [Parameter(Mandatory = $true)]
        [int] $RowsPerChunk,

        [Parameter(Mandatory = $true)]
        [int] $DelayMs,

        [Parameter(Mandatory = $true)]
        [bool] $MouseReporting
    )

    [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
    $escape = [char] 27
    if ($MouseReporting) {
        [Console]::Out.Write("${escape}[?1000h${escape}[?1006h")
    }

    try {
        [Console]::Out.WriteLine("VNM selection stress payload")
        [Console]::Out.WriteLine("Checkpoints: {0}" -f ($CheckpointValues -join ", "))
        [Console]::Out.WriteLine(
            "At each checkpoint, try selecting visible output, then press Enter to continue.")
        if ($MouseReporting) {
            [Console]::Out.WriteLine(
                "Mouse reporting is enabled for this run; use Shift-drag for local selection.")
        }
        [Console]::Out.WriteLine("")
        [Console]::Out.Flush()

        $totalRows = 0
        for ($phase = 0; $phase -lt $CheckpointValues.Count; ++$phase) {
            $targetRows = $CheckpointValues[$phase]
            $rowsToWrite = $targetRows - $totalRows
            $beginMessage = "BEGIN checkpoint {0}/{1}: writing rows {2} through {3}" -f @(
                ($phase + 1),
                $CheckpointValues.Count,
                ($totalRows + 1),
                $targetRows)
            [Console]::Out.WriteLine($beginMessage)
            [Console]::Out.Flush()

            Write-StressRows `
                -StartRow ($totalRows + 1) `
                -Count $rowsToWrite `
                -RowsPerChunk $RowsPerChunk `
                -Phase ($phase + 1) `
                -TargetRows $targetRows `
                -DelayMs $DelayMs

            $totalRows = $targetRows
            [Console]::Out.WriteLine("")
            $checkpointTemplate =
                "CHECKPOINT total_rows={0}: select text now. Press Enter to continue, or type q then Enter to stop."
            $markerTemplate = "SELECTION MARKER total_rows={0} phrase=keep-this-highlight-visible"
            $checkpointMessage = $checkpointTemplate -f @($totalRows)
            $markerMessage     = $markerTemplate -f @($totalRows)
            [Console]::Out.WriteLine($checkpointMessage)
            [Console]::Out.WriteLine($markerMessage)
            [Console]::Out.Flush()

            $response = [Console]::In.ReadLine()
            if ($response -match '^\s*q') {
                [Console]::Out.WriteLine("STOP requested at total_rows={0}" -f $totalRows)
                [Console]::Out.Flush()
                return
            }
        }

        [Console]::Out.WriteLine("")
        $doneTemplate =
            "DONE total_rows={0}: perform final selection checks, then close the terminal window when finished."
        $doneMessage = $doneTemplate -f @($totalRows)
        [Console]::Out.WriteLine($doneMessage)
        [Console]::Out.Flush()
    }
    finally {
        if ($MouseReporting) {
            [Console]::Out.Write("${escape}[?1000l${escape}[?1006l")
            [Console]::Out.Flush()
        }
    }
}

if ($MyInvocation.InvocationName -eq ".") {
    return
}

$checkpointValues = @(Parse-CheckpointList -Text $Checkpoints)

if ($PayloadOnly) {
    Write-InteractivePayload `
        -CheckpointValues $checkpointValues `
        -RowsPerChunk $RowsPerWrite `
        -DelayMs $InterChunkDelayMs `
        -MouseReporting ([bool] $EnableMouseReporting)
    exit 0
}

$resolvedTerminalExe = Resolve-TerminalExe -ConfiguredTerminalExe $TerminalExe
Assert-TerminalDeployment -ResolvedTerminalExe $resolvedTerminalExe

$artifactDirectory = Resolve-OutputDirectory -ConfiguredOutputDirectory $OutputDirectory
$metricsJsonPath = Join-Path $artifactDirectory "app_metrics_final.json"
$metricsTimelinePath = Join-Path $artifactDirectory "app_metrics_timeline.jsonl"
$captureOutputPath = Join-Path $artifactDirectory "backend_output.raw"
$selectionTracePath = Join-Path $artifactDirectory "selection_trace.log"

$maxCheckpoint = $checkpointValues[$checkpointValues.Count - 1]
$effectiveScrollbackLimit = if ($ScrollbackLimit -gt 0) {
    $ScrollbackLimit
}
else {
    $maxCheckpoint + 5000
}

$terminalArguments = @(
    "--keep-open-after-process-exits",
    "--scrollback-limit", $effectiveScrollbackLimit.ToString([System.Globalization.CultureInfo]::InvariantCulture),
    "--window-size", $WindowSize
)

if ($NativeTitlebar) {
    $terminalArguments += "--native-titlebar"
}
if ($DisablePrimaryRepaintRecovery) {
    $terminalArguments += "--disable-primary-repaint-recovery"
}
if ($SelectionTrace) {
    $terminalArguments += "--selection-trace"
}
if ($CaptureOutput) {
    $terminalArguments += @("--capture-output", $captureOutputPath)
}
if (!$NoAppMetrics) {
    $terminalArguments += @(
        "--metrics-json", $metricsJsonPath,
        "--metrics-timeline-jsonl", $metricsTimelinePath,
        "--metrics-timeline-interval-ms",
        $MetricsTimelineIntervalMs.ToString([System.Globalization.CultureInfo]::InvariantCulture)
    )
}

$scriptPath = (Resolve-Path -LiteralPath $PSCommandPath).ProviderPath
$payloadArguments = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", $scriptPath,
    "-PayloadOnly",
    "-Checkpoints", $Checkpoints,
    "-RowsPerWrite", $RowsPerWrite.ToString([System.Globalization.CultureInfo]::InvariantCulture),
    "-InterChunkDelayMs", $InterChunkDelayMs.ToString([System.Globalization.CultureInfo]::InvariantCulture)
)

if ($EnableMouseReporting) {
    $payloadArguments += "-EnableMouseReporting"
}

$terminalArguments += @("--", "powershell.exe") + $payloadArguments
$argumentString = Join-WindowsCommandLine -Arguments $terminalArguments

Write-Host "Launching: $resolvedTerminalExe"
Write-Host "Artifacts: $artifactDirectory"
Write-Host "Checkpoints: $($checkpointValues -join ', ')"
Write-Host "Scrollback limit: $effectiveScrollbackLimit"
Write-Host "Close the vnm_terminal window when the interactive checks are complete."

$startProcessParameters = @{
    FilePath     = $resolvedTerminalExe
    ArgumentList = $argumentString
    PassThru     = $true
}
if ($SelectionTrace) {
    $startProcessParameters.RedirectStandardError = $selectionTracePath
}

$terminalProcess = Start-Process @startProcessParameters

$terminalProcess.WaitForExit()
$targetExitCode = $terminalProcess.ExitCode

if (!$NoAppMetrics) {
    Invoke-PostLaunchDiagnostic -Description "app metrics artifacts" -Action {
        if (Test-Path -LiteralPath $metricsJsonPath -PathType Leaf) {
            Write-Host "App metrics final: $metricsJsonPath"
        }
        if (Test-Path -LiteralPath $metricsTimelinePath -PathType Leaf) {
            Write-Host "App metrics timeline: $metricsTimelinePath"
        }
    }
    Invoke-PostLaunchDiagnostic -Description "retained-history metrics summary" -Action {
        Write-RetainedHistoryEstimateSummary -MetricsPath $metricsJsonPath
    }
}
Invoke-PostLaunchDiagnostic -Description "backend output capture artifact" -Action {
    if ($CaptureOutput -and (Test-Path -LiteralPath $captureOutputPath -PathType Leaf)) {
        Write-Host "Backend output capture: $captureOutputPath"
    }
}
Invoke-PostLaunchDiagnostic -Description "selection trace artifact" -Action {
    if ($SelectionTrace -and (Test-Path -LiteralPath $selectionTracePath -PathType Leaf)) {
        Write-Host "Selection trace: $selectionTracePath"
    }
}

exit $targetExitCode
