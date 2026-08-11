#include "terminal_search_bar.h"

#include "qml_chrome.h"

#include "vnm_terminal/vnm_terminal_surface.h"

#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWindow>
#include <QStringList>
#include <QUrl>

namespace vnm_terminal::terminal_app {

namespace {

QString component_error_string(const QQmlComponent& component)
{
    QStringList output;
    const auto errors = component.errors();
    for (const QQmlError& error : errors) {
        output.push_back(error.toString());
    }
    return output.join(QStringLiteral("\n"));
}

constexpr const char* k_terminal_search_bar_qml = R"(
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

        width: Math.min(360, Math.max(0, content_right - terminalSurface.x))
        height: 30
        x: content_right - width
        y: terminalSurface.y
        color: searchBar.chromeBackgroundColor

        readonly property real edge_width: 1 / Screen.devicePixelRatio

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
            anchors.right: result_label.left
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
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
            anchors.right: previous_button.left
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            width: 72
            horizontalAlignment: Text.AlignRight
            color: searchBar.chromeActive ? "#ebeff5" : "#939ca9"
            font.pixelSize: 12
            elide: Text.ElideRight
            text: searchBar.resultText
        }

        Rectangle {
            id: previous_button
            objectName: "terminal_search_previous_button"
            anchors.right: next_button.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: 28
            color: previous_mouse.pressed ? "#343d4a"
                : previous_mouse.containsMouse ? "#272f3a" : "transparent"

            Text {
                objectName: "terminal_search_previous_icon"
                anchors.centerIn: parent
                text: "\uf139"
                color: searchBar.chromeActive ? "#e2e8f0" : "#8e97a3"
                font.family: "FontAwesome"
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
            anchors.right: dismiss_button.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: 28
            color: next_mouse.pressed ? "#343d4a"
                : next_mouse.containsMouse ? "#272f3a" : "transparent"

            Text {
                objectName: "terminal_search_next_icon"
                anchors.centerIn: parent
                text: "\uf13a"
                color: searchBar.chromeActive ? "#e2e8f0" : "#8e97a3"
                font.family: "FontAwesome"
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
            anchors.bottom: parent.bottom
            width: 28
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
)";

} // namespace

Terminal_search_bar::Terminal_search_bar(
    QQmlEngine&          engine,
    QQuickWindow&        window,
    VNM_TerminalSurface& surface)
:
    QObject(nullptr),
    m_window(&window),
    m_surface(&surface),
    m_context(std::make_unique<QQmlContext>(engine.rootContext()))
{
    m_context->setContextProperty(QStringLiteral("terminalSurface"), &surface);
    m_context->setContextProperty(QStringLiteral("searchBar"), this);

    QQmlComponent component(&engine);
    component.setData(
        k_terminal_search_bar_qml,
        QUrl(QStringLiteral("qrc:/vnm_terminal/terminal_search_bar.qml")));
    if (!component.isReady()) {
        m_error_string = component_error_string(component);
        return;
    }

    m_root_object.reset(component.create(m_context.get()));
    if (m_root_object == nullptr) {
        m_error_string = component_error_string(component);
        return;
    }

    m_root_item = qobject_cast<QQuickItem*>(m_root_object.get());
    if (m_root_item == nullptr) {
        m_error_string = QStringLiteral("terminal search QML root is not a QQuickItem");
        m_root_object.reset();
        return;
    }

    m_root_item->setParentItem(window.contentItem());
    m_root_item->setZ(11000.0);
    QObject::connect(
        &surface,
        &VNM_TerminalSurface::search_changed,
        this,
        &Terminal_search_bar::result_text_changed);
    QObject::connect(
        &window,
        &QWindow::activeChanged,
        this,
        &Terminal_search_bar::chrome_palette_changed);
}

Terminal_search_bar::~Terminal_search_bar() = default;

bool Terminal_search_bar::is_valid() const
{
    return m_root_item != nullptr;
}

bool Terminal_search_bar::is_visible() const
{
    return m_root_item != nullptr && m_root_item->isVisible();
}

QString Terminal_search_bar::error_string() const
{
    return m_error_string;
}

QString Terminal_search_bar::result_text() const
{
    if (m_surface == nullptr) {
        return {};
    }

    switch (m_surface->search_result_state()) {
        case VNM_TerminalSurface::Search_result_state::INACTIVE:
            return {};
        case VNM_TerminalSurface::Search_result_state::SOURCE_UNAVAILABLE:
            return QStringLiteral("Unavailable");
        case VNM_TerminalSurface::Search_result_state::NO_MATCH:
            return QStringLiteral("No matches");
        case VNM_TerminalSurface::Search_result_state::MATCH:
            return QStringLiteral("%1 of %2")
                .arg(m_surface->current_search_match())
                .arg(m_surface->search_match_count());
    }
    return {};
}

bool Terminal_search_bar::chrome_active() const
{
    return m_window != nullptr && m_window->isActive();
}

QColor Terminal_search_bar::chrome_background_color() const
{
    return terminal_chrome_background_color(chrome_active());
}

QColor Terminal_search_bar::chrome_frame_edge_color() const
{
    return terminal_chrome_frame_edge_color(chrome_active());
}

QQuickItem* Terminal_search_bar::root_item() const
{
    return m_root_item;
}

void Terminal_search_bar::show_search()
{
    if (m_root_item == nullptr || m_surface == nullptr) {
        return;
    }

    m_root_item->setProperty("query_text", m_surface->search_query());
    if (!m_root_item->isVisible()) {
        m_root_item->setVisible(true);
        emit visibility_changed(true);
    }

    QQuickItem* const query_input =
        m_root_item->findChild<QQuickItem*>(QStringLiteral("terminal_search_query_input"));
    if (query_input != nullptr) {
        query_input->forceActiveFocus(Qt::ShortcutFocusReason);
        QMetaObject::invokeMethod(query_input, "selectAll");
    }
}

void Terminal_search_bar::dismiss_search()
{
    if (m_root_item == nullptr) {
        return;
    }

    if (m_surface != nullptr) {
        m_surface->clear_search();
        m_surface->forceActiveFocus(Qt::ShortcutFocusReason);
    }
    if (m_root_item->isVisible()) {
        m_root_item->setVisible(false);
        emit visibility_changed(false);
    }
}

} // namespace vnm_terminal::terminal_app
