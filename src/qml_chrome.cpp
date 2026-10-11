#include "vnm_terminal/app_support/qml_chrome.h"

#include "qml_component_error_string.h"

#include "vnm_qml_chrome/vnm_qml_chrome_runtime.h"
#include "vnm_qml_chrome/vnm_system_window.h"

#include <QDateTime>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlError>
#include <QQmlProperty>
#include <QPointF>
#include <QQuickItem>
#include <QQuickWindow>
#include <QStringList>
#include <QUrl>
#include <QVariant>
#include <QVariantMap>
#include <QWindow>

// Retain the compiled-unit loader when app support is linked as a static library.
extern int qInitResources_qmlcache_vnm_terminal_app_support();

namespace chrome = vnm_terminal::terminal_app;

namespace vnm_terminal::terminal_app::detail {

QString qml_component_error_string(const QQmlComponent& component)
{
    QStringList errors;
    const auto component_errors = component.errors();
    for (const QQmlError& error : component_errors) {
        errors.push_back(error.toString());
    }
    return errors.join(QStringLiteral("\n"));
}

} // namespace vnm_terminal::terminal_app::detail

namespace {

constexpr int k_wheel_delivery_indicator_pulse_ms = 220;

chrome::Terminal_chrome_palette& mutable_terminal_chrome_palette()
{
    static chrome::Terminal_chrome_palette palette =
        chrome::default_terminal_chrome_palette();
    return palette;
}

} // namespace

chrome::Terminal_chrome_palette chrome::default_terminal_chrome_palette()
{
    // The focused fill and outline both sit a clear step above their unfocused
    // counterparts, which is what makes focus read at a glance. The outline
    // stays the brighter of the two, so the chrome keeps a defined border
    // rather than reading as one flat surface.
    Terminal_chrome_palette palette;
    palette.focused_background   = QColor(33, 39, 47);
    palette.unfocused_background = QColor(14, 17, 22);
    palette.focused_frame_edge   = QColor(59, 69, 83);
    palette.unfocused_frame_edge = QColor(31, 36, 44);
    // What the title bar's text used to be written as inside the chrome QML.
    palette.focused_title        = QColor(0xeb, 0xef, 0xf5);
    palette.unfocused_title      = QColor(0x93, 0x9c, 0xa9);
    return palette;
}

const chrome::Terminal_chrome_palette& chrome::terminal_chrome_palette()
{
    return mutable_terminal_chrome_palette();
}

void chrome::set_terminal_chrome_palette(const Terminal_chrome_palette& palette)
{
    mutable_terminal_chrome_palette() = palette;
}

QColor chrome::terminal_chrome_background_color(bool active)
{
    const Terminal_chrome_palette& palette = terminal_chrome_palette();
    return active ? palette.focused_background : palette.unfocused_background;
}

QColor chrome::terminal_chrome_frame_edge_color(bool active)
{
    const Terminal_chrome_palette& palette = terminal_chrome_palette();
    return active ? palette.focused_frame_edge : palette.unfocused_frame_edge;
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
    m_system_window = engine.singletonInstance<vnm_qml_chrome::System_window*>(
        "VNM_Chrome", "VNM_system_window");

    QQmlComponent component(&engine);
    qInitResources_qmlcache_vnm_terminal_app_support();
    component.loadUrl(
        QUrl(QStringLiteral("qrc:/vnm_terminal/terminal_qml_chrome.qml")));
    if (!component.isReady()) {
        m_error_string = chrome::detail::qml_component_error_string(component);
        return;
    }

    // The palette goes in as initial properties rather than after creation, so
    // the first frame the chrome paints already carries the configured colors.
    const Terminal_chrome_palette& palette = terminal_chrome_palette();
    m_root_object.reset(component.createWithInitialProperties(QVariantMap{
        {QStringLiteral("focused_frame_background_color"),   palette.focused_background},
        {QStringLiteral("unfocused_frame_background_color"), palette.unfocused_background},
        {QStringLiteral("focused_frame_edge_color"),         palette.focused_frame_edge},
        {QStringLiteral("unfocused_frame_edge_color"),       palette.unfocused_frame_edge},
        {QStringLiteral("focused_titlebar_text_color"),      palette.focused_title},
        {QStringLiteral("unfocused_titlebar_text_color"),    palette.unfocused_title},
    }));
    if (m_root_object == nullptr) {
        m_error_string = chrome::detail::qml_component_error_string(component);
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
    // Shared titlebar actions may carry their app's nominal titlebar height.
    // Clip their painting at this adapter's actual extent without changing
    // the shared resize-area geometry or event handlers.
    m_titlebar_item->setClip(true);

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

void chrome::Terminal_qml_chrome::set_title_editing_enabled(bool enabled)
{
    set_property("title_editing_enabled", enabled);
}

void chrome::Terminal_qml_chrome::set_title_reset_available(bool available)
{
    set_property("title_reset_available", available);
}

void chrome::Terminal_qml_chrome::set_activity_marker_text(const QString& marker_text)
{
    set_property("activity_marker_text", marker_text);
}

void chrome::Terminal_qml_chrome::set_settings_button_visible(bool visible)
{
    set_property("settings_button_visible", visible);
}

void chrome::Terminal_qml_chrome::set_trailing_action_component(
    QQmlComponent* component)
{
    set_property(
        "trailing_action_component",
        QVariant::fromValue(component));
}

void chrome::Terminal_qml_chrome::set_window_control_palette(
    const Terminal_chrome_window_control_palette& palette)
{
    set_property("titlebar_button_icon_color", palette.icon);
    set_property("titlebar_button_hover_color", palette.hover);
    set_property("titlebar_button_pressed_color", palette.pressed);
    set_property("titlebar_close_hover_color", palette.close_hover);
    set_property("titlebar_close_pressed_color", palette.close_pressed);
}

void chrome::Terminal_qml_chrome::set_titlebar_height(qreal height)
{
    if (m_root_object != nullptr) {
        QQmlProperty::write(m_root_object.get(), QStringLiteral("titlebar_height"), height);
    }
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
    const QRectF&    row_rect,
    const QDateTime& timestamp)
{
    // The timestamp and anchor go in before the visibility flip so the
    // tooltip never fades in showing stale content.
    set_property("row_timestamp_tooltip_timestamp", timestamp);
    set_property("row_timestamp_tooltip_row_rect", row_rect);
    set_property("row_timestamp_tooltip_visible", true);
}

void chrome::Terminal_qml_chrome::hide_row_timestamp_tooltip()
{
    set_property("row_timestamp_tooltip_visible", false);
}

void chrome::Terminal_qml_chrome::set_row_timestamp_tooltip_scrollbar_visible(
    bool visible)
{
    set_property("row_timestamp_tooltip_scrollbar_visible", visible);
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
    QObject::connect(
        m_root_object.get(),
        SIGNAL(title_edit_accepted(QString)),
        this,
        SLOT(handle_title_edit_accepted(QString)));
    QObject::connect(
        m_root_object.get(),
        SIGNAL(title_reset_requested()),
        this,
        SLOT(handle_title_reset_requested()));
}

void chrome::Terminal_qml_chrome::handle_move_requested()
{
    if (m_window != nullptr && m_system_window != nullptr) {
        (void)m_system_window->start_system_move(m_window);
    }
}

void chrome::Terminal_qml_chrome::handle_resize_requested(int edges)
{
    if (m_window != nullptr && m_system_window != nullptr) {
        (void)m_system_window->start_system_resize(m_window, edges);
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

void chrome::Terminal_qml_chrome::handle_title_edit_accepted(const QString& title)
{
    emit title_edit_accepted(title);
}

void chrome::Terminal_qml_chrome::handle_title_reset_requested()
{
    emit title_reset_requested();
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
