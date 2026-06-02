# CMDG benchmark suite

This is an opt-in live workload benchmark for `vnm_terminal`.

It launches configured CMDG scenes through `vnm_terminal`, disables CMDG
startup adjustment/splash/audio, exits after a fixed CMDG scene-frame limit,
and writes terminal/CMDG metric artifacts for each scene and repeat.

Each run writes under:

```text
<build-dir>/benchmarks/cmdg_nelostie/<scene>/repeat_<n>/
```

The benchmark is intentionally not part of default readiness because it depends
on the vendored .NET CMDG workload and media/assets under `THIRD_PARTY/CMDG`.
Use it for local performance investigation and for producing captures that can
later become deterministic surface replay benchmarks.

Decision-grade local performance runs use a visible window and the hardware
scene graph. Keep software rendering and offscreen mode disabled unless the run
is explicitly a headless/CI diagnostic; software/offscreen results must not be
compared against hardware-windowed baselines or used for land/no-land
performance decisions.

Hardware-windowed configure example:

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
  -DVNM_TERMINAL_CMDG_NELOSTIE_SOFTWARE_RENDERER=OFF `
  -DVNM_TERMINAL_CMDG_NELOSTIE_HIDE_CURSOR=ON
```

For headless/CI smoke diagnostics only, the same runner can be configured with
`VNM_TERMINAL_CMDG_NELOSTIE_OFFSCREEN=ON` and
`VNM_TERMINAL_CMDG_NELOSTIE_SOFTWARE_RENDERER=ON`. Mark those artifacts as
diagnostic and do not mix them with hardware-windowed comparison sets.

Thermal and frequency state matters. Single full-suite runs are not
decision-grade when the CPU can boost for a cold first run and then downclock
after the package warms up. That effect can move CMDG scene FPS by several
percent, which is large enough to hide or fabricate the benefit of many renderer
optimizations.

For optimization decisions, either warm the machine to a steady state before
collecting samples, or compare variants in an interleaved order with enough
repeats that both variants see comparable thermal state. Record CPU frequency
and processor-performance counters alongside each benchmark run when possible.
On Windows, useful counters include:

```text
\Processor Information(_Total)\% Processor Performance
\Processor Information(_Total)\% of Maximum Frequency
\Processor Information(_Total)\Processor Frequency
```

Thermal-zone counters may also be sampled when available, but they are platform
dependent and may not expose CPU package temperature directly. Treat a
single-run FPS delta of only a few percent as noise unless the same delta
reproduces under stable frequency/thermal conditions.

## Focused regression runs (Plasma + ParticleVortex)

The `Plasma` and `ParticleVortex` r1 benchmarks carry the extra CTest label
`cmdg_regression_focus`. To run only those two hardware/windowed single-repeat
benchmarks — useful for A/B comparing feature-flag toggles without paying
full-suite thermal noise — use:

```powershell
ctest --test-dir <build-dir> -L cmdg_regression_focus --output-on-failure
```

The build fixture is pulled in automatically. Confirm the configured tree has
`VNM_TERMINAL_CMDG_NELOSTIE_OFFSCREEN=OFF` and
`VNM_TERMINAL_CMDG_NELOSTIE_SOFTWARE_RENDERER=OFF` before using the results.

To keep artifacts from successive toggle runs side by side instead of
overwriting `<scene>/repeat_<n>/`, configure with a tag (matching
`^[A-Za-z][A-Za-z0-9_]*$`):

```powershell
cmake <build-dir> -DVNM_TERMINAL_CMDG_ARTIFACT_TAG=flags_baseline
ctest --test-dir <build-dir> -L cmdg_regression_focus --output-on-failure
cmake <build-dir> -DVNM_TERMINAL_CMDG_ARTIFACT_TAG=flags_candidate
# rebuild vnm_terminal_surface with the toggled flags, then:
ctest --test-dir <build-dir> -L cmdg_regression_focus --output-on-failure
```

Artifacts then land under `<scene>/<tag>/repeat_1/`. An empty tag (the default)
keeps the legacy `<scene>/repeat_<n>/` layout, so existing suites are unchanged.
Stage 4.2 feature flags are read once per process from
`VNM_TERMINAL_STAGE42_*` environment variables, so A/B toggles must use separate
benchmark processes (separate `ctest` invocations).

By default the runner builds `THIRD_PARTY/CMDG/CMDG/CMDG.csproj` in Release
and uses the resulting `CMDG.exe`. To run against an external/prebuilt CMDG,
set `VNM_TERMINAL_CMDG_NELOSTIE_EXE` and, if needed,
`VNM_TERMINAL_CMDG_NELOSTIE_WORKING_DIR`.

The default suite is a small autonomous scene mix:
`AssemblyWinter2025;Example2D;Plasma;JuliaSetTest;ParticleVortex;Example3D`.
`QuickHello` and `ContentWiggler` are supported for explicit runs but are not
defaults because they depend on pre-existing console content or scene-specific
assumptions.

The default benchmark window is `1920x1080` with font size `10`. Keep that
font size unless the window size is adjusted to preserve at least the CMDG
`310x75` framebuffer plus border/status rows. A larger font can reduce the
terminal below CMDG's framebuffer width and cause line wrapping, which makes
the demos render incorrectly and invalidates the profile.

The runner hides the CMDG terminal cursor by default
(`VNM_TERMINAL_CMDG_NELOSTIE_HIDE_CURSOR=ON`). This keeps the benchmark from
measuring caret overlay movement as part of the scene workload. Set it to `OFF`
only when intentionally comparing against older captures that included cursor
painting.

Run the full configured suite:

```powershell
ctest --test-dir C:\plms\varinomics\vnm_terminal\build_release_hw_windowed_ninja `
  -L cmdg_suite --output-on-failure
```

Run the legacy Nelostie-compatible entry only:

```powershell
ctest --test-dir C:\plms\varinomics\vnm_terminal\build_release_hw_windowed_ninja `
  -R vnm_terminal_cmdg_nelostie_benchmark --output-on-failure
```

For regression comparisons, compare frame-rate and counter deltas from the JSON
artifacts, not wall-clock demo duration or subjective visual smoothness.
