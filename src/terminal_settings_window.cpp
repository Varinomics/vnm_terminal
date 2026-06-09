#include "terminal_settings_window.h"

#include "terminal_settings_controller.h"

#include "vnm_qml_chrome/vnm_qml_chrome_runtime.h"

#include "vnm_terminal/vnm_terminal_surface.h"

#include <QPoint>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickWindow>
#include <QRect>
#include <QStringList>
#include <QUrl>
#include <QWindow>
#include <Qt>

namespace settings = vnm_terminal::terminal_app;

namespace {

QString component_error_string(const QQmlComponent& component)
{
    QStringList out;
    const auto errors = component.errors();
    for (const QQmlError& error : errors) {
        out.push_back(error.toString());
    }
    return out.join(QStringLiteral("\n"));
}

// A self-contained frameless QML Window styled with the shared VNM_Chrome
// titlebar so it visually belongs to the terminal. Its controls bind directly
// to the live surface (context property `surface`) for immediate apply; the
// `settings` controller supplies the monospace font list and persists changes.
constexpr const char* k_settings_window_qml = R"(
import QtQuick
import QtQuick.Window
import QtQuick.Layouts
import QtQuick.Controls.Basic as Basic
import VNM_Chrome

Window {
    id: win
    objectName: "terminal_settings_window"

    width: 480
    height: 460
    minimumWidth: 400
    minimumHeight: 320
    visible: false
    flags: Qt.Window | Qt.FramelessWindowHint | Qt.Dialog
    color: "#0e1116"
    title: "vnm_terminal — Settings"

    signal close_requested()
    signal move_requested()
    signal resize_requested(int edges)

    readonly property int titlebar_height: 32
    readonly property real frame_border_width: 1
    readonly property bool resize_enabled: visibility === Window.Windowed

    readonly property color section_color: "#c7ced8"
    readonly property color label_color: "#9aa4b2"
    readonly property color value_color: "#dfe5ee"

    VNM_ChromeTheme {
        id: settings_theme

        titlebar: "#12171e"
        titlebar_text: "#ebeff5"
        titlebar_button_icon: "#e2e8f0"
        titlebar_button_hover: "#272f3a"
        titlebar_button_pressed: "#343d4a"
        titlebar_close_hover: "#c6303a"
        titlebar_close_pressed: "#96222a"
        titlebar_content_border: "#0b0e13"
        window_frame_border: "#2a313c"
    }

    Rectangle {
        anchors.fill: parent
        color: settings_theme.window_frame_border
        z: -100
    }

    Item {
        id: content
        anchors.fill: parent
        anchors.margins: win.resize_enabled ? win.frame_border_width : 0
        clip: true

        Rectangle {
            anchors.fill: parent
            color: win.color
        }

        VNM_ChromeSideResizeLayer {
            anchors.fill: parent
            resize_enabled: win.resize_enabled
            resize_border_width: 6

            onResize_requested: (edges) => win.resize_requested(edges)
        }

        VNM_ChromeBottomResizeLayer {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 6
            resize_enabled: win.resize_enabled
            resize_border_width: 6

            onResize_requested: (edges) => win.resize_requested(edges)
        }

        VNM_ChromeTitleBar {
            id: settings_titlebar
            objectName: "settings_window_titlebar"

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: win.titlebar_height
            theme: settings_theme
            title: win.title
            active: win.active
            resize_enabled: win.resize_enabled
            animated_mark_visible: false
            minimize_button_visible: false
            maximize_button_visible: false

            onMove_requested: win.move_requested()
            onResize_requested: (edges) => win.resize_requested(edges)
            onClose_requested: win.close_requested()
        }

        Flickable {
            id: body
            objectName: "settings_window_body"

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: settings_titlebar.bottom
            anchors.bottom: parent.bottom
            anchors.margins: 18
            contentWidth: width
            contentHeight: form.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            ColumnLayout {
                id: form
                width: body.width
                spacing: 14

                Text {
                    text: "Color scheme"
                    color: win.section_color
                    font.pointSize: 10
                    font.bold: true
                }

                ListView {
                    id: scheme_list
                    objectName: "scheme_list"

                    Layout.fillWidth: true
                    Layout.preferredHeight: 190
                    clip: true
                    model: surface.available_color_schemes()
                    currentIndex: model.indexOf(surface.colorScheme)
                    boundsBehavior: Flickable.StopAtBounds

                    delegate: Rectangle {
                        required property int index
                        required property string modelData

                        readonly property var preview: surface.color_scheme_preview(modelData)
                        readonly property bool selected: modelData === surface.colorScheme

                        width: ListView.view ? ListView.view.width : 0
                        height: 30
                        radius: 3
                        color: selected ? "#26303d" : "transparent"

                        MouseArea {
                            anchors.fill: parent
                            onClicked: surface.colorScheme = modelData
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            spacing: 8

                            Rectangle {
                                Layout.alignment: Qt.AlignVCenter
                                width: swatch.implicitWidth + 4
                                height: 18
                                radius: 2
                                color: preview.background

                                Row {
                                    id: swatch
                                    anchors.centerIn: parent
                                    spacing: 0

                                    Repeater {
                                        model: 16
                                        Rectangle {
                                            width: 8
                                            height: 12
                                            color: preview.ansi[index]
                                        }
                                    }
                                }
                            }

                            Text {
                                Layout.fillWidth: true
                                text: modelData
                                color: win.value_color
                                elide: Text.ElideRight
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                    }
                }

                Text {
                    text: "Font"
                    color: win.section_color
                    font.pointSize: 10
                    font.bold: true
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Text {
                        text: "Family"
                        color: win.label_color
                        Layout.preferredWidth: 78
                    }

                    Basic.ComboBox {
                        id: family_combo
                        objectName: "font_family_combo"
                        Layout.fillWidth: true
                        model: settings.available_font_families()
                        currentIndex: Math.max(0, model.indexOf(surface.fontFamily))
                        onActivated: surface.fontFamily = currentText
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Text {
                        text: "Size"
                        color: win.label_color
                        Layout.preferredWidth: 78
                    }

                    Basic.SpinBox {
                        objectName: "font_size_spin"
                        from: 6
                        to: 72
                        value: Math.round(surface.fontSize)
                        onValueModified: surface.fontSize = value
                    }
                }

                Text {
                    text: "Renderer"
                    color: win.section_color
                    font.pointSize: 10
                    font.bold: true
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Text {
                        text: "Glyph mode"
                        color: win.label_color
                        Layout.preferredWidth: 78
                    }

                    Basic.ComboBox {
                        objectName: "renderer_mode_combo"
                        Layout.fillWidth: true
                        model: ["Auto", "MSDF", "Glyph"]
                        currentIndex: surface.textRendererMode
                        onActivated: surface.textRendererMode = currentIndex
                    }
                }

                Text {
                    text: "Scrollback"
                    color: win.section_color
                    font.pointSize: 10
                    font.bold: true
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Text {
                        text: "Lines"
                        color: win.label_color
                        Layout.preferredWidth: 78
                    }

                    Basic.SpinBox {
                        objectName: "scrollback_spin"
                        from: 0
                        to: 1000000
                        stepSize: 1000
                        editable: true
                        value: surface.scrollbackLimit
                        onValueModified: surface.scrollbackLimit = value
                    }
                }
            }
        }
    }
}
)";

} // namespace

settings::Terminal_settings_window::Terminal_settings_window(
    QQmlEngine&                   engine,
    VNM_TerminalSurface&          surface,
    Terminal_settings_controller& controller,
    QObject*                      parent)
:
    QObject(parent)
{
    if (!vnm_init_qml_chrome_runtime(engine)) {
        m_error_string = QStringLiteral("failed to initialize vnm_qml_chrome runtime");
        return;
    }

    auto* context = new QQmlContext(engine.rootContext(), this);
    context->setContextProperty(QStringLiteral("surface"), &surface);
    context->setContextProperty(QStringLiteral("settings"), &controller);

    QQmlComponent component(&engine);
    component.setData(
        k_settings_window_qml,
        QUrl(QStringLiteral("qrc:/vnm_terminal/terminal_settings_window.qml")));
    if (!component.isReady()) {
        m_error_string = component_error_string(component);
        return;
    }

    m_root_object.reset(component.create(context));
    if (m_root_object == nullptr) {
        m_error_string = component_error_string(component);
        return;
    }

    m_window = qobject_cast<QQuickWindow*>(m_root_object.get());
    if (m_window == nullptr) {
        m_error_string = QStringLiteral("settings window QML root is not a Window");
        m_root_object.reset();
        return;
    }

    QObject::connect(
        m_root_object.get(),
        SIGNAL(close_requested()),
        this,
        SLOT(handle_close_requested()));
    QObject::connect(
        m_root_object.get(),
        SIGNAL(move_requested()),
        this,
        SLOT(handle_move_requested()));
    QObject::connect(
        m_root_object.get(),
        SIGNAL(resize_requested(int)),
        this,
        SLOT(handle_resize_requested(int)));
}

settings::Terminal_settings_window::~Terminal_settings_window() = default;

bool settings::Terminal_settings_window::is_valid() const
{
    return m_window != nullptr;
}

QString settings::Terminal_settings_window::error_string() const
{
    return m_error_string;
}

void settings::Terminal_settings_window::set_transient_parent(QWindow* parent)
{
    if (m_window != nullptr) {
        m_window->setTransientParent(parent);
    }
}

void settings::Terminal_settings_window::show_window()
{
    if (m_window == nullptr) {
        return;
    }

    if (!m_window->isVisible()) {
        center_over_transient_parent();
        m_window->show();
    }

    m_window->raise();
    m_window->requestActivate();
}

void settings::Terminal_settings_window::center_over_transient_parent()
{
    if (m_window == nullptr || m_positioned) {
        return;
    }

    const QWindow* anchor = m_window->transientParent();
    if (anchor == nullptr) {
        return;
    }

    const QRect anchor_geometry = anchor->geometry();
    const QPoint top_left = anchor_geometry.center()
        - QPoint(m_window->width() / 2, m_window->height() / 2);
    m_window->setPosition(top_left);
    m_positioned = true;
}

void settings::Terminal_settings_window::handle_close_requested()
{
    if (m_window != nullptr) {
        m_window->hide();
    }
}

void settings::Terminal_settings_window::handle_move_requested()
{
    if (m_window != nullptr) {
        (void)m_window->startSystemMove();
    }
}

void settings::Terminal_settings_window::handle_resize_requested(int edges)
{
    if (m_window != nullptr) {
        (void)m_window->startSystemResize(Qt::Edges(QFlag(edges)));
    }
}
