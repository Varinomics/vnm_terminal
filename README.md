# vnm_terminal

[![CI Linux](https://github.com/Varinomics/vnm_terminal/actions/workflows/ci-linux.yml/badge.svg?branch=master)](https://github.com/Varinomics/vnm_terminal/actions/workflows/ci-linux.yml) [![CI Windows](https://github.com/Varinomics/vnm_terminal/actions/workflows/ci-windows.yml/badge.svg?branch=master)](https://github.com/Varinomics/vnm_terminal/actions/workflows/ci-windows.yml)

`vnm_terminal` is the home of the Qt Quick terminal surface used to embed
interactive PTY and ConPTY-backed command-line applications in Varinomics
products.

The primary visual component is `VNM_TerminalSurface`, a C++ `QQuickItem`.

## Start Here

For a first pass through the repository, read these in order:

1. [Developer orientation](docs/developer_orientation.md) - the shortest stable explanation
   of what the project is, where the important code lives, and how the pieces
   fit together.
2. [Architecture](docs/architecture.md) - the runtime model, ownership
   boundaries, and data flow.
3. [Public surface](docs/public_surface.md) - the `VNM_TerminalSurface` API and
   the example terminal host.
4. [Repository guide](docs/repository_guide.md) - build targets, test families,
   generated/provenance material, and common entry points.

The [documentation index](docs/README.md) has a time-budgeted reading path and
links to the reference material.

## Build And Run On Windows

Configure once:

```powershell
cmake -S . -B build -DVNM_TERMINAL_BUILD_EXAMPLES=ON -DBUILD_TESTING=ON
```

Build from an x64 MSVC Developer Command Prompt or another shell where the
Visual Studio C++ environment has already been initialized:

```bat
cmake --build build --target vnm_terminal_example_terminal --config Release
```

Launch the example terminal:

```powershell
.\build\examples\terminal_app\Release\vnm_terminal_example_terminal.exe
```

On validated platforms the example uses its built-in window chrome by default;
pass `--native-titlebar` to use the platform frame instead.

On Linux, use the same configure step and the normal generated build command for
the selected generator, then run `build/examples/terminal_app/vnm_terminal_example_terminal`.

Use the [repository guide](docs/repository_guide.md) for test families,
conformance controls, benchmarks, and focused validation commands.
