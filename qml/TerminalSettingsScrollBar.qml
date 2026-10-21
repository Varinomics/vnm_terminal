import QtQuick
import QtQuick.Controls.Basic as Basic

Basic.ScrollBar {
    id: bar

    required property TerminalSettingsStyle settings_style

    implicitWidth: 8
    implicitHeight: 8
    padding: 2
    policy: Basic.ScrollBar.AsNeeded
    visible: bar.size < 1.0

    background: Rectangle {
        color: "transparent"
    }

    contentItem: Rectangle {
        implicitWidth: 4
        radius: 2
        color: bar.pressed ? bar.settings_style.label_color : bar.settings_style.hint_color
    }
}
