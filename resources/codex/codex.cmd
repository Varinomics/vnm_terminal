@echo off
setlocal DisableDelayedExpansion

rem CMD keeps its own command lookup and argument parsing, including batch-file chaining.
rem Do not CALL the installed command: CALL expands its arguments a second time.
for %%D in (".\@CODEX_COMMAND@.cmd") do if /i "%%~dpD"=="%~dp0" (
    >&2 echo @CODEX_COMMAND@ cannot be launched from the terminal adapter directory.
    exit /b 1
)

rem Work on CMD's in-memory strings so Unicode paths never cross a text-codepage boundary.
set "vnm_codex_path=%PATH:"=%"
set "PATH="
:next_path
set "vnm_codex_entry="
for /f "tokens=1* delims=; eol=|" %%D in ("%vnm_codex_path%") do (
    set "vnm_codex_entry=%%D"
    set "vnm_codex_path=%%E"
)
if not defined vnm_codex_entry goto launch
for %%D in ("%vnm_codex_entry%\@CODEX_COMMAND@.cmd") do if /i "%%~dpD"=="%~dp0" goto next_path
if defined PATH (
    set "PATH=%PATH%;%vnm_codex_entry%"
) else (
    set "PATH=%vnm_codex_entry%"
)
goto next_path

:launch
set "vnm_codex_path="
set "vnm_codex_entry="
set "TERM_PROGRAM="
set "TERM_PROGRAM_VERSION="
set "GHOSTTY_RESOURCES_DIR="
set "WEZTERM_VERSION="
set "WEZTERM_EXECUTABLE="
set "ITERM_SESSION_ID="
set "ITERM_PROFILE="
set "ITERM_PROFILE_NAME="
set "TERM_SESSION_ID="
set "KITTY_WINDOW_ID="
set "ALACRITTY_SOCKET="
set "KONSOLE_VERSION="
set "GNOME_TERMINAL_SCREEN="
set "VTE_VERSION="
set "WT_SESSION="
set "TERM=vnm-terminal-sixel"
@CODEX_COMMAND@ -c "shell_environment_policy.set.TERM='xterm-256color'" %*
endlocal & exit /b %errorlevel%
