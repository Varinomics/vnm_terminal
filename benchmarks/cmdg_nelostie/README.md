# CMDG Nelostie benchmark

This is an opt-in live workload benchmark for `vnm_terminal`.

It launches the CMDG `AssemblyWinter2025` / Nelostie scene through
`vnm_terminal`, disables CMDG startup adjustment/splash/audio, exits after a
fixed CMDG scene-frame limit, and writes two metric artifacts:

- `vnm_terminal_cmdg_nelostie_terminal_metrics.json`
- `vnm_terminal_cmdg_nelostie_cmdg_metrics.json`

The benchmark is intentionally not part of default readiness because it depends
on the vendored .NET CMDG workload and media/assets under `third_party/CMDG`.
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
  -DVNM_TERMINAL_CMDG_NELOSTIE_OFFSCREEN=ON `
  -DVNM_TERMINAL_CMDG_NELOSTIE_SOFTWARE_RENDERER=ON
```

By default the runner builds `third_party/CMDG/CMDG/CMDG.csproj` in Release
and uses the resulting `CMDG.exe`. To run against an external/prebuilt CMDG,
set `VNM_TERMINAL_CMDG_NELOSTIE_EXE` and, if needed,
`VNM_TERMINAL_CMDG_NELOSTIE_WORKING_DIR`.

Run example:

```powershell
ctest --test-dir C:\plms\varinomics\vnm_terminal\build_release_no_profile_ninja `
  -R vnm_terminal_cmdg_nelostie_benchmark --output-on-failure
```

For regression comparisons, compare frame-rate and counter deltas from the JSON
artifacts, not wall-clock demo duration or subjective visual smoothness.
