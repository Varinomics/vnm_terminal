import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import VNM_Chrome

Basic.SpinBox {
    id: spin

    required property TerminalSettingsStyle settings_style

    implicitWidth: 110
    implicitHeight: 30
    editable: true
    hoverEnabled: true
    font.pixelSize: spin.settings_style.font_pixel_size
    Layout.minimumWidth: implicitWidth
    leftPadding: 26
    rightPadding: 26

    background: VNM_Snapped_rectangle {
        radius: 4
        color: spin.settings_style.field_color
        border_width: 1
        border_color: spin.activeFocus
            ? spin.settings_style.field_focus_color : spin.settings_style.field_border_color
    }

    contentItem: TextInput {
        text: spin.displayText
        font: spin.font
        color: spin.settings_style.value_color
        selectionColor: spin.settings_style.selection_color
        selectedTextColor: spin.settings_style.value_color
        horizontalAlignment: Qt.AlignHCenter
        verticalAlignment: Qt.AlignVCenter
        readOnly: !spin.editable
        validator: spin.validator
        inputMethodHints: Qt.ImhDigitsOnly
        clip: true
    }

    down.indicator: VNM_Snapped_rectangle {
        x: 2
        y: 2
        width: 22
        height: spin.height - 4
        radius: 3
        color: spin.down.pressed ? spin.settings_style.field_border_color
            : down_hover.hovered ? spin.settings_style.row_hover_color : "transparent"

        HoverHandler { id: down_hover }

        Text {
            anchors.centerIn: parent
            text: "\u2212"
            font.pixelSize: spin.settings_style.font_pixel_size
            color: spin.settings_style.label_color
            opacity: spin.value > spin.from ? 1.0 : 0.35
        }
    }

    up.indicator: VNM_Snapped_rectangle {
        x: spin.width - width - 2
        y: 2
        width: 22
        height: spin.height - 4
        radius: 3
        color: spin.up.pressed ? spin.settings_style.field_border_color
            : up_hover.hovered ? spin.settings_style.row_hover_color : "transparent"

        HoverHandler { id: up_hover }

        Text {
            anchors.centerIn: parent
            text: "+"
            font.pixelSize: spin.settings_style.font_pixel_size
            color: spin.settings_style.label_color
            opacity: spin.value < spin.to ? 1.0 : 0.35
        }
    }
}
