#pragma once

#include <QChar>
#include <QString>

#include <optional>

namespace vnm_terminal::terminal_app {

constexpr char32_t k_activity_marker_braille_first = 0x2800;
constexpr char32_t k_activity_marker_braille_last  = 0x28ff;
constexpr char32_t k_activity_marker_dingbat_first = 0x2731;
constexpr char32_t k_activity_marker_dingbat_last  = 0x273f;
constexpr char32_t k_activity_marker_block_first   = 0x2596;
constexpr char32_t k_activity_marker_block_last    = 0x259f;

static_assert(!QChar::requiresSurrogates(k_activity_marker_braille_first));
static_assert(!QChar::requiresSurrogates(k_activity_marker_braille_last));
static_assert(!QChar::requiresSurrogates(k_activity_marker_dingbat_first));
static_assert(!QChar::requiresSurrogates(k_activity_marker_dingbat_last));
static_assert(!QChar::requiresSurrogates(k_activity_marker_block_first));
static_assert(!QChar::requiresSurrogates(k_activity_marker_block_last));

struct Leading_activity_marker
{
    QString   marker;
    qsizetype code_unit_count = 0;
};

struct Terminal_title_content
{
    std::optional<QString> activity_marker;
    QString                display_title;
};

inline bool is_activity_marker_codepoint(char32_t codepoint)
{
    return
        (codepoint >= k_activity_marker_braille_first && codepoint <= k_activity_marker_braille_last) ||
        (codepoint >= k_activity_marker_dingbat_first && codepoint <= k_activity_marker_dingbat_last) ||
        (codepoint >= k_activity_marker_block_first && codepoint <= k_activity_marker_block_last);
}

inline Leading_activity_marker leading_activity_marker(const QString& text)
{
    if (text.isEmpty()) {
        return {};
    }

    const char32_t codepoint = text.at(0).unicode();
    if (!is_activity_marker_codepoint(codepoint)) {
        return {};
    }

    return {text.left(1), 1};
}

inline bool has_activity_marker(const Leading_activity_marker& marker)
{
    return marker.code_unit_count > 0;
}

inline Terminal_title_content derive_terminal_title_content(
    const QString& terminal_title,
    const QString& terminal_icon_name)
{
    const Leading_activity_marker title_marker = leading_activity_marker(terminal_title);
    const Leading_activity_marker icon_marker  = leading_activity_marker(terminal_icon_name);

    Terminal_title_content content;
    if (has_activity_marker(icon_marker)) {
        content.activity_marker = icon_marker.marker;
    }
    else if (has_activity_marker(title_marker)) {
        content.activity_marker = title_marker.marker;
    }

    qsizetype display_title_start = 0;
    if (has_activity_marker(title_marker)) {
        display_title_start = title_marker.code_unit_count;
        if (display_title_start < terminal_title.size()) {
            const ushort separator = terminal_title.at(display_title_start).unicode();
            if (separator == ' ' || separator == '\t') {
                ++display_title_start;
            }
        }
    }

    content.display_title = terminal_title.mid(display_title_start);
    return content;
}

inline QString activity_marker_text(const Terminal_title_content& content)
{
    return content.activity_marker.value_or(QString());
}

} // namespace vnm_terminal::terminal_app
