# vnm_terminal

[![CI Linux](https://github.com/Varinomics/vnm_terminal/actions/workflows/ci-linux.yml/badge.svg?branch=master)](https://github.com/Varinomics/vnm_terminal/actions/workflows/ci-linux.yml) [![CI Windows](https://github.com/Varinomics/vnm_terminal/actions/workflows/ci-windows.yml/badge.svg?branch=master)](https://github.com/Varinomics/vnm_terminal/actions/workflows/ci-windows.yml) [![CI macOS](https://github.com/Varinomics/vnm_terminal/actions/workflows/ci-macos.yml/badge.svg?branch=master)](https://github.com/Varinomics/vnm_terminal/actions/workflows/ci-macos.yml)

`vnm_terminal` is the Varinomics Qt terminal application. It uses
`vnm_terminal_surface` for terminal parsing, process hosting, screen state, and
rendering, while this repository owns the application window, command-line
options, window chrome, and packaging-facing behavior.

## Build

Clone `vnm_terminal_surface` beside this repository, or pass
`-DVNM_TERMINAL_SURFACE_SOURCE_DIR=<path>` during configure.

```powershell
cmake -S . -B build -DBUILD_TESTING=ON
```

Transcript capture/replay is sensitive debugging infrastructure and is compiled
out by default. Enable it only for local diagnostic builds with
`-DVNM_TERMINAL_ENABLE_TRANSCRIPT_CAPTURE_REPLAY=ON`. Distribution builds use
`-DVNM_TERMINAL_DISTRIBUTION_BUILD=ON`, which is incompatible with transcript
capture/replay.

Build from an x64 MSVC Developer Command Prompt or another shell where the
Visual Studio C++ environment has already been initialized:

```bat
cmake --build build --target vnm_terminal --config Release
```

## Run

```powershell
.\build\Release\vnm_terminal.exe
```

On Windows, build the `vnm_terminal` target before launching the raw build
artifact. The build copies the required Qt runtime DLLs beside
`vnm_terminal.exe` and copies the platform plugin to `platforms\`. A stale build
directory created before that post-build deployment step must be rebuilt before
manual launch. Do not move the raw executable away from its neighboring DLLs and
`platforms\` directory.

The app starts the platform default shell when no explicit command follows
`--`. On validated platforms it uses built-in window chrome by default; pass
`--native-titlebar` to use the platform frame instead.

Pass `--text-renderer msdf` or `--text-renderer glyph` before `--` to force one
terminal text renderer for manual comparison. The default `--text-renderer auto`
uses the surface renderer's automatic selection and fallback behavior.
Pass `--lcd-subpixel auto|none|rgb|bgr|vrgb|vbgr` before `--` to choose MSDF
LCD sampling. The default `auto` uses the display order reported by Qt and, on
Windows, falls back to the system ClearType orientation when Qt reports no
subpixel order.

Pass `--selection-trace` before `--` to write selection diagnostics to stderr.
Diagnostic builds configured with
`VNM_TERMINAL_ENABLE_TRANSCRIPT_CAPTURE_REPLAY=ON` also accept
`--capture-transcript <path>` and `--transcript-snapshot-diagnostics` for
deterministic terminal replay diagnostics. Distribution builds omit those
options. Transcripts are sensitive: they include launch argv, launch cwd,
backend output bytes, typed and pasted input, resize, scroll, selection events,
and any optional snapshot text diagnostics.

Pass `--disable-primary-repaint-recovery` before `--` to disable primary repaint
scrollback recovery for the launched session. It is enabled by default on
Windows and disabled by default elsewhere.

Pass `--paste-shortcut <mode>` before `--` to choose the keyboard shortcut that
pastes clipboard text into the terminal. The default `platform-default`
preserves the current behavior: Ctrl+V and Ctrl+Shift+V paste on every platform,
plus Cmd+V on macOS. `ctrl-v-and-ctrl-shift-v` keeps the Ctrl combinations
without the macOS Cmd+V shortcut, `ctrl-shift-v` restricts pasting to
Ctrl+Shift+V only, and `disabled` turns the paste shortcut off entirely. Mode
values are case-insensitive. Copy shortcuts are unaffected.

Synchronized-output scrolling is deferred until content publication by default.
Pass `--synchronized-output-scroll-policy immediate-public` before `--` to
opt in to immediate public-projection scrolling during DEC synchronized-output
holds. Pass `--synchronized-output-scroll-policy defer` to request the default
policy explicitly. Policy values are case-insensitive.

Manual validation for immediate public scrolling should use an app build whose
post-build deployment step has copied the Qt DLLs and `platforms\` plugin
beside `vnm_terminal.exe`. Diagnostic trace validation also requires a local
build configured with `VNM_TERMINAL_ENABLE_TRANSCRIPT_CAPTURE_REPLAY=ON`; those
transcript and wheel-trace flags are not present in distribution builds.

```powershell
.\tools\synchronized_output_scroll_policy_repro.ps1 `
    -TerminalExe .\build\Release\vnm_terminal.exe `
    -Policy immediate-public `
    -CaptureTranscript .\build\sync-scroll.ndjson `
    -WheelTrace `
    -TranscriptSnapshotDiagnostics
```

The launcher validates that the selected app has the Qt runtime DLLs and
`platforms\qwindows.dll` plugin deployed beside it, or that it is a portable
launcher with that deployed layout under `vnm_terminal_runtime\`. It then passes
the policy and diagnostic flags to the app and runs the deterministic payload in
`-PayloadOnly` mode as the child process. During the repro, scroll the terminal
while the script is inside the hold. The transcript should show an immediate
effective policy for the hold, a `PUBLIC_PROJECTION` scroll snapshot before
release, no hidden sentinel rows or metadata before release, a release
reconciliation result, and the post-release suffix only after release.

Run a specific command by placing it after `--`:

```powershell
.\build\Release\vnm_terminal.exe --window-size 1000x640 -- cmd.exe
```

Run tests:

```powershell
ctest --test-dir build -C Release --output-on-failure
```

## Windows Portable Build

Windows release zips use the same layout as the local portable distribution:
the top-level `vnm_terminal.exe` is a launcher, and the real Qt application
plus deployed runtime files live in `vnm_terminal_runtime\`.

Copy `build_config.bat.example` to `build_config.bat`, adjust the local Qt
MinGW, CMake, Ninja, and `vnm_terminal_surface` paths, then run:

```bat
build_portable.bat
```

The script writes `dist\portable\` and `dist\vnm_terminal_v1.0.1_w64.zip`.
Portable releases are built with `VNM_TERMINAL_DISTRIBUTION_BUILD=ON`,
`VNM_TERMINAL_ENABLE_PROFILING=OFF`, and transcript capture/replay disabled.
They are packaged from the Qt MinGW kit and include the required Qt and MinGW
runtime DLLs beside the real application in `vnm_terminal_runtime\`.
Launch the portable distribution through `dist\portable\vnm_terminal.exe`.
That top-level executable is a launcher; do not launch the raw
`build_portable\vnm_terminal.exe` as a substitute for portable validation.

## macOS Bundle Build

The macOS GitHub Actions workflow builds a Release `vnm_terminal.app` with
`VNM_TERMINAL_DISTRIBUTION_BUILD=ON`, `VNM_TERMINAL_ENABLE_PROFILING=OFF`, and
transcript capture/replay disabled, deploys its Qt runtime with `macdeployqt`,
ad-hoc signs the unsigned bundle, and uploads
`vnm_terminal_v<version>_macos_x64_unnotarized.zip` as a workflow artifact.
When a GitHub release is published, the workflow also attaches that ZIP to the
release.

The macOS bundle is not Apple-notarized. Gatekeeper may block it on first run.
Users who trust the downloaded build can remove the quarantine attribute:

```bash
xattr -dr com.apple.quarantine /path/to/vnm_terminal.app
open /path/to/vnm_terminal.app
```
