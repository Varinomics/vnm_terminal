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

Configure example:

```powershell
cmake -S C:\plms\varinomics\vnm_terminal `
  -B C:\plms\varinomics\vnm_terminal\build_release_no_profile_ninja `
  -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DVNM_TERMINAL_ENABLE_PROFILING=OFF `
  -DVNM_TERMINAL_APP_BUILD_BENCHMARKS=ON `
  -DVNM_TERMINAL_CMDG_NELOSTIE_FRAMES=180 `
  -DVNM_TERMINAL_CMDG_NELOSTIE_FONT_SIZE=10 `
  -DVNM_TERMINAL_CMDG_SCENES="AssemblyWinter2025;Example2D;Plasma" `
  -DVNM_TERMINAL_CMDG_REPEAT_COUNT=2 `
  -DVNM_TERMINAL_CMDG_NELOSTIE_OFFSCREEN=ON `
  -DVNM_TERMINAL_CMDG_NELOSTIE_SOFTWARE_RENDERER=ON `
  -DVNM_TERMINAL_CMDG_NELOSTIE_HIDE_CURSOR=ON
```

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
ctest --test-dir C:\plms\varinomics\vnm_terminal\build_release_no_profile_ninja `
  -L cmdg_suite --output-on-failure
```

Run the legacy Nelostie-compatible entry only:

```powershell
ctest --test-dir C:\plms\varinomics\vnm_terminal\build_release_no_profile_ninja `
  -R vnm_terminal_cmdg_nelostie_benchmark --output-on-failure
```

For regression comparisons, compare frame-rate and counter deltas from the JSON
artifacts, not wall-clock demo duration or subjective visual smoothness.
