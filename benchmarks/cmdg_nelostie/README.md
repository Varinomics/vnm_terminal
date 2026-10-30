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

Follow [the repository toolchain policy](../../AGENTS.md#local-windows-toolchain)
for the developer environment, compiler, generator and queue settings. Use a
fresh Release build directory when the compiler or generator changes. Every
configure or regeneration must run in that environment through the build queue.

First build the vendored CMDG workload from the repository root with one local
CPU slot and serial MSBuild/compiler settings:

```powershell
queued-build --slots 1 -- dotnet build THIRD_PARTY/CMDG/CMDG/CMDG.csproj `
  -c Release -m:1 -p:BuildInParallel=false -p:RestoreDisableParallel=true `
  -p:UseSharedCompilation=false -p:ConcurrentBuild=false
```

Add these benchmark arguments to the policy-selected, queued CMake configure.
Replace `<repo>` with the absolute checkout path:

```text
-DCMAKE_BUILD_TYPE=Release
-DVNM_TERMINAL_ENABLE_PROFILING=OFF
-DVNM_TERMINAL_APP_BUILD_BENCHMARKS=ON
-DVNM_TERMINAL_CMDG_NELOSTIE_EXE=<repo>/THIRD_PARTY/CMDG/CMDG/bin/Release/net8.0/CMDG.exe
-DVNM_TERMINAL_CMDG_NELOSTIE_FRAMES=180
-DVNM_TERMINAL_CMDG_NELOSTIE_FONT_SIZE=10
-DVNM_TERMINAL_CMDG_SCENES="AssemblyWinter2025;Example2D;Plasma;ParticleVortex"
-DVNM_TERMINAL_CMDG_REPEAT_COUNT=2
-DVNM_TERMINAL_CMDG_NELOSTIE_OFFSCREEN=OFF
-DVNM_TERMINAL_CMDG_NELOSTIE_HIDE_CURSOR=ON
```

Build the `vnm_terminal` target through the queue using the selected generator's
required flags and matching CPU width. Setting the explicit CMDG executable
above avoids the automatic CTest build fixture, whose direct `dotnet build`
does not apply the workstation queue policy. The working directory defaults to
the executable directory, where the .NET build copies the scene assets.

Run the configured suite after both executables have been built:

```powershell
ctest --test-dir <build-dir> -L cmdg_suite --output-on-failure
```

Run only the `Plasma` and `ParticleVortex` comparison set:

```powershell
ctest --test-dir <build-dir> -L cmdg_regression_focus --output-on-failure
```

Confirm that the configured tree has `VNM_TERMINAL_CMDG_NELOSTIE_OFFSCREEN=OFF`
before using the results for performance decisions.

To keep successive runs separate, add
`-DVNM_TERMINAL_CMDG_ARTIFACT_TAG=baseline` to the queued configure, then run the
focus set. Repeat with `candidate` for the next run. Tags must match
`^[A-Za-z][A-Za-z0-9_]*$`; changing a tag is a CMake regeneration and follows the
same developer-environment and queue requirements as initial configuration.

Tagged artifacts are written under `<scene>/<tag>/repeat_<n>/`. Without a tag,
the runner uses `<scene>/repeat_<n>/`.

Set `VNM_TERMINAL_CMDG_NELOSTIE_OFFSCREEN=ON` only for headless diagnostics.
Set `VNM_TERMINAL_CMDG_NELOSTIE_EXE` and, when needed,
`VNM_TERMINAL_CMDG_NELOSTIE_WORKING_DIR` to run an external CMDG build.

## Atlas gate

`run_canonical_atlas_cmdg_gate.ps1` combines application builds, scene runs and
metric validation, writing per-run JSON and `canonical_atlas_cmdg_gate.json`
under `artifacts/<tag>/`. Its build steps still hardcode Ninja, use an older
toolchain default and invoke compiler-capable commands without the queue.
Those steps need updating before this runner can be used under the current
workstation policy. Use the explicitly prebuilt workflow above for scene runs.

The default set is
`AssemblyWinter2025;Example2D;Plasma;JuliaSetTest;ParticleVortex;Example3D`,
with three repeats, a 1000-frame limit, a `1920x1080` window, font size `10`,
visible rendering, and the D3D11 RHI. Its focus set is `Plasma` and
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
