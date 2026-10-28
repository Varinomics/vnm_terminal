import QtQuick
import QtQuick.Window

Item {
    id: root
    objectName: "terminal_search_bar_root"
    anchors.fill: parent
    visible: false

    property alias query_text: query_input.text

    Rectangle {
        id: panel
        objectName: "terminal_search_bar_panel"
        readonly property bool scrollbar_visible:
            terminalSurface.scrollbackRows > 0 &&
            terminalSurface.viewportVisibleRows > 0 &&
            terminalSurface.width > 0 &&
            terminalSurface.height > 0
        readonly property real content_right: scrollbar_visible
            ? terminalSurface.x + terminalSurface.width
            : root.width - terminalSurface.x
        readonly property bool stacked: width < 400

        width: Math.min(460, Math.max(0, content_right - terminalSurface.x))
        height: stacked ? 60 : 30
        x: content_right - width
        y: terminalSurface.y
        color: searchBar.chromeBackgroundColor

        // The window ratio is the live one: the screen attached property never
        // re-notifies on a scale change, which would leave this hairline at the
        // width of the scale that was in force when the window opened.
        readonly property real edge_width:
            1 / (Window.window ? Window.window.devicePixelRatio : Screen.devicePixelRatio)

        Rectangle {
            objectName: "terminal_search_left_edge"
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: panel.edge_width
            color: searchBar.chromeFrameEdgeColor
            z: 1
        }

        Rectangle {
            objectName: "terminal_search_right_edge"
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: panel.edge_width
            color: searchBar.chromeFrameEdgeColor
            z: 1
        }

        Rectangle {
            objectName: "terminal_search_bottom_edge"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: panel.edge_width
            color: searchBar.chromeFrameEdgeColor
            z: 1
        }

        TextInput {
            id: query_input
            objectName: "terminal_search_query_input"
            anchors.left: parent.left
            anchors.leftMargin: 8
            anchors.right: panel.stacked ? dismiss_button.left : result_label.left
            anchors.rightMargin: 6
            y: 3
            height: 24
            color: searchBar.chromeActive ? "#ebeff5" : "#939ca9"
            selectionColor: "#315d8d"
            selectedTextColor: "#ffffff"
            font.pixelSize: 14
            clip: true
            selectByMouse: true
            verticalAlignment: TextInput.AlignVCenter

            onTextEdited: terminalSurface.set_search_query(text)
            Keys.onPressed: function(event) {
                if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                    if ((event.modifiers & Qt.ShiftModifier) !== 0)
                        terminalSurface.search_previous()
                    else
                        terminalSurface.search_next()
                    event.accepted = true
                }
            }
        }

        Text {
            id: result_label
            objectName: "terminal_search_result_text"
            anchors.right: match_case_button.left
            anchors.rightMargin: 8
            anchors.verticalCenter: match_case_button.verticalCenter
            width: 72
            horizontalAlignment: Text.AlignRight
            color: searchBar.chromeActive ? "#ebeff5" : "#939ca9"
            font.pixelSize: 12
            elide: Text.ElideRight
            text: searchBar.resultText
        }

        Rectangle {
            id: match_case_button
            objectName: "terminal_search_match_case_button"
            anchors.right: previous_button.left
            anchors.top: parent.top
            anchors.topMargin: panel.stacked ? 30 : 0
            anchors.bottom: parent.bottom
            width: 100
            color: match_case_mouse.pressed ? "#343d4a"
                : match_case_mouse.containsMouse ? "#272f3a" : "transparent"

            Text {
                anchors.centerIn: parent
                text: terminalSurface.searchCaseSensitive ? "\u2611 Match case" : "\u2610 Match case"
                color: searchBar.chromeActive ? "#e2e8f0" : "#8e97a3"
                font.pixelSize: 12
            }
            MouseArea {
                id: match_case_mouse
                anchors.fill: parent
                hoverEnabled: true
                onClicked: terminalSurface.searchCaseSensitive = !terminalSurface.searchCaseSensitive
            }
        }

        Rectangle {
            id: previous_button
            objectName: "terminal_search_previous_button"
            anchors.right: next_button.left
            anchors.top: parent.top
            anchors.topMargin: panel.stacked ? 30 : 0
            anchors.bottom: parent.bottom
            width: 28
            color: previous_mouse.pressed ? "#343d4a"
                : previous_mouse.containsMouse ? "#272f3a" : "transparent"

            Text {
                objectName: "terminal_search_previous_icon"
                anchors.centerIn: parent
                text: "\uf139"
                color: searchBar.chromeActive ? "#e2e8f0" : "#8e97a3"
                font.family: searchBar.iconFontFamily
                font.pixelSize: 14
            }
            MouseArea {
                id: previous_mouse
                anchors.fill: parent
                hoverEnabled: true
                onClicked: terminalSurface.search_previous()
            }
        }

        Rectangle {
            id: next_button
            objectName: "terminal_search_next_button"
            anchors.right: panel.stacked ? parent.right : dismiss_button.left
            anchors.top: parent.top
            anchors.topMargin: panel.stacked ? 30 : 0
            anchors.bottom: parent.bottom
            width: 28
            color: next_mouse.pressed ? "#343d4a"
                : next_mouse.containsMouse ? "#272f3a" : "transparent"

            Text {
                objectName: "terminal_search_next_icon"
                anchors.centerIn: parent
                text: "\uf13a"
                color: searchBar.chromeActive ? "#e2e8f0" : "#8e97a3"
                font.family: searchBar.iconFontFamily
                font.pixelSize: 14
            }
            MouseArea {
                id: next_mouse
                anchors.fill: parent
                hoverEnabled: true
                onClicked: terminalSurface.search_next()
            }
        }

        Rectangle {
            id: dismiss_button
            objectName: "terminal_search_dismiss_button"
            anchors.right: parent.right
            anchors.top: parent.top
            width: 28
            height: 30
            color: dismiss_mouse.pressed ? "#343d4a"
                : dismiss_mouse.containsMouse ? "#272f3a" : "transparent"

            Text {
                anchors.centerIn: parent
                text: "\u00d7"
                color: searchBar.chromeActive ? "#e2e8f0" : "#8e97a3"
                font.pixelSize: 18
            }
            MouseArea {
                id: dismiss_mouse
                anchors.fill: parent
                hoverEnabled: true
                onClicked: searchBar.dismiss_search()
            }
        }
    }
}
