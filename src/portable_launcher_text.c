#include "portable_launcher_text.h"

static int append_wide_character(
    wchar_t*  dst,
    size_t    capacity,
    size_t*   offset,
    wchar_t   value)
{
    // One slot is always reserved for the terminating NUL, so a successful write leaves
    // *offset at capacity - 1 at the very most.
    if (capacity == 0 || *offset >= capacity - 1U) {
        return 0;
    }

    if (dst) {
        dst[*offset] = value;
    }
    ++*offset;
    return 1;
}

static int append_quoted_arg_characters(
    wchar_t*        dst,
    size_t          capacity,
    size_t*         offset,
    const wchar_t*  arg)
{
    if (!append_wide_character(dst, capacity, offset, L'"')) {
        return 0;
    }

    size_t backslashes = 0;
    for (const wchar_t* p = arg; *p; ++p) {
        if (*p == L'\\') {
            backslashes++;
            continue;
        }
        if (*p == L'"') {
            // CommandLineToArgvW reads 2n backslashes before a quote as n literal
            // backslashes and the quote as a delimiter, so every backslash is doubled
            // and the quote itself gets one more escaping backslash.
            for (size_t i = 0; i < backslashes * 2U + 1U; ++i) {
                if (!append_wide_character(dst, capacity, offset, L'\\')) {
                    return 0;
                }
            }
            if (!append_wide_character(dst, capacity, offset, L'"')) {
                return 0;
            }
            backslashes = 0;
            continue;
        }
        while (backslashes > 0) {
            if (!append_wide_character(dst, capacity, offset, L'\\')) {
                return 0;
            }
            backslashes--;
        }
        if (!append_wide_character(dst, capacity, offset, *p)) {
            return 0;
        }
    }

    // Backslashes that end the argument precede the closing quote, so they double too.
    // This is the one place where the encoding is not length preserving.
    while (backslashes > 0) {
        if (!append_wide_character(dst, capacity, offset, L'\\') ||
            !append_wide_character(dst, capacity, offset, L'\\'))
        {
            return 0;
        }
        backslashes--;
    }

    return append_wide_character(dst, capacity, offset, L'"');
}

int portable_launcher_append_text(
    wchar_t*        dst,
    size_t          capacity,
    size_t*         offset,
    const wchar_t*  text)
{
    for (const wchar_t* p = text; *p; ++p) {
        if (!append_wide_character(dst, capacity, offset, *p)) {
            if (dst) {
                dst[*offset] = L'\0';
            }
            return 0;
        }
    }

    if (dst) {
        dst[*offset] = L'\0';
    }
    return 1;
}

int portable_launcher_append_quoted_arg(
    wchar_t*        dst,
    size_t          capacity,
    size_t*         offset,
    const wchar_t*  arg)
{
    int needs_quotes = arg[0] == L'\0';
    for (const wchar_t* p = arg; *p; ++p) {
        if (*p == L' ' || *p == L'\t' || *p == L'\n' || *p == L'\v' || *p == L'"') {
            needs_quotes = 1;
            break;
        }
    }

    if (!needs_quotes) {
        return portable_launcher_append_text(dst, capacity, offset, arg);
    }

    const int appended = append_quoted_arg_characters(dst, capacity, offset, arg);
    if (dst) {
        dst[*offset] = L'\0';
    }
    return appended;
}

int portable_launcher_join_text(
    const wchar_t* in_left,
    const wchar_t* in_separator,
    const wchar_t* in_right,
    size_t in_capacity,
    wchar_t* out_text)
{
    if (in_capacity == 0) {
        return 0;
    }

    // The same bounded writer measures first, leaving the caller's buffer untouched
    // on rejection. A null destination counts characters without storing them.
    size_t offset = 0;
    if (!portable_launcher_append_text(NULL, in_capacity, &offset, in_left) ||
        !portable_launcher_append_text(NULL, in_capacity, &offset, in_separator) ||
        !portable_launcher_append_text(NULL, in_capacity, &offset, in_right))
    {
        return 0;
    }

    offset = 0;
    portable_launcher_append_text(out_text, in_capacity, &offset, in_left);
    portable_launcher_append_text(out_text, in_capacity, &offset, in_separator);
    portable_launcher_append_text(out_text, in_capacity, &offset, in_right);
    return 1;
}

static int append_command_line(
    const wchar_t* in_target_path,
    int in_argc,
    wchar_t* const* in_argv,
    size_t in_capacity,
    wchar_t* out_command_line)
{
    size_t offset = 0;
    if (!portable_launcher_append_quoted_arg(
            out_command_line, in_capacity, &offset, in_target_path))
    {
        return 0;
    }
    for (int i = 1; i < in_argc; ++i) {
        if (!portable_launcher_append_text(out_command_line, in_capacity, &offset, L" ") ||
            !portable_launcher_append_quoted_arg(
                out_command_line, in_capacity, &offset, in_argv[i]))
        {
            return 0;
        }
    }
    return 1;
}

int portable_launcher_build_command_line(
    const wchar_t* in_target_path,
    int in_argc,
    wchar_t* const* in_argv,
    size_t in_capacity,
    wchar_t* out_command_line)
{
    if (!append_command_line(in_target_path, in_argc, in_argv, in_capacity, NULL)) {
        return 0;
    }
    return append_command_line(in_target_path, in_argc, in_argv, in_capacity, out_command_line);
}

void portable_launcher_trim_to_directory(size_t in_length, wchar_t* out_path)
{
    while (in_length > 0) {
        wchar_t character = out_path[in_length - 1];
        if (character == L'\\' || character == L'/') {
            out_path[in_length - 1] = L'\0';
            return;
        }
        --in_length;
    }
    out_path[0] = L'\0';
}

int portable_launcher_module_path_is_complete(size_t in_length, size_t in_capacity)
{
    return in_length > 0 && in_length < in_capacity;
}
