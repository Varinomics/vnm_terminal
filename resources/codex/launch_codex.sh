#!/bin/sh

# Codex's terminal-name heuristics need a SIXEL hint,
# while commands it launches must keep a terminal type available in terminfo.
set -eu

codex_command=$1
shift
shim_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
remaining_path=$PATH
original_path=
separator=
while :; do
    path_entry=${remaining_path%%:*}
    if [ "$path_entry" != "$shim_directory" ]; then
        original_path=$original_path$separator$path_entry
        separator=:
    fi
    case $remaining_path in
        *:*) remaining_path=${remaining_path#*:} ;;
        *) break ;;
    esac
done
PATH=$original_path
export PATH

unset TERM_PROGRAM TERM_PROGRAM_VERSION GHOSTTY_RESOURCES_DIR WEZTERM_VERSION WEZTERM_EXECUTABLE \
    ITERM_SESSION_ID ITERM_PROFILE ITERM_PROFILE_NAME TERM_SESSION_ID \
    KITTY_WINDOW_ID ALACRITTY_SOCKET KONSOLE_VERSION GNOME_TERMINAL_SCREEN \
    VTE_VERSION WT_SESSION
TERM=vnm-terminal-sixel
export TERM

exec "$codex_command" -c "shell_environment_policy.set.TERM='xterm-256color'" "$@"
