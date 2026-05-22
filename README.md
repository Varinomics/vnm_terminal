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

Build from an x64 MSVC Developer Command Prompt or another shell where the
Visual Studio C++ environment has already been initialized:

```bat
cmake --build build --target vnm_terminal --config Release
```

## Run

```powershell
.\build\Release\vnm_terminal.exe
```

The app starts the platform default shell when no explicit command follows
`--`. On validated platforms it uses built-in window chrome by default; pass
`--native-titlebar` to use the platform frame instead.

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
Portable releases are built with `VNM_TERMINAL_ENABLE_PROFILING=OFF`.
They are packaged from the Qt MinGW kit and include the required Qt and MinGW
runtime DLLs beside the real application in `vnm_terminal_runtime\`.

## macOS Bundle Build

The macOS GitHub Actions workflow builds a Release `vnm_terminal.app` with
`VNM_TERMINAL_ENABLE_PROFILING=OFF`, deploys its Qt runtime with `macdeployqt`,
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
