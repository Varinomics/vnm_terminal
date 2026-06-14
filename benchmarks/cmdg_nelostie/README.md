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
scene graph. Keep offscreen mode disabled unless the run is explicitly a
headless/CI diagnostic; offscreen results must not be compared against
hardware-windowed baselines or used for land/no-land performance decisions.

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
  -DVNM_TERMINAL_CMDG_NELOSTIE_HIDE_CURSOR=ON
```

For headless/CI smoke diagnostics only, the same runner can be configured with
`VNM_TERMINAL_CMDG_NELOSTIE_OFFSCREEN=ON`. Mark those artifacts as diagnostic
and do not mix them with hardware-windowed comparison sets.

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
benchmarks — useful for A/B comparing terminal builds without paying
full-suite thermal noise — use:

```powershell
ctest --test-dir <build-dir> -L cmdg_regression_focus --output-on-failure
```

The build fixture is pulled in automatically. Confirm the configured tree has
`VNM_TERMINAL_CMDG_NELOSTIE_OFFSCREEN=OFF` before using the results.

To keep artifacts from successive comparison runs side by side instead of
overwriting `<scene>/repeat_<n>/`, configure with a tag (matching
`^[A-Za-z][A-Za-z0-9_]*$`):

```powershell
cmake <build-dir> -DVNM_TERMINAL_CMDG_ARTIFACT_TAG=baseline
ctest --test-dir <build-dir> -L cmdg_regression_focus --output-on-failure
cmake <build-dir> -DVNM_TERMINAL_CMDG_ARTIFACT_TAG=candidate
# rebuild vnm_terminal_surface with the candidate changes, then:
ctest --test-dir <build-dir> -L cmdg_regression_focus --output-on-failure
```

Artifacts then land under `<scene>/<tag>/repeat_1/`. An empty tag (the default)
keeps the legacy `<scene>/repeat_<n>/` layout, so existing suites are unchanged.
Use separate artifact tags and separate benchmark processes when comparing
different terminal builds or runtime configurations.

## Canonical atlas CMDG gate

After the atlas cutover there is no in-repo alternate renderer path to select
for A/B runs. The retained CMDG gate builds one Release app with the atlas
renderer as the canonical path, runs the configured CMDG suite, validates
terminal/CMDG metrics including the `qsg_atlas.buffer_upload` budget counters,
and can compare the canonical run against an archived pre-cutover baseline.
The runner never rebuilds or selects the removed renderer.

Run the canonical atlas gate with:

```powershell
.\benchmarks\cmdg_nelostie\run_canonical_atlas_cmdg_gate.ps1 `
  -ArtifactTag canonical_atlas_cmdg_gate_<tag> `
  -ArchivedBaselineComparisonJson artifacts\stage4_cmdg_gate_repeat3_20260604_062208\stage4_cmdg_gate_comparison.json
```

The script configures a Release/profiling-off hardware-windowed CTest run,
assigns the provided artifact tag, copies each per-run terminal/CMDG metrics JSON
under `artifacts/<tag>/per_run_metrics/`, and writes
`canonical_atlas_cmdg_gate.json` plus a Markdown summary under
`artifacts/<tag>/`.

The gate treats terminal Qt frameSwapped proxy FPS
(`presentation.primary_frames_per_second`), CMDG `draw_frames_per_second`, and
CMDG `scene_frames_per_second` as evidence fields. The terminal evidence records
`presentation.frameSwapped.count` with
`primary_counter_source=QQuickWindow::frameSwapped`,
`primary_counter_semantics=qt_frame_swapped_proxy`, and
`scanout_verified=false`. The pass/fail checks require zero backend
errors/timeouts, positive terminal frame proxy evidence above the absolute FPS
floor, canonical atlas metrics to be present, positive atlas budget counters,
and zero atlas failed inserts. The per-run records include producer-sourced
`qsg_atlas.producer` counters for shaped work built versus reused; the legacy
`renderer.text_content_*` counters are not atlas reuse evidence. When
`-ArchivedBaselineComparisonJson` is supplied, the gate also requires at least a
25% median terminal frame proxy FPS improvement for the motivating scenes
(`Plasma` and `ParticleVortex`) and no more than a 5% median regression across
CMDG draw FPS and CMDG scene FPS for every archived-comparison scene. Terminal
paint FPS remains reported for diagnostics, but it is not the terminal frame
proxy evidence. The archived per-run records also preserve renderer frame time,
first-output startup latency, terminal app elapsed time, atlas memory, glyph
misses, page pressure, visible first-frame completion evidence, and
cold-glyph/frame-impact proxy counters such as rasterized glyphs and
glyph-buffer upload counts.

By default the runner builds `THIRD_PARTY/CMDG/CMDG/CMDG.csproj` in Release
and uses the resulting `CMDG.exe`. To run against an external/prebuilt CMDG,
set `VNM_TERMINAL_CMDG_NELOSTIE_EXE` and, if needed,
`VNM_TERMINAL_CMDG_NELOSTIE_WORKING_DIR`.

The default suite is a small autonomous scene mix:
`AssemblyWinter2025;Example2D;Plasma;JuliaSetTest;ParticleVortex;Example3D`.
`QuickHello` and `ContentWiggler` are supported for explicit runs but are not
defaults because they depend on pre-existing console content or scene-specific
assumptions.

The canonical gate defaults to three repeats, the full scene list above, the
frame limit selected by `-FrameLimit` /
`VNM_TERMINAL_CMDG_NELOSTIE_FRAMES`, `1920x1080`, font size `10`, visible
windowed rendering, and D3D11 RHI through the script environment.

### Exact high-resolution scratch scenario

The canonical atlas gate is not the AssemblyWinter2025 scratch block-only
benchmark. For that exact scenario, use a direct CMake/CTest run with the
scratch CMDG executable or working tree that is configured for a `620x150`
block-only framebuffer:

```powershell
cmake -S C:\plms\varinomics\vnm_terminal `
  -B C:\plms\varinomics\vnm_terminal\build_cmdg_aw2025_scratch_620x150 `
  -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_PREFIX_PATH=C:\Qt\6.10.1\msvc2022_64 `
  -DVNM_TERMINAL_ENABLE_PROFILING=OFF `
  -DVNM_TERMINAL_APP_BUILD_BENCHMARKS=ON `
  -DVNM_TERMINAL_CMDG_ARTIFACT_TAG=aw2025_scratch_620x150_block_3000 `
  -DVNM_TERMINAL_CMDG_SCENES=AssemblyWinter2025 `
  -DVNM_TERMINAL_CMDG_REPEAT_COUNT=1 `
  -DVNM_TERMINAL_CMDG_NELOSTIE_FRAMES=3000 `
  -DVNM_TERMINAL_CMDG_NELOSTIE_WINDOW_SIZE=3840x2160 `
  -DVNM_TERMINAL_CMDG_NELOSTIE_FONT_SIZE=10 `
  -DVNM_TERMINAL_CMDG_NELOSTIE_OFFSCREEN=OFF `
  -DVNM_TERMINAL_CMDG_NELOSTIE_HIDE_CURSOR=ON `
  -DVNM_TERMINAL_CMDG_NELOSTIE_EXE=<scratch-CMDG.exe> `
  -DVNM_TERMINAL_CMDG_NELOSTIE_WORKING_DIR=<scratch-CMDG-dir>

ctest --test-dir C:\plms\varinomics\vnm_terminal\build_cmdg_aw2025_scratch_620x150 `
  -R vnm_terminal_cmdg_nelostie_benchmark --output-on-failure
```

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
