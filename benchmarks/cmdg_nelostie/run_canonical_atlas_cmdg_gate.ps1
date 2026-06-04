param(
    [string] $TerminalRepo = (Resolve-Path "$PSScriptRoot\..\..").Path,
    [string] $SurfaceRepo = (Resolve-Path "$PSScriptRoot\..\..\..\vnm_terminal_surface").Path,
    [string] $ArtifactTag = "canonical_atlas_cmdg_gate_$(Get-Date -Format yyyyMMdd_HHmmss)",
    [string] $QtRoot = "C:\Qt\6.10.1\msvc2022_64",
    [string] $VcvarsAll = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat",
    [string] $SceneList = "AssemblyWinter2025;Example2D;Plasma;JuliaSetTest;ParticleVortex;Example3D",
    [string] $MotivatingScenes = "Plasma;ParticleVortex",
    [int] $RepeatCount = 3,
    [int] $FrameLimit = 300,
    [string] $WindowSize = "1920x1080",
    [string] $FontSize = "10",
    [string] $ArchivedBaselineComparisonJson = "",
    [double] $MotivatingPaintImprovementThresholdPercent = 25.0,
    [double] $DefaultSceneRegressionThresholdPercent = -5.0,
    [switch] $FocusOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$artifactRoot = Join-Path $TerminalRepo "artifacts\$ArtifactTag"
$buildDir = Join-Path $TerminalRepo "build_${ArtifactTag}_terminal_canonical_atlas"

if ($FocusOnly) {
    $SceneList = $MotivatingScenes
}

New-Item -ItemType Directory -Force -Path $artifactRoot | Out-Null

function Split-SceneNames {
    param([string] $Value)

    return @(
        $Value -split "[;,]" |
            ForEach-Object { $_.Trim() } |
            Where-Object { $_ -ne "" }
    )
}

function Join-CommandLine {
    param(
        [string]   $FilePath,
        [string[]] $Arguments
    )

    $tokens = @($FilePath) + $Arguments
    return ($tokens | ForEach-Object {
        '"' + ($_ -replace '"', '\"') + '"'
    }) -join " "
}

function Invoke-LoggedCommand {
    param(
        [string]   $FilePath,
        [string[]] $Arguments,
        [string]   $WorkingDirectory,
        [string]   $LogPath,
        [switch]   $AllowFailure
    )

    New-Item -ItemType Directory -Force -Path (Split-Path $LogPath -Parent) | Out-Null
    $commandText = Join-CommandLine $FilePath $Arguments
    @(
        "working_directory=$WorkingDirectory",
        "command=$commandText",
        ""
    ) | Set-Content -Encoding ASCII -Path $LogPath

    Push-Location $WorkingDirectory
    try {
        $previousErrorActionPreference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        & $FilePath @Arguments 2>&1 |
            Tee-Object -FilePath $LogPath -Append |
            Out-Host
        $exitCode = $LASTEXITCODE
        $ErrorActionPreference = $previousErrorActionPreference
    }
    finally {
        if (Get-Variable -Name previousErrorActionPreference -Scope Local -ErrorAction SilentlyContinue) {
            $ErrorActionPreference = $previousErrorActionPreference
        }
        Pop-Location
    }

    if ($exitCode -ne 0 -and !$AllowFailure) {
        throw "Command failed with exit code ${exitCode}: $commandText"
    }

    return $exitCode
}

function Invoke-VcvarsCommand {
    param(
        [string]   $FilePath,
        [string[]] $Arguments,
        [string]   $WorkingDirectory,
        [string]   $LogPath,
        [switch]   $AllowFailure
    )

    $innerCommand = Join-CommandLine $FilePath $Arguments
    return Invoke-LoggedCommand `
        -FilePath "cmd.exe" `
        -Arguments @("/a", "/s", "/c", "call `"$VcvarsAll`" x64 && $innerCommand") `
        -WorkingDirectory $WorkingDirectory `
        -LogPath $LogPath `
        -AllowFailure:$AllowFailure
}

function Set-HardwareQsgEnvironment {
    $env:QSG_RENDER_LOOP = "threaded"
    $env:QSG_RHI_BACKEND = "d3d11"
    $env:QSG_RENDER_TIMING = "1"
    $env:QSG_INFO = "1"
    $env:QT_LOGGING_RULES = "qt.scenegraph.*=true;qt.rhi.*=true"
}

function Write-RepoBaseline {
    param(
        [string] $Path,
        [string] $Name
    )

    $outPath = Join-Path $artifactRoot "${Name}_git_baseline.txt"
    @(
        "repo=$Name",
        "path=$Path",
        "rev=$(git -C $Path rev-parse HEAD)",
        "branch=$(git -C $Path branch --show-current)",
        "status --short:"
    ) | Set-Content -Encoding ASCII -Path $outPath
    git -C $Path status --short | Add-Content -Encoding ASCII -Path $outPath
}

function Write-MachineSnapshot {
    $os = Get-CimInstance Win32_OperatingSystem
    $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
    $gpu = Get-CimInstance Win32_VideoController | Select-Object -First 1
    [ordered]@{
        os_caption = $os.Caption
        os_version = $os.Version
        cpu_name = $cpu.Name
        logical_processors = $cpu.NumberOfLogicalProcessors
        gpu_name = $gpu.Name
        gpu_driver_version = $gpu.DriverVersion
    } | ConvertTo-Json -Depth 4 |
        Set-Content -Encoding ASCII -Path (Join-Path $artifactRoot "machine.json")
}

function Get-ObjectProperty {
    param(
        [object] $Object,
        [string] $Name
    )

    if ($null -eq $Object) {
        return $null
    }

    if ($Object -is [System.Collections.IDictionary] -and
        $Object.Contains($Name))
    {
        return $Object[$Name]
    }

    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) {
        return $null
    }

    return $property.Value
}

function Convert-MetricNumber {
    param([object] $Value)

    if ($null -eq $Value) {
        return $null
    }

    $culture = [System.Globalization.CultureInfo]::InvariantCulture
    $styles = [System.Globalization.NumberStyles]::Float -bor
        [System.Globalization.NumberStyles]::AllowThousands
    $parsed = 0.0
    if ([double]::TryParse([string] $Value, $styles, $culture, [ref] $parsed)) {
        return $parsed
    }

    return $null
}

function Convert-MetricBool {
    param([object] $Value)

    if ($null -eq $Value) {
        return $null
    }

    if ($Value -is [bool]) {
        return [bool] $Value
    }

    $text = ([string] $Value).Trim().ToLowerInvariant()
    if ($text -eq "true") {
        return $true
    }
    if ($text -eq "false") {
        return $false
    }

    return $null
}

function Get-MedianMetric {
    param(
        [object[]] $Records,
        [string]   $MetricName
    )

    $values = @(
        $Records |
            ForEach-Object {
                Convert-MetricNumber (Get-ObjectProperty $_ $MetricName)
            } |
            Where-Object { $null -ne $_ } |
            Sort-Object
    )

    if ($values.Count -eq 0) {
        return $null
    }

    $middle = [int] [Math]::Floor($values.Count / 2)
    if (($values.Count % 2) -eq 1) {
        return [double] $values[$middle]
    }

    return ([double] $values[$middle - 1] + [double] $values[$middle]) / 2.0
}

function Get-ImprovementPercent {
    param(
        [double] $Baseline,
        [double] $Candidate
    )

    if ($Baseline -eq 0.0) {
        return $null
    }

    return (($Candidate - $Baseline) / $Baseline) * 100.0
}

function Read-ArchivedBaselineRecords {
    param([string] $Path)

    if ($Path.Trim() -eq "") {
        return @()
    }

    $resolvedPath = (Resolve-Path $Path).Path
    try {
        $archive = Get-Content -Raw -Path $resolvedPath | ConvertFrom-Json
    }
    catch {
        throw "Could not parse archived baseline comparison JSON '$Path': $($_.Exception.Message)"
    }

    $baselineRecords = Get-ObjectProperty $archive "baseline_records"
    if ($null -eq $baselineRecords) {
        throw "Archived baseline comparison JSON '$Path' has no baseline_records array"
    }

    return @($baselineRecords)
}

function Compare-ArchivedBaseline {
    param(
        [object[]] $CurrentRecords,
        [object[]] $BaselineRecords,
        [string[]] $Scenes,
        [string[]] $MotivatingSceneNames,
        [string]   $ArchivePath
    )

    $comparisons = New-Object System.Collections.Generic.List[object]
    $missingScenes = New-Object System.Collections.Generic.List[string]

    foreach ($scene in $Scenes) {
        $currentSceneRecords = @($CurrentRecords | Where-Object {
            (Get-ObjectProperty $_ "scene") -eq $scene
        })
        $baselineSceneRecords = @($BaselineRecords | Where-Object {
            (Get-ObjectProperty $_ "scene") -eq $scene
        })

        $baselinePaint = Get-MedianMetric $baselineSceneRecords "paint_frames_per_second"
        $currentPaint = Get-MedianMetric $currentSceneRecords "paint_frames_per_second"
        $baselineDraw = Get-MedianMetric $baselineSceneRecords "draw_frames_per_second"
        $currentDraw = Get-MedianMetric $currentSceneRecords "draw_frames_per_second"
        $baselineScene = Get-MedianMetric $baselineSceneRecords "scene_frames_per_second"
        $currentScene = Get-MedianMetric $currentSceneRecords "scene_frames_per_second"

        if ($null -eq $baselinePaint -or $null -eq $currentPaint -or
            $null -eq $baselineDraw -or $null -eq $currentDraw -or
            $null -eq $baselineScene -or $null -eq $currentScene)
        {
            $missingScenes.Add($scene)
        }

        $paintImprovement = if ($null -ne $baselinePaint -and $null -ne $currentPaint) {
            Get-ImprovementPercent $baselinePaint $currentPaint
        }
        else {
            $null
        }
        $drawImprovement = if ($null -ne $baselineDraw -and $null -ne $currentDraw) {
            Get-ImprovementPercent $baselineDraw $currentDraw
        }
        else {
            $null
        }
        $sceneImprovement = if ($null -ne $baselineScene -and $null -ne $currentScene) {
            Get-ImprovementPercent $baselineScene $currentScene
        }
        else {
            $null
        }

        $motivatingScene = $MotivatingSceneNames -contains $scene
        $motivatingPaintPass = !$motivatingScene -or (
            $null -ne $paintImprovement -and
            $paintImprovement -ge $MotivatingPaintImprovementThresholdPercent
        )
        $defaultPaintRegressionPass = $motivatingScene -or (
            $null -ne $paintImprovement -and
            $paintImprovement -ge $DefaultSceneRegressionThresholdPercent
        )
        $defaultDrawRegressionPass = $null -ne $drawImprovement -and
            $drawImprovement -ge $DefaultSceneRegressionThresholdPercent
        $defaultSceneRegressionPass = $null -ne $sceneImprovement -and
            $sceneImprovement -ge $DefaultSceneRegressionThresholdPercent
        $motivatingPaintThreshold = if ($motivatingScene) {
            $MotivatingPaintImprovementThresholdPercent
        }
        else {
            $null
        }
        $defaultSceneRegressionThreshold = $DefaultSceneRegressionThresholdPercent

        $comparisons.Add([ordered]@{
            scene = $scene
            motivating_scene = $motivatingScene
            baseline_median_paint_fps = $baselinePaint
            canonical_median_paint_fps = $currentPaint
            paint_improvement_percent = $paintImprovement
            baseline_median_draw_fps = $baselineDraw
            canonical_median_draw_fps = $currentDraw
            draw_improvement_percent = $drawImprovement
            baseline_median_scene_fps = $baselineScene
            canonical_median_scene_fps = $currentScene
            scene_improvement_percent = $sceneImprovement
            motivating_paint_improvement_threshold_percent = $motivatingPaintThreshold
            default_scene_regression_threshold_percent = $defaultSceneRegressionThreshold
            motivating_paint_pass = $motivatingPaintPass
            default_paint_regression_pass = $defaultPaintRegressionPass
            default_draw_regression_pass = $defaultDrawRegressionPass
            default_scene_regression_pass = $defaultSceneRegressionPass
        }) | Out-Null
    }

    $comparisonPass = $missingScenes.Count -eq 0 -and
        @($comparisons | Where-Object {
            $_.motivating_paint_pass -ne $true -or
            $_.default_paint_regression_pass -ne $true -or
            $_.default_draw_regression_pass -ne $true -or
            $_.default_scene_regression_pass -ne $true
        }).Count -eq 0

    return [ordered]@{
        enabled = $true
        archive_path = (Resolve-Path $ArchivePath).Path
        comparison_pass = $comparisonPass
        missing_scenes = @($missingScenes.ToArray())
        comparisons = @($comparisons.ToArray())
    }
}

function Configure-CmdgBuild {
    Invoke-VcvarsCommand "cmake" @(
        "-S", $TerminalRepo,
        "-B", $buildDir,
        "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_PREFIX_PATH=$QtRoot",
        "-DQT_NO_PRIVATE_MODULE_WARNING=ON",
        "-DVNM_TERMINAL_ENABLE_PROFILING=OFF",
        "-DVNM_TERMINAL_APP_BUILD_BENCHMARKS=ON",
        "-DVNM_TERMINAL_CMDG_ARTIFACT_TAG=$ArtifactTag",
        "-DVNM_TERMINAL_CMDG_SCENES=$SceneList",
        "-DVNM_TERMINAL_CMDG_REPEAT_COUNT=$RepeatCount",
        "-DVNM_TERMINAL_CMDG_NELOSTIE_FRAMES=$FrameLimit",
        "-DVNM_TERMINAL_CMDG_NELOSTIE_WINDOW_SIZE=$WindowSize",
        "-DVNM_TERMINAL_CMDG_NELOSTIE_FONT_SIZE=$FontSize",
        "-DVNM_TERMINAL_CMDG_NELOSTIE_OFFSCREEN=OFF",
        "-DVNM_TERMINAL_CMDG_NELOSTIE_HIDE_CURSOR=ON"
    ) $TerminalRepo (Join-Path $artifactRoot "configure_canonical_atlas.log") | Out-Null
}

function Build-Terminal {
    Invoke-VcvarsCommand "cmake" @(
        "--build", $buildDir,
        "--target", "vnm_terminal"
    ) $TerminalRepo (Join-Path $artifactRoot "build_canonical_atlas.log") | Out-Null
}

function Run-CmdgTests {
    $label = if ($FocusOnly) { "cmdg_regression_focus" } else { "cmdg_suite" }
    return Invoke-VcvarsCommand "ctest" @(
        "--test-dir", $buildDir,
        "-L", $label,
        "--output-on-failure"
    ) $TerminalRepo (Join-Path $artifactRoot "ctest_canonical_atlas.log") -AllowFailure
}

function Read-CmdgRecord {
    param(
        [string] $Scene,
        [int]    $Repeat
    )

    $runId = "${Scene}_r${Repeat}"
    $runDir = Join-Path $buildDir "benchmarks\cmdg_nelostie\$Scene\$ArtifactTag\repeat_$Repeat"
    $cmdgMetricsPath = Join-Path $runDir "vnm_terminal_cmdg_${runId}_cmdg_metrics.json"
    $terminalMetricsPath = Join-Path $runDir "vnm_terminal_cmdg_${runId}_terminal_metrics.json"
    $errors = New-Object System.Collections.Generic.List[string]

    $cmdgMetrics = $null
    if (Test-Path $cmdgMetricsPath) {
        try {
            $cmdgMetrics = Get-Content -Raw -Path $cmdgMetricsPath | ConvertFrom-Json
        }
        catch {
            $errors.Add("could not parse CMDG metrics JSON: $($_.Exception.Message)")
        }
    }
    else {
        $errors.Add("missing CMDG metrics JSON")
    }

    $terminalMetrics = $null
    if (Test-Path $terminalMetricsPath) {
        try {
            $terminalMetrics = Get-Content -Raw -Path $terminalMetricsPath | ConvertFrom-Json
        }
        catch {
            $errors.Add("could not parse terminal metrics JSON: $($_.Exception.Message)")
        }
    }
    else {
        $errors.Add("missing terminal metrics JSON")
    }

    $atlasMetrics = Get-ObjectProperty $terminalMetrics "qsg_atlas"
    $bufferUploadMetrics = Get-ObjectProperty $atlasMetrics "buffer_upload"
    $atlasRenderer = Get-ObjectProperty $atlasMetrics "renderer"
    $atlasFailedInserts = Convert-MetricNumber (
        Get-ObjectProperty $bufferUploadMetrics "atlas_failed_inserts"
    )
    $atlasBudgetBytes = Convert-MetricNumber (
        Get-ObjectProperty $bufferUploadMetrics "atlas_budget_bytes"
    )
    $atlasPageBudget = Convert-MetricNumber (
        Get-ObjectProperty $bufferUploadMetrics "atlas_page_budget"
    )
    $atlasPageCount = Convert-MetricNumber (
        Get-ObjectProperty $bufferUploadMetrics "atlas_page_count"
    )
    $captureCount = Convert-MetricNumber (
        Get-ObjectProperty $atlasMetrics "capture_count"
    )
    $renderCount = Convert-MetricNumber (
        Get-ObjectProperty $atlasMetrics "render_count"
    )

    $backendErrorCount = Convert-MetricNumber (
        Get-ObjectProperty $terminalMetrics "backend_error_count"
    )
    $timeoutExpired = Convert-MetricBool (Get-ObjectProperty $terminalMetrics "timeout_expired")
    $exitReason = Get-ObjectProperty $cmdgMetrics "exit_reason"
    $exitCode = Convert-MetricNumber (Get-ObjectProperty $cmdgMetrics "exit_code")

    $atlasMetricsPresent = $atlasRenderer -eq "atlas" -and
        $null -ne $captureCount -and
        $null -ne $renderCount -and
        $null -ne $atlasFailedInserts -and
        $null -ne $atlasBudgetBytes -and
        $atlasBudgetBytes -gt 0.0 -and
        $null -ne $atlasPageBudget -and
        $atlasPageBudget -gt 0.0 -and
        $null -ne $atlasPageCount

    $runOk = $errors.Count -eq 0 -and
        $backendErrorCount -eq 0.0 -and
        $timeoutExpired -eq $false -and
        $exitReason -eq "frame_limit" -and
        $exitCode -eq 0.0

    return [ordered]@{
        variant = "canonical_atlas"
        scene = $Scene
        repeat = $Repeat
        run_dir = $runDir
        cmdg_metrics_path = $cmdgMetricsPath
        terminal_metrics_path = $terminalMetricsPath
        paint_frames_per_second = Convert-MetricNumber (
            Get-ObjectProperty $terminalMetrics "paint_frames_per_second"
        )
        scene_frames_per_second = Convert-MetricNumber (
            Get-ObjectProperty $cmdgMetrics "scene_frames_per_second"
        )
        draw_frames_per_second = Convert-MetricNumber (
            Get-ObjectProperty $cmdgMetrics "draw_frames_per_second"
        )
        exit_reason = $exitReason
        exit_code = $exitCode
        backend_error_count = $backendErrorCount
        timeout_expired = $timeoutExpired
        atlas_renderer = $atlasRenderer
        atlas_capture_count = $captureCount
        atlas_render_count = $renderCount
        atlas_page_count = $atlasPageCount
        atlas_page_budget = $atlasPageBudget
        atlas_budget_bytes = $atlasBudgetBytes
        atlas_failed_inserts = $atlasFailedInserts
        atlas_metrics_present = $atlasMetricsPresent
        run_ok = $runOk
        errors = @($errors)
    }
}

function Read-Records {
    param([string[]] $Scenes)

    $records = New-Object System.Collections.Generic.List[object]
    foreach ($scene in $Scenes) {
        foreach ($repeat in 1..$RepeatCount) {
            [void] $records.Add((Read-CmdgRecord $scene $repeat))
        }
    }

    return $records.ToArray()
}

function Write-GateReport {
    param([object] $Summary)

    $jsonPath = Join-Path $artifactRoot "canonical_atlas_cmdg_gate.json"
    $markdownPath = Join-Path $artifactRoot "canonical_atlas_cmdg_gate.md"
    $Summary | ConvertTo-Json -Depth 8 |
        Set-Content -Encoding ASCII -Path $jsonPath

    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("# Canonical atlas CMDG gate")
    $lines.Add("")
    $lines.Add("Gate result: $($Summary.gate_result)")
    $lines.Add("")
    $lines.Add("Validation flow:")
    $lines.Add("- Build one Release app with the atlas renderer as the only runtime path.")
    $lines.Add("- Run the CMDG suite, or the focused Plasma/ParticleVortex label with -FocusOnly.")
    $lines.Add("- Require zero backend errors/timeouts and canonical qsg_atlas metrics with atlas budget counters.")
    if ($Summary.archived_baseline_comparison.enabled) {
        $lines.Add("- Compare canonical atlas metrics against the archived retired-renderer baseline.")
    }
    else {
        $lines.Add("- No archived retired-renderer baseline was supplied for this canonical-only run.")
    }
    $lines.Add("")
    $lines.Add("| Scene | Paint FPS | Draw FPS | Scene FPS | Atlas metrics | Failed inserts | Run OK |")
    $lines.Add("| --- | ---: | ---: | ---: | --- | ---: | --- |")
    foreach ($record in @($Summary.records)) {
        $lines.Add(
            "| $($record.scene) | " +
            "$($record.paint_frames_per_second) | " +
            "$($record.draw_frames_per_second) | " +
            "$($record.scene_frames_per_second) | " +
            "$($record.atlas_metrics_present) | " +
            "$($record.atlas_failed_inserts) | " +
            "$($record.run_ok) |"
        )
    }
    $lines.Add("")
    if ($Summary.archived_baseline_comparison.enabled) {
        $lines.Add("Archived retired-renderer baseline: $($Summary.archived_baseline_comparison.archive_path)")
        $lines.Add("")
        $lines.Add("| Scene | Baseline Paint FPS | Canonical Paint FPS | Paint Delta % | Draw Delta % | Scene Delta % | Gate |")
        $lines.Add("| --- | ---: | ---: | ---: | ---: | ---: | --- |")
        foreach ($comparison in @($Summary.archived_baseline_comparison.comparisons)) {
            $gateOk = $comparison.motivating_paint_pass -and
                $comparison.default_paint_regression_pass -and
                $comparison.default_draw_regression_pass -and
                $comparison.default_scene_regression_pass
            $lines.Add(
                "| $($comparison.scene) | " +
                "$($comparison.baseline_median_paint_fps) | " +
                "$($comparison.canonical_median_paint_fps) | " +
                "$($comparison.paint_improvement_percent) | " +
                "$($comparison.draw_improvement_percent) | " +
                "$($comparison.scene_improvement_percent) | " +
                "$gateOk |"
            )
        }
        $lines.Add("")
    }
    $lines.Add("Gate JSON: $jsonPath")
    $lines | Set-Content -Encoding ASCII -Path $markdownPath

    return [ordered]@{
        json = $jsonPath
        markdown = $markdownPath
    }
}

$scenes = Split-SceneNames $SceneList
$motivatingSceneNames = Split-SceneNames $MotivatingScenes
$missingMotivatingScenes = @($motivatingSceneNames | Where-Object { $scenes -notcontains $_ })
if ($missingMotivatingScenes.Count -gt 0) {
    throw "SceneList must include motivating scenes: $($missingMotivatingScenes -join ', ')"
}

Write-RepoBaseline -Path $TerminalRepo -Name "vnm_terminal_start"
Write-RepoBaseline -Path $SurfaceRepo -Name "vnm_terminal_surface_start"
Write-MachineSnapshot
Set-HardwareQsgEnvironment
@(
    "QSG_RENDER_LOOP=$env:QSG_RENDER_LOOP",
    "QSG_RHI_BACKEND=$env:QSG_RHI_BACKEND",
    "QSG_RENDER_TIMING=$env:QSG_RENDER_TIMING",
    "QSG_INFO=$env:QSG_INFO",
    "QT_LOGGING_RULES=$env:QT_LOGGING_RULES",
    "VNM_TERMINAL_ENABLE_PROFILING=OFF",
    "VNM_TERMINAL_CMDG_NELOSTIE_OFFSCREEN=OFF",
    "renderer=canonical_atlas"
) | Set-Content -Encoding ASCII -Path (Join-Path $artifactRoot "qsg_environment.txt")

Configure-CmdgBuild
Build-Terminal
$ctestExit = Run-CmdgTests
$records = Read-Records $scenes
$archivedBaselineComparison = [ordered]@{
    enabled = $false
    archive_path = ""
    comparison_pass = $true
    missing_scenes = @()
    comparisons = @()
}
if ($ArchivedBaselineComparisonJson.Trim() -ne "") {
    $baselineRecords = Read-ArchivedBaselineRecords $ArchivedBaselineComparisonJson
    $archivedBaselineComparison = Compare-ArchivedBaseline `
        -CurrentRecords $records `
        -BaselineRecords $baselineRecords `
        -Scenes $scenes `
        -MotivatingSceneNames $motivatingSceneNames `
        -ArchivePath $ArchivedBaselineComparisonJson
}

$backendErrorsTimeoutsZero = @($records | Where-Object { $_.run_ok -ne $true }).Count -eq 0
$atlasMetricsPresent = @($records | Where-Object {
    $_.atlas_metrics_present -ne $true
}).Count -eq 0
$atlasFailedInsertsZero = @($records | Where-Object {
    $_.atlas_failed_inserts -ne 0.0
}).Count -eq 0

$gatePass = $ctestExit -eq 0 -and
    $backendErrorsTimeoutsZero -and
    $atlasMetricsPresent -and
    $atlasFailedInsertsZero -and
    $archivedBaselineComparison.comparison_pass

$summary = [ordered]@{
    gate_result = if ($gatePass) { "PASS" } else { "FAIL" }
    artifact_root = $artifactRoot
    build_dir = $buildDir
    artifact_tag = $ArtifactTag
    settings = [ordered]@{
        scene_list = $SceneList
        motivating_scenes = $MotivatingScenes
        repeat_count = $RepeatCount
        frame_limit = $FrameLimit
        window_size = $WindowSize
        font_size = $FontSize
        archived_baseline_comparison_json = $ArchivedBaselineComparisonJson
        focus_only = [bool] $FocusOnly
        profiling = "OFF"
        offscreen = "OFF"
        renderer = "canonical_atlas"
        qsg_rhi_backend = $env:QSG_RHI_BACKEND
        qsg_render_loop = $env:QSG_RENDER_LOOP
    }
    ctest_exit_code = $ctestExit
    thresholds = [ordered]@{
        backend_errors_timeouts_zero = $backendErrorsTimeoutsZero
        atlas_budget_metrics_present = $atlasMetricsPresent
        atlas_failed_inserts_zero = $atlasFailedInsertsZero
        archived_baseline_comparison_pass = $archivedBaselineComparison.comparison_pass
        motivating_paint_improvement_threshold_percent =
            $MotivatingPaintImprovementThresholdPercent
        default_scene_regression_threshold_percent =
            $DefaultSceneRegressionThresholdPercent
    }
    records = @($records)
    archived_baseline_comparison = $archivedBaselineComparison
}

$reportPaths = Write-GateReport $summary
Write-RepoBaseline -Path $TerminalRepo -Name "vnm_terminal_end"
Write-RepoBaseline -Path $SurfaceRepo -Name "vnm_terminal_surface_end"

Write-Host "Canonical atlas CMDG gate artifacts written to $artifactRoot"
Write-Host "Gate JSON: $($reportPaths.json)"
Write-Host "Gate report: $($reportPaths.markdown)"
Write-Host "Gate result: $($summary.gate_result)"

if (!$gatePass) {
    exit 1
}
