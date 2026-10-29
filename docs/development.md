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

## Codex integration for embedding applications

Link `vnm_terminal::vnm_terminal_app_support` and include
[`<vnm_terminal/app_support/codex_command_environment.h>`](../include/vnm_terminal/app_support/codex_command_environment.h).
Call `Codex_command_environment::prepare()` on the final command and
environment before starting the backend. Keep that object alive for the
terminal session; it owns the session's private command directory.

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
Shell aliases, functions, and explicit paths inside a shell can bypass the
adapter; multiplexer graphics restrictions still apply.

The app support library bundles readable scripts. On Windows, session-local
`.cmd` commands serve CMD and `.ps1` commands serve PowerShell; POSIX sessions
use shell scripts. The adapter directory is prepended to that session's PATH,
and the adapters remove it before resolving the installed command. No native
executable is extracted or compiled at runtime. Rebuild embedding applications
to pick up changes; no separate adapter deployment is needed.

CMD adapters follow ordinary batch argument parsing and command-length limits.
Quote prompts and paths containing CMD metacharacters. PowerShell and direct
launches preserve argument arrays through the PowerShell script, including
verified interpreter prefixes; Windows PowerShell 5.1 is supported.

WSL and SSH start programs in another environment. The local adapter directory
does not install adapters into a distribution or a remote host. Those sessions
need scripts on the destination if Codex-specific graphics adaptation is wanted;
their normal terminal identity is not changed by a local Codex launch.
