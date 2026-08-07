#include "qml_chrome.h"

#include "vnm_qml_chrome/vnm_qml_chrome_runtime.h"

#include <QDateTime>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlError>
#include <QPointF>
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
    property bool fullscreen: false
    property bool wheel_delivery_indicator_visible: false
    property bool settings_button_visible: true
    property bool row_timestamp_tooltip_visible: false
    property real row_timestamp_tooltip_anchor_x: 0
    property real row_timestamp_tooltip_anchor_y: 0
    property date row_timestamp_tooltip_timestamp: new Date()
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
    readonly property real frame_edge_thickness:
        1 / VNM_chrome_geometry.normalized_device_pixel_ratio(device_pixel_ratio)
    readonly property real active_frame_gap:
        Math.max(0, reduced_resize_border_width - frame_edge_thickness)
    readonly property real shell_outer_edge_thickness:
        fullscreen ? 0 : frame_edge_thickness
    readonly property real shell_inner_edge_thickness: frame_edge_thickness
    readonly property real shell_frame_gap: resize_enabled ? active_frame_gap : 0
    readonly property bool maximized_frame_overscan:
        maximized && !fullscreen && shell_outer_edge_thickness > 0
    readonly property color frame_edge_color: active ? "#2a313c" : "#1f242c"
    readonly property color frame_background_color: active ? "#12171e" : "#0e1116"
    readonly property rect content_interior_rect: Qt.rect(
        content_interior_x,
        content_interior_y,
        content_interior_width,
        content_interior_height)
    readonly property real content_interior_x:
        frame_shell.x + frame_shell.content_interior_x
    readonly property real content_interior_y:
        frame_shell.y + frame_shell.content_interior_y
    readonly property real content_interior_width: frame_shell.content_interior_width
    readonly property real content_interior_height: frame_shell.content_interior_height
    readonly property Item titlebar_item: frame_shell.titlebar_item

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
    signal settings_requested()

    VNM_ChromeTheme {
        id: terminal_chrome_theme

        titlebar: root.frame_background_color
        titlebar_text: root.active ? "#ebeff5" : "#939ca9"
        titlebar_button_icon: root.active ? "#e2e8f0" : "#8e97a3"
        titlebar_button_hover: "#272f3a"
        titlebar_button_pressed: "#343d4a"
        titlebar_close_hover: "#c6303a"
        titlebar_close_pressed: "#96222a"
        titlebar_activity_marker: root.active ? "#71b4ff" : "#5f7793"
        titlebar_content_border: "transparent"
        window_frame_border: root.frame_edge_color
    }

    VNM_NativeWindowFrame {
        window: root.Window.window
        frame_visible: false
        resize_enabled: root.resize_enabled
        resize_outward_margins.left: frame_shell.left_resize_outward_extent
        resize_outward_margins.top: frame_shell.top_resize_outward_extent
        resize_outward_margins.right: frame_shell.right_resize_outward_extent
        resize_outward_margins.bottom: frame_shell.bottom_resize_outward_extent
    }

    VNM_ChromeFrameShell {
        id: frame_shell
        objectName: "terminal_chrome_frame_shell"

        x: root.maximized_frame_overscan ? -root.shell_outer_edge_thickness : 0
        y: 0
        width: root.width
            + (root.maximized_frame_overscan ? 2 * root.shell_outer_edge_thickness : 0)
        height: root.height
            + (root.maximized_frame_overscan ? root.shell_outer_edge_thickness : 0)
        theme: terminal_chrome_theme
        frame_color: root.frame_background_color
        frame_outer_edge: root.shell_outer_edge_thickness
        frame_outer_edge_color: root.frame_edge_color
        frame_gap: root.shell_frame_gap
        frame_inner_edge: root.shell_inner_edge_thickness
        frame_inner_edge_color: root.frame_edge_color
        device_pixel_ratio: root.device_pixel_ratio
        titlebar_height: Math.min(root.reduced_titlebar_height, root.height)
        title: root.title
        active: root.active
        maximized: root.maximized
        resize_enabled: root.resize_enabled
        activity_marker_text: root.activity_marker_text
        trailing_action_component:
            root.wheel_delivery_indicator_visible ? trailing_actions_component : null
        custom_buttons: root.settings_button_visible
            ? [{
                object_name: "settings_button",
                component: settings_gear_component,
                tooltip: "Settings",
                action: function() { root.settings_requested() }
            }]
            : []

        onMove_requested: root.move_requested()
        onResize_requested: (edges) => root.resize_requested(edges)
        onMinimize_requested: root.minimize_requested()
        onMaximize_toggle_requested: root.maximize_toggle_requested()
        onClose_requested: root.close_requested()
    }

    // The settings gear is stroked here rather than taken from an icon font.
    // No single font family carries a gear on every platform, and the ones
    // that do disagree on how it looks, so a glyph would render as a different
    // drawing per system and as a missing-glyph box where none is installed.
    // Stroke weight and colour match the minimize/maximize/close marks the
    // chrome paints beside it.
    Component {
        id: settings_gear_component

        Canvas {
            anchors.fill: parent

            property color icon_color: terminal_chrome_theme.titlebar_button_icon

            // Proportions follow the Heroicons "cog-6-tooth" outline icon
            // (heroicons.com, MIT), measured off its path: a 24-unit box with
            // tip radius 9, root radius 6.75, hub radius 3, and a 1.5 stroke.
            // The geometry below is generated rather than traced, so it stays
            // resolution-independent and carries no bundled asset. The joins
            // depart from the reference deliberately; see the paint handler.
            readonly property int tooth_count: 6
            readonly property real stroke_width: 1.3
            // The reference draws its stroke at 1/6 of the tip radius. Solving
            // that for a 1.3 stroke gives a 7.8 radius, hence this box: the
            // stroke straddles the path, so the box also carries half a stroke
            // of inset to keep the teeth inside it.
            readonly property real icon_size: 17
            // The tip radius the reference would use for this box. Body, hub
            // and tip width are all measured against it, so extending the
            // teeth below moves nothing else.
            readonly property real reference_radius: icon_size / 2 - stroke_width / 2
            readonly property real root_radius: reference_radius * 0.75
            readonly property real hub_radius: reference_radius * 0.333
            // Teeth run longer than the reference's, which reads better at
            // this size. Only the tips move: they grow outward from the same
            // root circle, so the gear body and hub are untouched. The teeth
            // overshoot the nominal box by a fraction of a pixel, which the
            // button around them absorbs.
            readonly property real tooth_length_factor: 1.15
            readonly property real outer_radius:
                root_radius + (reference_radius - root_radius) * tooth_length_factor
            // Half-widths in radians of a tooth at its tip, on the outer
            // radius, and at its base, on the root radius. A gear tooth is a
            // trapezoid that narrows towards the tip, and arc length grows
            // with radius, so the tip needs the smaller share of the two: the
            // tooth only tapers outward while
            //     tip_half_angle < base_half_angle * (root_radius/outer_radius)
            // Equal angles would draw the flanks radially and flare the tooth
            // out into a petal instead. The reference tapers hard, to roughly
            // a third of the base width, which leaves the roots as narrow
            // valleys rather than flats.
            //
            // The tip half-angle is scaled back by the same factor the tips
            // moved out. An angle subtends more arc the further out it is, so
            // holding it fixed would have widened the tips as well as
            // lengthening them; this keeps the tip exactly as wide as the
            // reference's and lets the teeth only grow longer.
            readonly property real tip_half_angle: 0.144 * reference_radius / outer_radius
            readonly property real base_half_angle: 0.504
            // Half a tooth pitch, which for six teeth is 30 degrees. Teeth
            // generated straight from angle zero straddle the vertical and
            // leave a valley pointing up; the reference points a tooth up
            // instead, and this phase puts one back on the axis.
            readonly property real tooth_phase: Math.PI / tooth_count

            onIcon_colorChanged: requestPaint()
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()

            onPaint: {
                const ctx = getContext("2d")
                ctx.clearRect(0, 0, width, height)
                ctx.strokeStyle = icon_color
                ctx.lineWidth = stroke_width
                // Mitred rather than the reference's round joins, which blunt
                // the teeth at this size. The limit is what keeps that safe:
                // a tooth tip turns gently enough to be mitred to a crisp
                // corner, while the far sharper turn at the root of a valley
                // exceeds the limit and falls back to a bevel instead of
                // growing the long spike an unbounded miter would produce.
                ctx.lineJoin = "miter"
                ctx.miterLimit = 2

                const cx = width / 2
                const cy = height / 2
                const step = 2 * Math.PI / tooth_count

                // One closed outline. Each arc() call first draws a straight
                // line from the previous arc's end to its own start, which is
                // exactly the flank between a tooth and the root beside it.
                ctx.beginPath()
                for (var i = 0; i < tooth_count; ++i) {
                    const tooth_centre = i * step + tooth_phase
                    ctx.arc(
                        cx, cy, outer_radius,
                        tooth_centre - tip_half_angle,
                        tooth_centre + tip_half_angle,
                        false)
                    ctx.arc(
                        cx, cy, root_radius,
                        tooth_centre + base_half_angle,
                        tooth_centre + step - base_half_angle,
                        false)
                }
                ctx.closePath()
                ctx.stroke()

                ctx.beginPath()
                ctx.arc(cx, cy, hub_radius, 0, 2 * Math.PI, false)
                ctx.stroke()
            }
        }
    }

    Component {
        id: trailing_actions_component

        Item {
            id: wheel_indicator
            width: 14
            height: Math.max(0, root.reduced_titlebar_height)

            Rectangle {
                anchors.centerIn: parent
                width: 7
                height: 7
                radius: width / 2
                color: "#ffdc2a"
                border.color: "#6c4a00"
                border.width: 1
                opacity: 0.92
            }
        }
    }

    // Row-timestamp hover tooltip. It lives in the chrome because the chrome
    // root is the always-on-top layer over the terminal surface, so the
    // tooltip can float at any pointer position without a second overlay
    // mechanism. Palette matches the settings dialog.
    Rectangle {
        id: row_timestamp_tooltip
        objectName: "row_timestamp_tooltip"

        readonly property real anchor_offset: 12
        readonly property real edge_margin: 4

        x: Math.max(edge_margin, Math.min(
            root.row_timestamp_tooltip_anchor_x + anchor_offset,
            root.width  - width  - edge_margin))
        y: Math.max(edge_margin, Math.min(
            root.row_timestamp_tooltip_anchor_y + anchor_offset,
            root.height - height - edge_margin))
        width: row_timestamp_tooltip_text.implicitWidth + 12
        height: row_timestamp_tooltip_text.implicitHeight + 8
        radius: 5
        color: "#161c26"
        border.width: 1
        border.color: "#2c3645"
        opacity: root.row_timestamp_tooltip_visible ? 1 : 0
        visible: opacity > 0
        // Pure feedback: the tooltip must never steal pointer events from
        // the terminal underneath it.
        enabled: false
        z: 1000

        Behavior on opacity { NumberAnimation { duration: 100 } }

        Text {
            id: row_timestamp_tooltip_text
            objectName: "row_timestamp_tooltip_text"
            anchors.centerIn: parent
            text: Qt.formatDateTime(
                root.row_timestamp_tooltip_timestamp, "yyyy-MM-dd hh:mm:ss")
            color: "#dfe5ee"
            font.pixelSize: 11
        }
    }
}
)";

} // namespace

QColor chrome::terminal_chrome_background_color(bool active)
{
    return active ? QColor(18, 23, 30) : QColor(14, 17, 22);
}

QColor chrome::terminal_chrome_frame_edge_color(bool active)
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

    m_titlebar_item =
        qobject_cast<QQuickItem*>(m_root_object->property("titlebar_item").value<QObject*>());
    if (m_titlebar_item == nullptr) {
        m_error_string = QStringLiteral("terminal chrome QML titlebar was not created");
        m_root_object.reset();
        m_root_item = nullptr;
        return;
    }

    m_root_item->setParentItem(window.contentItem());
    m_root_item->setZ(10000.0);
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

QRectF chrome::Terminal_qml_chrome::content_interior_rect() const
{
    if (m_root_object == nullptr) {
        return {};
    }

    return QRectF(
        m_root_object->property("content_interior_x").toReal(),
        m_root_object->property("content_interior_y").toReal(),
        m_root_object->property("content_interior_width").toReal(),
        m_root_object->property("content_interior_height").toReal());
}

qreal chrome::Terminal_qml_chrome::device_pixel_ratio() const
{
    if (m_root_object == nullptr) {
        return 1.0;
    }

    return m_root_object->property("device_pixel_ratio").toReal();
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

void chrome::Terminal_qml_chrome::set_fullscreen(bool fullscreen)
{
    set_property("fullscreen", fullscreen);
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

void chrome::Terminal_qml_chrome::show_row_timestamp_tooltip(
    const QPointF&   position,
    const QDateTime& timestamp)
{
    // The timestamp and anchor go in before the visibility flip so the
    // tooltip never fades in showing stale content.
    set_property("row_timestamp_tooltip_timestamp", timestamp);
    set_property("row_timestamp_tooltip_anchor_x", position.x());
    set_property("row_timestamp_tooltip_anchor_y", position.y());
    set_property("row_timestamp_tooltip_visible", true);
}

void chrome::Terminal_qml_chrome::hide_row_timestamp_tooltip()
{
    set_property("row_timestamp_tooltip_visible", false);
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
    QObject::connect(
        m_root_object.get(),
        SIGNAL(settings_requested()),
        this,
        SLOT(handle_settings_requested()));
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

void chrome::Terminal_qml_chrome::handle_settings_requested()
{
    emit settings_requested();
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
