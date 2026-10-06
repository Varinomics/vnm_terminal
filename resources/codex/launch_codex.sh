#!/bin/sh

# Codex's terminal-name heuristics need a SIXEL hint,
# while commands it launches must keep a terminal type available in terminfo.
set -eu

argument_prefix_size=$1
codex_command=$2
shift 2
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

if [ "$argument_prefix_size" -eq 1 ]; then
    exec "$codex_command" -c "shell_environment_policy.set.TERM='xterm-256color'" "$@"
fi

# Rotate the fixed executable arguments around the CLI tail so options reach
# Codex after its entry point, while preserving every original argument.
prefix_remaining=$((argument_prefix_size - 1))
suffix_remaining=$(($# - prefix_remaining))
while [ "$prefix_remaining" -gt 0 ]; do
    set -- "$@" "$1"
    shift
    prefix_remaining=$((prefix_remaining - 1))
done
set -- "$@" -c "shell_environment_policy.set.TERM='xterm-256color'"
while [ "$suffix_remaining" -gt 0 ]; do
    set -- "$@" "$1"
    shift
    suffix_remaining=$((suffix_remaining - 1))
done
exec "$codex_command" "$@"
