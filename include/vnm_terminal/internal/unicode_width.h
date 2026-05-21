#pragma once

#include "vnm_terminal/internal/unicode_width_tables.h"

#include <QByteArrayView>
#include <QString>
#include <QtGlobal>
#include <cstdint>
#include <vector>

namespace vnm_terminal::internal {

constexpr int k_unicode_width_version_major = 16;
constexpr int k_unicode_width_version_minor = 0;
constexpr int k_unicode_width_version_patch = 0;

enum class Terminal_unicode_width_status
{
    OK,
    OK_WITH_REPLACEMENT,
};

enum class Terminal_unicode_width_class
{
    INVALID,
    ZERO,
    CONTROL,
    NARROW,
    WIDE,
    AMBIGUOUS_NARROW,
    EMOJI_PRESENTATION,
};

enum class Terminal_unicode_presentation
{
    DEFAULT,
    TEXT,
    EMOJI,
};

struct Terminal_codepoint_width
{
    char32_t                      codepoint    = 0U;
    int                           cells        = 0;
    Terminal_unicode_width_class  width_class  = Terminal_unicode_width_class::INVALID;
    Terminal_unicode_presentation presentation = Terminal_unicode_presentation::DEFAULT;
};

struct Terminal_utf8_width_result
{
    Terminal_unicode_width_status      status            = Terminal_unicode_width_status::OK;
    int                                cells             = 0;
    qsizetype                          invalid_offset    = -1;
    int                                replacement_count = 0;
    std::vector<Terminal_codepoint_width>
                                       codepoints;
};

struct Terminal_utf8_decode_step
{
    bool       ok             = false;
    char32_t   codepoint      = 0U;
    qsizetype  bytes_consumed = 0;
};

inline QString unicode_width_table_version()
{
    return QStringLiteral("16.0.0");
}

inline bool is_unicode_scalar_value(char32_t codepoint)
{
    return codepoint <= 0x10ffffU && !(codepoint >= 0xd800U && codepoint <= 0xdfffU);
}

inline bool is_utf8_continuation_byte(unsigned char value)
{
    return (value & 0xc0U) == 0x80U;
}

inline unsigned char byte_at(QByteArrayView bytes, qsizetype offset)
{
    return static_cast<unsigned char>(bytes[offset]);
}

inline Terminal_utf8_decode_step decode_utf8_scalar(QByteArrayView bytes, qsizetype offset)
{
    if (offset < 0 || offset >= bytes.size()) {
        return {};
    }

    const unsigned char first = byte_at(bytes, offset);
    if (first < 0x80U) {
        return {true, first, 1};
    }

    auto continuation = [&](qsizetype continuation_offset) -> unsigned char {
        if (continuation_offset >= bytes.size()) {
            return 0U;
        }
        return byte_at(bytes, continuation_offset);
    };

    auto has_continuation = [&](qsizetype continuation_offset) {
        return
            continuation_offset < bytes.size() &&
            is_utf8_continuation_byte(byte_at(bytes, continuation_offset));
    };

    if (first >= 0xc2U && first <= 0xdfU) {
        if (!has_continuation(offset + 1)) {
            return {};
        }

        const char32_t codepoint =
            (static_cast<char32_t>(first & 0x1fU) << 6U) |
            static_cast<char32_t>(continuation(offset + 1) & 0x3fU);
        return {true, codepoint, 2};
    }

    if (first >= 0xe0U && first <= 0xefU) {
        if (!has_continuation(offset + 1) || !has_continuation(offset + 2)) {
            return {};
        }

        const unsigned char second = continuation(offset + 1);
        if ((first == 0xe0U && second < 0xa0U) || (first == 0xedU && second >= 0xa0U)) {
            return {};
        }

        const char32_t codepoint =
            (static_cast<char32_t>(first & 0x0fU) << 12U) |
            (static_cast<char32_t>(second & 0x3fU) << 6U) |
            static_cast<char32_t>(continuation(offset + 2) & 0x3fU);
        return {true, codepoint, 3};
    }

    if (first >= 0xf0U && first <= 0xf4U) {
        if (!has_continuation(offset + 1) || !has_continuation(offset + 2) || !has_continuation(offset + 3))
        {
            return {};
        }

        const unsigned char second = continuation(offset + 1);
        if ((first == 0xf0U && second < 0x90U) || (first == 0xf4U && second > 0x8fU)) {
            return {};
        }

        const char32_t codepoint =
            (static_cast<char32_t>(first & 0x07U) << 18U)                   |
            (static_cast<char32_t>(second & 0x3fU) << 12U)                  |
            (static_cast<char32_t>(continuation(offset + 2) & 0x3fU) << 6U) |
            static_cast<char32_t>(continuation(offset + 3) & 0x3fU);
        return {true, codepoint, 4};
    }

    return {};
}

inline bool is_terminal_combining_codepoint(char32_t codepoint)
{
    return
        unicode_width_is_zero_width(codepoint) &&
        !((codepoint >= 0xfe00U && codepoint <= 0xfe0fU) || (codepoint >= 0xe0100U && codepoint <= 0xe01efU));
}

inline bool is_terminal_variation_selector(char32_t codepoint)
{
    return (codepoint >= 0xfe00U && codepoint <= 0xfe0fU) ||
        (codepoint >= 0xe0100U && codepoint <= 0xe01efU);
}

inline bool is_terminal_text_variation_selector(char32_t codepoint)
{
    return codepoint == 0xfe0eU;
}

inline bool is_terminal_emoji_variation_selector(char32_t codepoint)
{
    return codepoint == 0xfe0fU;
}

inline bool is_terminal_cjk_wide_codepoint(char32_t codepoint)
{
    return unicode_width_is_wide(codepoint);
}

inline bool is_terminal_ambiguous_narrow_codepoint(char32_t codepoint)
{
    return unicode_width_is_ambiguous(codepoint);
}

inline bool is_terminal_default_emoji_codepoint(char32_t codepoint)
{
    return unicode_width_is_default_emoji(codepoint);
}

inline bool is_terminal_emoji_variation_base(char32_t codepoint)
{
    return unicode_width_is_emoji_variation_base(codepoint);
}

inline Terminal_codepoint_width width_for_codepoint(
    char32_t                       codepoint,
    Terminal_unicode_presentation  presentation = Terminal_unicode_presentation::DEFAULT)
{
    if (!is_unicode_scalar_value(codepoint)) {
        return {
            codepoint,
            0,
            Terminal_unicode_width_class::INVALID,
            presentation,
        };
    }

    if (codepoint < 0x20U || (codepoint >= 0x7fU && codepoint <= 0x9fU)) {
        return {
            codepoint,
            0,
            Terminal_unicode_width_class::CONTROL,
            presentation,
        };
    }

    if (codepoint < 0x7fU) {
        return {
            codepoint,
            1,
            Terminal_unicode_width_class::NARROW,
            presentation,
        };
    }

    if (is_terminal_combining_codepoint(codepoint) ||
        is_terminal_variation_selector(codepoint))
    {
        return {
            codepoint,
            0,
            Terminal_unicode_width_class::ZERO,
            presentation,
        };
    }

    if (presentation == Terminal_unicode_presentation::TEXT &&
        is_terminal_emoji_variation_base(codepoint))
    {
        return {
            codepoint,
            1,
            Terminal_unicode_width_class::NARROW,
            presentation,
        };
    }

    if ((presentation == Terminal_unicode_presentation::EMOJI &&
        is_terminal_emoji_variation_base(codepoint))
        ||
        is_terminal_default_emoji_codepoint(codepoint))
    {
        return {
            codepoint,
            2,
            Terminal_unicode_width_class::EMOJI_PRESENTATION,
            presentation,
        };
    }

    if (is_terminal_cjk_wide_codepoint(codepoint)) {
        return {
            codepoint,
            2,
            Terminal_unicode_width_class::WIDE,
            presentation,
        };
    }

    if (is_terminal_ambiguous_narrow_codepoint(codepoint)) {
        return {
            codepoint,
            1,
            Terminal_unicode_width_class::AMBIGUOUS_NARROW,
            presentation,
        };
    }

    return {
        codepoint,
        1,
        Terminal_unicode_width_class::NARROW,
        presentation,
    };
}

inline void apply_variation_to_previous(
    Terminal_utf8_width_result&    result,
    Terminal_unicode_presentation  presentation)
{
    if (result.codepoints.size() < 2U) {
        return;
    }

    Terminal_codepoint_width& previous = result.codepoints[result.codepoints.size() - 2U];
    if (!is_terminal_emoji_variation_base(previous.codepoint)) {
        return;
    }

    result.cells -= previous.cells;
    previous      = width_for_codepoint(previous.codepoint, presentation);
    result.cells += previous.cells;
}

inline Terminal_utf8_width_result measure_utf8_width(QByteArrayView bytes)
{
    Terminal_utf8_width_result result;

    for (qsizetype offset = 0; offset < bytes.size();) {
        const Terminal_utf8_decode_step step = decode_utf8_scalar(bytes, offset);
        if (!step.ok) {
            if (result.status == Terminal_unicode_width_status::OK) {
                result.status         = Terminal_unicode_width_status::OK_WITH_REPLACEMENT;
                result.invalid_offset = offset;
            }

            Terminal_codepoint_width replacement = width_for_codepoint(0xfffdU);
            result.codepoints.push_back(replacement);
            result.cells += replacement.cells;
            ++result.replacement_count;
            ++offset;
            continue;
        }

        Terminal_codepoint_width width = width_for_codepoint(step.codepoint);
        if (is_terminal_text_variation_selector(step.codepoint)) {
            width.presentation = Terminal_unicode_presentation::TEXT;
        }
        else
        if (is_terminal_emoji_variation_selector(step.codepoint)) {
            width.presentation = Terminal_unicode_presentation::EMOJI;
        }

        result.codepoints.push_back(width);
        result.cells += width.cells;

        if (is_terminal_text_variation_selector(step.codepoint)) {
            apply_variation_to_previous(result, Terminal_unicode_presentation::TEXT);
        }
        else
        if (is_terminal_emoji_variation_selector(step.codepoint)) {
            apply_variation_to_previous(result, Terminal_unicode_presentation::EMOJI);
        }

        offset += step.bytes_consumed;
    }

    return result;
}

}
