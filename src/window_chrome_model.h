#pragma once

#include <QChar>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QtGlobal>

#include <algorithm>
#include <array>
#include <cmath>
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

enum class Window_chrome_button_role
{
    MINIMIZE,
    MAXIMIZE_RESTORE,
    CLOSE,
};

enum class Window_chrome_button_state
{
    NORMAL,
    HOVERED,
    PRESSED,
    DISABLED,
};

struct Leading_activity_marker
{
    QString                        marker;
    qsizetype                      code_unit_count = 0;
};

struct Window_chrome_title_content
{
    std::optional<QString>         spinner;
    QString                        display_title;
};

struct Window_chrome_button_states
{
    Window_chrome_button_state     minimize         = Window_chrome_button_state::NORMAL;
    Window_chrome_button_state     maximize_restore = Window_chrome_button_state::NORMAL;
    Window_chrome_button_state     close            = Window_chrome_button_state::NORMAL;
    bool                           window_maximized = false;
};

struct Window_chrome_layout_metrics
{
    qreal                          horizontal_padding = 8.0;
    qreal                          icon_size          = 16.0;
    qreal                          icon_spinner_gap   = 6.0;
    qreal                          spinner_slot_width = 18.0;
    qreal                          title_gap          = 8.0;
    qreal                          title_button_gap   = 8.0;
    qreal                          wheel_delivery_indicator_size       = 7.0;
    qreal                          wheel_delivery_indicator_title_gap  = 8.0;
    qreal                          wheel_delivery_indicator_button_gap = 8.0;
    qreal                          button_width       = 46.0;
};

struct Window_chrome_button_geometry
{
    Window_chrome_button_role      role  = Window_chrome_button_role::MINIMIZE;
    Window_chrome_button_state     state = Window_chrome_button_state::NORMAL;
    QRectF                         rect;
};

struct Window_chrome_layout
{
    QRectF                         icon_rect;
    QRectF                         spinner_slot_rect;
    QRectF                         title_text_rect;
    QRectF                         wheel_delivery_indicator_rect;
    bool                           window_maximized = false;
    std::array<Window_chrome_button_geometry, 3> buttons = {};
};

struct Window_chrome_model
{
    Window_chrome_title_content    title;
    Window_chrome_layout           layout;
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

inline Window_chrome_title_content derive_window_chrome_title_content(
    const QString& terminal_title,
    const QString& terminal_icon_name)
{
    const Leading_activity_marker title_marker = leading_activity_marker(terminal_title);
    const Leading_activity_marker icon_marker  = leading_activity_marker(terminal_icon_name);

    Window_chrome_title_content content;
    if (has_activity_marker(icon_marker)) {
        content.spinner = icon_marker.marker;
    }
    else
    if (has_activity_marker(title_marker)) {
        content.spinner = title_marker.marker;
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

inline qreal nonnegative(qreal value)
{
    return std::max<qreal>(0.0, value);
}

inline Window_chrome_layout calculate_window_chrome_layout(
    const QSizeF&                  titlebar_size,
    Window_chrome_button_states    button_states = {},
    Window_chrome_layout_metrics   metrics = {})
{
    const qreal titlebar_width  = nonnegative(titlebar_size.width());
    const qreal titlebar_height = nonnegative(titlebar_size.height());
    const qreal icon_size       = std::min(nonnegative(metrics.icon_size), titlebar_height);

    const qreal icon_x          = nonnegative(metrics.horizontal_padding);
    const qreal icon_y          = (titlebar_height - icon_size) / 2.0;

    const qreal spinner_slot_x  =
        icon_x + icon_size + nonnegative(metrics.icon_spinner_gap);
    const qreal title_text_x =
        spinner_slot_x +
        nonnegative(metrics.spinner_slot_width) +
        nonnegative(metrics.title_gap);

    const qreal button_width = nonnegative(metrics.button_width);
    // Extremely narrow windows keep the intrinsic button sequence and rely on
    // window/item clipping instead of collapsing control hit regions.
    const qreal buttons_left = nonnegative(titlebar_width - button_width * 3.0);
    const qreal title_button_gap = nonnegative(metrics.title_button_gap);
    const qreal indicator_size =
        std::min(nonnegative(metrics.wheel_delivery_indicator_size), titlebar_height);
    const qreal indicator_title_gap =
        nonnegative(metrics.wheel_delivery_indicator_title_gap);
    const qreal indicator_button_gap =
        nonnegative(metrics.wheel_delivery_indicator_button_gap);
    const qreal indicator_left =
        buttons_left - indicator_button_gap - indicator_size;
    const bool indicator_fits =
        indicator_size > 0.0 &&
        titlebar_height > 0.0 &&
        indicator_left >= title_text_x + indicator_title_gap;
    const qreal title_text_right = indicator_fits
        ? indicator_left - indicator_title_gap
        : buttons_left - title_button_gap;
    const qreal title_text_width = nonnegative(title_text_right - title_text_x);

    Window_chrome_layout layout;
    layout.icon_rect         = QRectF(icon_x, icon_y, icon_size, icon_size);
    layout.spinner_slot_rect = QRectF(
        spinner_slot_x,
        0.0,
        nonnegative(metrics.spinner_slot_width),
        titlebar_height);
    layout.title_text_rect   = QRectF(
        title_text_x,
        0.0,
        title_text_width,
        titlebar_height);
    if (indicator_fits) {
        layout.wheel_delivery_indicator_rect = QRectF(
            indicator_left,
            (titlebar_height - indicator_size) / 2.0,
            indicator_size,
            indicator_size);
    }
    layout.window_maximized  = button_states.window_maximized;
    layout.buttons           = {
        Window_chrome_button_geometry{
            Window_chrome_button_role::MINIMIZE,
            button_states.minimize,
            QRectF(buttons_left, 0.0, button_width, titlebar_height),
        },
        Window_chrome_button_geometry{
            Window_chrome_button_role::MAXIMIZE_RESTORE,
            button_states.maximize_restore,
            QRectF(buttons_left + button_width, 0.0, button_width, titlebar_height),
        },
        Window_chrome_button_geometry{
            Window_chrome_button_role::CLOSE,
            button_states.close,
            QRectF(buttons_left + button_width * 2.0, 0.0, button_width, titlebar_height),
        },
    };
    return layout;
}

inline Window_chrome_model make_window_chrome_model(
    const QString&                 terminal_title,
    const QString&                 terminal_icon_name,
    const QSizeF&                  titlebar_size,
    Window_chrome_button_states    button_states = {},
    Window_chrome_layout_metrics   metrics = {})
{
    return {
        derive_window_chrome_title_content(terminal_title, terminal_icon_name),
        calculate_window_chrome_layout(titlebar_size, button_states, metrics),
    };
}

inline qsizetype title_capacity_code_units(
    const Window_chrome_layout&    layout,
    qreal                          code_unit_width)
{
    if (code_unit_width <= 0.0) {
        return 0;
    }

    return static_cast<qsizetype>(
        std::floor(nonnegative(layout.title_text_rect.width()) / code_unit_width));
}

// These helpers are intentionally code-unit based for pure model tests. The
// painted titlebar must use font metrics for production text elision.
inline QString elide_display_title_to_code_units(
    const QString& display_title,
    qsizetype      max_code_units)
{
    if (max_code_units <= 0) {
        return {};
    }

    if (display_title.size() <= max_code_units) {
        return display_title;
    }

    const QChar ellipsis(0x2026);
    if (max_code_units == 1) {
        return QString(ellipsis);
    }

    QString prefix = display_title.left(max_code_units - 1);
    if (!prefix.isEmpty()) {
        constexpr ushort k_high_surrogate_first = 0xd800;
        constexpr ushort k_high_surrogate_last  = 0xdbff;
        const ushort     last_code_unit         = prefix.back().unicode();
        if (last_code_unit >= k_high_surrogate_first &&
            last_code_unit <= k_high_surrogate_last)
        {
            prefix.chop(1);
        }
    }

    prefix.append(ellipsis);
    return prefix;
}

inline QString elide_window_chrome_title(
    const Window_chrome_title_content& title,
    const Window_chrome_layout&        layout,
    qreal                              code_unit_width)
{
    return elide_display_title_to_code_units(
        title.display_title,
        title_capacity_code_units(layout, code_unit_width));
}

} // namespace vnm_terminal::terminal_app
