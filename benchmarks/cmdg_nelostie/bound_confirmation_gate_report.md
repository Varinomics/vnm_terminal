# Bound Confirmation Gate - 2026-06-03

Accepted result: PASS for the pre-Stage-0 GPU atlas renderer bound-confirmation gate on this
workstation.

This is a durable benchmark/reporting artifact for the gate. Raw JSON, logs, and profile text remain
ignored under `artifacts/` and the generated build directories.

## Baseline

Accepted rerun tag: `bound_confirmation_20260603_final`.

The accepted gate was rerun after the surface repository already had tracked posture/package-smoke
dirty changes, and after the benchmark raw-attempt serialization change in
`benchmarks/embedded_terminal/embedded_terminal_benchmark.cpp`. It was not accepted from the earlier
ignored-only report.

`vnm_terminal`, start and end of accepted runner:

```text
repo=vnm_terminal_start
path=C:\plms\varinomics\vnm_terminal
rev=af5f742b6e52449206bb6bbb867376e7005c1653
branch=master
status --short:
?? benchmarks/cmdg_nelostie/run_bound_confirmation_gate.ps1
```

The runner also wrote `vnm_terminal_end_git_baseline.txt`; the end status was identical.

`vnm_terminal_surface`, start and end of accepted runner:

```text
repo=vnm_terminal_surface_start
path=C:\plms\varinomics\vnm_terminal_surface
rev=94dae10fc8504e1ebe4185833e3225c11e77789b
branch=codex/gpu-atlas-renderer
status --short:
 M CMakeLists.txt
 M benchmarks/embedded_terminal/embedded_terminal_benchmark.cpp
 M cmake/vnm_terminal_qt_posture.cmake
 M cmake/vnm_terminal_surfaceConfig.cmake.in
 M docs/gpu_atlas_renderer_plan.md
 M docs/qt_rendering_policy.md
 M tests/CMakeLists.txt
 M tests/package_smoke/CMakeLists.txt
A  tests/package_smoke/expect_private_qt_minor_mismatch.cmake
 M tests/qt_posture/qt_posture_tests.cpp
```

The runner also wrote `vnm_terminal_surface_end_git_baseline.txt`; the end status was identical.

## Machine And Runtime

- Windows 11 Enterprise 10.0.22631
- CPU: AMD Ryzen 7 7840U w/ Radeon 780M Graphics, 16 logical processors
- GPU: AMD Radeon 780M Graphics, driver 32.0.31007.1017
- Qt: 6.10.1 from `C:\Qt\6.10.1\msvc2022_64`
- QSG env: `QSG_RENDER_LOOP=threaded`, `QSG_RHI_BACKEND=d3d11`,
  `QSG_RENDER_TIMING=1`, `QSG_INFO=1`,
  `QT_LOGGING_RULES=qt.scenegraph.*=true;qt.rhi.*=true`
- QSG/RHI evidence from `surface_scaling_cmdg_grid.log`: threaded render loop,
  D3D11 QRhi, AMD Radeon 780M adapter, vsync 16.68 ms, `Timestamps: 0`

## Runner

Tracked runner:

```powershell
cd C:\plms\varinomics\vnm_terminal
.\benchmarks\cmdg_nelostie\run_bound_confirmation_gate.ps1 `
  -ArtifactTag bound_confirmation_20260603_final
```

The runner records the full command and working directory at the top of each log in
`C:\plms\varinomics\vnm_terminal\artifacts\bound_confirmation_20260603_final`.

Configure commands used by the runner:

```powershell
cmake -S C:\plms\varinomics\vnm_terminal -B C:\plms\varinomics\vnm_terminal\build_bound_confirmation_20260603_final_terminal_release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:\Qt\6.10.1\msvc2022_64 -DVNM_TERMINAL_ENABLE_PROFILING=OFF -DVNM_TERMINAL_APP_BUILD_BENCHMARKS=ON -DVNM_TERMINAL_CMDG_ARTIFACT_TAG=bound_confirmation_20260603_final "-DVNM_TERMINAL_CMDG_SCENES=AssemblyWinter2025;Example2D;Plasma;JuliaSetTest;ParticleVortex;Example3D" -DVNM_TERMINAL_CMDG_REPEAT_COUNT=1 -DVNM_TERMINAL_CMDG_NELOSTIE_FRAMES=300 -DVNM_TERMINAL_CMDG_NELOSTIE_WINDOW_SIZE=1920x1080 -DVNM_TERMINAL_CMDG_NELOSTIE_FONT_SIZE=10 -DVNM_TERMINAL_CMDG_NELOSTIE_OFFSCREEN=OFF -DVNM_TERMINAL_CMDG_NELOSTIE_SOFTWARE_RENDERER=OFF -DVNM_TERMINAL_CMDG_NELOSTIE_HIDE_CURSOR=ON
cmake -S C:\plms\varinomics\vnm_terminal -B C:\plms\varinomics\vnm_terminal\build_bound_confirmation_20260603_final_terminal_profile -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:\Qt\6.10.1\msvc2022_64 -DVNM_TERMINAL_ENABLE_PROFILING=ON -DVNM_TERMINAL_APP_BUILD_BENCHMARKS=ON -DVNM_TERMINAL_CMDG_ARTIFACT_TAG=bound_confirmation_20260603_final -DVNM_TERMINAL_CMDG_SCENES=AssemblyWinter2025 -DVNM_TERMINAL_CMDG_REPEAT_COUNT=1 -DVNM_TERMINAL_CMDG_NELOSTIE_FRAMES=300 -DVNM_TERMINAL_CMDG_NELOSTIE_WINDOW_SIZE=1920x1080 -DVNM_TERMINAL_CMDG_NELOSTIE_FONT_SIZE=10 -DVNM_TERMINAL_CMDG_NELOSTIE_OFFSCREEN=OFF -DVNM_TERMINAL_CMDG_NELOSTIE_SOFTWARE_RENDERER=OFF -DVNM_TERMINAL_CMDG_NELOSTIE_HIDE_CURSOR=ON -DVNM_TERMINAL_CMDG_NELOSTIE_WRITE_PROFILE_TEXT=ON
cmake -S C:\plms\varinomics\vnm_terminal_surface -B C:\plms\varinomics\vnm_terminal_surface\build_bound_confirmation_20260603_final_surface_release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:\Qt\6.10.1\msvc2022_64 -DVNM_TERMINAL_BUILD_BENCHMARKS=ON -DVNM_TERMINAL_SURFACE_BUILD_TESTING=OFF -DVNM_TERMINAL_ENABLE_PROFILING=OFF
cmake -S C:\plms\varinomics\vnm_terminal_surface -B C:\plms\varinomics\vnm_terminal_surface\build_bound_confirmation_20260603_final_surface_profile -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:\Qt\6.10.1\msvc2022_64 -DVNM_TERMINAL_BUILD_BENCHMARKS=ON -DVNM_TERMINAL_SURFACE_BUILD_TESTING=OFF -DVNM_TERMINAL_ENABLE_PROFILING=ON
```

Build and run commands:

```powershell
cmake --build C:\plms\varinomics\vnm_terminal\build_bound_confirmation_20260603_final_terminal_release --target vnm_terminal
cmake --build C:\plms\varinomics\vnm_terminal\build_bound_confirmation_20260603_final_terminal_profile --target vnm_terminal
cmake --build C:\plms\varinomics\vnm_terminal_surface\build_bound_confirmation_20260603_final_surface_release --target vnm_terminal_embedded_benchmark vnm_terminal_surface_stress_benchmark
cmake --build C:\plms\varinomics\vnm_terminal_surface\build_bound_confirmation_20260603_final_surface_profile --target vnm_terminal_embedded_benchmark vnm_terminal_surface_stress_benchmark
ctest --test-dir C:\plms\varinomics\vnm_terminal\build_bound_confirmation_20260603_final_terminal_release -L cmdg_suite --verbose
ctest --test-dir C:\plms\varinomics\vnm_terminal\build_bound_confirmation_20260603_final_terminal_profile -R vnm_terminal_cmdg_nelostie_benchmark --verbose
```

Surface executable working directory for the three scaling runs:
`C:\plms\varinomics\vnm_terminal\artifacts\bound_confirmation_20260603_final`.
Each run used `--iterations 20 --warmup 3 --include-attempts --quiet --validate-json`.

Direct terminal QSG command used working directory
`C:\plms\varinomics\vnm_terminal\THIRD_PARTY\CMDG\CMDG\bin\Release\net8.0`:

```powershell
C:\plms\varinomics\vnm_terminal\build_bound_confirmation_20260603_final_terminal_release\vnm_terminal.exe --metrics-json C:\plms\varinomics\vnm_terminal\artifacts\bound_confirmation_20260603_final\direct_terminal_qsg_terminal_metrics.json --font-size 10 --window-size 1920x1080 --timeout-ms 180000 --require-output --cwd C:\plms\varinomics\vnm_terminal\THIRD_PARTY\CMDG\CMDG\bin\Release\net8.0 -- C:\plms\varinomics\vnm_terminal\THIRD_PARTY\CMDG\CMDG\bin\Release\net8.0\CMDG.exe
```

It reached `frame_limit` with backend errors `0`, but no QSG timing text was emitted by the
GUI-subsystem executable. QSG timing was therefore captured from the console embedded-surface
benchmark using the same forced QSG env.

## CMDG Evidence

All rows used hardware/windowed rendering, `1920x1080`, font size `10`, frame limit `300`,
DPR `1.25`, and terminal grid `104x378`.

| Scene | Exit | CMDG frames | Scene FPS | Draw FPS | Paint frames | Paint FPS | Backend errors | Timeout |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| AssemblyWinter2025 | frame_limit | 300 | 34.429 | 31.904 | 200 | 22.364 | 0 | false |
| Example2D | frame_limit | 300 | 31.908 | 32.014 | 177 | 18.474 | 0 | false |
| Plasma | frame_limit | 300 | 31.627 | 25.091 | 145 | 14.630 | 0 | false |
| JuliaSetTest | frame_limit | 300 | 32.028 | 32.134 | 300 | 31.523 | 0 | false |
| ParticleVortex | frame_limit | 300 | 30.342 | 9.406 | 59 | 5.510 | 0 | false |
| Example3D | frame_limit | 300 | 31.885 | 32.097 | 302 | 31.597 | 0 | false |

The release CMDG suite passed: `7/7` tests, `0` failures.

## Raw Surface Samples

The surface benchmark now supports `--include-attempts`. The accepted raw sample artifacts are:

- `surface_scaling_cmdg_grid_attempt_samples.jsonl`: 60 records
- `surface_scaling_half_grid_attempt_samples.jsonl`: 60 records
- `surface_scaling_small_window_same_grid_attempt_samples.jsonl`: 60 records

Each record includes `scenario`, `attempt_index`, `status`, `elapsed_ns`,
`scene_graph_update_latency_ns`, `scene_graph_render_wait_ns`, `readback_ns`,
`completed_count`, and `render_consumed_count`. The aggregate JSON also retains
`scenarios[].attempts[]`; for `surface_scaling_cmdg_grid`, each scenario had
20 attempts, 20 completed frames, and 20 bridge-consumed updates.

The p50/p95 table below was recomputed from the JSONL raw samples:

| Run | Scenario | Window | Grid | Completed | elapsed p50 ms | elapsed p95 ms | update p50 ms | update p95 ms | render-wait p50 ms | render-wait p95 ms |
| --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| cmdg_grid | ascii_full_dirty_reuse_only | 1920x1080 | 104x378 | 20 | 1150.286 | 1205.842 | 21.601 | 29.894 | 21.320 | 29.711 |
| cmdg_grid | dense_repaint | 1920x1080 | 104x378 | 20 | 1201.139 | 1219.523 | 34.080 | 41.404 | 33.722 | 41.088 |
| cmdg_grid | mixed_text_full_dirty_reuse_only | 1920x1080 | 104x378 | 20 | 1286.105 | 1319.004 | 26.548 | 38.497 | 26.033 | 38.156 |
| half_grid | ascii_full_dirty_reuse_only | 1920x1080 | 52x189 | 20 | 99.626 | 116.101 | 15.751 | 26.214 | 15.633 | 26.062 |
| half_grid | dense_repaint | 1920x1080 | 52x189 | 20 | 100.953 | 122.022 | 18.099 | 28.906 | 17.946 | 28.728 |
| half_grid | mixed_text_full_dirty_reuse_only | 1920x1080 | 52x189 | 20 | 139.058 | 150.688 | 21.273 | 29.456 | 21.065 | 29.319 |
| small_window_same_grid | ascii_full_dirty_reuse_only | 960x540 | 104x378 | 20 | 1101.843 | 1118.014 | 17.657 | 23.867 | 17.423 | 23.697 |
| small_window_same_grid | dense_repaint | 960x540 | 104x378 | 20 | 1171.972 | 1201.493 | 33.365 | 44.038 | 33.036 | 43.840 |
| small_window_same_grid | mixed_text_full_dirty_reuse_only | 960x540 | 104x378 | 20 | 1283.859 | 1305.501 | 22.076 | 34.093 | 21.562 | 33.738 |

Scaling conclusion: halving the grid from `104x378` to `52x189` at the same window cuts
readback-inclusive elapsed p50 by about `9x` to `12x`. Halving window resolution to `960x540`
while keeping the `104x378` grid does not materially reduce elapsed p50. The control evidence
therefore points at grid/cell workload dominance.

## QSG Timing

From `surface_scaling_cmdg_grid.log`:

| Metric | Count | Min ms | p50 ms | p95 ms | Max ms | Mean ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| QSG frame rendered | 71 | 0 | 64 | 98 | 10506 | 194.90 |
| QSG sync | 71 | 0 | 8 | 19 | 7642 | 116.17 |
| QSG render | 71 | 0 | 46 | 79 | 2845 | 73.21 |
| QSG swap | 71 | 0 | 1 | 14 | 18 | 4.54 |
| Renderer total | 139 | 0 | 38 | 55 | 2845 | 45.14 |
| Renderer rendering | 139 | 0 | 5 | 52 | 538 | 21.83 |

The largest values are first-frame/glyph-cache outliers, but the p50/p95 render-loop data still
shows render-thread work in the heavy-grid path.

## Profile Evidence

Profile run artifact:
`C:\plms\varinomics\vnm_terminal\build_bound_confirmation_20260603_final_terminal_profile\benchmarks\cmdg_nelostie\AssemblyWinter2025\bound_confirmation_20260603_final\repeat_1\vnm_terminal_cmdg_AssemblyWinter2025_r1_profile.txt`.

Profile terminal metrics: `elapsed_ms=9020`, `paint_frames=196`, `paint_fps=21.729`,
backend errors `0`, timeout `false`.

| Scope | Calls | Total ms | Mean ms | Share of wall |
| --- | ---: | ---: | ---: | ---: |
| Terminal_screen_model::render_snapshot::append_rows::append_cells | 112632 | 941.158 | 0.008 | 10.4% |
| Terminal_screen_model::render_snapshot | 1083 | 1032.577 | 0.953 | 11.4% |
| Terminal_session::publish_backend_render_snapshot | 1083 | 1157.814 | 1.069 | 12.8% |
| build_terminal_render_frame | 196 | 554.680 | 2.830 | 6.1% |
| Qsg_terminal_renderer::update_node | 196 | 3048.476 | 15.553 | 33.8% |
| VNM_TerminalSurface::updatePaintNode | 196 | 3621.546 | 18.477 | 40.1% |

The append/copy path and full render snapshot path are below the 20% frame-wall threshold.

Surface stress CPU-boundary artifact:
`surface_stress_cmdg_grid_mixed_non_ascii.txt`.

Key values: `total_ms=2950.38`, `frames_per_second=101.682`,
`ingest_ms_per_frame=4.4443`, `snapshot_ms_per_frame=0.841568`,
`render_frame_ms_per_frame=4.54864`, `snapshot_cells_per_frame=39312`.

## Decision

PASS, with limitations.

Evidence for pass:

- Full hardware-windowed CMDG scene set reached `frame_limit`.
- Terminal backend errors were `0`; terminal timeouts were `false`.
- Vsync is enabled, but observed paint rates for heavy scenes are well below the 60 Hz cap.
- Model/snapshot append/copy is below 20% of profile wall time.
- Raw surface scaling samples show grid/cell workload dominance rather than host/input/vsync or
  pixel-resolution-only dominance.
- QSG evidence confirms threaded render loop, D3D11 QRhi, hardware adapter, render timing, and vsync.

Limitations:

- Direct `vnm_terminal.exe` QSG timing text is unavailable because the app is a GUI-subsystem
  executable. The direct command did write terminal/CMDG metrics and reached `frame_limit`.
- GPU timer queries are unavailable in current tooling: QSG/RHI reports `Timestamps: 0`, and no
  QRhi/GPU timer-query benchmark instrumentation is present.
- `ParticleVortex` has meaningful CMDG-side draw cost, but it still reaches `frame_limit` and the
  terminal paint rate is lower than CMDG draw rate in the accepted run.

## Raw Artifact Index

Primary ignored artifact directory:
`C:\plms\varinomics\vnm_terminal\artifacts\bound_confirmation_20260603_final`.

CMDG release metrics:
`C:\plms\varinomics\vnm_terminal\build_bound_confirmation_20260603_final_terminal_release\benchmarks\cmdg_nelostie\<scene>\bound_confirmation_20260603_final\repeat_1\`.

Surface scaling raw samples:

- `surface_scaling_cmdg_grid.json`
- `surface_scaling_cmdg_grid_attempt_samples.jsonl`
- `surface_scaling_half_grid.json`
- `surface_scaling_half_grid_attempt_samples.jsonl`
- `surface_scaling_small_window_same_grid.json`
- `surface_scaling_small_window_same_grid_attempt_samples.jsonl`
- `surface_scaling_raw_summary.json`

Other logs and artifacts:

- `cmdg_release_ctest.log`
- `cmdg_profile_ctest.log`
- `direct_terminal_qsg.log`
- `direct_terminal_qsg_terminal_metrics.json`
- `direct_terminal_qsg_cmdg_metrics.json`
- `surface_scaling_cmdg_grid.log`
- `surface_stress_cmdg_grid_mixed_non_ascii.txt`
