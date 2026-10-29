# Developing vnm_terminal

## Source builds

Builds require CMake 3.25 or newer, a C++20 compiler, and Qt 6.12 or newer.
Place `vnm_terminal_surface`, `vnm_qml_chrome`, and `vnm_fonts` beside this
repository, or set `VNM_TERMINAL_SURFACE_SOURCE_DIR`,
`VNM_QML_CHROME_SOURCE_DIR`, and `FETCHCONTENT_SOURCE_DIR_VNM_FONTS` for custom
locations. Without a local checkout, `vnm_fonts` is fetched from `master`.

The surface must provide the
`vnm_terminal_surface::vnm_terminal_surface` target. The chrome version
requirement is declared in
[`cmake/vnm_qml_chrome_dependency.cmake`](../cmake/vnm_qml_chrome_dependency.cmake).

On Linux or macOS, with Qt available through `CMAKE_PREFIX_PATH`:

```sh
queued-build --slots 1 -- cmake -S . -B build/development -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
queued-build --slots 4 -- cmake --build build/development --parallel 4
ctest --test-dir build/development --output-on-failure
```

For the Windows portable build, copy
[`build_config.bat.example`](../build_config.bat.example) to `build_config.bat`
and set the Qt, MinGW, and dependency paths, including the source-built ConPTY
runtime described in [the packaging instructions](releasing.md#local-packaging).
Then run:

```powershell
queued-build --slots 4 -- cmd.exe /d /c build_portable.bat
```

The executable and deployed Qt runtime form one launchable build artifact;
keep them together. See [AGENTS.md](../AGENTS.md) for repository toolchain rules
and [releasing.md](releasing.md) for distribution builds.

## Tests and diagnostics

Automated runs must not write the user's persisted settings. Defining
`VNM_TERMINAL_SETTINGS_NO_PERSIST` disables window and appearance persistence,
even if its value is `0` or empty. Remove it to restore persistence. The
offscreen Qt platform also disables persistence, and test registrations set
the variable themselves.

Transcript capture/replay is opt-in for local diagnostics through
`-DVNM_TERMINAL_ENABLE_TRANSCRIPT_CAPTURE_REPLAY=ON`. Distribution builds use
`-DVNM_TERMINAL_DISTRIBUTION_BUILD=ON` and reject transcript capture.

## QML runtime

Application QML and `VNM_Chrome` use embedded compiled QML units. Keep
`QT_QML_NO_CACHEGEN=OFF`; configure rejects disabling generation. Release
omits QML source text from resources, so rebuild the application and its QML
dependencies when changing Qt. QML debugging that disables compiled units
cannot load Release components without source fallback.

Before constructing Qt application or QML engine objects, the standalone
application sets `QML_DISK_CACHE=aot` and removes `QML_FORCE_DISK_CACHE` and
`QML_DISABLE_DISK_CACHE`. This uses embedded units without reading or writing
generated `.qmlc` disk caches. Child commands retain the caller's original
environment. Embedding hosts must apply this policy themselves; the libraries
do not modify their host's environment.

## Clipboard integration for hosted Windows terminals

Link `vnm_terminal::vnm_terminal_app_support`, include
`<vnm_terminal/app_support/clipboard_reader.h>`, and call
`install_hosted_clipboard_reader(surface)` when creating each terminal surface.
Deploy the `vnm_terminal_clipboard_broker` target beside the executable that
hosts the surface. The helper needs that host's Qt platform-plugin deployment
and `qt.conf` paths. Both the reader and helper build with
`VNM_TERMINAL_BUILD_STANDALONE_APP=OFF`.

Hosts with independently deployed Qt layouts must give their broker a distinct
output filename and pass its program path to `install_hosted_clipboard_reader`.
This keeps each host's broker and embedded Qt paths under its own deployment
ownership when the hosts share a runtime directory.

The helper reads the local clipboard outside the GUI process. Real text takes
precedence over an image; an image without a Windows text format sends Alt+V,
including when a screenshot tool also supplies a file reference. The receiving
CLI must support that image-paste shortcut. Cancelling a read or destroying the
surface suppresses its callbacks. Other platforms keep the ordinary Qt paste
path. The standalone application's existing self-dispatch remains unchanged.

Configure `VNM_TERMINAL_APP_SUPPORT_BUILD_TESTS=ON` to build the focused reader
tests without the standalone application. The Windows
`vnm_terminal_hosted_clipboard` test runs the deployed helper against the native
clipboard and exercises the installed reader through a terminal surface.

## Installed Codex launchers

`vnm-codex` and `vnm-codex-pet` run the corresponding installed Codex command
in the current terminal. They forward arguments and the working directory,
apply the SIXEL environment to Codex, and preserve the calling shell's
environment. The terminal does not change PATH or install shell functions.
Use the helper's full path when its directory is not on PATH.

| Distribution | Helper location |
| --- | --- |
| Windows installer or portable ZIP | `vnm-codex.cmd` / `vnm-codex.ps1` and their `vnm-codex-pet` counterparts beside `vnm_terminal.exe` |
| Linux DEB, RPM, or CMake install | `vnm-codex` and `vnm-codex-pet` in the installation's `bin` directory |
| Linux RUN installer | `/opt/vnm_terminal/bin/vnm-codex` and `/opt/vnm_terminal/bin/vnm-codex-pet` with the default installation directory |
| Linux AppImage | `usr/bin/vnm-codex` and `usr/bin/vnm-codex-pet` inside the mounted or extracted AppImage |
| macOS application bundle | `vnm_terminal.app/Contents/Resources/bin/vnm-codex` and `vnm_terminal.app/Contents/Resources/bin/vnm-codex-pet` |

Keep the adjacent `launch_codex.sh` or `launch_codex.ps1` with the launchers.
PowerShell uses the `.ps1` commands and supports Windows PowerShell 5.1 and
PowerShell 7. CMD uses `.cmd` commands with ordinary batch argument parsing
and command-length limits; quote prompts and paths containing metacharacters.

WSL and SSH start programs in another environment. Install and invoke the
scripts in the distribution or remote host where Codex runs. Local helpers do
not propagate there. Ordinary shells retain `TERM=xterm-256color`; no custom
terminal description is needed on the destination.

## Codex integration for embedding applications

Link `vnm_terminal::vnm_terminal_app_support` and include
[`<vnm_terminal/app_support/codex_command_environment.h>`](../include/vnm_terminal/app_support/codex_command_environment.h).
Call `Codex_command_environment::prepare()` on the final command and
environment before starting the backend. Keep that object alive for the
terminal session; it owns the scripts prepared for a direct Codex launch.
Other commands and their environments pass through unchanged.

For a verified Codex executable or interpreter invocation, pass
`Codex_invocation{argument_prefix_size}` to `prepare()`. The prefix includes
the absolute executable and any fixed interpreter arguments: one for a native
Codex executable, or two for Node and its verified Codex entry point. On
Windows, the executable must be native. The adapter inserts Codex options
after that prefix and preserves the remaining arguments. Without an explicit
invocation, it detects `codex` and `codex-pet` by executable name.

The adapter enables SIXEL for Codex and passes
`-c 'shell_environment_policy.set.TERM="xterm-256color"'` for commands run
through Codex's shell tools. Codex environment allowlists or later command-line
overrides can change that result. It does not configure external MCP servers.
Multiplexer graphics restrictions still apply.

The app support library bundles the readable launch scripts and materializes
them only for direct Codex launches. It leaves PATH unchanged and creates no
replacement `codex` commands. Rebuild embedding applications to pick up changes;
their direct launches need no separate script deployment. Applications that
offer the explicit shell commands must also deploy the installed launchers.

Direct Windows launches preserve argument arrays through the PowerShell script,
including verified interpreter prefixes. Windows PowerShell 5.1 is supported.
