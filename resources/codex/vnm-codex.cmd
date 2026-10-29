@echo off
setlocal DisableDelayedExpansion

rem CMD keeps its own command lookup and argument parsing, including batch-file chaining.
rem Do not CALL the installed command: CALL expands its arguments a second time.
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
