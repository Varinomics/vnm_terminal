# CMDG benchmark suite

This opt-in live benchmark launches CMDG scenes through `vnm_terminal`, disables
CMDG startup adjustment, splash, and audio, and exits after a fixed scene-frame
limit. It writes terminal and CMDG metrics for every scene and repeat under:

```text
<build-dir>/benchmarks/cmdg_nelostie/<scene>/repeat_<n>/
```

The benchmark depends on the vendored .NET workload and media under
`THIRD_PARTY/CMDG`, so it is not part of default readiness. Use visible,
hardware-windowed runs for performance decisions. Offscreen runs are suitable
only for headless diagnostics and are not comparable with windowed results.

## Configure and run

Initialize the MSVC x64 environment, then configure a Release build:

```powershell
cmake -S C:\plms\varinomics\vnm_terminal `
  -B C:\plms\varinomics\vnm_terminal\build_release_hw_windowed_ninja `
  -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DVNM_TERMINAL_ENABLE_PROFILING=OFF `
  -DVNM_TERMINAL_APP_BUILD_BENCHMARKS=ON `
  -DVNM_TERMINAL_CMDG_NELOSTIE_FRAMES=180 `
  -DVNM_TERMINAL_CMDG_NELOSTIE_FONT_SIZE=10 `
  -DVNM_TERMINAL_CMDG_SCENES="AssemblyWinter2025;Example2D;Plasma" `
  -DVNM_TERMINAL_CMDG_REPEAT_COUNT=2 `
  -DVNM_TERMINAL_CMDG_NELOSTIE_OFFSCREEN=OFF `
  -DVNM_TERMINAL_CMDG_NELOSTIE_HIDE_CURSOR=ON
```

Run the configured suite:

```powershell
ctest --test-dir C:\plms\varinomics\vnm_terminal\build_release_hw_windowed_ninja `
  -L cmdg_suite --output-on-failure
```

Run only the `Plasma` and `ParticleVortex` comparison set:

```powershell
ctest --test-dir <build-dir> -L cmdg_regression_focus --output-on-failure
```

The CMDG build fixture is selected automatically. Confirm that the configured
tree has `VNM_TERMINAL_CMDG_NELOSTIE_OFFSCREEN=OFF` before using the results for
performance decisions.

To keep successive runs separate, configure an artifact tag matching
`^[A-Za-z][A-Za-z0-9_]*$`:

```powershell
cmake <build-dir> -DVNM_TERMINAL_CMDG_ARTIFACT_TAG=baseline
ctest --test-dir <build-dir> -L cmdg_regression_focus --output-on-failure
cmake <build-dir> -DVNM_TERMINAL_CMDG_ARTIFACT_TAG=candidate
ctest --test-dir <build-dir> -L cmdg_regression_focus --output-on-failure
```

Tagged artifacts are written under `<scene>/<tag>/repeat_<n>/`. Without a tag,
the runner uses `<scene>/repeat_<n>/`.

Set `VNM_TERMINAL_CMDG_NELOSTIE_OFFSCREEN=ON` only for headless diagnostics.
Set `VNM_TERMINAL_CMDG_NELOSTIE_EXE` and, when needed,
`VNM_TERMINAL_CMDG_NELOSTIE_WORKING_DIR` to run an external CMDG build.

## Atlas gate

The atlas gate builds a Release app, runs the configured scene set, validates
terminal and CMDG metrics, and writes per-run JSON plus
`canonical_atlas_cmdg_gate.json` under `artifacts/<tag>/`:

```powershell
.\benchmarks\cmdg_nelostie\run_canonical_atlas_cmdg_gate.ps1 `
  -ArtifactTag canonical_atlas_cmdg_gate_local
```

The default set is
`AssemblyWinter2025;Example2D;Plasma;JuliaSetTest;ParticleVortex;Example3D`,
with three repeats, a 1000-frame limit, a `1920x1080` window, font size `10`,
visible rendering, and the D3D11 RHI. `-FocusOnly` selects `Plasma` and
`ParticleVortex`.

The gate requires successful process completion, output, positive presentation
and atlas frame evidence, positive atlas budget counters, and zero atlas failed
inserts. `presentation.primary_frames_per_second` is the Qt `frameSwapped`
proxy; `scanout_verified=false` means it is not direct display scanout evidence.

An optional `-ArchivedBaselineComparisonJson <path>` compares the run with a
comparison JSON file containing a `baseline_records` array. Comparison runs
must use the same workload, window, renderer configuration, and machine
conditions.

## Measurement discipline

Use multiple interleaved repeats when CPU boost, temperature, or frequency can
change during a run. Record processor-performance counters when possible. On
Windows, useful counters include:

```text
\Processor Information(_Total)\% Processor Performance
\Processor Information(_Total)\% of Maximum Frequency
\Processor Information(_Total)\Processor Frequency
```

Compare the JSON frame-rate and counter fields, not wall-clock demo duration or
subjective smoothness. Do not treat a small single-run FPS delta as meaningful
unless it reproduces under comparable thermal and frequency conditions.
