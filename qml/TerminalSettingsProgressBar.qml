import QtQuick
import QtQuick.Controls.Basic as Basic

Basic.ProgressBar {
    id: progress

    required property TerminalSettingsStyle settings_style

    implicitHeight: 4

    background: Rectangle {
        radius: 2
        color: progress.settings_style.field_color
    }

    contentItem: Item {
        clip: true

        Rectangle {
            width: parent.width * progress.position
            height: parent.height
            radius: 2
            color: progress.settings_style.accent_color
            visible: !progress.indeterminate
        }

        Rectangle {
            id: progress_sweep
            width: parent.width * 0.3
            height: parent.height
            radius: 2
            color: progress.settings_style.accent_color
            visible: progress.indeterminate

            NumberAnimation on x {
                running: progress.indeterminate && progress.visible
                loops: Animation.Infinite
                from: -progress_sweep.width
                to: progress.width
                duration: 1200
            }
        }
    }
}
