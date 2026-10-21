import QtQuick

QtObject {
    required property int font_pixel_size
    required property string icon_font_family
    property bool dark_mode: true

    readonly property int caption_font_pixel_size: Math.max(1, Math.round(font_pixel_size * 10 / 12))
    readonly property int hint_font_pixel_size: Math.max(1, Math.round(font_pixel_size * 11 / 12))
    readonly property color window_color: dark_mode ? "#202020" : "#f4f4f4"

    readonly property color section_color:      dark_mode ? "#8d99a8" : "#465366"
    readonly property color label_color:        dark_mode ? "#9aa4b2" : "#465366"
    readonly property color value_color:        dark_mode ? "#dfe5ee" : "#202834"
    readonly property color hint_color:         dark_mode ? "#6c7886" : "#596575"
    readonly property color warning_color:      dark_mode ? "#d6b25a" : "#896315"
    readonly property color accent_color:       dark_mode ? "#71b4ff" : "#638ceb"
    readonly property color switch_on_color:    dark_mode ? accent_color : "#4472c4"
    readonly property color separator_color:    dark_mode ? "#343434" : "#d0d0d0"
    readonly property color field_color:        dark_mode ? "#262626" : "#ffffff"
    readonly property color field_hover_color:  dark_mode ? "#2e2e2e" : "#eaeaea"
    readonly property color field_border_color: dark_mode ? "#3a3a3a" : "#b8b8b8"
    readonly property color field_focus_color:  dark_mode ? "#5a5a5a" : "#638ceb"
    readonly property color popup_color:        dark_mode ? "#242424" : "#ffffff"
    readonly property color row_hover_color:    dark_mode ? "#303030" : "#e5e5e5"
    readonly property color card_color:         dark_mode ? "#252525" : "#ffffff"
    readonly property color card_hover_color:   dark_mode ? "#2d2d2d" : "#eaeaea"
    readonly property color card_border_color:  dark_mode ? "#383838" : "#b8b8b8"
    readonly property color selection_color:    dark_mode ? "#2f4a6b" : "#c4dfff"
}
