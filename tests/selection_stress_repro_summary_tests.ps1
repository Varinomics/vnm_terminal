[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $ToolPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. $ToolPath

function Write-MetricsFixture
{
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path,

        [string] $ContractVersion = "2",

        [object] $Columns = "160",

        [string] $MaxColumns = "187"
    )

    $metrics = @{
        schema = "vnm_terminal_runtime_metrics_v3"
        retained_history = @{
            byte_budget = "10"
            prefix_plain_ascii_estimate = @{
                contract_version = $ContractVersion
                source_width_columns = $Columns
                record_bytes = "316"
                retained_rows = "777777"
                target_rows = "205000"
                max_columns_at_target_rows = $MaxColumns
            }
        }
    } | ConvertTo-Json -Depth 4 -Compress

    [System.IO.File]::WriteAllText(
        $Path,
        $metrics,
        [System.Text.UTF8Encoding]::new($false))
}

function Invoke-MetricsFixture
{
    param(
        [Parameter(Mandatory = $true)]
        [scriptblock] $Write,

        [Parameter(Mandatory = $true)]
        [scriptblock] $Validate
    )

    $path = Join-Path (
        [System.IO.Path]::GetTempPath()
    ) "vnm_terminal_retained_estimate_$([Guid]::NewGuid().ToString('N')).json"
    try {
        & $Write $path
        & $Validate $path
    }
    finally {
        Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
    }
}

function Assert-Warning
{
    param(
        [Parameter(Mandatory = $true)]
        [string] $MetricsPath,

        [Parameter(Mandatory = $true)]
        [string] $CaseName,

        [Parameter(Mandatory = $true)]
        [string] $ExpectedWarning
    )

    $warnings = @()
    $output = @(
        Write-RetainedHistoryEstimateSummary -MetricsPath $MetricsPath `
            -WarningVariable warnings -WarningAction SilentlyContinue 6>&1
    ) -join [Environment]::NewLine
    if ($warnings.Count -eq 0) {
        throw "$CaseName should produce a best-effort warning"
    }
    if (($warnings -join [Environment]::NewLine) -notmatch
        [regex]::Escape($ExpectedWarning))
    {
        throw "$CaseName should identify '$ExpectedWarning'"
    }
    if ($output -match "Current terminal columns:" -or
        $output -match "Estimated full-width prefix-plain-ASCII retained-row capacity:")
    {
        throw "$CaseName must not print a confident retained-history summary"
    }
}

Invoke-MetricsFixture -Write {
    param($Path)
    Write-MetricsFixture -Path $Path
} -Validate {
    param($Path)
    $summary = @(
        Write-RetainedHistoryEstimateSummary -MetricsPath $Path 6>&1
    ) -join [Environment]::NewLine
    if ($summary -notmatch "Current terminal columns: 160" -or
        $summary -notmatch "777777 rows at 316 bytes per row")
    {
        throw "summary must report producer values without recomputing byte-budget arithmetic"
    }
}

Invoke-MetricsFixture -Write {
    param($Path)
    Write-MetricsFixture -Path $Path -ContractVersion "1"
} -Validate {
    param($Path)
    Assert-Warning -MetricsPath $Path -CaseName "unexpected estimate contract" `
        -ExpectedWarning "contract_version must be 2"
}

Invoke-MetricsFixture -Write {
    param($Path)
    Write-MetricsFixture -Path $Path -Columns 160
} -Validate {
    param($Path)
    Assert-Warning -MetricsPath $Path -CaseName "numeric estimate counter" `
        -ExpectedWarning "source_width_columns must be an unsigned decimal counter string"
}

Invoke-MetricsFixture -Write {
    param($Path)
    Write-MetricsFixture -Path $Path -MaxColumns "4097"
} -Validate {
    param($Path)
    Assert-Warning -MetricsPath $Path -CaseName "out-of-range estimate counter" `
        -ExpectedWarning "max_columns_at_target_rows is outside 0..4096"
}

Assert-Warning -MetricsPath $PSCommandPath -CaseName "malformed metrics" -ExpectedWarning "could not be parsed"

function Invoke-LauncherFixture
{
    param(
        [Parameter(Mandatory = $true)]
        [string] $CaseName,

        [AllowNull()]
        [string] $MetricsJson,

        [scriptblock] $PrepareArtifactRoot = {},

        [Parameter(Mandatory = $true)]
        [scriptblock] $Validate
    )

    $fixtureRoot = Join-Path (
        [System.IO.Path]::GetTempPath()
    ) "vnm_terminal_selection_launch_$([Guid]::NewGuid().ToString('N'))"
    $artifactRoot = Join-Path $fixtureRoot "artifacts"
    $platformDirectory = Join-Path $fixtureRoot "platforms"
    $fakeTerminalPath = Join-Path $fixtureRoot "fake_terminal.cmd"
    try {
        [System.IO.Directory]::CreateDirectory($artifactRoot) | Out-Null
        [System.IO.Directory]::CreateDirectory($platformDirectory) | Out-Null
        [System.IO.File]::WriteAllBytes(
            (Join-Path $fixtureRoot "Qt6Core.dll"),
            [byte[]] @(0))
        [System.IO.File]::WriteAllBytes(
            (Join-Path $platformDirectory "qwindows.dll"),
            [byte[]] @(0))
        & $PrepareArtifactRoot $artifactRoot

        $fakeTerminalLines = @("@echo off")
        if (![string]::IsNullOrEmpty($MetricsJson)) {
            $fakeTerminalLines += @(
                ":parse",
                'if %~1== goto done',
                'if /i %~1==--metrics-json (',
                "  >%~2 echo $MetricsJson",
                "  goto done",
                ")",
                "shift",
                "goto parse",
                ":done"
            )
        }
        $fakeTerminalLines += "exit /b 37"
        [System.IO.File]::WriteAllText(
            $fakeTerminalPath,
            ($fakeTerminalLines -join ([char] 13 + [char] 10)) + [char] 13 + [char] 10,
            [System.Text.Encoding]::ASCII)

        $currentRuntime = (Get-Process -Id $PID).Path
        $launcherArguments = @(
            "-NoProfile",
            "-ExecutionPolicy", "Bypass",
            "-File", $ToolPath,
            "-TerminalExe", $fakeTerminalPath,
            "-Checkpoints", "1",
            "-CaptureOutput",
            "-OutputDirectory", $artifactRoot
        )
        $launcherOutput = @(
            & $currentRuntime @launcherArguments 2>&1
        ) -join [Environment]::NewLine
        $launcherExitCode = $LASTEXITCODE
        if ($launcherExitCode -ne 37) {
            throw "$CaseName should preserve launched target exit 37, got $launcherExitCode"
        }

        $artifactMatch = [regex]::Match(
            $launcherOutput,
            'Artifacts:\s*(?<path>[A-Za-z]:\\[^\r\n]+)')
        if (!$artifactMatch.Success) {
            throw "$CaseName should print its unique current-run artifact path. Output: $launcherOutput"
        }
        $runDirectory = $artifactMatch.Groups["path"].Value.Trim()
        if ($runDirectory -eq $artifactRoot -or
            (Split-Path -Parent $runDirectory) -ne $artifactRoot -or
            !(Test-Path -LiteralPath $runDirectory -PathType Container))
        {
            throw "$CaseName should use one unique child directory under the artifact root"
        }
        if ((Test-Path -LiteralPath (Join-Path $runDirectory "process_samples.csv")) -or
            $launcherOutput -match "Process samples:")
        {
            throw "$CaseName must not create or report process-tree samples"
        }

        & $Validate $launcherOutput $artifactRoot $runDirectory
    }
    finally {
        Remove-Item -LiteralPath $fixtureRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

$validMetrics = (
    '{"schema":"vnm_terminal_runtime_metrics_v3","retained_history":{"byte_budget":"10",' +
    '"prefix_plain_ascii_estimate":{"contract_version":"2","source_width_columns":"160",' +
    '"record_bytes":"316","retained_rows":"777777","target_rows":"205000",' +
    '"max_columns_at_target_rows":"187"}}}')
Invoke-LauncherFixture -CaseName "sampler-free producer estimate" -MetricsJson $validMetrics -Validate {
    param($Output, $ArtifactRoot, $RunDirectory)
    if ($Output -notmatch "777777 rows at 316 bytes per row" -or
        $Output -notmatch "App metrics final:")
    {
        throw "launcher should report the current run's producer-owned estimate"
    }
}

Invoke-LauncherFixture -CaseName "malformed current-run metrics" -MetricsJson '{malformed current metrics' -Validate {
    param($Output, $ArtifactRoot, $RunDirectory)
    if ($Output -notmatch "could not be parsed" -or
        $Output -notmatch "App metrics final:")
    {
        throw "malformed current-run metrics should warn without replacing target exit 37"
    }
}

$staleMetrics = (
    '{"schema":"vnm_terminal_runtime_metrics_v3","retained_history":{' +
    '"prefix_plain_ascii_estimate":{"contract_version":"2","source_width_columns":"80",' +
    '"record_bytes":"236","retained_rows":"9","target_rows":"205000",' +
    '"max_columns_at_target_rows":"187"}}}')
Invoke-LauncherFixture -CaseName "stale artifact provenance" -MetricsJson $null -PrepareArtifactRoot {
    param($ArtifactRoot)
    $stalePaths = @{
        (Join-Path $ArtifactRoot "app_metrics_final.json") = $staleMetrics
        (Join-Path $ArtifactRoot "app_metrics_timeline.jsonl") = "stale-timeline"
        (Join-Path $ArtifactRoot "backend_output.raw") = "stale-capture"
    }
    foreach ($entry in $stalePaths.GetEnumerator()) {
        [System.IO.File]::WriteAllText(
            $entry.Key,
            $entry.Value,
            [System.Text.UTF8Encoding]::new($false))
    }
} -Validate {
    param($Output, $ArtifactRoot, $RunDirectory)
    $stalePaths = @{
        (Join-Path $ArtifactRoot "app_metrics_final.json") = $staleMetrics
        (Join-Path $ArtifactRoot "app_metrics_timeline.jsonl") = "stale-timeline"
        (Join-Path $ArtifactRoot "backend_output.raw") = "stale-capture"
    }
    foreach ($entry in $stalePaths.GetEnumerator()) {
        if ([System.IO.File]::ReadAllText($entry.Key) -ne $entry.Value) {
            throw "stale artifact provenance should preserve $($entry.Key)"
        }
        if ($Output.Contains($entry.Key)) {
            throw "stale artifact provenance should not report root artifact $($entry.Key)"
        }
    }
    if ($Output -notmatch "Final app metrics are unavailable" -or
        $Output -match "App metrics final:" -or
        $Output -match "App metrics timeline:" -or
        $Output -match "Backend output capture:" -or
        $Output -match "Current terminal columns:")
    {
        throw "stale artifacts must not masquerade as current-run evidence. Output: $Output"
    }
}

$resolverRoot = Join-Path (
    [System.IO.Path]::GetTempPath()
) "vnm_terminal_repro_resolver_$([Guid]::NewGuid().ToString('N'))"
try {
    $resolverTools = Join-Path $resolverRoot "tools"
    $portableExe = Join-Path $resolverRoot "dist\portable_candidate\vnm_terminal.exe"
    New-Item -ItemType Directory -Path $resolverTools | Out-Null
    New-Item -ItemType Directory -Path (Split-Path -Parent $portableExe) | Out-Null
    Copy-Item `
        -LiteralPath (Join-Path (Split-Path -Parent $ToolPath) "terminal_repro_common.ps1") `
        -Destination $resolverTools
    New-Item -ItemType File -Path $portableExe | Out-Null

    . (Join-Path $resolverTools "terminal_repro_common.ps1")
    if ((Resolve-DefaultTerminalExe) -ne $portableExe) {
        throw "default resolver should find dist\portable_candidate\vnm_terminal.exe"
    }
}
finally {
    Remove-Item -LiteralPath $resolverRoot -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "selection stress metrics summary tests passed"
