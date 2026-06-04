param(
    [string] $TerminalRepo = (Resolve-Path "$PSScriptRoot\..\..").Path,
    [string] $SurfaceRepo = (Resolve-Path "$PSScriptRoot\..\..\..\vnm_terminal_surface").Path,
    [string] $ArtifactTag = "stage4_cmdg_gate_$(Get-Date -Format yyyyMMdd_HHmmss)",
    [string] $QtRoot = "C:\Qt\6.10.1\msvc2022_64",
    [string] $VcvarsAll = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat",
    [string] $SceneList = "AssemblyWinter2025;Example2D;Plasma;JuliaSetTest;ParticleVortex;Example3D",
    [string] $MotivatingScenes = "Plasma;ParticleVortex",
    [int] $RepeatCount = 1,
    [int] $FrameLimit = 300,
    [string] $WindowSize = "1920x1080",
    [string] $FontSize = "10",
    [switch] $FocusOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$comparisonArtifactRoot = Join-Path $TerminalRepo "artifacts\$ArtifactTag"
$baselineTag = "${ArtifactTag}_qsg_text_node"
$atlasTag = "${ArtifactTag}_atlas_probe"
$baselineBuild = Join-Path $TerminalRepo "build_${ArtifactTag}_terminal_qsg_text_node"
$atlasBuild = Join-Path $TerminalRepo "build_${ArtifactTag}_terminal_atlas_probe"

if ($FocusOnly) {
    $SceneList = $MotivatingScenes
}

New-Item -ItemType Directory -Force -Path $comparisonArtifactRoot | Out-Null

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

    $outPath = Join-Path $comparisonArtifactRoot "${Name}_git_baseline.txt"
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
        Set-Content -Encoding ASCII -Path (Join-Path $comparisonArtifactRoot "machine.json")
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

function Get-Median {
    param([double[]] $Values)

    if ($null -eq $Values) {
        return $null
    }

    $items = @($Values)
    if ($items.Count -eq 0) {
        return $null
    }

    $sorted = @($items | Sort-Object)
    $middle = [int]($sorted.Count / 2)
    if (($sorted.Count % 2) -eq 0) {
        return ($sorted[$middle - 1] + $sorted[$middle]) / 2.0
    }

    return $sorted[$middle]
}

function Get-MetricValues {
    param(
        [object[]] $Records,
        [string]   $PropertyName
    )

    return @(
        $Records |
            ForEach-Object {
                $value = Get-ObjectProperty $_ $PropertyName
                if ($null -ne $value) {
                    $value
                }
            }
    )
}

function Get-ImprovementPercent {
    param(
        [object] $BaselineMedian,
        [object] $AtlasMedian
    )

    if ($null -ne $BaselineMedian -and $BaselineMedian -gt 0.0 -and
        $null -ne $AtlasMedian)
    {
        return (($AtlasMedian - $BaselineMedian) / $BaselineMedian) * 100.0
    }

    return $null
}

function Format-FpsValue {
    param([object] $Value)

    if ($null -eq $Value) {
        return "n/a"
    }

    return "{0:N3}" -f $Value
}

function Format-PercentValue {
    param([object] $Value)

    if ($null -eq $Value) {
        return "n/a"
    }

    return "{0:N2}%" -f $Value
}

function Configure-CmdgBuild {
    param(
        [string] $BuildDir,
        [string] $RunTag,
        [bool]   $AtlasProbeEnabled
    )

    $atlasProbeValue = if ($AtlasProbeEnabled) { "ON" } else { "OFF" }
    Invoke-VcvarsCommand "cmake" @(
        "-S", $TerminalRepo,
        "-B", $BuildDir,
        "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_PREFIX_PATH=$QtRoot",
        "-DVNM_TERMINAL_ENABLE_PROFILING=OFF",
        "-DVNM_TERMINAL_APP_BUILD_BENCHMARKS=ON",
        "-DVNM_TERMINAL_CMDG_ARTIFACT_TAG=$RunTag",
        "-DVNM_TERMINAL_CMDG_SCENES=$SceneList",
        "-DVNM_TERMINAL_CMDG_REPEAT_COUNT=$RepeatCount",
        "-DVNM_TERMINAL_CMDG_NELOSTIE_FRAMES=$FrameLimit",
        "-DVNM_TERMINAL_CMDG_NELOSTIE_WINDOW_SIZE=$WindowSize",
        "-DVNM_TERMINAL_CMDG_NELOSTIE_FONT_SIZE=$FontSize",
        "-DVNM_TERMINAL_CMDG_NELOSTIE_OFFSCREEN=OFF",
        "-DVNM_TERMINAL_CMDG_NELOSTIE_SOFTWARE_RENDERER=OFF",
        "-DVNM_TERMINAL_CMDG_NELOSTIE_HIDE_CURSOR=ON",
        "-DVNM_TERMINAL_CMDG_NELOSTIE_QSG_ATLAS_STAGE1_PROBE=$atlasProbeValue"
    ) $TerminalRepo (Join-Path $comparisonArtifactRoot "configure_${RunTag}.log") | Out-Null
}

function Build-Terminal {
    param(
        [string] $BuildDir,
        [string] $RunTag
    )

    Invoke-VcvarsCommand "cmake" @(
        "--build", $BuildDir,
        "--target", "vnm_terminal"
    ) $TerminalRepo (Join-Path $comparisonArtifactRoot "build_${RunTag}.log") | Out-Null
}

function Run-CmdgTests {
    param(
        [string] $BuildDir,
        [string] $RunTag
    )

    $label = if ($FocusOnly) { "cmdg_regression_focus" } else { "cmdg_suite" }
    return Invoke-VcvarsCommand "ctest" @(
        "--test-dir", $BuildDir,
        "-L", $label,
        "--output-on-failure"
    ) $TerminalRepo (Join-Path $comparisonArtifactRoot "ctest_${RunTag}.log") -AllowFailure
}

function Read-CmdgRecord {
    param(
        [string] $Variant,
        [string] $BuildDir,
        [string] $RunTag,
        [string] $Scene,
        [int]    $Repeat
    )

    $runId = "${Scene}_r${Repeat}"
    $runDir = Join-Path $BuildDir "benchmarks\cmdg_nelostie\$Scene\$RunTag\repeat_$Repeat"
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

    $probeMetrics = Get-ObjectProperty $terminalMetrics "qsg_atlas_stage1_probe"
    $stage4Metrics = Get-ObjectProperty $probeMetrics "stage4"
    $atlasFailedInserts = Convert-MetricNumber (
        Get-ObjectProperty $stage4Metrics "atlas_failed_inserts"
    )
    $atlasBudgetBytes = Convert-MetricNumber (
        Get-ObjectProperty $stage4Metrics "atlas_budget_bytes"
    )
    $atlasPageBudget = Convert-MetricNumber (
        Get-ObjectProperty $stage4Metrics "atlas_page_budget"
    )
    $atlasPageCount = Convert-MetricNumber (
        Get-ObjectProperty $stage4Metrics "atlas_page_count"
    )

    $atlasEnabled = Convert-MetricBool (Get-ObjectProperty $probeMetrics "enabled")
    $backendErrorCount = Convert-MetricNumber (
        Get-ObjectProperty $terminalMetrics "backend_error_count"
    )
    $timeoutExpired = Convert-MetricBool (Get-ObjectProperty $terminalMetrics "timeout_expired")
    $exitReason = Get-ObjectProperty $cmdgMetrics "exit_reason"
    $exitCode = Convert-MetricNumber (Get-ObjectProperty $cmdgMetrics "exit_code")

    $atlasMetricsPresent = $atlasEnabled -eq $true -and
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
        variant = $Variant
        scene = $Scene
        repeat = $Repeat
        run_tag = $RunTag
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
        atlas_probe_enabled = $atlasEnabled
        atlas_page_count = $atlasPageCount
        atlas_page_budget = $atlasPageBudget
        atlas_budget_bytes = $atlasBudgetBytes
        atlas_failed_inserts = $atlasFailedInserts
        atlas_metrics_present = $atlasMetricsPresent
        run_ok = $runOk
        errors = @($errors)
    }
}

function Read-VariantRecords {
    param(
        [string]   $Variant,
        [string]   $BuildDir,
        [string]   $RunTag,
        [string[]] $Scenes
    )

    $records = New-Object System.Collections.Generic.List[object]
    foreach ($scene in $Scenes) {
        foreach ($repeat in 1..$RepeatCount) {
            [void] $records.Add((Read-CmdgRecord $Variant $BuildDir $RunTag $scene $repeat))
        }
    }

    return $records.ToArray()
}

function Compare-SceneRecords {
    param(
        [string[]] $Scenes,
        [string[]] $MotivatingSceneNames,
        [object[]] $BaselineRecords,
        [object[]] $AtlasRecords
    )

    $comparisons = New-Object System.Collections.Generic.List[object]
    foreach ($scene in $Scenes) {
        $baselineSceneRecords = @($BaselineRecords | Where-Object { $_.scene -eq $scene })
        $atlasSceneRecords = @($AtlasRecords | Where-Object { $_.scene -eq $scene })
        $baselinePaintMedian = Get-Median ([double[]] (
            Get-MetricValues $baselineSceneRecords "paint_frames_per_second"
        ))
        $atlasPaintMedian = Get-Median ([double[]] (
            Get-MetricValues $atlasSceneRecords "paint_frames_per_second"
        ))
        $paintImprovementPercent =
            Get-ImprovementPercent $baselinePaintMedian $atlasPaintMedian

        $baselineDrawMedian = Get-Median ([double[]] (
            Get-MetricValues $baselineSceneRecords "draw_frames_per_second"
        ))
        $atlasDrawMedian = Get-Median ([double[]] (
            Get-MetricValues $atlasSceneRecords "draw_frames_per_second"
        ))
        $drawImprovementPercent =
            Get-ImprovementPercent $baselineDrawMedian $atlasDrawMedian

        $baselineSceneMedian = Get-Median ([double[]] (
            Get-MetricValues $baselineSceneRecords "scene_frames_per_second"
        ))
        $atlasSceneMedian = Get-Median ([double[]] (
            Get-MetricValues $atlasSceneRecords "scene_frames_per_second"
        ))
        $sceneImprovementPercent =
            Get-ImprovementPercent $baselineSceneMedian $atlasSceneMedian

        $isMotivatingScene = $MotivatingSceneNames -contains $scene
        $paintRegressionPass = $null -ne $paintImprovementPercent -and
            $paintImprovementPercent -ge -5.0
        $drawRegressionPass = $null -ne $drawImprovementPercent -and
            $drawImprovementPercent -ge -5.0
        $sceneRegressionPass = $null -ne $sceneImprovementPercent -and
            $sceneImprovementPercent -ge -5.0
        $motivatingPaintPass = !$isMotivatingScene -or
            ($null -ne $paintImprovementPercent -and
                $paintImprovementPercent -ge 25.0)
        $motivatingPaintThreshold =
            if ($isMotivatingScene) { 25.0 } else { $null }

        [void] $comparisons.Add([ordered]@{
            scene = $scene
            motivating_scene = $isMotivatingScene
            baseline_median_paint_fps = $baselinePaintMedian
            atlas_median_paint_fps = $atlasPaintMedian
            paint_improvement_percent = $paintImprovementPercent
            baseline_median_draw_fps = $baselineDrawMedian
            atlas_median_draw_fps = $atlasDrawMedian
            draw_improvement_percent = $drawImprovementPercent
            baseline_median_scene_fps = $baselineSceneMedian
            atlas_median_scene_fps = $atlasSceneMedian
            scene_improvement_percent = $sceneImprovementPercent
            renderer_regression_threshold_percent = -5.0
            producer_scene_regression_threshold_percent = -5.0
            motivating_paint_improvement_threshold_percent = $motivatingPaintThreshold
            default_renderer_paint_regression_pass = $paintRegressionPass
            default_renderer_draw_regression_pass = $drawRegressionPass
            producer_scene_regression_pass = $sceneRegressionPass
            motivating_paint_pass = $motivatingPaintPass
        })
    }

    return $comparisons.ToArray()
}

function Write-GateReport {
    param(
        [object] $Summary
    )

    $jsonPath = Join-Path $comparisonArtifactRoot "stage4_cmdg_gate_comparison.json"
    $markdownPath = Join-Path $comparisonArtifactRoot "stage4_cmdg_gate_comparison.md"
    $Summary | ConvertTo-Json -Depth 8 |
        Set-Content -Encoding ASCII -Path $jsonPath

    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("# Stage 4 CMDG gate comparison")
    $lines.Add("")
    $lines.Add("Gate result: $($Summary.gate_result)")
    $lines.Add("")
    $lines.Add("Baseline tag: $($Summary.artifact_tags.baseline)")
    $lines.Add("Atlas tag: $($Summary.artifact_tags.atlas)")
    $lines.Add("CTest baseline exit: $($Summary.ctest_exit_codes.baseline)")
    $lines.Add("CTest atlas exit: $($Summary.ctest_exit_codes.atlas)")
    $lines.Add("")
    $lines.Add("Metric interpretation:")
    $lines.Add("- Primary renderer threshold: terminal paint_frames_per_second.")
    $lines.Add("- Corroborating/backpressure guard: CMDG draw_frames_per_second.")
    $lines.Add("- Producer/user-visible guard: CMDG scene_frames_per_second; CMDG caps this near 31 FPS.")
    $lines.Add("")
    $lines.Add(
        "| Scene | Paint base | Paint atlas | Paint delta | Draw base | " +
        "Draw atlas | Draw delta | Scene base | Scene atlas | Scene delta | " +
        "Paint guard | Draw guard | Scene guard | Motivating paint |"
    )
    $lines.Add("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- | --- | --- | --- |")
    foreach ($comparison in @($Summary.scene_comparisons)) {
        $lines.Add(
            "| $($comparison.scene) | " +
            "$(Format-FpsValue $comparison.baseline_median_paint_fps) | " +
            "$(Format-FpsValue $comparison.atlas_median_paint_fps) | " +
            "$(Format-PercentValue $comparison.paint_improvement_percent) | " +
            "$(Format-FpsValue $comparison.baseline_median_draw_fps) | " +
            "$(Format-FpsValue $comparison.atlas_median_draw_fps) | " +
            "$(Format-PercentValue $comparison.draw_improvement_percent) | " +
            "$(Format-FpsValue $comparison.baseline_median_scene_fps) | " +
            "$(Format-FpsValue $comparison.atlas_median_scene_fps) | " +
            "$(Format-PercentValue $comparison.scene_improvement_percent) | " +
            "$($comparison.default_renderer_paint_regression_pass) | " +
            "$($comparison.default_renderer_draw_regression_pass) | " +
            "$($comparison.producer_scene_regression_pass) | " +
            "$($comparison.motivating_paint_pass) |"
        )
    }
    $lines.Add("")
    $lines.Add("Atlas metrics present: $($Summary.thresholds.atlas_budget_metrics_present)")
    $lines.Add("Backend errors/timeouts zero: $($Summary.thresholds.backend_errors_timeouts_zero)")
    $lines.Add("Default scene paint regression within 5%: $($Summary.thresholds.default_scene_paint_regression_within_5_percent)")
    $lines.Add("Default scene draw regression within 5%: $($Summary.thresholds.default_scene_draw_regression_within_5_percent)")
    $lines.Add("Producer scene FPS regression within 5%: $($Summary.thresholds.producer_scene_regression_within_5_percent)")
    $lines.Add("Motivating scenes paint FPS at least 25% faster: $($Summary.thresholds.motivating_scenes_paint_25_percent_faster)")
    $lines.Add("")
    $lines.Add("Comparison JSON: $jsonPath")
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
    "VNM_TERMINAL_CMDG_NELOSTIE_SOFTWARE_RENDERER=OFF"
) | Set-Content -Encoding ASCII -Path (Join-Path $comparisonArtifactRoot "qsg_environment.txt")

Configure-CmdgBuild $baselineBuild $baselineTag $false
Build-Terminal $baselineBuild $baselineTag
Configure-CmdgBuild $atlasBuild $atlasTag $true
Build-Terminal $atlasBuild $atlasTag

$baselineCtestExit = Run-CmdgTests $baselineBuild $baselineTag
$atlasCtestExit = Run-CmdgTests $atlasBuild $atlasTag

$baselineRecords = Read-VariantRecords "qsg_text_node" $baselineBuild $baselineTag $scenes
$atlasRecords = Read-VariantRecords "atlas_probe" $atlasBuild $atlasTag $scenes
$sceneComparisons = Compare-SceneRecords $scenes $motivatingSceneNames $baselineRecords $atlasRecords

$allRecords = @($baselineRecords) + @($atlasRecords)
$backendErrorsTimeoutsZero = @($allRecords | Where-Object { $_.run_ok -ne $true }).Count -eq 0
$atlasMetricsPresent = @($atlasRecords | Where-Object {
    $_.atlas_metrics_present -ne $true
}).Count -eq 0
$baselineProbeDisabled = @($baselineRecords | Where-Object {
    $_.atlas_probe_enabled -eq $true
}).Count -eq 0
$defaultScenePaintRegressionPass = @($sceneComparisons | Where-Object {
    $_.default_renderer_paint_regression_pass -ne $true
}).Count -eq 0
$defaultSceneDrawRegressionPass = @($sceneComparisons | Where-Object {
    $_.default_renderer_draw_regression_pass -ne $true
}).Count -eq 0
$producerSceneRegressionPass = @($sceneComparisons | Where-Object {
    $_.producer_scene_regression_pass -ne $true
}).Count -eq 0
$motivatingScenePaintImprovementPass = @($sceneComparisons | Where-Object {
    $_.motivating_scene -eq $true -and $_.motivating_paint_pass -ne $true
}).Count -eq 0

$gatePass = $baselineCtestExit -eq 0 -and
    $atlasCtestExit -eq 0 -and
    $backendErrorsTimeoutsZero -and
    $atlasMetricsPresent -and
    $baselineProbeDisabled -and
    $defaultScenePaintRegressionPass -and
    $defaultSceneDrawRegressionPass -and
    $producerSceneRegressionPass -and
    $motivatingScenePaintImprovementPass

$summary = [ordered]@{
    gate_result = if ($gatePass) { "PASS" } else { "FAIL" }
    artifact_root = $comparisonArtifactRoot
    build_dirs = [ordered]@{
        baseline = $baselineBuild
        atlas = $atlasBuild
    }
    artifact_tags = [ordered]@{
        baseline = $baselineTag
        atlas = $atlasTag
    }
    settings = [ordered]@{
        scene_list = $SceneList
        motivating_scenes = $MotivatingScenes
        repeat_count = $RepeatCount
        frame_limit = $FrameLimit
        window_size = $WindowSize
        font_size = $FontSize
        focus_only = [bool] $FocusOnly
        profiling = "OFF"
        offscreen = "OFF"
        software_renderer = "OFF"
        qsg_rhi_backend = $env:QSG_RHI_BACKEND
        qsg_render_loop = $env:QSG_RENDER_LOOP
    }
    ctest_exit_codes = [ordered]@{
        baseline = $baselineCtestExit
        atlas = $atlasCtestExit
    }
    thresholds = [ordered]@{
        motivating_scenes_paint_25_percent_faster = $motivatingScenePaintImprovementPass
        default_scene_paint_regression_within_5_percent = $defaultScenePaintRegressionPass
        default_scene_draw_regression_within_5_percent = $defaultSceneDrawRegressionPass
        producer_scene_regression_within_5_percent = $producerSceneRegressionPass
        backend_errors_timeouts_zero = $backendErrorsTimeoutsZero
        atlas_budget_metrics_present = $atlasMetricsPresent
        baseline_probe_disabled = $baselineProbeDisabled
    }
    scene_comparisons = @($sceneComparisons)
    baseline_records = @($baselineRecords)
    atlas_records = @($atlasRecords)
}

$reportPaths = Write-GateReport $summary
Write-RepoBaseline -Path $TerminalRepo -Name "vnm_terminal_end"

Write-Host "Stage 4 CMDG gate artifacts written to $comparisonArtifactRoot"
Write-Host "Comparison JSON: $($reportPaths.json)"
Write-Host "Comparison report: $($reportPaths.markdown)"
Write-Host "Gate result: $($summary.gate_result)"
Write-RepoBaseline -Path $SurfaceRepo -Name "vnm_terminal_surface_end"

if (!$gatePass) {
    exit 1
}
