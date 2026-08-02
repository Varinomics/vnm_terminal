#pragma once

#include <stddef.h>

// CreateProcessW accepts at most 32,767 wchar_t for lpCommandLine including the
// terminating NUL, so an assembled command line holds at most this many characters.
#define VNM_TERMINAL_MAX_CMDLINE    32766

// Longest module path GetModuleFileNameW can report, in characters.
#define VNM_TERMINAL_MAX_PATH_CHARS 32767

#ifdef __cplusplus
extern "C" {
#endif

// Appends `text` at `*offset` in `dst`. `capacity` counts the wchar_t the buffer holds,
// including the terminating NUL, and must be at least one.
//
// Returns 1 on success. Returns 0 when the text does not fit; `dst` then holds only the
// characters that did fit and `*offset` indexes their terminating NUL, so the partial
// result is safe to read but must never be used as a command line or a path. Truncating
// and reporting success is not an option, because a truncated command line hands the
// child process a corrupted argument vector and a truncated path silently addresses a
// different file. `dst` is NUL-terminated on both paths.
int portable_launcher_append_text(
    wchar_t*        dst,
    size_t          capacity,
    size_t*         offset,
    const wchar_t*  text);

// Appends `arg` at `*offset` in `dst`, quoted and backslash-escaped so that
// CommandLineToArgvW recovers `arg` unchanged. Same capacity contract and same return
// values as portable_launcher_append_text.
//
// The escaping is not length preserving: a run of backslashes at the end of a quoted
// argument is doubled, so an argument that fits by itself can still fail to append.
int portable_launcher_append_quoted_arg(
    wchar_t*        dst,
    size_t          capacity,
    size_t*         offset,
    const wchar_t*  arg);

#ifdef __cplusplus
}
#endif
