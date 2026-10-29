# vnm_terminal

[![CI Linux](https://github.com/Varinomics/vnm_terminal/actions/workflows/ci-linux.yml/badge.svg?branch=master)](https://github.com/Varinomics/vnm_terminal/actions/workflows/ci-linux.yml) [![CI Windows](https://github.com/Varinomics/vnm_terminal/actions/workflows/ci-windows.yml/badge.svg?branch=master)](https://github.com/Varinomics/vnm_terminal/actions/workflows/ci-windows.yml) [![CI macOS](https://github.com/Varinomics/vnm_terminal/actions/workflows/ci-macos.yml/badge.svg?branch=master)](https://github.com/Varinomics/vnm_terminal/actions/workflows/ci-macos.yml)

`vnm_terminal` is a terminal emulator for Windows, macOS, and Linux. Each
window runs your default shell or a command you choose.

**[Download the latest release](https://github.com/Varinomics/vnm_terminal/releases/latest)**

https://github.com/user-attachments/assets/8e9f8d95-cac4-434f-8612-d7b36ef34d94

*An interactive session, including appearance and behavior settings.*

## Features

- GPU-accelerated text rendering, Unicode support, and inline SIXEL graphics.
- Configurable fonts, color schemes, scrollback, and window appearance.
- Search through terminal output, with next and previous result navigation.
- Text selection, configurable paste shortcuts, IME input, and clickable links.

## Downloads

| Platform | Packages | Notes |
| --- | --- | --- |
| Windows x64 | Signed EXE installer, portable ZIP | The installer adds a Start Menu shortcut; the ZIP runs without installation. |
| Linux x64 | RUN installer, DEB, RPM, AppImage | Choose a native package for your distribution or AppImage for portable use. |
| macOS Intel (x64), Apple Silicon (arm64) | Application ZIP per architecture | Requires macOS 14.4 or newer. Choose the ZIP matching your Mac. |

Packages include the Qt runtime. Source archives are available on the
[release page](https://github.com/Varinomics/vnm_terminal/releases/latest).

The macOS bundles are ad-hoc signed and not Apple-notarized; Gatekeeper may
quarantine them on first launch.

The Linux RUN installer installs under `/opt/vnm_terminal` and adds the
`vnm_terminal` command and a desktop launcher. Its installed
`vnm_terminal_maintenance` tool uninstalls it.

## Everyday use

Open the application to start your default shell. Appearance and behavior
options are available in **Settings**.

| Action | Default interaction |
| --- | --- |
| Search | `Ctrl+F` (`Cmd+F` on macOS). `Enter` or `F3` goes to the next result; add `Shift` for the previous result. |
| Open a link | `Ctrl+left-click` on an HTTP, HTTPS, or mailto link. |
| Paste | `Ctrl+V` or `Ctrl+Shift+V`; configurable in Settings. On Windows, pasting while the clipboard holds an image and no text sends `Alt+V` to the program, which is how Claude Code and Codex attach a clipboard image; the file reference a screenshot tool adds beside the image does not count as text. |
| Copy on selection | Enable under **Settings > Behavior**; disabled by default. |
| Rename the window | Alt+left-click the built-in titlebar, edit, then press `Enter`. |

To launch a specific command, put it after `--`:

```text
vnm_terminal [options] [-- command [arguments...]]
```

For example, on Windows:

```powershell
vnm_terminal --window-size 1000x640 -- cmd.exe
```

Use `--native-titlebar` for the platform's window frame, or `--help` for all
command-line options.

With SIXEL enabled, run the installed `vnm-codex` or `vnm-codex-pet` helper
inside your shell for Codex graphics in the same window. They run your installed
Codex command and return to the shell when it exits. Direct launches such as
`vnm_terminal -- codex` also work.

The terminal leaves PATH unchanged. If the helper's directory is not already
on PATH, use its full path. For example, in PowerShell:

```powershell
& 'C:\Tools\vnm_terminal\vnm-codex.ps1'
& 'C:\Tools\vnm_terminal\vnm-codex-pet.ps1'
```

Use the `.cmd` files in CMD. Linux and macOS helpers have no extension; macOS
bundles keep them in `vnm_terminal.app/Contents/Resources/bin`. See
[Codex launcher locations and shell behavior](docs/development.md#installed-codex-launchers).

## Development

The application uses
[`vnm_terminal_surface`](https://github.com/Varinomics/vnm_terminal_surface)
for terminal emulation and rendering.

- [Building, testing, and embedding support](docs/development.md)
- [Packaging and releasing](docs/releasing.md)

## License

GNU General Public License version 3 only. See [LICENSE](LICENSE).
