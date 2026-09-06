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

// Atomic assembly: returns 0 without modifying the output if the complete result
// (including its terminating NUL) exceeds in_capacity. Inputs must not overlap output.
int portable_launcher_join_text(
    const wchar_t* in_left,
    const wchar_t* in_separator,
    const wchar_t* in_right,
    size_t in_capacity,
    wchar_t* out_text);

// Replaces argv[0] with in_target_path and forwards argv[1..in_argc). Uses the same
// escaping as append_quoted_arg, but rejects an oversized whole command atomically.
// Inputs must not overlap output; in_capacity includes the terminating NUL.
int portable_launcher_build_command_line(
    const wchar_t* in_target_path,
    int in_argc,
    wchar_t* const* in_argv,
    size_t in_capacity,
    wchar_t* out_command_line);

// in_length is the path length excluding NUL; both Windows path separators are accepted.
void portable_launcher_trim_to_directory(size_t in_length, wchar_t* out_path);

// GetModuleFileNameW returns the buffer capacity when the reported path is truncated.
int portable_launcher_module_path_is_complete(size_t in_length, size_t in_capacity);

#ifdef __cplusplus
}
#endif
