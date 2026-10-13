#!/bin/sh

# Qt uses argv[0] to locate qt.conf, so the global symlink must resolve to a
# launcher that executes the real binary from its installed directory.
launcher_path=$(readlink -f -- "$0") || exit 1
exec "$(dirname -- "$launcher_path")/vnm_terminal" "$@"
