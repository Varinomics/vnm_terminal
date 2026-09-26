#!/bin/sh

# Run inside vnm_terminal. Codex's terminal-name heuristics need a SIXEL hint,
# while commands it launches must keep a terminal type available in terminfo.
set -eu

unset TERM_PROGRAM TERM_PROGRAM_VERSION GHOSTTY_RESOURCES_DIR WEZTERM_VERSION WEZTERM_EXECUTABLE \
    ITERM_SESSION_ID ITERM_PROFILE ITERM_PROFILE_NAME TERM_SESSION_ID \
    KITTY_WINDOW_ID ALACRITTY_SOCKET KONSOLE_VERSION GNOME_TERMINAL_SCREEN \
    VTE_VERSION WT_SESSION
TERM=vnm-terminal-sixel
export TERM

exec codex -c 'shell_environment_policy.set.TERM="xterm-256color"' "$@"
