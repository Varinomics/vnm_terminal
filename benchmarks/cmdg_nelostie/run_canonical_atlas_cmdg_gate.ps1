param(
    [string] $TerminalRepo = (Resolve-Path "$PSScriptRoot\..\..").Path,
    [string] $SurfaceRepo = (Resolve-Path "$PSScriptRoot\..\..\..\vnm_terminal_surface").Path,
    [string] $ArtifactTag = "canonical_atlas_cmdg_gate_$(Get-Date -Format yyyyMMdd_HHmmss)",
    [string] $QtRoot = "C:\Qt\6.10.1\msvc2022_64",
    [string] $VcvarsAll = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat",
    [string] $SceneList = "AssemblyWinter2025;Example2D;Plasma;JuliaSetTest;ParticleVortex;Example3D",
    [string] $MotivatingScenes = "Plasma;ParticleVortex",
    [int] $RepeatCount = 3,
    [int] $FrameLimit = 1000,
    [int] $BenchmarkWindowMs = 5000,
    [int] $BenchmarkMinWindows = 3,
    [string] $WindowSize = "1920x1080",
    [string] $FontSize = "10",
    [string] $ArchivedBaselineComparisonJson = "",
    [Alias("MotivatingPaintImprovementThresholdPercent")]
    [double] $MotivatingTerminalFrameProxyImprovementThresholdPercent = 25.0,
    [double] $DefaultSceneRegressionThresholdPercent = -5.0,
    [double] $MinimumRendererFrameFps = 5.0,
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

function Get-RecordTerminalFrameEvidence {
    param([object] $Record)

    $presentation = Get-ObjectProperty $Record "presentation_frame_evidence"
    $presentationFps = Convert-MetricNumber (
        Get-ObjectProperty $presentation "frames_per_second"
    )
    if ($null -ne $presentationFps) {
        return [ordered]@{
            frames_per_second = $presentationFps
            source = "presentation_frame_evidence"
            counter_path = Get-ObjectProperty $presentation "counter_path"
            primary_counter_source = Get-ObjectProperty $presentation "primary_counter_source"
            primary_counter_semantics = Get-ObjectProperty `
                $presentation `
                "primary_counter_semantics"
            scanout_verified = Convert-MetricBool (
                Get-ObjectProperty $presentation "scanout_verified"
            )
        }
    }

    $runtimePresentation = Get-ObjectProperty $Record "presentation"
    $runtimePresentationFps = Convert-MetricNumber (
        Get-ObjectProperty $runtimePresentation "primary_frames_per_second"
    )
    if ($null -ne $runtimePresentationFps) {
        return [ordered]@{
            frames_per_second = $runtimePresentationFps
            source = "runtime_metrics.presentation"
            counter_path = Get-ObjectProperty $runtimePresentation "primary_counter_path"
            primary_counter_source = Get-ObjectProperty `
                $runtimePresentation `
                "primary_counter_source"
            primary_counter_semantics = Get-ObjectProperty `
                $runtimePresentation `
                "primary_counter_semantics"
            scanout_verified = Convert-MetricBool (
                Get-ObjectProperty $runtimePresentation "scanout_verified"
            )
        }
    }

    $frameEvidence = Get-ObjectProperty $Record "renderer_frame_evidence"
    $frameEvidenceFps = Convert-MetricNumber (
        Get-ObjectProperty $frameEvidence "frames_per_second"
    )
    if ($null -ne $frameEvidenceFps) {
        return [ordered]@{
            frames_per_second = $frameEvidenceFps
            source = "renderer_frame_evidence"
            counter_path = Get-ObjectProperty $frameEvidence "counter_path"
            primary_counter_source = $null
            primary_counter_semantics = "renderer_frame_counter"
            scanout_verified = $false
        }
    }

    return $null
}

function Get-MedianTerminalFrameFps {
    param([object[]] $Records)

    $values = @(
        $Records |
            ForEach-Object {
                $evidence = Get-RecordTerminalFrameEvidence $_
                Convert-MetricNumber (Get-ObjectProperty $evidence "frames_per_second")
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

        $baselineTerminalFrame = Get-MedianTerminalFrameFps $baselineSceneRecords
        $currentTerminalFrame = Get-MedianTerminalFrameFps $currentSceneRecords
        $baselineDraw = Get-MedianMetric $baselineSceneRecords "draw_frames_per_second"
        $currentDraw = Get-MedianMetric $currentSceneRecords "draw_frames_per_second"
        $baselineScene = Get-MedianMetric $baselineSceneRecords "scene_frames_per_second"
        $currentScene = Get-MedianMetric $currentSceneRecords "scene_frames_per_second"

        if ($null -eq $baselineTerminalFrame -or $null -eq $currentTerminalFrame -or
            $null -eq $baselineDraw -or $null -eq $currentDraw -or
            $null -eq $baselineScene -or $null -eq $currentScene)
        {
            $missingScenes.Add($scene)
        }

        $terminalFrameImprovement = if (
            $null -ne $baselineTerminalFrame -and
            $null -ne $currentTerminalFrame)
        {
            Get-ImprovementPercent $baselineTerminalFrame $currentTerminalFrame
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
        $motivatingTerminalFramePass = !$motivatingScene -or (
            $null -ne $terminalFrameImprovement -and
            $terminalFrameImprovement -ge
                $MotivatingTerminalFrameProxyImprovementThresholdPercent
        )
        $defaultTerminalFrameRegressionPass = $motivatingScene -or (
            $null -ne $terminalFrameImprovement -and
            $terminalFrameImprovement -ge $DefaultSceneRegressionThresholdPercent
        )
        $defaultDrawRegressionPass = $null -ne $drawImprovement -and
            $drawImprovement -ge $DefaultSceneRegressionThresholdPercent
        $defaultSceneRegressionPass = $null -ne $sceneImprovement -and
            $sceneImprovement -ge $DefaultSceneRegressionThresholdPercent
        $motivatingPaintThreshold = if ($motivatingScene) {
            $MotivatingTerminalFrameProxyImprovementThresholdPercent
        }
        else {
            $null
        }
        $defaultSceneRegressionThreshold = $DefaultSceneRegressionThresholdPercent

        $comparisons.Add([ordered]@{
            scene = $scene
            motivating_scene = $motivatingScene
            baseline_median_terminal_frame_fps = $baselineTerminalFrame
            canonical_median_terminal_frame_fps = $currentTerminalFrame
            terminal_frame_improvement_percent = $terminalFrameImprovement
            baseline_median_draw_fps = $baselineDraw
            canonical_median_draw_fps = $currentDraw
            draw_improvement_percent = $drawImprovement
            baseline_median_scene_fps = $baselineScene
            canonical_median_scene_fps = $currentScene
            scene_improvement_percent = $sceneImprovement
            motivating_terminal_frame_proxy_improvement_threshold_percent =
                $motivatingPaintThreshold
            default_scene_regression_threshold_percent = $defaultSceneRegressionThreshold
            motivating_terminal_frame_pass = $motivatingTerminalFramePass
            default_terminal_frame_regression_pass = $defaultTerminalFrameRegressionPass
            default_draw_regression_pass = $defaultDrawRegressionPass
            default_scene_regression_pass = $defaultSceneRegressionPass
        }) | Out-Null
    }

    $comparisonPass = $missingScenes.Count -eq 0 -and
        @($comparisons | Where-Object {
            $_.motivating_terminal_frame_pass -ne $true -or
            $_.default_terminal_frame_regression_pass -ne $true -or
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
        "-DVNM_TERMINAL_CMDG_BENCHMARK_WINDOW_MS=$BenchmarkWindowMs",
        "-DVNM_TERMINAL_CMDG_BENCHMARK_MIN_WINDOWS=$BenchmarkMinWindows",
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
    return Invoke-VcvarsCommand "ctest" @(
        "--test-dir", $buildDir,
        "-L", "cmdg_suite",
        "--output-on-failure"
    ) $TerminalRepo (Join-Path $artifactRoot "ctest_canonical_atlas.log") -AllowFailure
}

function Copy-RunMetricArtifact {
    param(
        [string] $SourcePath,
        [string] $Scene,
        [int]    $Repeat
    )

    if (!(Test-Path $SourcePath)) {
        return ""
    }

    $destinationDir = Join-Path $artifactRoot "per_run_metrics\$Scene\repeat_$Repeat"
    New-Item -ItemType Directory -Force -Path $destinationDir | Out-Null
    $destinationPath = Join-Path $destinationDir (Split-Path -Leaf $SourcePath)
    Copy-Item -LiteralPath $SourcePath -Destination $destinationPath -Force
    return $destinationPath
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
    $terminalTimelinePath = Join-Path $runDir "vnm_terminal_cmdg_${runId}_terminal_timeline.jsonl"
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

    if (!(Test-Path $terminalTimelinePath)) {
        $errors.Add("missing terminal metrics timeline JSONL")
    }

    $atlasMetrics = Get-ObjectProperty $terminalMetrics "qsg_atlas"
    $presentationMetrics = Get-ObjectProperty $terminalMetrics "presentation"
    $frameEvidenceMetrics = Get-ObjectProperty $terminalMetrics "renderer_frame_evidence"
    $startupMetrics = Get-ObjectProperty $terminalMetrics "startup"
    $bufferUploadMetrics = Get-ObjectProperty $atlasMetrics "buffer_upload"
    $producerMetrics = Get-ObjectProperty $atlasMetrics "producer"
    $warmLazyMetrics = Get-ObjectProperty $atlasMetrics "warm_lazy"
    $glyphBufferMetrics = Get-ObjectProperty $bufferUploadMetrics "glyph_buffer"
    $terminalElapsedMs = Convert-MetricNumber (
        Get-ObjectProperty $terminalMetrics "elapsed_ms"
    )
    $firstOutputElapsedMs = Convert-MetricNumber (
        Get-ObjectProperty $startupMetrics "first_output_elapsed_ms"
    )
    $visibleFirstFrameCompleted = Convert-MetricBool (
        Get-ObjectProperty $startupMetrics "visible_first_frame_completed"
    )
    $visibleFirstFrameCounterPath = Get-ObjectProperty `
        $startupMetrics `
        "visible_first_frame_counter_path"
    $paintFramesPerSecond = Convert-MetricNumber (
        Get-ObjectProperty $terminalMetrics "paint_frames_per_second"
    )
    $presentationCounterPath = Get-ObjectProperty `
        $presentationMetrics `
        "primary_counter_path"
    $presentationCounterSource = Get-ObjectProperty `
        $presentationMetrics `
        "primary_counter_source"
    $presentationCounterSemantics = Get-ObjectProperty `
        $presentationMetrics `
        "primary_counter_semantics"
    $presentationScanoutVerified = Convert-MetricBool (
        Get-ObjectProperty $presentationMetrics "scanout_verified"
    )
    $presentationFrameCount = Convert-MetricNumber (
        Get-ObjectProperty $presentationMetrics "primary_frame_count"
    )
    $presentationFramesPerSecond = Convert-MetricNumber (
        Get-ObjectProperty $presentationMetrics "primary_frames_per_second"
    )
    $frameSwappedMetrics = Get-ObjectProperty $presentationMetrics "frameSwapped"
    $frameSwappedCount = Convert-MetricNumber (
        Get-ObjectProperty $frameSwappedMetrics "count"
    )
    $frameEvidenceCounterPath = Get-ObjectProperty $frameEvidenceMetrics "counter_path"
    $frameEvidenceCount = Convert-MetricNumber (
        Get-ObjectProperty $frameEvidenceMetrics "frame_count"
    )
    $frameEvidenceFramesPerSecond = Convert-MetricNumber (
        Get-ObjectProperty $frameEvidenceMetrics "frames_per_second"
    )
    $atlasRenderer = Get-ObjectProperty $atlasMetrics "renderer"
    $atlasFailedInserts = Convert-MetricNumber (
        Get-ObjectProperty $bufferUploadMetrics "atlas_failed_inserts"
    )
    $atlasBudgetBytes = Convert-MetricNumber (
        Get-ObjectProperty $bufferUploadMetrics "atlas_budget_bytes"
    )
    $atlasAllocatedBytes = Convert-MetricNumber (
        Get-ObjectProperty $bufferUploadMetrics "atlas_allocated_bytes"
    )
    $atlasUsedBytes = Convert-MetricNumber (
        Get-ObjectProperty $bufferUploadMetrics "atlas_used_bytes"
    )
    $atlasPagePressure = Convert-MetricBool (
        Get-ObjectProperty $bufferUploadMetrics "atlas_page_pressure"
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
    $rasterizedGlyphs = Convert-MetricNumber (
        Get-ObjectProperty $atlasMetrics "rasterized_glyphs"
    )
    $rawFontRasterized = Convert-MetricBool (
        Get-ObjectProperty $atlasMetrics "raw_font_rasterized"
    )
    $glyphMissedInstances = Convert-MetricNumber (
        Get-ObjectProperty $atlasMetrics "glyph_missed_instances"
    )
    $glyphCoverageFailures = Convert-MetricNumber (
        Get-ObjectProperty $atlasMetrics "glyph_coverage_failures"
    )
    $glyphAtlasInsertFailures = Convert-MetricNumber (
        Get-ObjectProperty $atlasMetrics "glyph_atlas_insert_failures"
    )
    $glyphBufferUploadedBytes = Convert-MetricNumber (
        Get-ObjectProperty $glyphBufferMetrics "uploaded_bytes"
    )
    $glyphBufferPartialUploads = Convert-MetricNumber (
        Get-ObjectProperty $glyphBufferMetrics "partial_uploads"
    )
    $glyphBufferFullUploads = Convert-MetricNumber (
        Get-ObjectProperty $glyphBufferMetrics "full_uploads"
    )
    $producerTextRunsConsidered = Convert-MetricNumber (
        Get-ObjectProperty $producerMetrics "text_runs_considered"
    )
    $producerShapeCacheHits = Convert-MetricNumber (
        Get-ObjectProperty $producerMetrics "shape_cache_hits"
    )
    $producerShapeCacheMisses = Convert-MetricNumber (
        Get-ObjectProperty $producerMetrics "shape_cache_misses"
    )
    $producerShapeCachePruned = Convert-MetricNumber (
        Get-ObjectProperty $producerMetrics "shape_cache_pruned"
    )
    $producerShapedRunsBuilt = Convert-MetricNumber (
        Get-ObjectProperty $producerMetrics "shaped_runs_built"
    )
    $producerShapedRunsReused = Convert-MetricNumber (
        Get-ObjectProperty $producerMetrics "shaped_runs_reused"
    )
    $producerShapedGlyphsBuilt = Convert-MetricNumber (
        Get-ObjectProperty $producerMetrics "shaped_glyph_records_built"
    )
    $producerShapedGlyphsReused = Convert-MetricNumber (
        Get-ObjectProperty $producerMetrics "shaped_glyph_records_reused"
    )
    $warmCompleted = Convert-MetricBool (
        Get-ObjectProperty $warmLazyMetrics "warm_completed"
    )
    $warmEpoch = Convert-MetricNumber (
        Get-ObjectProperty $warmLazyMetrics "warm_epoch"
    )
    $warmSeedStrings = Convert-MetricNumber (
        Get-ObjectProperty $warmLazyMetrics "warm_seed_strings"
    )
    $warmShapedGlyphRecords = Convert-MetricNumber (
        Get-ObjectProperty $warmLazyMetrics "warm_shaped_glyph_records"
    )
    $warmCoveredGlyphRecords = Convert-MetricNumber (
        Get-ObjectProperty $warmLazyMetrics "warm_covered_glyph_records"
    )
    $warmSkippedGlyphRecords = Convert-MetricNumber (
        Get-ObjectProperty $warmLazyMetrics "warm_skipped_glyph_records"
    )
    $warmEnvironmentSkippedGlyphRecords = Convert-MetricNumber (
        Get-ObjectProperty $warmLazyMetrics "warm_environment_skipped_glyph_records"
    )
    $warmFailedGlyphRecords = Convert-MetricNumber (
        Get-ObjectProperty $warmLazyMetrics "warm_failed_glyph_records"
    )
    $warmMissingStringIndexes = Convert-MetricNumber (
        Get-ObjectProperty $warmLazyMetrics "warm_missing_string_indexes"
    )
    $warmInvalidStringIndexes = Convert-MetricNumber (
        Get-ObjectProperty $warmLazyMetrics "warm_invalid_string_indexes"
    )
    $warmUnsupportedImages = Convert-MetricNumber (
        Get-ObjectProperty $warmLazyMetrics "warm_unsupported_images"
    )
    $warmCacheHits = Convert-MetricNumber (
        Get-ObjectProperty $warmLazyMetrics "warm_cache_hits"
    )
    $warmInsertAttempts = Convert-MetricNumber (
        Get-ObjectProperty $warmLazyMetrics "warm_insert_attempts"
    )
    $warmInserts = Convert-MetricNumber (
        Get-ObjectProperty $warmLazyMetrics "warm_inserts"
    )
    $warmFailedInserts = Convert-MetricNumber (
        Get-ObjectProperty $warmLazyMetrics "warm_failed_inserts"
    )
    $warmElapsedMs = Convert-MetricNumber (
        Get-ObjectProperty $warmLazyMetrics "warm_elapsed_ms"
    )
    $warmPagePressure = Convert-MetricBool (
        Get-ObjectProperty $warmLazyMetrics "warm_page_pressure"
    )
    $lazyInsertAttempts = Convert-MetricNumber (
        Get-ObjectProperty $warmLazyMetrics "lazy_insert_attempts"
    )
    $lazyInserts = Convert-MetricNumber (
        Get-ObjectProperty $warmLazyMetrics "lazy_inserts"
    )
    $lazyFailedInserts = Convert-MetricNumber (
        Get-ObjectProperty $warmLazyMetrics "lazy_failed_inserts"
    )
    $lazyElapsedMs = Convert-MetricNumber (
        Get-ObjectProperty $warmLazyMetrics "lazy_elapsed_ms"
    )
    $lazyMaxInsertUs = Convert-MetricNumber (
        Get-ObjectProperty $warmLazyMetrics "lazy_max_insert_us"
    )
    $lazyFrames = Convert-MetricNumber (
        Get-ObjectProperty $warmLazyMetrics "lazy_frames"
    )
    $incompleteFrames = Convert-MetricNumber (
        Get-ObjectProperty $warmLazyMetrics "incomplete_frames"
    )
    $rendererFrameTimeMs = if (
        $null -ne $frameEvidenceFramesPerSecond -and
        $frameEvidenceFramesPerSecond -gt 0.0)
    {
        1000.0 / $frameEvidenceFramesPerSecond
    }
    else {
        $null
    }
    $presentationFrameTimeMs = if (
        $null -ne $presentationFramesPerSecond -and
        $presentationFramesPerSecond -gt 0.0)
    {
        1000.0 / $presentationFramesPerSecond
    }
    else {
        $null
    }
    $terminalMetricsArtifactPath = Copy-RunMetricArtifact `
        -SourcePath $terminalMetricsPath `
        -Scene $Scene `
        -Repeat $Repeat
    $cmdgMetricsArtifactPath = Copy-RunMetricArtifact `
        -SourcePath $cmdgMetricsPath `
        -Scene $Scene `
        -Repeat $Repeat
    $terminalTimelineArtifactPath = Copy-RunMetricArtifact `
        -SourcePath $terminalTimelinePath `
        -Scene $Scene `
        -Repeat $Repeat

    $frameEvidenceCounterMatches =
        $frameEvidenceCounterPath -eq "qsg_atlas.render_count" -and
        $null -ne $renderCount -and
        $frameEvidenceCount -eq $renderCount
    $presentationCounterMatches =
        $presentationCounterPath -eq "presentation.frameSwapped.count" -and
        $presentationCounterSource -eq "QQuickWindow::frameSwapped" -and
        $presentationCounterSemantics -eq "qt_frame_swapped_proxy" -and
        $presentationScanoutVerified -eq $false -and
        $null -ne $frameSwappedCount -and
        $presentationFrameCount -eq $frameSwappedCount

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
        $null -ne $atlasPageCount -and
        $null -ne $producerTextRunsConsidered -and
        $null -ne $producerShapedRunsBuilt -and
        $null -ne $producerShapedRunsReused -and
        $null -ne $warmCompleted -and
        $null -ne $warmEpoch -and
        $null -ne $warmSeedStrings -and
        $warmSeedStrings -gt 0.0 -and
        $null -ne $warmShapedGlyphRecords -and
        $warmShapedGlyphRecords -gt 0.0 -and
        $null -ne $warmCoveredGlyphRecords -and
        $warmCoveredGlyphRecords -gt 0.0 -and
        $null -ne $warmSkippedGlyphRecords -and
        $null -ne $warmEnvironmentSkippedGlyphRecords -and
        $null -ne $warmFailedGlyphRecords -and
        $null -ne $warmMissingStringIndexes -and
        $null -ne $warmInvalidStringIndexes -and
        $null -ne $warmUnsupportedImages -and
        $null -ne $warmCacheHits -and
        $null -ne $warmInsertAttempts -and
        $null -ne $warmInserts -and
        $null -ne $warmFailedInserts -and
        $null -ne $warmElapsedMs -and
        $null -ne $warmPagePressure -and
        $null -ne $lazyInsertAttempts -and
        $null -ne $lazyInserts -and
        $null -ne $lazyFailedInserts -and
        $null -ne $lazyElapsedMs -and
        $null -ne $lazyMaxInsertUs -and
        $null -ne $lazyFrames -and
        $null -ne $incompleteFrames
    $glyphMissCountersZero =
        $glyphMissedInstances -eq 0.0 -and
        $glyphCoverageFailures -eq 0.0 -and
        $glyphAtlasInsertFailures -eq 0.0
    $warmLazyCountersOk =
        $warmCompleted -eq $true -and
        $warmFailedGlyphRecords -eq 0.0 -and
        $warmMissingStringIndexes -eq 0.0 -and
        $warmInvalidStringIndexes -eq 0.0 -and
        $warmUnsupportedImages -eq 0.0 -and
        $warmFailedInserts -eq 0.0 -and
        $lazyFailedInserts -eq 0.0 -and
        $incompleteFrames -eq 0.0

    $frameEvidencePresent = $null -ne $frameEvidenceCounterPath -and
        $null -ne $frameEvidenceCount -and
        $frameEvidenceCount -gt 0.0 -and
        $null -ne $frameEvidenceFramesPerSecond -and
        $frameEvidenceFramesPerSecond -gt 0.0 -and
        $frameEvidenceCounterMatches
    $rendererFrameFloorPass = $null -ne $frameEvidenceFramesPerSecond -and
        $frameEvidenceFramesPerSecond -ge $MinimumRendererFrameFps
    $presentationFrameEvidencePresent = $null -ne $presentationCounterPath -and
        $null -ne $presentationFrameCount -and
        $presentationFrameCount -gt 0.0 -and
        $null -ne $presentationFramesPerSecond -and
        $presentationFramesPerSecond -gt 0.0 -and
        $presentationCounterMatches
    $presentationFrameFloorPass = $null -ne $presentationFramesPerSecond -and
        $presentationFramesPerSecond -ge $MinimumRendererFrameFps

    $runOk = $errors.Count -eq 0 -and
        $backendErrorCount -eq 0.0 -and
        $timeoutExpired -eq $false -and
        $exitReason -eq "frame_limit" -and
        $exitCode -eq 0.0 -and
        $presentationFrameEvidencePresent -and
        $presentationFrameFloorPass -and
        $glyphMissCountersZero -and
        $warmLazyCountersOk

    return [ordered]@{
        variant = "canonical_atlas"
        scene = $Scene
        repeat = $Repeat
        run_dir = $runDir
        cmdg_metrics_path = $cmdgMetricsPath
        terminal_metrics_path = $terminalMetricsPath
        terminal_timeline_path = $terminalTimelinePath
        cmdg_metrics_artifact_path = $cmdgMetricsArtifactPath
        terminal_metrics_artifact_path = $terminalMetricsArtifactPath
        terminal_timeline_artifact_path = $terminalTimelineArtifactPath
        startup_latency_ms = $firstOutputElapsedMs
        app_elapsed_ms = $terminalElapsedMs
        presentation_frame_evidence = [ordered]@{
            counter_path = $presentationCounterPath
            primary_counter_source = $presentationCounterSource
            primary_counter_semantics = $presentationCounterSemantics
            scanout_verified = $presentationScanoutVerified
            frame_count = $presentationFrameCount
            frames_per_second = $presentationFramesPerSecond
            frame_time_ms = $presentationFrameTimeMs
        }
        terminal_frame_evidence = [ordered]@{
            source = "presentation_frame_evidence"
            counter_path = $presentationCounterPath
            primary_counter_source = $presentationCounterSource
            primary_counter_semantics = $presentationCounterSemantics
            scanout_verified = $presentationScanoutVerified
            frame_count = $presentationFrameCount
            frames_per_second = $presentationFramesPerSecond
            frame_time_ms = $presentationFrameTimeMs
        }
        presentation_frame_evidence_present = $presentationFrameEvidencePresent
        presentation_frame_floor_pass = $presentationFrameFloorPass
        minimum_presentation_frame_fps = $MinimumRendererFrameFps
        paint_frames_per_second = $paintFramesPerSecond
        renderer_frame_evidence = [ordered]@{
            counter_path = $frameEvidenceCounterPath
            frame_count = $frameEvidenceCount
            frames_per_second = $frameEvidenceFramesPerSecond
            frame_time_ms = $rendererFrameTimeMs
        }
        renderer_frame_evidence_present = $frameEvidencePresent
        renderer_frame_floor_pass = $rendererFrameFloorPass
        minimum_renderer_frame_fps = $MinimumRendererFrameFps
        frame_completion = [ordered]@{
            atlas_capture_count = $captureCount
            atlas_render_count = $renderCount
            presentation_frame_evidence_present = $presentationFrameEvidencePresent
            renderer_frame_evidence_present = $frameEvidencePresent
            visible_first_frame_completed = $visibleFirstFrameCompleted
            visible_first_frame_counter_path = $visibleFirstFrameCounterPath
        }
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
        atlas_memory = [ordered]@{
            page_count = $atlasPageCount
            page_budget = $atlasPageBudget
            allocated_bytes = $atlasAllocatedBytes
            budget_bytes = $atlasBudgetBytes
            used_bytes = $atlasUsedBytes
            page_pressure = $atlasPagePressure
        }
        glyph_misses = [ordered]@{
            glyph_missed_instances = $glyphMissedInstances
            glyph_coverage_failures = $glyphCoverageFailures
            glyph_atlas_insert_failures = $glyphAtlasInsertFailures
        }
        cold_glyph_insertion_frame_impact = [ordered]@{
            rasterized_glyphs = $rasterizedGlyphs
            raw_font_rasterized = $rawFontRasterized
            glyph_buffer_uploaded_bytes = $glyphBufferUploadedBytes
            glyph_buffer_partial_uploads = $glyphBufferPartialUploads
            glyph_buffer_full_uploads = $glyphBufferFullUploads
        }
        producer = [ordered]@{
            text_runs_considered = $producerTextRunsConsidered
            shape_cache_hits = $producerShapeCacheHits
            shape_cache_misses = $producerShapeCacheMisses
            shape_cache_pruned = $producerShapeCachePruned
            shaped_runs_built = $producerShapedRunsBuilt
            shaped_runs_reused = $producerShapedRunsReused
            shaped_glyph_records_built = $producerShapedGlyphsBuilt
            shaped_glyph_records_reused = $producerShapedGlyphsReused
        }
        warm_lazy = [ordered]@{
            warm_completed = $warmCompleted
            warm_epoch = $warmEpoch
            warm_seed_strings = $warmSeedStrings
            warm_shaped_glyph_records = $warmShapedGlyphRecords
            warm_covered_glyph_records = $warmCoveredGlyphRecords
            warm_skipped_glyph_records = $warmSkippedGlyphRecords
            warm_environment_skipped_glyph_records = $warmEnvironmentSkippedGlyphRecords
            warm_failed_glyph_records = $warmFailedGlyphRecords
            warm_missing_string_indexes = $warmMissingStringIndexes
            warm_invalid_string_indexes = $warmInvalidStringIndexes
            warm_unsupported_images = $warmUnsupportedImages
            warm_cache_hits = $warmCacheHits
            warm_insert_attempts = $warmInsertAttempts
            warm_inserts = $warmInserts
            warm_failed_inserts = $warmFailedInserts
            warm_elapsed_ms = $warmElapsedMs
            warm_page_pressure = $warmPagePressure
            lazy_insert_attempts = $lazyInsertAttempts
            lazy_inserts = $lazyInserts
            lazy_failed_inserts = $lazyFailedInserts
            lazy_elapsed_ms = $lazyElapsedMs
            lazy_max_insert_us = $lazyMaxInsertUs
            lazy_frames = $lazyFrames
            incomplete_frames = $incompleteFrames
        }
        atlas_failed_inserts = $atlasFailedInserts
        atlas_metrics_present = $atlasMetricsPresent
        glyph_miss_counters_zero = $glyphMissCountersZero
        warm_lazy_counters_ok = $warmLazyCountersOk
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
    $lines.Add("- Run the CMDG suite, or the focused Plasma/ParticleVortex scene list with -FocusOnly.")
    $lines.Add(
        "- Require zero backend errors/timeouts, positive Qt frameSwapped proxy evidence, " +
        "the minimum terminal frame proxy FPS floor, canonical qsg_atlas metrics with atlas " +
        "budget counters, and complete warm/lazy atlas diagnostics.")
    $lines.Add(
        "- Require at least $($Summary.settings.benchmark_min_windows) comparable " +
        "$($Summary.settings.benchmark_window_ms) ms CMDG windows and terminal " +
        "timeline samples for phase diagnostics.")
    if ($Summary.archived_baseline_comparison.enabled) {
        $lines.Add("- Compare canonical atlas metrics against the archived retired-renderer baseline.")
    }
    else {
        $lines.Add("- No archived retired-renderer baseline was supplied for this canonical-only run.")
    }
    $lines.Add(
        "- Per-run terminal/CMDG aggregate JSONs and terminal timeline JSONLs " +
        "are copied under `per_run_metrics/`.")
    $lines.Add("")
    $lines.Add("| Scene | Terminal Frame Proxy FPS | Frame Time ms | Frame Counter | Shaped Built | Shaped Reused | Draw FPS | Scene FPS | Atlas Used Bytes | Page Pressure | Warm Complete | Warm Failed | Lazy Failed | Incomplete Frames | Glyph Misses | First Output ms | Run OK |")
    $lines.Add("| --- | ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: | --- | --- | ---: | ---: | ---: | ---: | ---: | --- |")
    foreach ($record in @($Summary.records)) {
        $frameEvidence = Get-ObjectProperty $record "terminal_frame_evidence"
        $atlasMemory = Get-ObjectProperty $record "atlas_memory"
        $glyphMisses = Get-ObjectProperty $record "glyph_misses"
        $producer = Get-ObjectProperty $record "producer"
        $warmLazy = Get-ObjectProperty $record "warm_lazy"
        $lines.Add(
            "| $($record.scene) | " +
            "$(Get-ObjectProperty $frameEvidence 'frames_per_second') | " +
            "$(Get-ObjectProperty $frameEvidence 'frame_time_ms') | " +
            "$(Get-ObjectProperty $frameEvidence 'counter_path') | " +
            "$(Get-ObjectProperty $producer 'shaped_glyph_records_built') | " +
            "$(Get-ObjectProperty $producer 'shaped_glyph_records_reused') | " +
            "$($record.draw_frames_per_second) | " +
            "$($record.scene_frames_per_second) | " +
            "$(Get-ObjectProperty $atlasMemory 'used_bytes') | " +
            "$(Get-ObjectProperty $atlasMemory 'page_pressure') | " +
            "$(Get-ObjectProperty $warmLazy 'warm_completed') | " +
            "$(Get-ObjectProperty $warmLazy 'warm_failed_inserts') | " +
            "$(Get-ObjectProperty $warmLazy 'lazy_failed_inserts') | " +
            "$(Get-ObjectProperty $warmLazy 'incomplete_frames') | " +
            "$(Get-ObjectProperty $glyphMisses 'glyph_missed_instances') | " +
            "$($record.startup_latency_ms) | " +
            "$($record.run_ok) |"
        )
    }
    $lines.Add("")
    if ($Summary.archived_baseline_comparison.enabled) {
        $lines.Add("Archived retired-renderer baseline: $($Summary.archived_baseline_comparison.archive_path)")
        $lines.Add("")
        $lines.Add("| Scene | Baseline Terminal Frame Proxy FPS | Canonical Terminal Frame Proxy FPS | Frame Proxy Delta % | Draw Delta % | Scene Delta % | Gate |")
        $lines.Add("| --- | ---: | ---: | ---: | ---: | ---: | --- |")
        foreach ($comparison in @($Summary.archived_baseline_comparison.comparisons)) {
            $gateOk = $comparison.motivating_terminal_frame_pass -and
                $comparison.default_terminal_frame_regression_pass -and
                $comparison.default_draw_regression_pass -and
                $comparison.default_scene_regression_pass
            $lines.Add(
                "| $($comparison.scene) | " +
                "$($comparison.baseline_median_terminal_frame_fps) | " +
                "$($comparison.canonical_median_terminal_frame_fps) | " +
                "$($comparison.terminal_frame_improvement_percent) | " +
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

$backendErrorsTimeoutsZero = @($records | Where-Object {
    $_.errors.Count -ne 0 -or
    $_.backend_error_count -ne 0.0 -or
    $_.timeout_expired -ne $false -or
    $_.exit_reason -ne "frame_limit" -or
    $_.exit_code -ne 0.0
}).Count -eq 0
$atlasMetricsPresent = @($records | Where-Object {
    $_.atlas_metrics_present -ne $true
}).Count -eq 0
$presentationFrameEvidencePresent = @($records | Where-Object {
    $_.presentation_frame_evidence_present -ne $true
}).Count -eq 0
$presentationFrameFloorPass = @($records | Where-Object {
    $_.presentation_frame_floor_pass -ne $true
}).Count -eq 0
$atlasFailedInsertsZero = @($records | Where-Object {
    $_.atlas_failed_inserts -ne 0.0
}).Count -eq 0
$glyphMissCountersZero = @($records | Where-Object {
    $_.glyph_miss_counters_zero -ne $true
}).Count -eq 0
$warmLazyCountersOk = @($records | Where-Object {
    $_.warm_lazy_counters_ok -ne $true
}).Count -eq 0

$gatePass = $ctestExit -eq 0 -and
    $backendErrorsTimeoutsZero -and
    $presentationFrameEvidencePresent -and
    $presentationFrameFloorPass -and
    $atlasMetricsPresent -and
    $atlasFailedInsertsZero -and
    $glyphMissCountersZero -and
    $warmLazyCountersOk -and
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
        benchmark_window_ms = $BenchmarkWindowMs
        benchmark_min_windows = $BenchmarkMinWindows
        window_size = $WindowSize
        font_size = $FontSize
        minimum_presentation_frame_fps = $MinimumRendererFrameFps
        minimum_renderer_frame_fps = $MinimumRendererFrameFps
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
        presentation_frame_evidence_present = $presentationFrameEvidencePresent
        presentation_frame_floor_pass = $presentationFrameFloorPass
        minimum_presentation_frame_fps = $MinimumRendererFrameFps
        minimum_renderer_frame_fps = $MinimumRendererFrameFps
        atlas_budget_metrics_present = $atlasMetricsPresent
        atlas_failed_inserts_zero = $atlasFailedInsertsZero
        glyph_miss_counters_zero = $glyphMissCountersZero
        warm_lazy_counters_ok = $warmLazyCountersOk
        archived_baseline_comparison_pass = $archivedBaselineComparison.comparison_pass
        motivating_terminal_frame_proxy_improvement_threshold_percent =
            $MotivatingTerminalFrameProxyImprovementThresholdPercent
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
Write-Host "Per-run terminal timeline JSONLs are copied under per_run_metrics."
Write-Host "Gate result: $($summary.gate_result)"

if (!$gatePass) {
    exit 1
}
