# vnm_terminal

[![CI Linux](https://github.com/Varinomics/vnm_terminal/actions/workflows/ci-linux.yml/badge.svg?branch=master)](https://github.com/Varinomics/vnm_terminal/actions/workflows/ci-linux.yml) [![CI Windows](https://github.com/Varinomics/vnm_terminal/actions/workflows/ci-windows.yml/badge.svg?branch=master)](https://github.com/Varinomics/vnm_terminal/actions/workflows/ci-windows.yml)

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
.\build\Release\vnm_terminal.exe --window-size 1000x640 -- pwsh -NoLogo
```

Run tests:

```powershell
ctest --test-dir build -C Release --output-on-failure
```
