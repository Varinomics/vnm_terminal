# Repository Guide

This guide maps the repository for build, test, provenance, and maintenance tasks.
It is the quick operational reference after `docs/developer_orientation.md`,
`docs/architecture.md`, and `docs/public_surface.md`.

## Top-Level Layout

- `include/vnm_terminal/vnm_terminal_surface.h` is the public Qt Quick item.
- `include/vnm_terminal/internal` contains internal contracts shared by the
  session, backend, model, input, renderer, fixture, and generated table code.
- `src` contains the implementation for the surface, ordered terminal session,
  screen model, input encoder, renderer, Unicode width tables, and platform
  backends.
- `resources` contains assets compiled into the library, including the bundled
  monospace font.
- `cmake` contains project CMake helpers. The Qt posture helper validates the
  accepted Qt route and allowed Qt module targets.
- `examples/terminal_app` contains the runnable Qt terminal host and its window
  chrome helpers.
- `tools/terminal_canvas_fixture` contains the scripted terminal-canvas fixture
  used by automated tests.
- `tools/unicode_width` contains the Unicode width table generator.
- `tools/conformance` contains local setup helpers for external conformance
  tools and Unicode data.
- `tests` contains contract, model, backend, renderer, surface, example,
  conformance, randomized, and lifecycle tests.
- `benchmarks/embedded_terminal` contains the embedded terminal benchmark.
- `docs` contains first-read orientation and stable reference material.
- `THIRD_PARTY` and `THIRD_PARTY_NOTICES.md` record dependency and provenance
  information.

The root `CMakeLists.txt` is the best single-file build map. It declares the
library source list, resources, platform backend additions, examples, tests, and
benchmarks.

## Build Options

The project requires CMake 3.21 or newer, C++20, and Qt 6.7 Core, Gui, and
Quick.

- `BUILD_TESTING` is the standard CTest switch. Leave it on for normal
  development.
- `VNM_TERMINAL_BUILD_EXAMPLES` is `ON` by default. The example terminal is
  added only on Windows and Linux, where a native backend exists.
- `VNM_TERMINAL_BUILD_BENCHMARKS` is `OFF` by default. It adds the embedded
  benchmark target and benchmark CTest validation tests when testing is enabled.
- `VNM_TERMINAL_BUILD_REQUIRED_READINESS` is `OFF` by default. It requires
  `BUILD_TESTING=ON` and also enables benchmarks and profiling validation.
- `VNM_TERMINAL_ENABLE_PROFILING` is `OFF` by default. Turning it on compiles
  profile-scope instrumentation into the library and enables profile-output
  validation paths.
- `VNM_TERMINAL_QT_LICENSE_ROUTE` is `lgpl_dynamic` by default and also accepts
  `commercial`. The LGPL route requires shared Qt libraries.

Useful configure shapes:

```powershell
cmake -S . -B build -DVNM_TERMINAL_BUILD_EXAMPLES=ON
cmake -S . -B build -DVNM_TERMINAL_BUILD_BENCHMARKS=ON -DBUILD_TESTING=ON
cmake -S . -B build -DVNM_TERMINAL_BUILD_REQUIRED_READINESS=ON -DBUILD_TESTING=ON
cmake -S . -B build-profile -DVNM_TERMINAL_BUILD_BENCHMARKS=ON -DVNM_TERMINAL_ENABLE_PROFILING=ON -DBUILD_TESTING=ON
```

## Supported Platforms

The native product targets are:

| Platform | Architecture | Backend |
| --- | --- | --- |
| Windows | x64 | ConPTY |
| Linux | x86_64 | PTY |

Other platforms have no native backend support claim. A platform without a
native backend reports an unavailable-backend error for process launch.

## Important Targets

The root CMake project builds:

- `vnm_terminal`, the static library, with alias `vnm_terminal::vnm_terminal`;
- `vnm_terminal_canvas_fixture`, the scripted child process used by backend,
  surface, example, and conformance tests;
- `vnm_terminal_example_terminal`, the runnable host in `examples/terminal_app`;
- `vnm_terminal_embedded_benchmark`, when benchmarks are enabled;
- test executables named after their CTest entries when `BUILD_TESTING` is on;
- optional conformance targets created by CMake cache variables under
  `tests/CMakeLists.txt`;
- optional `vnm_terminal_parser_libfuzzer`, when
  `VNM_TERMINAL_BUILD_LIBFUZZER=ON` is configured with Clang;
- optional manual target `vnm_terminal_external_vttest`, when
  `VNM_TERMINAL_VTTEST_COMMAND` is set.

## Local Windows Build

Use an x64 MSVC Developer Command Prompt or another shell where the Visual
Studio C++ environment has already been initialized:

```bat
cmake --build build --config Release
```

Build only the example terminal:

```bat
cmake --build build --target vnm_terminal_example_terminal --config Release
```

## Running The Example

The example starts the platform default shell when no explicit command follows
`--`: `COMSPEC` or `cmd.exe` on Windows, `SHELL` or `/bin/sh` on Linux.

```powershell
.\build\examples\terminal_app\Release\vnm_terminal_example_terminal.exe
```

On validated platforms the example uses the built-in window chrome by default;
pass `--native-titlebar` to use the platform frame instead.

Run a specific command by placing it after `--`:

```powershell
.\build\examples\terminal_app\Release\vnm_terminal_example_terminal.exe --window-size 1000x640 -- pwsh -NoLogo
```

Useful options include `--cwd`, `--font-family`, `--font-size`, `--theme`,
`--window-size`, `--native-titlebar`,
`--alternate-wheel`, `--capture-output`, `--software-renderer`,
`--keep-open-after-process-exits`, `--timeout-ms`, and `--require-output`.

## Test Families

CTest names are the stable way to find a test. The main families are:

- `vnm_terminal_parser_*`, `vnm_terminal_sequence_matrix`, and
  `vnm_terminal_unicode_*`: parser IR, authored parser seeds, sequence
  coverage, and Unicode width behavior.
- `vnm_terminal_screen_*`, `vnm_terminal_terminal_modes`, and
  `vnm_terminal_viewport`: screen model behavior, SGR, model operations,
  alternate screen, private modes, and viewport control.
- `vnm_terminal_backend_session`, `vnm_terminal_windows_conpty_backend`, and
  `vnm_terminal_linux_pty_backend`: session and platform backend behavior.
- `vnm_terminal_qt_*`, `vnm_terminal_render_*`,
  `vnm_terminal_qsg_*`, and `vnm_terminal_shaping_contract`: metrics, render
  snapshots, render frames, QSG rendering, shaping, and QSG text-node checks.
- `vnm_terminal_surface_host`, `vnm_terminal_input_encoder`, and
  `vnm_terminal_behavior_smoke`: public surface host behavior, input encoding,
  and behavior-smoke launches through the canvas fixture.
- `vnm_terminal_compat_smoke`: compatibility smoke behavior when the target
  exists.
- `vnm_terminal_resource_lifecycle`, `vnm_terminal_profiler`,
  `vnm_terminal_qt_posture`, and `vnm_terminal_canvas_fixture_contract`:
  lifecycle, profiling, Qt posture, and fixture protocol tests.
- `vnm_terminal_example_terminal_*`: example host, command-line, capture,
  window chrome, titlebar, and behavior-smoke tests.
- `vnm_terminal_*_conformance`, `vnm_terminal_parser_fuzz_smoke`, and
  `vnm_terminal_libvterm_differential`: optional conformance and differential
  tests controlled by CMake cache variables.
- `vnm_terminal_embedded_benchmark_*`: readiness benchmark validation tests when
  benchmarks are enabled.

Run the configured suite:

```powershell
ctest --test-dir build -C Release --output-on-failure
```

Focused examples:

```powershell
ctest --test-dir build -C Release -R "^vnm_terminal_(parser|sequence_matrix|unicode)" --output-on-failure
ctest --test-dir build -C Release -R "^vnm_terminal_(screen|terminal_modes|viewport)" --output-on-failure
ctest --test-dir build -C Release -R "^vnm_terminal_(backend_session|windows_conpty_backend|linux_pty_backend)$" --output-on-failure
ctest --test-dir build -C Release -R "^vnm_terminal_(qt|render|qsg|shaping)" --output-on-failure
ctest --test-dir build -C Release -R "^vnm_terminal_(surface_host|input_encoder|behavior_smoke)$" --output-on-failure
```

Focused example-terminal smoke tests:

```powershell
ctest --test-dir build -C Release -R "^vnm_terminal_example_terminal_(smoke|custom_titlebar_smoke|native_titlebar_smoke)$" --output-on-failure
```

Default capture replay and parser fuzz smoke are added when
`tests/conformance/captures` exists:

```powershell
ctest --test-dir build -C Release -R "^(vnm_terminal_capture_replay_conformance|vnm_terminal_parser_fuzz_smoke)$" --output-on-failure
```

Single-config generators do not use `-C Release`.

## Conformance Controls

The conformance controls live in `tests/CMakeLists.txt` and are documented in
`tests/conformance/README.md`.

- `VNM_TERMINAL_CAPTURE_REPLAY_DIR` adds
  `vnm_terminal_capture_replay_conformance`.
- `VNM_TERMINAL_FUZZ_SMOKE_CORPUS_DIR` adds
  `vnm_terminal_parser_fuzz_smoke`.
- `VNM_TERMINAL_GRAPHICS_CAPTURE_REPLAY_DIR` adds
  `vnm_terminal_graphics_capture_replay_conformance`.
- `VNM_TERMINAL_UNICODE_CONFORMANCE_DATA_DIR` adds
  `vnm_terminal_unicode_data_conformance`.
- `VNM_TERMINAL_UNICODE_ASPIRATIONAL_CONFORMANCE=ON` adds the aspirational
  Unicode conformance CTest entry.
- `VNM_TERMINAL_LIBVTERM_INCLUDE_DIR`,
  `VNM_TERMINAL_LIBVTERM_LIBRARY`, and
  `VNM_TERMINAL_LIBVTERM_CORPUS_DIR` add the linked libvterm differential
  target.
- `VNM_TERMINAL_LIBVTERM_DIFF_COMMAND` and
  `VNM_TERMINAL_GRAPHICS_CONFORMANCE_COMMAND` add external CTest runners.

Optional strong-copyleft runners are not part of the checked-in test tree. If a
local ignored `more_tests/CMakeLists.txt` exists, the top-level build includes
it after `tests/` so normal and local-only tests can run from one CTest tree.

Semicolon-separated command variables are CMake lists. Put the executable first
and each argument in its own element.

## Benchmarks

Configure without profiling for user-visible timing and structural validation:

```powershell
cmake -S . -B build -DVNM_TERMINAL_BUILD_BENCHMARKS=ON -DBUILD_TESTING=ON
cmake --build build --target vnm_terminal_embedded_benchmark --config Release
ctest --test-dir build -C Release -R "^vnm_terminal_embedded_benchmark_validate$" --output-on-failure
```

Configure a separate profiling build only for attribution and scope timing:

```powershell
cmake -S . -B build-profile -DVNM_TERMINAL_BUILD_BENCHMARKS=ON -DVNM_TERMINAL_ENABLE_PROFILING=ON -DBUILD_TESTING=ON
cmake --build build-profile --target vnm_terminal_embedded_benchmark --config Release
ctest --test-dir build-profile -C Release -R "^vnm_terminal_embedded_benchmark_profile_validate$" --output-on-failure
```

The benchmark supports `--list-scenarios`, repeated `--scenario <name>`,
`--iterations`, `--warmup`, `--grid`, `--window-size`, JSON output,
hierarchical profile output, `--software-renderer`, and `--validate-json`.
Profile flags such as `--profile`, `--profile-json`, and `--profile-text`
require a `VNM_TERMINAL_ENABLE_PROFILING=ON` build.

Benchmark JSON uses `schema_version` 16. Profile JSON uses
`profile_schema_version` 2, `time_unit` `ns`, and
`thread_semantics` `separate_thread_trees`, with separate GUI and render thread
trees. Schema 16 includes text coalescing counters:
`text_coalescing_candidate_groups`, `text_coalescing_enabled_groups`,
`text_resource_runs_before_coalescing`, and
`text_resource_runs_after_coalescing`. Validation enforces
`text_coalescing_enabled_groups <= text_coalescing_candidate_groups` and
`text_resource_runs_after_coalescing <= text_resource_runs_before_coalescing`.

`vnm_terminal_embedded_benchmark_validate` is structural validation under the
offscreen/software renderer path, not a performance comparison. Use no-profile
Release benchmark runs for user-visible timing. Use profiling-enabled Release
runs only for attribution, route counts, and scope timing.

Benchmark comparisons should record the build directory, profiling state,
renderer backend, scenario list, grid, window size, warmup count, iteration
count, command line, output JSON, and any profile artifacts.

## Generated And Provenance Artifacts

- `tools/unicode_width/generate_unicode_width_tables.py` is network-free. With
  `--source-dir`, it consumes Unicode 16.0.0 files and emits deterministic
  generated artifacts. Without a source directory, it emits a representative
  authored table for self-tests and local smoke checks.
- The generated table artifacts are
  `include/vnm_terminal/internal/unicode_width_tables.h` and
  `src/unicode_width_tables.cpp`. The source exposes
  `unicode_width_tables_manifest_json()` with generator command, generator
  version, Unicode version, input URLs, and input hashes.
- `tools/conformance/fetch_unicode_data.ps1` and
  `tools/conformance/fetch_unicode_data.sh` create local Unicode conformance
  data directories with manifests. Unicode source data is not vendored.
- Authored terminal fixtures use provenance headers. The relevant checked-in
  file kinds are `*.vnm_fixture`, `*.vnm_capture`, and `*.vnm_seed`.
- The conformance fixture readers validate checked-in fixture provenance.
  `tests/conformance/captures` stores authored capture replay fixtures, and
  `tests/parser_randomized` stores authored parser seeds.
- `tests/conformance/README.md` defines the fixture, capture, and seed formats.
  Use it before adding or changing fixture data.

## Reference And Licensing Boundaries

The project is BSD-licensed. Dependency and reference boundaries are part of the
build contract:

- Qt Core, Gui, and Quick are allowed through either the commercial route or the
  LGPL-compatible dynamic-linking route. GPL-only Qt modules are not allowed in
  the product dependency graph.
- The renderer uses public Qt Scene Graph and text APIs. Private Qt APIs,
  per-cell `QQuickItem` trees, and production row/frame `QImage` text rendering
  are outside the renderer contract.
- `THIRD_PARTY/*.toml` records reviewed dependency metadata. Qt and Unicode data
  are not vendored dependency source trees.
- `THIRD_PARTY_NOTICES.md` records project notices for Qt and Unicode data.
- `docs/terminal_conformance_oracles.md` records which sources may establish
  terminal behavior.
- `docs/terminal_reference_inventory.md` records external terminal references,
  import decisions, and forbidden uses.
- The parser is owned in-tree. External terminal projects and parser suites may
  inform behavior questions only within the oracle and provenance rules.
- Unicode width behavior is defined by generated Unicode 16.0.0 tables and
  [Unicode width policy](unicode_width_policy.md), not by Qt shaping metrics or
  copied `wcwidth` tables.
- Strong-copyleft tools may be installed and run locally for behavior questions
  when the oracle policy allows it. Do not check in their source, test names,
  transcripts, captured output, byte streams, fixture bytes, or goldens.
- Checked-in fixtures must be independently authored or pass the documented
  provenance gate. Candidate reference projects do not authorize material import
  by themselves.

## Reading Pointers

- Build shape: `CMakeLists.txt`, `tests/CMakeLists.txt`,
  `examples/CMakeLists.txt`, `benchmarks/CMakeLists.txt`.
- Runtime architecture: `docs/architecture.md`.
- Public component API: `docs/public_surface.md` and
  `include/vnm_terminal/vnm_terminal_surface.h`.
- Terminal behavior matrix: `docs/terminal_sequence_matrix.md`.
- Reference and licensing policy: `docs/terminal_conformance_oracles.md`,
  `docs/terminal_reference_inventory.md`, `THIRD_PARTY_NOTICES.md`, and
  `THIRD_PARTY/`.
- Fixture format: `tests/conformance/README.md`.
- Unicode width policy: `docs/unicode_width_policy.md` and
  `tools/unicode_width/generate_unicode_width_tables.py`.
- Qt rendering policy: `docs/qt_rendering_policy.md`.
