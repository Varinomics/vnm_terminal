#include "qml_chrome.h"

#include "vnm_qml_chrome/vnm_qml_chrome_runtime.h"

#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlError>
#include <QList>
#include <QQuickItem>
#include <QQuickWindow>
#include <QStringList>
#include <QUrl>
#include <QVariant>
#include <QWindow>

namespace chrome = vnm_terminal::terminal_app;

namespace {

constexpr int k_wheel_delivery_indicator_pulse_ms = 220;

QString component_error_string(const QQmlComponent& component)
{
    QStringList out;
    const auto errors = component.errors();
    for (const QQmlError& error : errors) {
        out.push_back(error.toString());
    }
    return out.join(QStringLiteral("\n"));
}

QObject* find_child_object(QObject& root, const QString& object_name)
{
    if (root.objectName() == object_name) {
        return &root;
    }

    const auto children = root.children();
    for (QObject* child : children) {
        QObject* found = find_child_object(*child, object_name);
        if (found != nullptr) {
            return found;
        }
    }

    auto* item = qobject_cast<QQuickItem*>(&root);
    if (item == nullptr) {
        return nullptr;
    }

    const auto child_items = item->childItems();
    for (QQuickItem* child : child_items) {
        QObject* found = find_child_object(*child, object_name);
        if (found != nullptr) {
            return found;
        }
    }

    return nullptr;
}

constexpr const char* k_terminal_chrome_qml = R"(
import QtQuick
import QtQuick.Window
import VNM_Chrome

Item {
    id: root
    objectName: "terminal_qml_chrome_root"

    property string title: ""
    property string activity_marker_text: ""
    property bool active: true
    property bool maximized: false
    property bool resize_enabled: true
    property bool wheel_delivery_indicator_visible: false
    property real content_border_x: 0
    property real content_border_y: 0
    property real content_border_width: 0
    property real content_border_height: 0
    property real content_border_line_width: 0
    property real device_pixel_ratio: Screen.devicePixelRatio
    readonly property real base_resize_border_width: 6
    readonly property real resize_border_physical_reduction: 2
    readonly property real reduced_resize_border_width:
        reduced_chrome_width(
            base_resize_border_width,
            resize_border_physical_reduction)
    readonly property real base_titlebar_height: 32
    readonly property real titlebar_physical_reduction: 2
    readonly property real reduced_titlebar_height:
        reduced_chrome_width(
            base_titlebar_height,
            titlebar_physical_reduction)
    readonly property bool content_border_visible:
        content_border_width > 0
        && content_border_height > 0
        && content_border_line_width > 0
    readonly property color content_border_color: active ? "#2a313c" : "#1f242c"

    function reduced_chrome_width(logical_width, physical_reduction) {
        var dpr = VNM_chrome_geometry.normalized_device_pixel_ratio(device_pixel_ratio)
        return Math.max(
            0,
            VNM_chrome_geometry.snapped_logical_edge(logical_width, dpr)
                - physical_reduction / dpr)
    }

    signal move_requested()
    signal resize_requested(int edges)
    signal minimize_requested()
    signal maximize_toggle_requested()
    signal close_requested()

    VNM_ChromeTheme {
        id: terminal_chrome_theme

        titlebar: root.active ? "#12171e" : "#0e1116"
        titlebar_text: root.active ? "#ebeff5" : "#939ca9"
        titlebar_button_icon: root.active ? "#e2e8f0" : "#8e97a3"
        titlebar_button_hover: "#272f3a"
        titlebar_button_pressed: "#343d4a"
        titlebar_close_hover: "#c6303a"
        titlebar_close_pressed: "#96222a"
        titlebar_activity_marker: root.active ? "#71b4ff" : "#5f7793"
        titlebar_content_border: "transparent"
        window_frame_border: root.content_border_color
    }

    VNM_ChromeSideResizeLayer {
        anchors.fill: parent
        resize_enabled: root.resize_enabled
        resize_border_width: root.reduced_resize_border_width

        onResize_requested: (edges) => root.resize_requested(edges)
    }

    VNM_ChromeBottomResizeLayer {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: root.reduced_resize_border_width
        resize_enabled: root.resize_enabled
        resize_border_width: root.reduced_resize_border_width

        onResize_requested: (edges) => root.resize_requested(edges)
    }

    VNM_ChromeTitleBar {
        id: titlebar
        objectName: "terminal_chrome_titlebar"

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: Math.min(root.reduced_titlebar_height, root.height)
        theme: terminal_chrome_theme
        title: root.title
        active: root.active
        maximized: root.maximized
        resize_enabled: root.resize_enabled
        resize_border_width: root.reduced_resize_border_width
        activity_marker_text: root.activity_marker_text
        trailing_action_component:
            root.wheel_delivery_indicator_visible ? wheel_indicator_component : null

        onMove_requested: root.move_requested()
        onResize_requested: (edges) => root.resize_requested(edges)
        onMinimize_requested: root.minimize_requested()
        onMaximize_toggle_requested: root.maximize_toggle_requested()
        onClose_requested: root.close_requested()
    }

    Component {
        id: wheel_indicator_component

        Item {
            width: 7
            height: 7

            Rectangle {
                anchors.fill: parent
                radius: width / 2
                color: "#ffdc2a"
                border.color: "#6c4a00"
                border.width: 1
                opacity: 0.92
            }
        }
    }

    Rectangle {
        objectName: "terminal_chrome_content_border_top"
        x: root.content_border_x
        y: root.content_border_y
        width: root.content_border_width
        height: root.content_border_line_width
        color: root.content_border_color
        visible: root.content_border_visible
    }

    Rectangle {
        objectName: "terminal_chrome_content_border_bottom"
        x: root.content_border_x
        y: root.content_border_y + root.content_border_height - root.content_border_line_width
        width: root.content_border_width
        height: root.content_border_line_width
        color: root.content_border_color
        visible: root.content_border_visible
    }

    Rectangle {
        objectName: "terminal_chrome_content_border_left"
        x: root.content_border_x
        y: root.content_border_y
        width: root.content_border_line_width
        height: root.content_border_height
        color: root.content_border_color
        visible: root.content_border_visible
    }

    Rectangle {
        objectName: "terminal_chrome_content_border_right"
        x: root.content_border_x + root.content_border_width - root.content_border_line_width
        y: root.content_border_y
        width: root.content_border_line_width
        height: root.content_border_height
        color: root.content_border_color
        visible: root.content_border_visible
    }
}
)";

} // namespace

QColor chrome::terminal_chrome_background_color(bool active)
{
    return active ? QColor(18, 23, 30) : QColor(14, 17, 22);
}

QColor chrome::terminal_chrome_content_border_color(bool active)
{
    return active ? QColor(42, 49, 60) : QColor(31, 36, 44);
}

chrome::Terminal_qml_chrome::Terminal_qml_chrome(QQmlEngine& engine, QQuickWindow& window)
:
    QObject(nullptr),
    m_window(&window)
{
    if (!vnm_init_qml_chrome_runtime(engine)) {
        m_error_string = QStringLiteral("failed to initialize vnm_qml_chrome runtime");
        return;
    }

    QQmlComponent component(&engine);
    component.setData(
        k_terminal_chrome_qml,
        QUrl(QStringLiteral("qrc:/vnm_terminal/terminal_qml_chrome.qml")));
    if (!component.isReady()) {
        m_error_string = component_error_string(component);
        return;
    }

    m_root_object.reset(component.create());
    if (m_root_object == nullptr) {
        m_error_string = component_error_string(component);
        return;
    }

    m_root_item = qobject_cast<QQuickItem*>(m_root_object.get());
    if (m_root_item == nullptr) {
        m_error_string = QStringLiteral("terminal chrome QML root is not a QQuickItem");
        m_root_object.reset();
        return;
    }

    auto* titlebar_object = find_child_object(
        *m_root_object,
        QStringLiteral("terminal_chrome_titlebar"));
    m_titlebar_item = qobject_cast<QQuickItem*>(titlebar_object);
    if (m_titlebar_item == nullptr) {
        m_error_string = QStringLiteral("terminal chrome QML titlebar was not created");
        m_root_object.reset();
        m_root_item = nullptr;
        return;
    }

    m_root_item->setParentItem(window.contentItem());
    m_root_item->setZ(100.0);
    set_size(QSizeF(window.width(), window.height()));
    connect_window_commands();

    m_wheel_delivery_indicator_timer.setSingleShot(true);
    m_wheel_delivery_indicator_timer.setInterval(k_wheel_delivery_indicator_pulse_ms);
    QObject::connect(
        &m_wheel_delivery_indicator_timer,
        &QTimer::timeout,
        this,
        [this] {
            set_wheel_delivery_indicator_visible(false);
        });
}

chrome::Terminal_qml_chrome::~Terminal_qml_chrome() = default;

bool chrome::Terminal_qml_chrome::is_valid() const
{
    return m_root_item != nullptr;
}

QString chrome::Terminal_qml_chrome::error_string() const
{
    return m_error_string;
}

QQuickItem* chrome::Terminal_qml_chrome::root_item() const
{
    return m_root_item;
}

QQuickItem* chrome::Terminal_qml_chrome::titlebar_item() const
{
    return m_titlebar_item;
}

void chrome::Terminal_qml_chrome::set_size(const QSizeF& size)
{
    if (m_root_item == nullptr) {
        return;
    }

    m_root_item->setSize(size);
}

void chrome::Terminal_qml_chrome::set_content_border_rect(
    const QRectF& rect,
    qreal         border_width)
{
    set_property("content_border_x", rect.x());
    set_property("content_border_y", rect.y());
    set_property("content_border_width", rect.width());
    set_property("content_border_height", rect.height());
    set_property("content_border_line_width", border_width);
}

void chrome::Terminal_qml_chrome::set_title(const QString& title)
{
    set_property("title", title);
}

void chrome::Terminal_qml_chrome::set_activity_marker_text(const QString& marker_text)
{
    set_property("activity_marker_text", marker_text);
}

void chrome::Terminal_qml_chrome::set_active(bool active)
{
    set_property("active", active);
}

void chrome::Terminal_qml_chrome::set_maximized(bool maximized)
{
    set_property("maximized", maximized);
}

void chrome::Terminal_qml_chrome::set_resize_enabled(bool resize_enabled)
{
    set_property("resize_enabled", resize_enabled);
}

void chrome::Terminal_qml_chrome::pulse_wheel_delivery_indicator()
{
    set_wheel_delivery_indicator_visible(true);
    m_wheel_delivery_indicator_timer.start();
}

void chrome::Terminal_qml_chrome::connect_window_commands()
{
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
    QObject::connect(
        m_root_object.get(),
        SIGNAL(minimize_requested()),
        this,
        SLOT(handle_minimize_requested()));
    QObject::connect(
        m_root_object.get(),
        SIGNAL(maximize_toggle_requested()),
        this,
        SLOT(handle_maximize_toggle_requested()));
    QObject::connect(
        m_root_object.get(),
        SIGNAL(close_requested()),
        this,
        SLOT(handle_close_requested()));
}

void chrome::Terminal_qml_chrome::handle_move_requested()
{
    if (m_window != nullptr) {
        (void)m_window->startSystemMove();
    }
}

void chrome::Terminal_qml_chrome::handle_resize_requested(int edges)
{
    if (m_window != nullptr) {
        (void)m_window->startSystemResize(Qt::Edges(QFlag(edges)));
    }
}

void chrome::Terminal_qml_chrome::handle_minimize_requested()
{
    if (m_window != nullptr) {
        m_window->showMinimized();
    }
}

void chrome::Terminal_qml_chrome::handle_maximize_toggle_requested()
{
    toggle_window_maximized();
}

void chrome::Terminal_qml_chrome::handle_close_requested()
{
    if (m_window != nullptr) {
        m_window->close();
    }
}

void chrome::Terminal_qml_chrome::set_property(
    const char*     property_name,
    const QVariant& value)
{
    if (m_root_object != nullptr) {
        m_root_object->setProperty(property_name, value);
    }
}

void chrome::Terminal_qml_chrome::set_wheel_delivery_indicator_visible(bool visible)
{
    set_property("wheel_delivery_indicator_visible", visible);
}

void chrome::Terminal_qml_chrome::toggle_window_maximized()
{
    if (m_window == nullptr) {
        return;
    }

    const Qt::WindowStates states = m_window->windowStates();
    if (states.testFlag(Qt::WindowMaximized) ||
        states.testFlag(Qt::WindowFullScreen))
    {
        m_window->showNormal();
        return;
    }

    m_window->showMaximized();
}
