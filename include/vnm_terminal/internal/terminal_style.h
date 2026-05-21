#pragma once

#include <QtGlobal>
#include <array>
#include <cstdint>

namespace vnm_terminal::internal {

using Terminal_style_id = std::uint32_t;

constexpr Terminal_style_id k_default_terminal_style_id        = 0U;
constexpr quint32           k_terminal_default_foreground_rgba = 0xffffffffU;
constexpr quint32           k_terminal_default_background_rgba = 0xff000000U;
constexpr quint32           k_terminal_default_cursor_rgba     = 0xffffffffU;

enum class Terminal_color_ref_kind
{
    DEFAULT,
    PALETTE_INDEX,
    RGB,
};

struct Terminal_color_ref
{
    Terminal_color_ref_kind    kind          = Terminal_color_ref_kind::DEFAULT;
    std::uint16_t              palette_index = 0U;
    quint32                    rgba          = k_terminal_default_foreground_rgba;
};

enum class Terminal_style_attribute : std::uint16_t
{
    BOLD      = 1U << 0U,
    FAINT     = 1U << 1U,
    ITALIC    = 1U << 2U,
    UNDERLINE = 1U << 3U,
    BLINK     = 1U << 4U,
    INVERSE   = 1U << 5U,
    INVISIBLE = 1U << 6U,
    STRIKE    = 1U << 7U,
};

struct Terminal_text_style
{
    Terminal_color_ref foreground = {
        Terminal_color_ref_kind::DEFAULT,
        0U,
        k_terminal_default_foreground_rgba,
    };
    Terminal_color_ref background = {
        Terminal_color_ref_kind::DEFAULT,
        0U,
        k_terminal_default_background_rgba,
    };
    std::uint16_t              attributes              = 0U;
};

struct Terminal_color_state
{
    quint32                    default_foreground_rgba = k_terminal_default_foreground_rgba;
    quint32                    default_background_rgba = k_terminal_default_background_rgba;
    quint32                    cursor_rgba             = k_terminal_default_cursor_rgba;
    std::array<quint32, 256>   palette_rgba            = {};
};

constexpr Terminal_color_ref make_default_terminal_color_ref(quint32 rgba = 0U)
{
    return {Terminal_color_ref_kind::DEFAULT, 0U, rgba};
}

constexpr Terminal_color_ref make_palette_terminal_color_ref(std::uint16_t index, quint32 rgba = 0U)
{
    return {Terminal_color_ref_kind::PALETTE_INDEX, index, rgba};
}

constexpr Terminal_color_ref make_rgb_terminal_color_ref(quint32 rgba)
{
    return {Terminal_color_ref_kind::RGB, 0U, rgba};
}

constexpr Terminal_text_style make_default_terminal_text_style()
{
    return {
        make_default_terminal_color_ref(),
        make_default_terminal_color_ref(),
        0U,
    };
}

constexpr std::uint16_t terminal_style_attribute_mask(Terminal_style_attribute attribute)
{
    return static_cast<std::uint16_t>(attribute);
}

constexpr bool terminal_style_has_attribute(
    Terminal_text_style            style,
    Terminal_style_attribute       attribute)
{
    return (style.attributes & terminal_style_attribute_mask(attribute)) != 0U;
}

constexpr quint32 resolve_terminal_color_ref(
    const Terminal_color_ref&      color,
    const Terminal_color_state&    color_state,
    bool                           foreground)
{
    switch (color.kind) {
        case Terminal_color_ref_kind::DEFAULT:
            return foreground
                ? color_state.default_foreground_rgba
                : color_state.default_background_rgba;
        case Terminal_color_ref_kind::PALETTE_INDEX:
            return color.palette_index < color_state.palette_rgba.size()
                ? color_state.palette_rgba[color.palette_index]
                : (foreground
                    ? color_state.default_foreground_rgba
                    : color_state.default_background_rgba);
        case Terminal_color_ref_kind::RGB:
            return color.rgba;
    }

    return foreground
        ? color_state.default_foreground_rgba
        : color_state.default_background_rgba;
}

constexpr void set_terminal_style_attribute(
    Terminal_text_style&           style,
    Terminal_style_attribute       attribute)
{
    style.attributes |= terminal_style_attribute_mask(attribute);
}

constexpr bool operator==(const Terminal_color_ref& left, const Terminal_color_ref& right)
{
    if (left.kind != right.kind) {
        return false;
    }

    switch (left.kind) {
        case Terminal_color_ref_kind::DEFAULT:
            return true;
        case Terminal_color_ref_kind::PALETTE_INDEX:
            return left.palette_index == right.palette_index;
        case Terminal_color_ref_kind::RGB:
            return left.rgba == right.rgba;
    }

    return false;
}

constexpr bool operator==(const Terminal_text_style& left, const Terminal_text_style& right)
{
    return
        left.foreground == right.foreground &&
        left.background == right.background &&
        left.attributes == right.attributes;
}

}
