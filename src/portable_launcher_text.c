#include "portable_launcher_text.h"

static int append_wide_character(
    wchar_t*  dst,
    size_t    capacity,
    size_t*   offset,
    wchar_t   value)
{
    // One slot is always reserved for the terminating NUL, so a successful write leaves
    // *offset at capacity - 1 at the very most.
    if (*offset + 1U >= capacity) {
        return 0;
    }

    dst[(*offset)++] = value;
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

    unsigned backslashes = 0;
    for (const wchar_t* p = arg; *p; ++p) {
        if (*p == L'\\') {
            backslashes++;
            continue;
        }
        if (*p == L'"') {
            // CommandLineToArgvW reads 2n backslashes before a quote as n literal
            // backslashes and the quote as a delimiter, so every backslash is doubled
            // and the quote itself gets one more escaping backslash.
            for (unsigned i = 0; i < backslashes * 2U + 1U; ++i) {
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
            dst[*offset] = L'\0';
            return 0;
        }
    }

    dst[*offset] = L'\0';
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
    dst[*offset] = L'\0';
    return appended;
}
