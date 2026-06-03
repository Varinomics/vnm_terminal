param(
    [string] $TerminalRepo = (Resolve-Path "$PSScriptRoot\..\..").Path,
    [string] $SurfaceRepo = (Resolve-Path "$PSScriptRoot\..\..\..\vnm_terminal_surface").Path,
    [string] $ArtifactTag = "bound_confirmation_20260603_final",
    [string] $QtRoot = "C:\Qt\6.10.1\msvc2022_64",
    [string] $VcvarsAll = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$sceneList = "AssemblyWinter2025;Example2D;Plasma;JuliaSetTest;ParticleVortex;Example3D"
$scalingScenarios = @(
    "dense_repaint",
    "ascii_full_dirty_reuse_only",
    "mixed_text_full_dirty_reuse_only"
)

$artifactRoot = Join-Path $TerminalRepo "artifacts\$ArtifactTag"
$terminalReleaseBuild = Join-Path $TerminalRepo "build_${ArtifactTag}_terminal_release"
$terminalProfileBuild = Join-Path $TerminalRepo "build_${ArtifactTag}_terminal_profile"
$surfaceReleaseBuild = Join-Path $SurfaceRepo "build_${ArtifactTag}_surface_release"
$surfaceProfileBuild = Join-Path $SurfaceRepo "build_${ArtifactTag}_surface_profile"

New-Item -ItemType Directory -Force -Path $artifactRoot | Out-Null

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
        [string]   $LogPath
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
        & $FilePath @Arguments 2>&1 | Tee-Object -FilePath $LogPath -Append
        $exitCode = $LASTEXITCODE
        $ErrorActionPreference = $previousErrorActionPreference
    }
    finally {
        if (Get-Variable -Name previousErrorActionPreference -Scope Local -ErrorAction SilentlyContinue) {
            $ErrorActionPreference = $previousErrorActionPreference
        }
        Pop-Location
    }

    if ($exitCode -ne 0) {
        throw "Command failed with exit code ${exitCode}: $commandText"
    }
}

function Invoke-VcvarsCommand {
    param(
        [string]   $FilePath,
        [string[]] $Arguments,
        [string]   $WorkingDirectory,
        [string]   $LogPath
    )

    $innerCommand = Join-CommandLine $FilePath $Arguments
    Invoke-LoggedCommand `
        -FilePath "cmd.exe" `
        -Arguments @("/a", "/s", "/c", "call `"$VcvarsAll`" x64 && $innerCommand") `
        -WorkingDirectory $WorkingDirectory `
        -LogPath $LogPath
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

function Read-CMakeCacheValue {
    param(
        [string] $BuildDir,
        [string] $Name
    )

    $cachePath = Join-Path $BuildDir "CMakeCache.txt"
    $match = Select-String -Path $cachePath -Pattern "^${Name}(:[^=]+)?=(.*)$" | Select-Object -First 1
    if ($null -eq $match) {
        return $null
    }

    return $match.Matches[0].Groups[2].Value
}

function Get-QtBinDir {
    param([string] $BuildDir)

    $qt6Dir = Read-CMakeCacheValue -BuildDir $BuildDir -Name "Qt6_DIR"
    if ([string]::IsNullOrWhiteSpace($qt6Dir)) {
        return $null
    }

    $qtRoot = Split-Path (Split-Path (Split-Path $qt6Dir -Parent) -Parent) -Parent
    return Join-Path $qtRoot "bin"
}

function Get-SampleSummary {
    param([Int64[]] $Values)

    if ($Values.Count -eq 0) {
        return [ordered]@{
            sample_count = 0
            median = $null
            p95 = $null
        }
    }

    $sorted = @($Values | Sort-Object)
    $middle = [int]($sorted.Count / 2)
    if (($sorted.Count % 2) -eq 0) {
        $median = ($sorted[$middle - 1] + $sorted[$middle]) / 2.0
    }
    else {
        $median = $sorted[$middle]
    }

    $p95Index = [Math]::Min([Math]::Ceiling($sorted.Count * 0.95) - 1, $sorted.Count - 1)
    return [ordered]@{
        sample_count = $sorted.Count
        median = $median
        p95 = $sorted[$p95Index]
    }
}

function Export-SurfaceAttemptSamples {
    param(
        [string] $RunName,
        [string] $JsonPath,
        [string] $RawPath
    )

    $document = Get-Content -Raw -Path $JsonPath | ConvertFrom-Json
    $records = New-Object System.Collections.Generic.List[object]
    foreach ($scenario in @($document.scenarios)) {
        foreach ($attempt in @($scenario.attempts)) {
            $records.Add([ordered]@{
                run = $RunName
                window = "$($document.window_size.width)x$($document.window_size.height)"
                grid = "$($document.rows)x$($document.columns)"
                scenario = [string] $attempt.scenario
                attempt_index = [int] $attempt.attempt_index
                status = [string] $attempt.status
                elapsed_ns = [Int64] $attempt.elapsed_ns
                scene_graph_update_latency_ns = [Int64] $attempt.scene_graph_update_latency_ns
                scene_graph_render_wait_ns = [Int64] $attempt.scene_graph_render_wait_ns
                readback_ns = [Int64] $attempt.readback_ns
                completed_count = [int] $attempt.completed_count
                render_consumed_count = [Int64] $attempt.render_consumed_count
            })
        }
    }

    $records | ForEach-Object {
        $_ | ConvertTo-Json -Compress -Depth 4
    } | Set-Content -Encoding ASCII -Path $RawPath

    $summaries = New-Object System.Collections.Generic.List[object]
    foreach ($scenarioName in @($records | ForEach-Object { $_.scenario } | Sort-Object -Unique)) {
        $scenarioRecords = @($records | Where-Object {
            $_.scenario -eq $scenarioName -and $_.status -eq "ok" -and $_.completed_count -eq 1
        })
        $summaries.Add([ordered]@{
            run = $RunName
            scenario = $scenarioName
            window = $scenarioRecords[0].window
            grid = $scenarioRecords[0].grid
            completed_attempts = $scenarioRecords.Count
            elapsed_ns = Get-SampleSummary @($scenarioRecords | ForEach-Object { $_.elapsed_ns })
            scene_graph_update_latency_ns = Get-SampleSummary @(
                $scenarioRecords | ForEach-Object { $_.scene_graph_update_latency_ns }
            )
            scene_graph_render_wait_ns = Get-SampleSummary @(
                $scenarioRecords | ForEach-Object { $_.scene_graph_render_wait_ns }
            )
            readback_ns = Get-SampleSummary @($scenarioRecords | ForEach-Object { $_.readback_ns })
        })
    }

    return $summaries
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
    "QT_LOGGING_RULES=$env:QT_LOGGING_RULES"
) | Set-Content -Encoding ASCII -Path (Join-Path $artifactRoot "qsg_environment.txt")

Invoke-VcvarsCommand "cmake" @(
    "-S", $TerminalRepo,
    "-B", $terminalReleaseBuild,
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_PREFIX_PATH=$QtRoot",
    "-DVNM_TERMINAL_ENABLE_PROFILING=OFF",
    "-DVNM_TERMINAL_APP_BUILD_BENCHMARKS=ON",
    "-DVNM_TERMINAL_CMDG_ARTIFACT_TAG=$ArtifactTag",
    "-DVNM_TERMINAL_CMDG_SCENES=$sceneList",
    "-DVNM_TERMINAL_CMDG_REPEAT_COUNT=1",
    "-DVNM_TERMINAL_CMDG_NELOSTIE_FRAMES=300",
    "-DVNM_TERMINAL_CMDG_NELOSTIE_WINDOW_SIZE=1920x1080",
    "-DVNM_TERMINAL_CMDG_NELOSTIE_FONT_SIZE=10",
    "-DVNM_TERMINAL_CMDG_NELOSTIE_OFFSCREEN=OFF",
    "-DVNM_TERMINAL_CMDG_NELOSTIE_SOFTWARE_RENDERER=OFF",
    "-DVNM_TERMINAL_CMDG_NELOSTIE_HIDE_CURSOR=ON"
) $TerminalRepo (Join-Path $artifactRoot "configure_terminal_release.log")

Invoke-VcvarsCommand "cmake" @(
    "--build", $terminalReleaseBuild,
    "--target", "vnm_terminal"
) $TerminalRepo (Join-Path $artifactRoot "build_terminal_release.log")

Invoke-VcvarsCommand "cmake" @(
    "-S", $TerminalRepo,
    "-B", $terminalProfileBuild,
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_PREFIX_PATH=$QtRoot",
    "-DVNM_TERMINAL_ENABLE_PROFILING=ON",
    "-DVNM_TERMINAL_APP_BUILD_BENCHMARKS=ON",
    "-DVNM_TERMINAL_CMDG_ARTIFACT_TAG=$ArtifactTag",
    "-DVNM_TERMINAL_CMDG_SCENES=AssemblyWinter2025",
    "-DVNM_TERMINAL_CMDG_REPEAT_COUNT=1",
    "-DVNM_TERMINAL_CMDG_NELOSTIE_FRAMES=300",
    "-DVNM_TERMINAL_CMDG_NELOSTIE_WINDOW_SIZE=1920x1080",
    "-DVNM_TERMINAL_CMDG_NELOSTIE_FONT_SIZE=10",
    "-DVNM_TERMINAL_CMDG_NELOSTIE_OFFSCREEN=OFF",
    "-DVNM_TERMINAL_CMDG_NELOSTIE_SOFTWARE_RENDERER=OFF",
    "-DVNM_TERMINAL_CMDG_NELOSTIE_HIDE_CURSOR=ON",
    "-DVNM_TERMINAL_CMDG_NELOSTIE_WRITE_PROFILE_TEXT=ON"
) $TerminalRepo (Join-Path $artifactRoot "configure_terminal_profile.log")

Invoke-VcvarsCommand "cmake" @(
    "--build", $terminalProfileBuild,
    "--target", "vnm_terminal"
) $TerminalRepo (Join-Path $artifactRoot "build_terminal_profile.log")

Invoke-VcvarsCommand "cmake" @(
    "-S", $SurfaceRepo,
    "-B", $surfaceReleaseBuild,
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_PREFIX_PATH=$QtRoot",
    "-DVNM_TERMINAL_BUILD_BENCHMARKS=ON",
    "-DVNM_TERMINAL_SURFACE_BUILD_TESTING=OFF",
    "-DVNM_TERMINAL_ENABLE_PROFILING=OFF"
) $SurfaceRepo (Join-Path $artifactRoot "configure_surface_release.log")

Invoke-VcvarsCommand "cmake" @(
    "--build", $surfaceReleaseBuild,
    "--target", "vnm_terminal_embedded_benchmark", "vnm_terminal_surface_stress_benchmark"
) $SurfaceRepo (Join-Path $artifactRoot "build_surface_release.log")

Invoke-VcvarsCommand "cmake" @(
    "-S", $SurfaceRepo,
    "-B", $surfaceProfileBuild,
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_PREFIX_PATH=$QtRoot",
    "-DVNM_TERMINAL_BUILD_BENCHMARKS=ON",
    "-DVNM_TERMINAL_SURFACE_BUILD_TESTING=OFF",
    "-DVNM_TERMINAL_ENABLE_PROFILING=ON"
) $SurfaceRepo (Join-Path $artifactRoot "configure_surface_profile.log")

Invoke-VcvarsCommand "cmake" @(
    "--build", $surfaceProfileBuild,
    "--target", "vnm_terminal_embedded_benchmark", "vnm_terminal_surface_stress_benchmark"
) $SurfaceRepo (Join-Path $artifactRoot "build_surface_profile.log")

$qtBin = Get-QtBinDir $surfaceReleaseBuild
if (![string]::IsNullOrWhiteSpace($qtBin)) {
    $env:PATH = "$qtBin;$env:PATH"
}

Invoke-LoggedCommand "ctest" @(
    "--test-dir", $terminalReleaseBuild,
    "-L", "cmdg_suite",
    "--verbose"
) $TerminalRepo (Join-Path $artifactRoot "cmdg_release_ctest.log")

$terminalExe = Join-Path $terminalReleaseBuild "vnm_terminal.exe"
$cmdgExe = Join-Path $TerminalRepo "THIRD_PARTY\CMDG\CMDG\bin\Release\net8.0\CMDG.exe"
$cmdgWorkingDir = Split-Path $cmdgExe -Parent
$env:CMDG_BENCHMARK = "1"
$env:CMDG_SCENE = "AssemblyWinter2025"
$env:CMDG_DISABLE_AUDIO = "1"
$env:CMDG_ADJUST_SCREEN = "0"
$env:CMDG_SPLASH_SCREEN = "0"
$env:CMDG_BENCHMARK_FRAME_LIMIT = "300"
$env:CMDG_BENCHMARK_HIDE_CURSOR = "1"
$env:CMDG_BENCHMARK_METRICS = Join-Path $artifactRoot "direct_terminal_qsg_cmdg_metrics.json"
Invoke-LoggedCommand $terminalExe @(
    "--metrics-json", (Join-Path $artifactRoot "direct_terminal_qsg_terminal_metrics.json"),
    "--font-size", "10",
    "--window-size", "1920x1080",
    "--timeout-ms", "180000",
    "--require-output",
    "--cwd", $cmdgWorkingDir,
    "--",
    $cmdgExe
) $cmdgWorkingDir (Join-Path $artifactRoot "direct_terminal_qsg.log")

$surfaceBenchmark = Join-Path $surfaceReleaseBuild "benchmarks\embedded_terminal\vnm_terminal_embedded_benchmark.exe"
$surfaceRuns = @(
    [ordered]@{ name = "surface_scaling_cmdg_grid"; window = "1920x1080"; grid = "104x378" },
    [ordered]@{ name = "surface_scaling_half_grid"; window = "1920x1080"; grid = "52x189" },
    [ordered]@{ name = "surface_scaling_small_window_same_grid"; window = "960x540"; grid = "104x378" }
)
$rawSummaries = New-Object System.Collections.Generic.List[object]
foreach ($run in $surfaceRuns) {
    $jsonPath = Join-Path $artifactRoot "$($run.name).json"
    $rawPath = Join-Path $artifactRoot "$($run.name)_attempt_samples.jsonl"
    $args = @(
        "--iterations", "20",
        "--warmup", "3",
        "--window-size", $run.window,
        "--grid", $run.grid
    )
    foreach ($scenario in $scalingScenarios) {
        $args += @("--scenario", $scenario)
    }
    $args += @(
        "--include-attempts",
        "--output", $jsonPath,
        "--quiet",
        "--validate-json"
    )

    Invoke-LoggedCommand `
        $surfaceBenchmark `
        $args `
        $artifactRoot `
        (Join-Path $artifactRoot "$($run.name).log")
    $summaries = Export-SurfaceAttemptSamples $run.name $jsonPath $rawPath
    foreach ($summary in $summaries) {
        $rawSummaries.Add($summary)
    }
}

$rawSummaries | ConvertTo-Json -Depth 8 |
    Set-Content -Encoding ASCII -Path (Join-Path $artifactRoot "surface_scaling_raw_summary.json")

$surfaceStress = Join-Path $surfaceReleaseBuild "benchmarks\surface_stress\vnm_terminal_surface_stress_benchmark.exe"
Invoke-LoggedCommand $surfaceStress @(
    "--frames", "300",
    "--warmup-frames", "10",
    "--rows", "104",
    "--cols", "378",
    "--dirty-rows", "104",
    "--text-pattern", "mixed_non_ascii"
) $artifactRoot (Join-Path $artifactRoot "surface_stress_cmdg_grid_mixed_non_ascii.txt")

Invoke-LoggedCommand "ctest" @(
    "--test-dir", $terminalProfileBuild,
    "-R", "vnm_terminal_cmdg_nelostie_benchmark",
    "--verbose"
) $TerminalRepo (Join-Path $artifactRoot "cmdg_profile_ctest.log")

Write-RepoBaseline -Path $TerminalRepo -Name "vnm_terminal_end"
Write-RepoBaseline -Path $SurfaceRepo -Name "vnm_terminal_surface_end"

Write-Host "Bound-confirmation artifacts written to $artifactRoot"
