import QtQuick
import QtQuick.Controls.Basic as Basic
import VNM_Chrome

Basic.ComboBox {
    id: combo

    required property TerminalSettingsStyle settings_style

    // Per-row hooks for the popup list: `row_enabled` grays out rows that
    // are not currently selectable, and `use_row_font` renders each row in
    // the font family it names (for font pickers).
    property var  row_enabled: (row) => true
    property bool use_row_font: false

    implicitHeight: 30
    rightPadding: 4
    font.pixelSize: combo.settings_style.font_pixel_size
    hoverEnabled: true

    background: VNM_Snapped_rectangle {
        radius: 4
        color: combo.hovered || combo.down
            ? combo.settings_style.field_hover_color : combo.settings_style.field_color
        border_width: 1
        border_color: combo.activeFocus || combo.down
            ? combo.settings_style.field_focus_color : combo.settings_style.field_border_color

        Behavior on color { ColorAnimation { duration: 100 } }
    }

    contentItem: Text {
        leftPadding: 10
        rightPadding: combo.rightPadding + combo.indicator.width + 5
        text: combo.displayText
        font: combo.font
        color: combo.settings_style.value_color
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    indicator: Text {
        x: combo.width - width - combo.rightPadding
        y: (combo.height - height) / 2 - font.pixelSize / 32
        text: "\uf107"
        font { family: combo.settings_style.icon_font_family; pixelSize: combo.font.pixelSize; }
        color: combo.settings_style.label_color
    }

    delegate: Basic.ItemDelegate {
        id: combo_row

        required property int index
        required property var modelData

        width: ListView.view ? ListView.view.width : 0
        height: 28
        enabled: combo.row_enabled(modelData)
        highlighted: combo.highlightedIndex === index
        hoverEnabled: true

        contentItem: Text {
            leftPadding: 4
            text: combo.textRole ? combo_row.modelData[combo.textRole] : combo_row.modelData
            font.pixelSize: combo.settings_style.font_pixel_size
            font.family: combo.use_row_font ? combo_row.modelData : combo.font.family
            color: !combo_row.enabled ? combo.settings_style.hint_color
                : combo_row.index === combo.currentIndex
                    ? combo.settings_style.accent_color : combo.settings_style.value_color
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }

        background: VNM_Snapped_rectangle {
            radius: 3
            color: combo_row.highlighted ? combo.settings_style.row_hover_color : "transparent"
        }
    }

    popup: Basic.Popup {
        y: combo.height + 4
        width: combo.width
        padding: 6
        implicitHeight: Math.min(
            contentItem.implicitHeight + topPadding + bottomPadding, 280)

        background: VNM_Snapped_rectangle {
            radius: 6
            color: combo.settings_style.popup_color
            border_width: 1
            border_color: combo.settings_style.field_border_color
        }

        contentItem: ListView {
            implicitHeight: contentHeight
            clip: true
            model: combo.popup.visible ? combo.delegateModel : null
            currentIndex: combo.highlightedIndex
            boundsBehavior: Flickable.StopAtBounds

            Basic.ScrollBar.vertical: TerminalSettingsScrollBar { settings_style: combo.settings_style }
        }
    }
}
