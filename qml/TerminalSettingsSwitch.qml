import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts

Basic.Switch {
    id: sw

    required property TerminalSettingsStyle settings_style

    implicitWidth: 38
    implicitHeight: 30
    hoverEnabled: true
    leftPadding: 0
    rightPadding: 0
    topPadding: 0
    bottomPadding: 0
    Layout.minimumWidth: implicitWidth

    indicator: Rectangle {
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        implicitWidth: 38
        implicitHeight: 20
        radius: height / 2
        color: sw.settings_style.dark_mode
            ? (sw.checked ? "#33598a" : "#222a35")
            : (sw.checked ? sw.settings_style.switch_on_color : sw.settings_style.field_hover_color)
        border.width: 1
        border.color: sw.settings_style.dark_mode
            ? (sw.checked ? "#46719f" : "#36404e")
            : (sw.checked ? sw.settings_style.switch_on_color : sw.settings_style.field_border_color)

        Behavior on color { ColorAnimation { duration: 120 } }

        Rectangle {
            width: 14
            height: 14
            radius: 7
            anchors.verticalCenter: parent.verticalCenter
            x: sw.checked ? parent.width - width - 3 : 3
            color: !sw.settings_style.dark_mode && !sw.checked
                ? (sw.pressed ? "#202834" : sw.hovered ? "#465366" : "#596575")
                : (sw.pressed ? "#c4d2e2" : sw.hovered ? "#eef4fb" : "#dfe9f5")

            Behavior on x {
                NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
            }
        }
    }
}
