#include "qml_chrome.h"
#include "terminal_scrollbar.h"
#include "terminal_title_metadata.h"

#include "vnm_terminal/vnm_terminal_surface.h"

#include "vnm_qml_chrome/vnm_chrome_geometry.h"

#include "vnm_terminal/internal/hierarchical_profiler.h"
#include "vnm_terminal/internal/qt_grid_metrics_provider.h"
#include "vnm_terminal/internal/vnm_terminal_font.h"
#include "vnm_terminal/internal/vnm_terminal_surface_render_bridge.h"

#include <QByteArray>
#include <QClipboard>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QKeyEvent>
#include <QMetaEnum>
#include <QObject>
#include <QPoint>
#include <QPointF>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRect>
#include <QRectF>
#include <QScreen>
#include <QSettings>
#include <QSize>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QSurfaceFormat>
#include <QTextStream>
#include <QTimer>
#include <QWindow>
#include <QtGlobal>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#ifndef VNM_TERMINAL_VERSION_STRING
#define VNM_TERMINAL_VERSION_STRING "0.0.0"
#endif

#ifndef VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
#define VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED 0
#endif

namespace {

namespace term   = vnm_terminal::internal;
namespace chrome = vnm_terminal::terminal_app;

constexpr int   k_exit_usage_error             = 2;
constexpr int   k_exit_start_failed            = 3;
constexpr int   k_exit_process_failed          = 4;
constexpr int   k_exit_timeout                 = 5;
constexpr int   k_exit_no_output               = 6;
constexpr int   k_timeout_force_exit_grace_ms  = 5000;
constexpr qreal k_custom_titlebar_height             = 32.0;
constexpr qreal k_custom_titlebar_physical_reduction = 2.0;
constexpr qreal k_terminal_scrollbar_width           = 12.0;
constexpr int   k_persisted_window_min_axis    = 1;
constexpr int   k_text_area_resize_max_rows    = 512;
constexpr int   k_text_area_resize_max_columns = 512;

constexpr qreal k_text_area_resize_max_window_axis = 8192.0;

constexpr char k_window_settings_group[]       = "window";
constexpr char k_window_settings_font_size[]   = "font_size";
constexpr char k_window_settings_height[]      = "height";
constexpr char k_window_settings_maximized[]   = "maximized";
constexpr char k_window_settings_width[]       = "width";
constexpr char k_window_settings_x[]           = "x";
constexpr char k_window_settings_y[]           = "y";
#if defined(_WIN32) || defined(__linux__)
constexpr bool k_custom_titlebar_supported_on_platform = true;
#else
constexpr bool k_custom_titlebar_supported_on_platform = false;
#endif

constexpr bool k_custom_titlebar_default_enabled =
    k_custom_titlebar_supported_on_platform;

QString default_window_title()
{
    return QStringLiteral("vnm_terminal example terminal");
}

struct App_options
{
    QStringList        command;
    QString            working_directory;
    QString            backend_output_capture_path;
    QString            transcript_capture_path;
    QString            profile_text_path;
    QString            metrics_json_path;
    QString            font_family = term::vnm_terminal_default_monospace_font_family();
    qreal              font_size   = term::k_vnm_terminal_default_font_pixel_size;
    QString            theme       = QStringLiteral("default");
    QSize              window_size = QSize(900, 600);
    std::optional<QPoint> window_position;
    VNM_TerminalSurface::Alternate_screen_wheel_policy alternate_screen_wheel_policy =
        VNM_TerminalSurface::Alternate_screen_wheel_policy::MOUSE_REPORTING_FIRST;
    VNM_TerminalSurface::Synchronized_output_scroll_policy synchronized_output_scroll_policy =
        VNM_TerminalSurface::Synchronized_output_scroll_policy::DEFER_UNTIL_CONTENT_PUBLICATION;
    std::optional<int> timeout_ms;
    std::optional<int> scrollback_limit;
    bool               shell_requested                    = false;
    bool               keep_open_after_process_exits      = false;
    bool               require_output                     = false;
    bool               custom_titlebar                    = k_custom_titlebar_default_enabled;
    bool               selection_trace_enabled            = false;
    bool               transcript_snapshot_diagnostics    = false;
    bool               transcript_timing_diagnostics      = false;
    bool               wheel_trace_enabled                 = false;
    std::optional<bool> primary_repaint_recovery_enabled;
    bool               font_size_explicit                 = false;
    bool               window_size_explicit               = false;
    bool               restore_maximized_window_state     = false;
};

struct Parse_result
{
    App_options        options;
    QString            error;
    bool               help_requested                = false;
};

struct Runtime_state
{
    int                backend_error_count = 0;
    int                process_exit_code   = 0;
    qint64             first_output_elapsed_ms = -1;
    VNM_TerminalSurface::Exit_reason process_exit_reason =
        VNM_TerminalSurface::Exit_reason::EXITED;
    bool               output_seen     = false;
    bool               process_exited  = false;
    bool               timeout_expired = false;
};

struct metrics_timing_t
{
    qint64             app_elapsed_ms           = 0;
    qint64             profile_write_elapsed_ms = 0;
    bool               profile_text_requested   = false;
};

struct Terminal_shell_geometry
{
    QRectF             chrome_rect;
    QRectF             content_border_rect;
    QRectF             terminal_rect;
    QRectF             scrollbar_rect;
};

struct Persisted_terminal_window_state
{
    std::optional<QPoint> position;
    std::optional<QSize>  size;
    std::optional<qreal>  font_size;
    bool                  maximized = false;
};

bool persisted_window_axis_is_valid(int value)
{
    return
        value >= k_persisted_window_min_axis &&
        value <= static_cast<int>(k_text_area_resize_max_window_axis);
}

std::optional<int> settings_int_value(QSettings& settings, const char* key)
{
    if (!settings.contains(QLatin1String(key))) {
        return std::nullopt;
    }

    bool      ok    = false;
    const int value = settings.value(QLatin1String(key)).toInt(&ok);
    if (!ok) {
        return std::nullopt;
    }

    return value;
}

std::optional<qreal> settings_font_size(QSettings& settings)
{
    if (!settings.contains(QLatin1String(k_window_settings_font_size))) {
        return std::nullopt;
    }

    bool         ok        = false;
    const double font_size =
        settings.value(QLatin1String(k_window_settings_font_size)).toDouble(&ok);
    if (!ok || !std::isfinite(font_size) || font_size <= 0.0) {
        return std::nullopt;
    }

    return static_cast<qreal>(font_size);
}

std::optional<QSize> settings_window_size(QSettings& settings)
{
    const std::optional<int> width  = settings_int_value(settings, k_window_settings_width);
    const std::optional<int> height = settings_int_value(settings, k_window_settings_height);
    if (!width.has_value() || !height.has_value()) {
        return std::nullopt;
    }

    if (!persisted_window_axis_is_valid(*width) ||
        !persisted_window_axis_is_valid(*height))
    {
        return std::nullopt;
    }

    return QSize(*width, *height);
}

std::optional<QPoint> settings_window_position(QSettings& settings)
{
    const std::optional<int> x = settings_int_value(settings, k_window_settings_x);
    const std::optional<int> y = settings_int_value(settings, k_window_settings_y);
    if (!x.has_value() || !y.has_value()) {
        return std::nullopt;
    }

    return QPoint(*x, *y);
}

Persisted_terminal_window_state load_persisted_terminal_window_state(
    QSettings& settings)
{
    Persisted_terminal_window_state state;
    settings.beginGroup(QLatin1String(k_window_settings_group));
    state.font_size = settings_font_size(settings);
    state.size      = settings_window_size(settings);
    state.position  = settings_window_position(settings);
    state.maximized =
        settings.value(QLatin1String(k_window_settings_maximized), false).toBool();
    settings.endGroup();
    return state;
}

void save_persisted_terminal_window_state(
    QSettings& settings,
    const Persisted_terminal_window_state& state)
{
    settings.beginGroup(QLatin1String(k_window_settings_group));
    if (state.font_size.has_value() &&
        std::isfinite(*state.font_size) &&
        *state.font_size > 0.0)
    {
        settings.setValue(QLatin1String(k_window_settings_font_size), *state.font_size);
    }

    if (state.size.has_value() &&
        persisted_window_axis_is_valid(state.size->width()) &&
        persisted_window_axis_is_valid(state.size->height()))
    {
        settings.setValue(QLatin1String(k_window_settings_width),  state.size->width());
        settings.setValue(QLatin1String(k_window_settings_height), state.size->height());
    }

    if (state.position.has_value()) {
        settings.setValue(QLatin1String(k_window_settings_x), state.position->x());
        settings.setValue(QLatin1String(k_window_settings_y), state.position->y());
    }

    settings.setValue(QLatin1String(k_window_settings_maximized), state.maximized);
    settings.endGroup();
    settings.sync();
}

bool terminal_window_persistence_enabled()
{
    return QGuiApplication::platformName() != QStringLiteral("offscreen");
}

bool window_geometry_intersects_available_screen(
    const QPoint& position,
    const QSize&  size)
{
    const QRect window_rect(position, size);
    for (const QScreen* screen : QGuiApplication::screens()) {
        if (screen->availableGeometry().intersects(window_rect)) {
            return true;
        }
    }

    return false;
}

void apply_persisted_terminal_window_state(
    const Persisted_terminal_window_state& state,
    App_options*                           options)
{
    if (!options->font_size_explicit && state.font_size.has_value()) {
        options->font_size = *state.font_size;
    }

    if (!options->window_size_explicit && state.size.has_value()) {
        options->window_size = *state.size;
    }

    if (state.position.has_value() &&
        window_geometry_intersects_available_screen(
            *state.position, options->window_size))
    {
        options->window_position = *state.position;
    }

    options->restore_maximized_window_state =
        state.maximized && !options->window_size_explicit;
}

std::optional<Persisted_terminal_window_state> restorable_terminal_window_state(
    const QWindow&              window,
    const VNM_TerminalSurface&  surface)
{
    const Qt::WindowStates window_states = window.windowStates();
    if (window_states.testFlag(Qt::WindowMinimized) ||
        window_states.testFlag(Qt::WindowMaximized) ||
        window_states.testFlag(Qt::WindowFullScreen))
    {
        return std::nullopt;
    }

    const QSize size = window.size();
    if (!persisted_window_axis_is_valid(size.width()) ||
        !persisted_window_axis_is_valid(size.height()))
    {
        return std::nullopt;
    }

    Persisted_terminal_window_state state;
    state.position  = window.position();
    state.size      = size;
    state.font_size = surface.font_size();
    state.maximized = false;
    return state;
}

bool custom_titlebar_supported_on_platform()
{
    return k_custom_titlebar_supported_on_platform;
}

class Terminal_shortcut_filter final : public QObject
{
public:
    explicit Terminal_shortcut_filter(VNM_TerminalSurface* surface)
    :
        QObject(surface),
        m_surface(surface)
    {}

protected:
    bool eventFilter(QObject*, QEvent* event) override
    {
        if (event->type() != QEvent::KeyPress) {
            return false;
        }

        auto* key_event = static_cast<QKeyEvent*>(event);
        const Qt::KeyboardModifiers modifiers =
            key_event->modifiers() &
            (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier);
        const bool control_paste_shortcut =
            key_event->key() == Qt::Key_V &&
            (modifiers == Qt::ControlModifier ||
             modifiers == (Qt::ControlModifier | Qt::ShiftModifier));
#if defined(Q_OS_MACOS)
        const bool platform_paste_shortcut =
            key_event->key() == Qt::Key_V &&
            modifiers       == Qt::MetaModifier;
#else
        constexpr bool platform_paste_shortcut = false;
#endif
        const bool paste_shortcut = control_paste_shortcut || platform_paste_shortcut;
#if defined(Q_OS_MACOS)
        const bool copy_shortcut =
            key_event->key() == Qt::Key_C &&
            modifiers       == Qt::MetaModifier;
#else
        constexpr bool copy_shortcut = false;
#endif
        if (!paste_shortcut && !copy_shortcut) {
            return false;
        }

        if (!m_surface->hasActiveFocus()) {
            return false;
        }

        if (copy_shortcut) {
            return copy_selected_text();
        }

        return paste_clipboard_text();
    }

private:
    bool copy_selected_text()
    {
        QClipboard* clipboard = QGuiApplication::clipboard();
        if (clipboard == nullptr) {
            return true;
        }

        const QString text = m_surface->selected_text();
        if (!text.isEmpty()) {
            clipboard->setText(text);
        }
        return true;
    }

    bool paste_clipboard_text()
    {
        QClipboard* clipboard = QGuiApplication::clipboard();
        if (clipboard != nullptr) {
            m_surface->paste_text(clipboard->text());
            return true;
        }

        return false;
    }

    VNM_TerminalSurface* m_surface = nullptr;
};

class Wheel_delivery_indicator_filter final : public QObject
{
public:
    explicit Wheel_delivery_indicator_filter(chrome::Terminal_qml_chrome& titlebar)
    :
        QObject(&titlebar),
        m_titlebar(titlebar)
    {}

protected:
    bool eventFilter(QObject*, QEvent* event) override
    {
        if (event->type() == QEvent::Wheel) {
            m_titlebar.pulse_wheel_delivery_indicator();
        }

        return false;
    }

private:
    chrome::Terminal_qml_chrome& m_titlebar;
};

void install_wheel_delivery_indicator_filter(
    VNM_TerminalSurface&             surface,
    chrome::Terminal_scrollbar&      scrollbar,
    chrome::Terminal_qml_chrome*     titlebar,
    bool                             enabled)
{
    if (!enabled || titlebar == nullptr) {
        return;
    }

    auto* filter = new Wheel_delivery_indicator_filter(*titlebar);
    surface.installEventFilter(filter);
    scrollbar.installEventFilter(filter);
}

void split_terminal_area(
    Terminal_shell_geometry& geometry,
    const QRectF&            area)
{
    const qreal scrollbar_width = std::min(k_terminal_scrollbar_width, area.width());
    geometry.terminal_rect  = QRectF(
        area.left(),
        area.top(),
        std::max<qreal>(0.0, area.width() - scrollbar_width),
        area.height());
    geometry.scrollbar_rect = QRectF(
        area.right() - scrollbar_width,
        area.top(),
        scrollbar_width,
        area.height());
}

void snap_terminal_shell_geometry(
    Terminal_shell_geometry& geometry,
    qreal                    device_pixel_ratio)
{
    const qreal dpr =
        vnm_qml_chrome::normalized_device_pixel_ratio(device_pixel_ratio);
    geometry.content_border_rect = vnm_qml_chrome::snapped_logical_rect(
        geometry.content_border_rect,
        dpr);
    geometry.terminal_rect = vnm_qml_chrome::snapped_logical_rect(
        geometry.terminal_rect,
        dpr);
    geometry.scrollbar_rect = vnm_qml_chrome::snapped_logical_rect(
        geometry.scrollbar_rect,
        dpr);
}

qreal reduced_chrome_span(
    qreal logical_span,
    qreal physical_reduction,
    qreal device_pixel_ratio)
{
    const qreal dpr = vnm_qml_chrome::normalized_device_pixel_ratio(device_pixel_ratio);
    const qreal snapped_logical_span =
        vnm_qml_chrome::snapped_logical_edge(logical_span, dpr);
    const qreal logical_reduction = physical_reduction / dpr;
    return std::max<qreal>(
        0.0,
        snapped_logical_span - logical_reduction);
}

qreal reduced_frameless_resize_border_width(qreal device_pixel_ratio)
{
    return reduced_chrome_span(
        chrome::k_default_frameless_resize_border_width,
        chrome::k_frameless_resize_border_physical_reduction,
        device_pixel_ratio);
}

qreal reduced_custom_titlebar_height(qreal device_pixel_ratio)
{
    return reduced_chrome_span(
        k_custom_titlebar_height,
        k_custom_titlebar_physical_reduction,
        device_pixel_ratio);
}

Terminal_shell_geometry terminal_shell_geometry(
    const QSizeF&  window_size,
    bool           custom_titlebar,
    bool           resize_border_active = true,
    qreal          content_border_width = 0.0,
    qreal          device_pixel_ratio   = 1.0)
{
    const qreal width  = std::max<qreal>(0.0, window_size.width());
    const qreal height = std::max<qreal>(0.0, window_size.height());

    Terminal_shell_geometry geometry;
    if (!custom_titlebar) {
        split_terminal_area(geometry, QRectF(0.0, 0.0, width, height));
        return geometry;
    }

    const qreal border = resize_border_active
        ? reduced_frameless_resize_border_width(device_pixel_ratio)
        : 0.0;
    const qreal titlebar_height =
        std::min(reduced_custom_titlebar_height(device_pixel_ratio), height);
    const qreal horizontal_inset = std::min(border, width / 2.0);
    const qreal terminal_width_available =
        std::max<qreal>(0.0, width - horizontal_inset * 2.0);
    const qreal terminal_height_available = std::max<qreal>(0.0, height - titlebar_height);
    const qreal bottom_inset              = std::min(border, terminal_height_available);
    const qreal frame_border =
        std::isfinite(content_border_width)
            ? std::max<qreal>(0.0, content_border_width)
            : 0.0;

    geometry.chrome_rect = QRectF(0.0, 0.0, width, titlebar_height);
    geometry.content_border_rect = QRectF(
        horizontal_inset,
        titlebar_height,
        terminal_width_available,
        std::max<qreal>(0.0, terminal_height_available - bottom_inset));
    const qreal content_horizontal_inset =
        std::min(frame_border, geometry.content_border_rect.width() / 2.0);
    const qreal content_vertical_inset =
        std::min(frame_border, geometry.content_border_rect.height() / 2.0);
    split_terminal_area(
        geometry,
        geometry.content_border_rect.adjusted(
            content_horizontal_inset,
            content_vertical_inset,
            -content_horizontal_inset,
            -content_vertical_inset));
    snap_terminal_shell_geometry(geometry, device_pixel_ratio);
    return geometry;
}

qreal logical_device_pixel_width(const QQuickWindow& window)
{
    const qreal device_pixel_ratio = window.devicePixelRatio();
    if (!std::isfinite(device_pixel_ratio) || device_pixel_ratio <= 0.0) {
        return 1.0;
    }

    return 1.0 / device_pixel_ratio;
}

bool custom_titlebar_resize_border_active(const QQuickWindow& window)
{
    const Qt::WindowStates states = window.windowStates();
    return
        !states.testFlag(Qt::WindowMaximized) &&
        !states.testFlag(Qt::WindowMinimized) &&
        !states.testFlag(Qt::WindowFullScreen);
}

void apply_terminal_shell_geometry(
    QQuickWindow&                  window,
    VNM_TerminalSurface&           surface,
    chrome::Terminal_scrollbar&    scrollbar,
    chrome::Terminal_qml_chrome*   titlebar,
    bool                           custom_titlebar)
{
    const qreal content_border_width = titlebar != nullptr
        ? logical_device_pixel_width(window)
        : 0.0;
    const Terminal_shell_geometry geometry = terminal_shell_geometry(
        QSizeF(window.width(), window.height()),
        custom_titlebar,
        custom_titlebar_resize_border_active(window),
        content_border_width,
        window.devicePixelRatio());

    if (titlebar != nullptr) {
        titlebar->set_size(QSizeF(window.width(), window.height()));
        titlebar->set_content_border_rect(
            geometry.content_border_rect,
            content_border_width);
        if (QQuickItem* root_item = titlebar->root_item()) {
            root_item->setVisible(custom_titlebar);
        }
    }

    surface.setPosition(geometry.terminal_rect.topLeft());
    surface.setSize(geometry.terminal_rect.size());
    scrollbar.setPosition(geometry.scrollbar_rect.topLeft());
    scrollbar.setSize(geometry.scrollbar_rect.size());
}

void apply_synchronized_output_scroll_policy_option(
    VNM_TerminalSurface& surface,
    const App_options&   options)
{
    surface.set_synchronized_output_scroll_policy(
        options.synchronized_output_scroll_policy);
}

void apply_primary_repaint_recovery_option(
    VNM_TerminalSurface& surface,
    const App_options&   options)
{
    if (options.primary_repaint_recovery_enabled.has_value()) {
        surface.set_primary_repaint_recovery_enabled(
            *options.primary_repaint_recovery_enabled);
    }
}

void apply_scrollback_limit_option(
    VNM_TerminalSurface& surface,
    const App_options&   options)
{
    if (options.scrollback_limit.has_value()) {
        surface.set_scrollback_limit(*options.scrollback_limit);
    }
}

bool resize_window_for_text_area_request(
    QQuickWindow&                  window,
    const VNM_TerminalSurface&     surface,
    int                            rows,
    int                            columns)
{
    if (rows <= 0                            || columns <= 0                              ||
        rows >  k_text_area_resize_max_rows  || columns >  k_text_area_resize_max_columns ||
        surface.rows() <= 0                  || surface.columns() <= 0                    ||
        surface.width() <= 0.0               || surface.height() <= 0.0)
    {
        return false;
    }

    const term::Qt_grid_metrics_provider metrics_provider(
        term::vnm_terminal_font(surface.font_family(), surface.font_size()),
        window.devicePixelRatio());
    const term::terminal_cell_metrics_t cell_metrics =
        metrics_provider.cell_metrics();
    if (!term::is_valid_cell_metrics(cell_metrics)) {
        return false;
    }

    const qreal requested_surface_width =
        cell_metrics.width * static_cast<qreal>(columns);
    const qreal requested_surface_height =
        cell_metrics.height * static_cast<qreal>(rows);
    const qreal requested_window_width = std::clamp<qreal>(
        static_cast<qreal>(window.width()) + requested_surface_width - surface.width(),
        1.0,
        k_text_area_resize_max_window_axis);
    const qreal requested_window_height = std::clamp<qreal>(
        static_cast<qreal>(window.height()) + requested_surface_height - surface.height(),
        1.0,
        k_text_area_resize_max_window_axis);

    const QSize requested_size(
        static_cast<int>(std::round(requested_window_width)),
        static_cast<int>(std::round(requested_window_height)));
    if (requested_size == window.size()) {
        return false;
    }

    window.resize(requested_size);
    return true;
}

QString visible_terminal_title(QString terminal_title)
{
    terminal_title = terminal_title.trimmed();
    return terminal_title.isEmpty() ? default_window_title() : terminal_title;
}

void sync_terminal_title(
    QQuickWindow&                  window,
    chrome::Terminal_qml_chrome*   titlebar,
    const QString&                 terminal_title,
    const QString&                 terminal_icon_name)
{
    const QString visible_title = visible_terminal_title(terminal_title);
    window.setTitle(visible_title);
    if (titlebar != nullptr) {
        const chrome::Terminal_title_content content =
            chrome::derive_terminal_title_content(visible_title, terminal_icon_name);
        titlebar->set_title(content.display_title);
        titlebar->set_activity_marker_text(chrome::activity_marker_text(content));
    }
}

void connect_terminal_metadata_to_chrome(
    VNM_TerminalSurface&           surface,
    QQuickWindow&                  window,
    chrome::Terminal_qml_chrome*   titlebar)
{
    auto sync_metadata = [titlebar, &window, &surface] {
        sync_terminal_title(
            window,
            titlebar,
            surface.terminal_title(),
            surface.terminal_icon_name());
    };

    QObject::connect(
        &surface,
        &VNM_TerminalSurface::terminal_title_changed,
        &window,
        sync_metadata);
    QObject::connect(
        &surface,
        &VNM_TerminalSurface::terminal_icon_name_changed,
        &window,
        sync_metadata);
    sync_metadata();
}

void sync_chrome_window_state(
    chrome::Terminal_qml_chrome&    titlebar,
    QQuickWindow&                  window)
{
    titlebar.set_active(window.isActive());
    titlebar.set_maximized(
        window.windowStates().testFlag(Qt::WindowMaximized) ||
        window.windowStates().testFlag(Qt::WindowFullScreen));
    titlebar.set_resize_enabled(custom_titlebar_resize_border_active(window));
    window.setColor(chrome::terminal_chrome_background_color(window.isActive()));
}

void print_error(const QString& message)
{
    const QByteArray bytes = message.toUtf8();
    std::cerr << "vnm_terminal: " << bytes.constData() << '\n';
}

void print_usage()
{
    std::cout
        << "usage: vnm_terminal [options]\n"
        << "       vnm_terminal [options] -- <program> [args...]\n"
        << "\n"
        << "options:\n"
        << "  --shell                         launch the default shell; also the default without --\n"
        << "  --cwd <path>                    launch in a working directory\n"
        << "  --font-family <family>          terminal font family\n"
        << "  --font-size <pixels>            terminal font size in pixels\n"
        << "  --theme <name>                  terminal color theme\n"
        << "  --scrollback-limit <rows>       maximum retained scrollback rows\n"
        << "  --window-size <width>x<height>  window size in logical pixels\n"
#if defined(_WIN32) || defined(__linux__)
        << "  --native-titlebar               use the platform titlebar instead of built-in chrome\n"
#endif
        << "  --alternate-wheel <mode>        alternate-screen wheel: mouse(default), cursor, or page\n"
        << "  --synchronized-output-scroll-policy=<policy>\n"
        << "                                  DEC synchronized-output scroll: defer(default) "
        << "or immediate-public (case-insensitive)\n"
        << "  --disable-primary-repaint-recovery\n"
        << "                                  disable primary repaint scrollback recovery "
        << "when enabled\n"
        << "  --capture-output <path>         write raw backend output bytes to a file\n"
        << "  --metrics-json <path>           write lightweight terminal runtime metrics\n"
#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
        << "  --capture-transcript <path>     write sensitive NDJSON replay transcript\n"
        << "  --transcript-snapshot-diagnostics include visible row text/provenance snapshots\n"
        << "  --transcript-timing-diagnostics include thresholded transcript hot-path timings\n"
        << "  --wheel-trace                   include diagnostic wheel routing events in transcript\n"
#endif
        << "  --selection-trace               write selection diagnostics to stderr\n"
#if VNM_TERMINAL_PROFILING_ENABLED
        << "  --profile-text <path>           write profile and dirty-row diagnostics\n"
#endif
        << "  --keep-open-after-process-exits leave the window open after the child exits\n"
        << "  --timeout-ms <n>                fail if the run is still active after n ms\n"
        << "  --require-output                fail if no terminal output activity is observed\n"
        << "  --help                          show this help\n"
        << "\n"
        << "interactions:\n"
        << "  mouse-reporting apps receive unmodified mouse drags; Shift-drag selects locally\n"
#if defined(Q_OS_MACOS)
        << "  Command+C copies selected text; Command+V pastes clipboard text; "
        << "Ctrl+C sends terminal input\n"
#else
        << "  Ctrl+C copies selected text, otherwise sends Ctrl+C; "
        << "Ctrl+V/Ctrl+Shift+V paste clipboard text\n"
#endif
        << "  OSC 52 clipboard writes are allowed for target c/clipboard and denied otherwise\n";
}

template <typename T>
QString enum_key(T value)
{
    const QMetaEnum meta = QMetaEnum::fromType<T>();
    const char*     key  = meta.valueToKey(static_cast<int>(value));
    if (key != nullptr) {
        return QString::fromLatin1(key);
    }

    return QString::number(static_cast<int>(value));
}

QString environment_or_default(const char* name, const QString& fallback)
{
    const QString value = QString::fromLocal8Bit(qgetenv(name));
    return value.trimmed().isEmpty() ? fallback : value;
}

QStringList default_shell_argv()
{
#if defined(_WIN32)
    return {environment_or_default("COMSPEC", QStringLiteral("cmd.exe"))};
#elif defined(__linux__) || defined(__APPLE__)
    return {environment_or_default("SHELL", QStringLiteral("/bin/sh"))};
#else
    return {};
#endif
}

bool argument_is(const QString& argument, const char* expected)
{
    return argument == QLatin1String(expected);
}

bool take_option_value(
    const QStringList& arguments,
    int&               index,
    QString*           out_value,
    QString*           out_error)
{
    if (index + 1 >= arguments.size()) {
        *out_error = QStringLiteral("%1 requires a value").arg(arguments[index]);
        return false;
    }

    *out_value = arguments[index + 1];
    index += 2;
    return true;
}

std::optional<QSize> parse_window_size(const QString& value)
{
    int separator = value.indexOf(QLatin1Char('x'));
    if (separator < 0) {
        separator = value.indexOf(QLatin1Char('X'));
    }

    if (separator <= 0 || separator + 1 >= value.size()) {
        return std::nullopt;
    }

    bool      width_ok  = false;
    bool      height_ok = false;
    const int width     = value.left(separator).toInt(&width_ok);
    const int height    = value.mid(separator + 1).toInt(&height_ok);
    if (!width_ok || !height_ok || width <= 0 || height <= 0) {
        return std::nullopt;
    }

    return QSize(width, height);
}

bool parse_font_size(
    const QString&         value,
    qreal*                 out_font_size,
    QString*               out_error)
{
    bool         ok        = false;
    const double font_size = value.toDouble(&ok);
    if (!ok || !std::isfinite(font_size) || font_size <= 0.0) {
        *out_error = QStringLiteral("--font-size requires a positive pixel size");
        return false;
    }

    *out_font_size = static_cast<qreal>(font_size);
    return true;
}

bool parse_timeout_ms(
    const QString&         value,
    std::optional<int>*    out_timeout_ms,
    QString*               out_error)
{
    bool ok = false;
    const qlonglong timeout_ms = value.toLongLong(&ok);
    if (!ok ||
        timeout_ms <= 0 ||
        timeout_ms >  static_cast<qlonglong>(std::numeric_limits<int>::max()))
    {
        *out_error = QStringLiteral("--timeout-ms requires a positive integer");
        return false;
    }

    *out_timeout_ms = static_cast<int>(timeout_ms);
    return true;
}

bool parse_scrollback_limit(
    const QString&         value,
    std::optional<int>*    out_scrollback_limit,
    QString*               out_error)
{
    bool ok = false;
    const qlonglong limit = value.toLongLong(&ok);
    if (!ok || limit < 0 || limit > static_cast<qlonglong>(std::numeric_limits<int>::max())) {
        *out_error = QStringLiteral("--scrollback-limit requires a non-negative integer");
        return false;
    }

    *out_scrollback_limit = static_cast<int>(limit);
    return true;
}

bool parse_alternate_wheel_policy(
    const QString&         value,
    VNM_TerminalSurface::Alternate_screen_wheel_policy*
                           out_policy,
    QString*               out_error)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("page")) {
        *out_policy = VNM_TerminalSurface::Alternate_screen_wheel_policy::PAGE_KEYS;
        return true;
    }
    if (normalized == QStringLiteral("cursor")) {
        *out_policy = VNM_TerminalSurface::Alternate_screen_wheel_policy::CURSOR_KEYS;
        return true;
    }
    if (normalized == QStringLiteral("mouse")) {
        *out_policy =
            VNM_TerminalSurface::Alternate_screen_wheel_policy::MOUSE_REPORTING_FIRST;
        return true;
    }

    *out_error = QStringLiteral("--alternate-wheel supports only page, cursor, or mouse");
    return false;
}

bool parse_synchronized_output_scroll_policy(
    const QString&         value,
    VNM_TerminalSurface::Synchronized_output_scroll_policy*
                           out_policy,
    QString*               out_error)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("defer")) {
        *out_policy = VNM_TerminalSurface::Synchronized_output_scroll_policy::
            DEFER_UNTIL_CONTENT_PUBLICATION;
        return true;
    }
    if (normalized == QStringLiteral("immediate-public")) {
        *out_policy = VNM_TerminalSurface::Synchronized_output_scroll_policy::
            IMMEDIATE_PUBLIC_PROJECTION;
        return true;
    }

    *out_error = QStringLiteral(
        "--synchronized-output-scroll-policy supports only defer or immediate-public");
    return false;
}

bool take_synchronized_output_scroll_policy_value(
    const QString&  argument,
    QString*        out_value,
    QString*        out_error)
{
    if (argument_is(argument, "--synchronized-output-scroll-policy")) {
        *out_error = QStringLiteral(
            "--synchronized-output-scroll-policy requires =defer or =immediate-public");
        return false;
    }

    const QString prefix =
        QStringLiteral("--synchronized-output-scroll-policy=");
    if (!argument.startsWith(prefix)) {
        return false;
    }

    *out_value = argument.mid(prefix.size());
    return true;
}

QString comparable_capture_path(QString path)
{
    path = QDir::cleanPath(std::move(path));
#if defined(Q_OS_WIN)
    path = path.toCaseFolded();
#endif
    return path;
}

bool validate_capture_path(
    const QString& option_name,
    const QString& path,
    QString*       out_absolute_path,
    QString*       out_error)
{
    if (path.trimmed().isEmpty()) {
        *out_error = QStringLiteral("%1 requires a non-empty path").arg(option_name);
        return false;
    }

    const QFileInfo file_info(path);
    const QDir parent_dir = file_info.absoluteDir();
    if (!parent_dir.exists()) {
        *out_error = QStringLiteral("%1 parent directory does not exist: %2")
            .arg(option_name, parent_dir.absolutePath());
        return false;
    }
    if (file_info.exists() && file_info.isDir()) {
        *out_error = QStringLiteral("%1 points to a directory: %2")
            .arg(option_name, file_info.absoluteFilePath());
        return false;
    }

    *out_absolute_path = file_info.absoluteFilePath();
    return true;
}

bool validate_capture_paths(App_options* options, QString* out_error)
{
    if (!options->backend_output_capture_path.isEmpty() &&
        !validate_capture_path(
            QStringLiteral("--capture-output"),
            options->backend_output_capture_path,
            &options->backend_output_capture_path,
            out_error))
    {
        return false;
    }

    if (!options->transcript_capture_path.isEmpty() &&
        !validate_capture_path(
            QStringLiteral("--capture-transcript"),
            options->transcript_capture_path,
            &options->transcript_capture_path,
            out_error))
    {
        return false;
    }

    if (!options->metrics_json_path.isEmpty() &&
        !validate_capture_path(
            QStringLiteral("--metrics-json"),
            options->metrics_json_path,
            &options->metrics_json_path,
            out_error))
    {
        return false;
    }

#if VNM_TERMINAL_PROFILING_ENABLED
    if (!options->profile_text_path.isEmpty() &&
        !validate_capture_path(
            QStringLiteral("--profile-text"),
            options->profile_text_path,
            &options->profile_text_path,
            out_error))
    {
        return false;
    }
#endif

    if (!options->backend_output_capture_path.isEmpty() &&
        !options->transcript_capture_path.isEmpty() &&
        comparable_capture_path(options->backend_output_capture_path) ==
            comparable_capture_path(options->transcript_capture_path))
    {
        *out_error = QStringLiteral(
            "--capture-output and --capture-transcript must use different paths: %1")
            .arg(options->backend_output_capture_path);
        return false;
    }

    if (!options->backend_output_capture_path.isEmpty() &&
        !options->metrics_json_path.isEmpty() &&
        comparable_capture_path(options->backend_output_capture_path) ==
            comparable_capture_path(options->metrics_json_path))
    {
        *out_error = QStringLiteral(
            "--capture-output and --metrics-json must use different paths: %1")
            .arg(options->backend_output_capture_path);
        return false;
    }

    if (!options->transcript_capture_path.isEmpty() &&
        !options->metrics_json_path.isEmpty() &&
        comparable_capture_path(options->transcript_capture_path) ==
            comparable_capture_path(options->metrics_json_path))
    {
        *out_error = QStringLiteral(
            "--capture-transcript and --metrics-json must use different paths: %1")
            .arg(options->transcript_capture_path);
        return false;
    }

#if VNM_TERMINAL_PROFILING_ENABLED
    if (!options->profile_text_path.isEmpty() &&
        !options->metrics_json_path.isEmpty() &&
        comparable_capture_path(options->profile_text_path) ==
            comparable_capture_path(options->metrics_json_path))
    {
        *out_error = QStringLiteral(
            "--profile-text and --metrics-json must use different paths: %1")
            .arg(options->profile_text_path);
        return false;
    }
#endif

    return true;
}

bool prepare_capture_file(
    const QString& option_name,
    const QString& path,
    QString*       out_error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *out_error = QStringLiteral("%1 could not open %2: %3")
            .arg(option_name, path, file.errorString());
        return false;
    }

    return true;
}

#if VNM_TERMINAL_PROFILING_ENABLED
bool prepare_profile_text_file(
    const QString& path,
    QString*       out_error)
{
    if (path.trimmed().isEmpty()) {
        *out_error = QStringLiteral("--profile-text requires a non-empty path");
        return false;
    }

    const QFileInfo file_info(path);
    const QDir parent_dir = file_info.absoluteDir();
    if (!parent_dir.exists()) {
        *out_error = QStringLiteral("--profile-text parent directory does not exist: %1")
            .arg(parent_dir.absolutePath());
        return false;
    }
    if (file_info.exists() && file_info.isDir()) {
        *out_error = QStringLiteral("--profile-text points to a directory: %1")
            .arg(file_info.absoluteFilePath());
        return false;
    }

    return true;
}
#endif

Parse_result parse_arguments(const QStringList& arguments)
{
    Parse_result result;
    bool explicit_command_separator = false;

    int index = 1;
    while (index < arguments.size()) {
        const QString argument = arguments[index];

        if (argument_is(argument, "--")) {
            explicit_command_separator = true;
            result.options.command = arguments.mid(index + 1);
            break;
        }

        if (argument_is(argument, "--help") || argument_is(argument, "-h")) {
            result.help_requested = true;
            return result;
        }

        if (argument_is(argument, "--shell")) {
            result.options.shell_requested = true;
            ++index;
            continue;
        }

        if (argument_is(argument, "--exit-when-process-exits")) {
            result.options.keep_open_after_process_exits = false;
            ++index;
            continue;
        }

        if (argument_is(argument, "--keep-open-after-process-exits")) {
            result.options.keep_open_after_process_exits = true;
            ++index;
            continue;
        }

        if (argument_is(argument, "--require-output")) {
            result.options.require_output = true;
            ++index;
            continue;
        }

        if (argument_is(argument, "--disable-primary-repaint-recovery")) {
            result.options.primary_repaint_recovery_enabled = false;
            ++index;
            continue;
        }

        if (argument_is(argument, "--selection-trace")) {
            result.options.selection_trace_enabled = true;
            ++index;
            continue;
        }

        if (argument_is(argument, "--native-titlebar")) {
            if (!custom_titlebar_supported_on_platform()) {
                result.error = QStringLiteral(
                    "--native-titlebar is supported only after platform validation");
                return result;
            }

            result.options.custom_titlebar = false;
            ++index;
            continue;
        }

        QString value;
        if (argument_is(argument, "--cwd")) {
            if (!take_option_value(arguments, index, &value, &result.error)) {
                return result;
            }

            result.options.working_directory = value;
            continue;
        }

        if (argument_is(argument, "--font-family")) {
            if (!take_option_value(arguments, index, &value, &result.error)) {
                return result;
            }

            result.options.font_family = value;
            continue;
        }

        if (argument_is(argument, "--font-size")) {
            if (!take_option_value(arguments, index, &value, &result.error) ||
                !parse_font_size(value, &result.options.font_size, &result.error))
            {
                return result;
            }

            result.options.font_size_explicit = true;
            continue;
        }

        if (argument_is(argument, "--theme")) {
            if (!take_option_value(arguments, index, &value, &result.error)) {
                return result;
            }

            result.options.theme = value;
            continue;
        }

        if (argument_is(argument, "--window-size")) {
            if (!take_option_value(arguments, index, &value, &result.error)) {
                return result;
            }

            const std::optional<QSize> window_size = parse_window_size(value);
            if (!window_size.has_value()) {
                result.error = QStringLiteral(
                    "--window-size requires <positive-width>x<positive-height>");
                return result;
            }

            result.options.window_size = *window_size;
            result.options.window_size_explicit = true;
            continue;
        }

        if (argument_is(argument, "--scrollback-limit")) {
            if (!take_option_value(arguments, index, &value, &result.error) ||
                !parse_scrollback_limit(value, &result.options.scrollback_limit, &result.error))
            {
                return result;
            }

            continue;
        }

        if (argument_is(argument, "--alternate-wheel")) {
            if (!take_option_value(arguments, index, &value, &result.error) ||
                !parse_alternate_wheel_policy(
                    value, &result.options.alternate_screen_wheel_policy, &result.error))
            {
                return result;
            }

            continue;
        }

        if (take_synchronized_output_scroll_policy_value(
                argument, &value, &result.error))
        {
            if (!parse_synchronized_output_scroll_policy(
                    value, &result.options.synchronized_output_scroll_policy, &result.error))
            {
                return result;
            }

            ++index;
            continue;
        }
        if (!result.error.isEmpty()) {
            return result;
        }

        if (argument_is(argument, "--capture-output")) {
            if (!take_option_value(arguments, index, &value, &result.error)) {
                return result;
            }
            if (value.trimmed().isEmpty()) {
                result.error = QStringLiteral("--capture-output requires a non-empty path");
                return result;
            }

            result.options.backend_output_capture_path = value;
            continue;
        }

        if (argument_is(argument, "--capture-transcript")) {
#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
            if (!take_option_value(arguments, index, &value, &result.error)) {
                return result;
            }
            if (value.trimmed().isEmpty()) {
                result.error = QStringLiteral("--capture-transcript requires a non-empty path");
                return result;
            }

            result.options.transcript_capture_path = value;
            continue;
#else
            result.error = QStringLiteral(
                "--capture-transcript is unavailable because transcript capture/replay is disabled in this build");
            return result;
#endif
        }

        if (argument_is(argument, "--transcript-snapshot-diagnostics")) {
#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
            result.options.transcript_snapshot_diagnostics = true;
            ++index;
            continue;
#else
            result.error = QStringLiteral(
                "--transcript-snapshot-diagnostics is unavailable because transcript capture/replay is disabled in this build");
            return result;
#endif
        }

        if (argument_is(argument, "--transcript-timing-diagnostics")) {
#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
            result.options.transcript_timing_diagnostics = true;
            ++index;
            continue;
#else
            result.error = QStringLiteral(
                "--transcript-timing-diagnostics is unavailable because transcript capture/replay is disabled in this build");
            return result;
#endif
        }

        if (argument_is(argument, "--wheel-trace")) {
#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
            result.options.wheel_trace_enabled = true;
            ++index;
            continue;
#else
            result.error = QStringLiteral(
                "--wheel-trace is unavailable because transcript capture/replay is disabled in this build");
            return result;
#endif
        }

        if (argument_is(argument, "--profile-text")) {
#if VNM_TERMINAL_PROFILING_ENABLED
            if (!take_option_value(arguments, index, &value, &result.error)) {
                return result;
            }
            if (value.trimmed().isEmpty()) {
                result.error = QStringLiteral("--profile-text requires a non-empty path");
                return result;
            }

            result.options.profile_text_path = value;
            continue;
#else
            result.error = QStringLiteral(
                "--profile-text requires VNM_TERMINAL_ENABLE_PROFILING=ON");
            return result;
#endif
        }

        if (argument_is(argument, "--metrics-json")) {
            if (!take_option_value(arguments, index, &value, &result.error)) {
                return result;
            }
            if (value.trimmed().isEmpty()) {
                result.error = QStringLiteral("--metrics-json requires a non-empty path");
                return result;
            }

            result.options.metrics_json_path = value;
            continue;
        }

        if (argument_is(argument, "--timeout-ms")) {
            if (!take_option_value(arguments, index, &value, &result.error) ||
                !parse_timeout_ms(value, &result.options.timeout_ms, &result.error))
            {
                return result;
            }

            continue;
        }

        result.error = QStringLiteral("unexpected argument '%1'; use -- before a command")
            .arg(argument);
        return result;
    }

#if VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED
    if (result.options.wheel_trace_enabled &&
        result.options.transcript_capture_path.isEmpty())
    {
        result.options.wheel_trace_enabled = false;
        result.error = QStringLiteral(
            "--wheel-trace requires --capture-transcript <path>");
        return result;
    }
    if (result.options.transcript_timing_diagnostics &&
        result.options.transcript_capture_path.isEmpty())
    {
        result.options.transcript_timing_diagnostics = false;
        result.error = QStringLiteral(
            "--transcript-timing-diagnostics requires --capture-transcript <path>");
        return result;
    }
    if (result.options.transcript_snapshot_diagnostics &&
        result.options.transcript_capture_path.isEmpty())
    {
        result.options.transcript_snapshot_diagnostics = false;
        result.error = QStringLiteral(
            "--transcript-snapshot-diagnostics requires --capture-transcript <path>");
        return result;
    }
#endif

    if (explicit_command_separator) {
        if (result.options.command.isEmpty()) {
            result.error = QStringLiteral("explicit command after -- must name a program");
            return result;
        }

        if (result.options.shell_requested) {
            result.error = QStringLiteral("--shell cannot be combined with an explicit command");
            return result;
        }
    }
    else {
        result.options.command = default_shell_argv();
        if (result.options.command.isEmpty()) {
            result.error = QStringLiteral("no default shell is available on this platform");
            return result;
        }
    }

    if (result.options.font_family.trimmed().isEmpty()) {
        result.error = QStringLiteral("--font-family requires a non-empty family name");
        return result;
    }

    const QString theme = result.options.theme.trimmed();
    if (theme.isEmpty()) {
        result.error = QStringLiteral("--theme requires a non-empty theme name");
        return result;
    }

    if (theme.compare(QStringLiteral("default"), Qt::CaseInsensitive) != 0 &&
        theme.compare(QStringLiteral("light"), Qt::CaseInsensitive)   != 0)
    {
        result.error = QStringLiteral("--theme supports only 'default' or 'light'");
        return result;
    }
    result.options.theme = theme;

    return result;
}

QStringList raw_arguments(int argc, char** argv)
{
    QStringList arguments;
    for (int index = 0; index < argc; ++index) {
        arguments.push_back(QString::fromLocal8Bit(argv[index]));
    }
    return arguments;
}

struct Qt_arguments
{
    std::vector<QByteArray>    storage;
    std::vector<char*>         argv;
    int                        argc = 0;
};

Qt_arguments make_qt_arguments(int argc, char** argv)
{
    Qt_arguments arguments;
    for (int index = 0; index < argc; ++index) {
        if (index > 0 && QByteArray(argv[index]) == QByteArrayLiteral("--")) {
            break;
        }

        arguments.storage.push_back(QByteArray(argv[index]));
    }

    arguments.argv.reserve(arguments.storage.size());
    for (QByteArray& argument : arguments.storage) {
        arguments.argv.push_back(argument.data());
    }
    arguments.argc = static_cast<int>(arguments.storage.size());
    arguments.argv.push_back(nullptr);
    return arguments;
}

void request_vsync_surface_format()
{
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(format);
}

bool osc52_clipboard_target_allowed(const QString& target_selection)
{
    return
        target_selection == QStringLiteral("c") ||
        target_selection == QStringLiteral("clipboard");
}

void handle_clipboard_write_request(
    VNM_TerminalSurface&   surface,
    quint64                request_id,
    const QString&         target_selection)
{
    if (osc52_clipboard_target_allowed(target_selection)) {
        if (!surface.respond_clipboard_write(
                request_id, VNM_TerminalSurface::Clipboard_response_decision::ALLOW))
        {
            print_error(QStringLiteral("OSC 52 clipboard write could not be allowed"));
        }
        return;
    }

    if (!surface.respond_clipboard_write(
            request_id, VNM_TerminalSurface::Clipboard_response_decision::DENY))
    {
        print_error(QStringLiteral("OSC 52 clipboard write could not be denied"));
    }
}

int process_exit_status(VNM_TerminalSurface::Exit_reason reason, int exit_code)
{
    switch (reason) {
        case VNM_TerminalSurface::Exit_reason::EXITED:
            return exit_code;
        case VNM_TerminalSurface::Exit_reason::INTERRUPTED:
        case VNM_TerminalSurface::Exit_reason::TERMINATED:
            return exit_code != 0 ? exit_code : k_exit_process_failed;
        case VNM_TerminalSurface::Exit_reason::FAILED_TO_START:
            return k_exit_start_failed;
    }

    return k_exit_process_failed;
}

int app_status_after_process_exit(
    const App_options&     options,
    const Runtime_state&   state)
{
    const int status = process_exit_status(
        state.process_exit_reason,
        state.process_exit_code);

    if (status != 0) {
        print_error(QStringLiteral("process exited with %1, code %2")
            .arg(enum_key(state.process_exit_reason))
            .arg(state.process_exit_code));
        return status;
    }

    if (options.require_output && !state.output_seen) {
        print_error(QStringLiteral("required terminal output activity was not observed"));
        return k_exit_no_output;
    }

    return 0;
}

template<typename Stats>
std::uint64_t renderer_text_resource_dirty_row_metric_value(const Stats& stats)
{
    if constexpr (requires { stats.text_resource_dirty_row_lookups; }) {
        return static_cast<std::uint64_t>(stats.text_resource_dirty_row_lookups);
    }
    else {
        return static_cast<std::uint64_t>(stats.text_resource_dirty_row_probes);
    }
}

template<typename Stats>
const char* renderer_text_resource_dirty_row_metric_name(const Stats&)
{
    if constexpr (requires(Stats stats) { stats.text_resource_dirty_row_lookups; }) {
        return "text_resource_dirty_row_lookups";
    }
    else {
        return "text_resource_dirty_row_probes";
    }
}

#if VNM_TERMINAL_PROFILING_ENABLED
qint64 profile_nanoseconds(std::chrono::nanoseconds duration)
{
    return static_cast<qint64>(duration.count());
}

qint64 profile_mean_nanoseconds(
    std::chrono::nanoseconds   total_time,
    std::uint64_t              call_count)
{
    return call_count == 0U
        ? 0
        : static_cast<qint64>(
            total_time.count() / static_cast<std::int64_t>(call_count));
}

void append_profile_counter(
    QTextStream&               stream,
    const char*                name,
    std::uint64_t              value)
{
    stream << "  " << name << '=' << static_cast<qulonglong>(value) << '\n';
}

void append_profile_bool(
    QTextStream&               stream,
    const char*                name,
    bool                       value)
{
    stream << "  " << name << '=' << (value ? "true" : "false") << '\n';
}

template<typename Frame_stats>
void append_renderer_frame_stats_text(
    QTextStream&       stream,
    const Frame_stats& stats)
{
    append_profile_counter(
        stream,
        "frame_visible_rows",
        static_cast<std::uint64_t>(stats.visible_rows));
    append_profile_counter(
        stream,
        "frame_dirty_rows",
        static_cast<std::uint64_t>(stats.dirty_rows));
    append_profile_counter(
        stream,
        "frame_full_dirty_rows",
        static_cast<std::uint64_t>(stats.full_dirty_rows));
    append_profile_counter(
        stream,
        "frame_cell_pass_input_cells",
        static_cast<std::uint64_t>(stats.cell_pass_input_cells));
    append_profile_counter(
        stream,
        "frame_packed_pass_input_cells",
        static_cast<std::uint64_t>(stats.packed_pass_input_cells));
    if constexpr (requires { stats.packed_pass_cells_scanned; }) {
        append_profile_counter(
            stream,
            "frame_packed_pass_cells_scanned",
            static_cast<std::uint64_t>(stats.packed_pass_cells_scanned));
    }
    if constexpr (requires { stats.packed_text_sidecars_enabled; }) {
        append_profile_counter(
            stream,
            "frame_packed_text_sidecars_enabled",
            static_cast<std::uint64_t>(stats.packed_text_sidecars_enabled));
    }
    if constexpr (requires { stats.packed_text_sidecars_disabled; }) {
        append_profile_counter(
            stream,
            "frame_packed_text_sidecars_disabled",
            static_cast<std::uint64_t>(stats.packed_text_sidecars_disabled));
    }
    if constexpr (requires { stats.packed_text_disabled_cells_skipped; }) {
        append_profile_counter(
            stream,
            "frame_packed_text_disabled_cells_skipped",
            static_cast<std::uint64_t>(stats.packed_text_disabled_cells_skipped));
    }
    if constexpr (requires { stats.packed_data_passes_skipped; }) {
        append_profile_counter(
            stream,
            "frame_packed_data_passes_skipped",
            static_cast<std::uint64_t>(stats.packed_data_passes_skipped));
    }
    if constexpr (requires { stats.packed_inline_fallbacks; }) {
        append_profile_counter(
            stream,
            "frame_packed_inline_fallbacks",
            static_cast<std::uint64_t>(stats.packed_inline_fallbacks));
    }
    if constexpr (requires { stats.packed_cells_appended; }) {
        append_profile_counter(
            stream,
            "frame_packed_cells_appended",
            static_cast<std::uint64_t>(stats.packed_cells_appended));
    }
    append_profile_counter(
        stream,
        "frame_dirty_row_lookup_count",
        static_cast<std::uint64_t>(stats.dirty_row_lookup_count));
    append_profile_counter(
        stream,
        "frame_cells_considered",
        static_cast<std::uint64_t>(stats.cells_considered));
    append_profile_counter(
        stream,
        "frame_cells_skipped_invalid",
        static_cast<std::uint64_t>(stats.cells_skipped_invalid));
    append_profile_counter(
        stream,
        "frame_cells_skipped_wide_continuation",
        static_cast<std::uint64_t>(stats.cells_skipped_wide_continuation));
    append_profile_counter(
        stream,
        "frame_cells_rendered",
        static_cast<std::uint64_t>(stats.cells_rendered));
    append_profile_counter(
        stream,
        "frame_text_cells_empty",
        static_cast<std::uint64_t>(stats.text_cells_empty));
    append_profile_counter(
        stream,
        "frame_text_cells_rendered_as_text",
        static_cast<std::uint64_t>(stats.text_cells_rendered_as_text));
    append_profile_counter(
        stream,
        "frame_text_cells_printable_ascii",
        static_cast<std::uint64_t>(stats.text_cells_printable_ascii));
    append_profile_counter(
        stream,
        "frame_text_cells_other_ascii",
        static_cast<std::uint64_t>(stats.text_cells_other_ascii));
    append_profile_counter(
        stream,
        "frame_text_cells_non_ascii",
        static_cast<std::uint64_t>(stats.text_cells_non_ascii));
    append_profile_counter(
        stream,
        "frame_text_cells_simple_ascii",
        static_cast<std::uint64_t>(stats.text_cells_simple_ascii));
    append_profile_counter(
        stream,
        "frame_text_cells_single_width",
        static_cast<std::uint64_t>(stats.text_cells_single_width));
    append_profile_counter(
        stream,
        "frame_text_cells_multi_width",
        static_cast<std::uint64_t>(stats.text_cells_multi_width));
    append_profile_counter(
        stream,
        "frame_text_cells_with_decorations",
        static_cast<std::uint64_t>(stats.text_cells_with_decorations));
    append_profile_counter(
        stream,
        "frame_text_cells_with_hyperlink",
        static_cast<std::uint64_t>(stats.text_cells_with_hyperlink));
    append_profile_counter(
        stream,
        "frame_text_style_changes",
        static_cast<std::uint64_t>(stats.text_style_changes));
    append_profile_counter(
        stream,
        "frame_text_distinct_styles",
        static_cast<std::uint64_t>(stats.text_distinct_styles));
    append_profile_counter(
        stream,
        "frame_background_rects_emitted",
        static_cast<std::uint64_t>(stats.background_rects_emitted));
    append_profile_counter(
        stream,
        "frame_selection_rects_emitted",
        static_cast<std::uint64_t>(stats.selection_rects_emitted));
    append_profile_counter(
        stream,
        "frame_graphic_rects_emitted",
        static_cast<std::uint64_t>(stats.graphic_rects_emitted));
    append_profile_counter(
        stream,
        "frame_graphic_arcs_emitted",
        static_cast<std::uint64_t>(stats.graphic_arcs_emitted));
    append_profile_counter(
        stream,
        "frame_text_runs_emitted",
        static_cast<std::uint64_t>(stats.text_runs_emitted));
    append_profile_counter(
        stream,
        "frame_cursor_text_runs_emitted",
        static_cast<std::uint64_t>(stats.cursor_text_runs_emitted));
    append_profile_counter(
        stream,
        "frame_decoration_rects_emitted",
        static_cast<std::uint64_t>(stats.decoration_rects_emitted));
    append_profile_counter(
        stream,
        "frame_cursor_rects_emitted",
        static_cast<std::uint64_t>(stats.cursor_rects_emitted));
    append_profile_counter(
        stream,
        "frame_overlay_rects_emitted",
        static_cast<std::uint64_t>(stats.overlay_rects_emitted));
    append_profile_counter(
        stream,
        "frame_packed_rows",
        static_cast<std::uint64_t>(stats.packed_rows));
    append_profile_counter(
        stream,
        "frame_packed_text_spans",
        static_cast<std::uint64_t>(stats.packed_text_spans));
    append_profile_counter(
        stream,
        "frame_packed_text_cells",
        static_cast<std::uint64_t>(stats.packed_text_cells));
    if constexpr (requires { stats.packed_text_ascii_direct_cells; }) {
        append_profile_counter(
            stream,
            "frame_packed_text_ascii_direct_cells",
            static_cast<std::uint64_t>(stats.packed_text_ascii_direct_cells));
    }
    if constexpr (requires { stats.packed_text_ascii_direct_bytes; }) {
        append_profile_counter(
            stream,
            "frame_packed_text_ascii_direct_bytes",
            static_cast<std::uint64_t>(stats.packed_text_ascii_direct_bytes));
    }
    if constexpr (requires { stats.packed_text_utf8_cells; }) {
        append_profile_counter(
            stream,
            "frame_packed_text_utf8_cells",
            static_cast<std::uint64_t>(stats.packed_text_utf8_cells));
    }
    if constexpr (requires { stats.packed_text_utf8_input_units; }) {
        append_profile_counter(
            stream,
            "frame_packed_text_utf8_input_code_units",
            static_cast<std::uint64_t>(stats.packed_text_utf8_input_units));
    }
    if constexpr (requires { stats.packed_text_utf8_output_bytes; }) {
        append_profile_counter(
            stream,
            "frame_packed_text_utf8_output_bytes",
            static_cast<std::uint64_t>(stats.packed_text_utf8_output_bytes));
    }
    append_profile_counter(
        stream,
        "frame_packed_payload_bytes",
        static_cast<std::uint64_t>(stats.packed_payload_bytes));
}

QString profile_string_literal(const QString& value)
{
    QString out;
    out.reserve(value.size() + 2);
    out += QLatin1Char('"');
    for (const QChar character : value) {
        const ushort code_unit = character.unicode();
        switch (code_unit) {
            case '\\': out += QStringLiteral("\\\\"); break;
            case '"':  out += QStringLiteral("\\\""); break;
            case '\n': out += QStringLiteral("\\n");  break;
            case '\r': out += QStringLiteral("\\r");  break;
            case '\t': out += QStringLiteral("\\t");  break;
            default:
                if (code_unit < 0x20U || code_unit == 0x7FU) {
                    out += QStringLiteral("\\u%1")
                        .arg(code_unit, 4, 16, QLatin1Char('0'))
                        .toUpper();
                }
                else {
                    out += character;
                }
                break;
        }
    }
    out += QLatin1Char('"');
    return out;
}

void append_profile_node_text(
    QTextStream&                           stream,
    const term::Profile_node_snapshot&     node,
    int                                    depth)
{
    const QString indent(depth * 2, QLatin1Char(' '));
    stream
        << indent
        << QString::fromStdString(node.name)
        << " calls="    << static_cast<qulonglong>(node.call_count)
        << " total_ns=" << profile_nanoseconds(node.total_time)
        << " mean_ns="  << profile_mean_nanoseconds(node.total_time, node.call_count)
        << " self_ns="  << profile_nanoseconds(node.self_time)
        << " child_ns=" << profile_nanoseconds(node.child_time)
        << " min_ns="   << profile_nanoseconds(node.min_time)
        << " max_ns="   << profile_nanoseconds(node.max_time)
        << '\n';

    for (const term::Profile_node_snapshot& child : node.children) {
        append_profile_node_text(stream, child, depth + 1);
    }
}

void append_profile_timeline_text(
    QTextStream&                           stream,
    const QString&                         label,
    const term::Profile_timeline_snapshot& timeline)
{
    stream
        << label
        << "_timeline bucket_width_ms="
        << static_cast<qulonglong>(timeline.bucket_width.count())
        << " buckets=" << static_cast<qulonglong>(timeline.buckets.size())
        << '\n';

    for (const term::Profile_timeline_bucket_snapshot& bucket : timeline.buckets) {
        if (bucket.scopes.empty()) {
            continue;
        }

        stream
            << "  bucket start_ms="
            << static_cast<qulonglong>(bucket.start_time.count())
            << " end_ms=" << static_cast<qulonglong>(bucket.end_time.count())
            << " scopes=" << static_cast<qulonglong>(bucket.scopes.size())
            << '\n';
        for (const term::Profile_timeline_scope_snapshot& scope : bucket.scopes) {
            stream
                << "    " << QString::fromStdString(scope.name)
                << " calls="    << static_cast<qulonglong>(scope.call_count)
                << " total_ns=" << profile_nanoseconds(scope.total_time)
                << " mean_ns="
                << profile_mean_nanoseconds(scope.total_time, scope.call_count)
                << " min_ns="   << profile_nanoseconds(scope.min_time)
                << " max_ns="   << profile_nanoseconds(scope.max_time)
                << '\n';
        }
    }
}

void append_dirty_row_stats_text(
    QTextStream&           stream,
    const term::Terminal_screen_model_dirty_row_stats&
                           stats)
{
    stream << "dirty_rows\n";
    stream << "  enabled=" << (stats.enabled ? "true" : "false") << '\n';
    append_profile_counter(stream, "mark_requests", stats.mark_requests);
    append_profile_counter(
        stream,
        "duplicate_mark_requests",
        stats.duplicate_mark_requests);
    append_profile_counter(
        stream,
        "out_of_bounds_mark_requests",
        stats.out_of_bounds_mark_requests);
    append_profile_counter(
        stream,
        "unique_pending_row_marks",
        stats.unique_pending_row_marks);
    append_profile_counter(stream, "mark_all_dirty_calls", stats.mark_all_dirty_calls);
    append_profile_counter(
        stream,
        "dirty_rows_snapshot_calls",
        stats.dirty_rows_snapshot_calls);
    append_profile_counter(
        stream,
        "dirty_rows_snapshot_rows",
        stats.dirty_rows_snapshot_rows);
    append_profile_counter(
        stream,
        "collect_synchronized_calls",
        stats.collect_synchronized_calls);
    append_profile_counter(
        stream,
        "collect_synchronized_rows",
        stats.collect_synchronized_rows);
    append_profile_counter(stream, "publish_pending_calls", stats.publish_pending_calls);
    append_profile_counter(stream, "published_unique_rows", stats.published_unique_rows);
    append_profile_counter(
        stream,
        "release_synchronized_calls",
        stats.release_synchronized_calls);
    append_profile_counter(
        stream,
        "released_synchronized_rows",
        stats.released_synchronized_rows);
    append_profile_counter(
        stream,
        "max_pending_dirty_rows",
        stats.max_pending_dirty_rows);
    append_profile_counter(
        stream,
        "max_synchronized_dirty_rows",
        stats.max_synchronized_dirty_rows);
}

bool dirty_row_bucket_has_activity(
    const term::Terminal_screen_model_dirty_row_bucket_stats& bucket)
{
    return
        bucket.mark_requests              != 0U ||
        bucket.dirty_rows_snapshot_calls  != 0U ||
        bucket.collect_synchronized_calls != 0U ||
        bucket.publish_pending_calls      != 0U ||
        bucket.release_synchronized_calls != 0U;
}

void append_dirty_row_timeline_text(
    QTextStream&           stream,
    const term::Terminal_screen_model_dirty_row_timeline&
                           timeline)
{
    stream
        << "dirty_row_timeline bucket_width_ms="
        << static_cast<qulonglong>(timeline.bucket_width_ms)
        << " buckets=" << static_cast<qulonglong>(timeline.buckets.size())
        << '\n';

    for (const term::Terminal_screen_model_dirty_row_bucket_stats& bucket :
        timeline.buckets)
    {
        if (!dirty_row_bucket_has_activity(bucket)) {
            continue;
        }

        stream
            << "  bucket start_ms=" << static_cast<qulonglong>(bucket.start_ms)
            << " end_ms="           << static_cast<qulonglong>(bucket.end_ms)
            << " mark_requests="    << static_cast<qulonglong>(bucket.mark_requests)
            << " duplicate_mark_requests="
            << static_cast<qulonglong>(bucket.duplicate_mark_requests)
            << " unique_pending_row_marks="
            << static_cast<qulonglong>(bucket.unique_pending_row_marks)
            << " mark_all_dirty_calls="
            << static_cast<qulonglong>(bucket.mark_all_dirty_calls)
            << " dirty_rows_snapshot_calls="
            << static_cast<qulonglong>(bucket.dirty_rows_snapshot_calls)
            << " dirty_rows_snapshot_rows="
            << static_cast<qulonglong>(bucket.dirty_rows_snapshot_rows)
            << " collect_synchronized_calls="
            << static_cast<qulonglong>(bucket.collect_synchronized_calls)
            << " collect_synchronized_rows="
            << static_cast<qulonglong>(bucket.collect_synchronized_rows)
            << " publish_pending_calls="
            << static_cast<qulonglong>(bucket.publish_pending_calls)
            << " published_unique_rows="
            << static_cast<qulonglong>(bucket.published_unique_rows)
            << " release_synchronized_calls="
            << static_cast<qulonglong>(bucket.release_synchronized_calls)
            << " released_synchronized_rows="
            << static_cast<qulonglong>(bucket.released_synchronized_rows)
            << " max_pending_dirty_rows="
            << static_cast<qulonglong>(bucket.max_pending_dirty_rows)
            << " max_synchronized_dirty_rows="
            << static_cast<qulonglong>(bucket.max_synchronized_dirty_rows)
            << '\n';
    }
}

void append_model_profile_stats_text(
    QTextStream&                                      stream,
    const term::Terminal_screen_model_profile_stats&  stats)
{
    stream << "model_profile_stats\n";
    stream << "  enabled=" << (stats.enabled ? "true" : "false") << '\n';
    append_profile_counter(stream, "print_text_calls", stats.print_text_calls);
    append_profile_counter(stream, "printable_ascii_span_calls", stats.printable_ascii_span_calls);
    append_profile_counter(stream, "printable_ascii_span_characters", stats.printable_ascii_span_characters);
    append_profile_counter(stream, "printable_ascii_cells_written", stats.printable_ascii_cells_written);
    append_profile_counter(
        stream,
        "max_printable_ascii_span_characters",
        stats.max_printable_ascii_span_characters);
    append_profile_counter(
        stream,
        "printable_ascii_local_cells_inspected",
        stats.printable_ascii_local_cells_inspected);
    append_profile_counter(
        stream,
        "scalar_span_local_cells_inspected",
        stats.scalar_span_local_cells_inspected);
    append_profile_counter(
        stream,
        "row_content_generation_comparisons",
        stats.row_content_generation_comparisons);
    append_profile_counter(
        stream,
        "row_content_generation_comparison_cells",
        stats.row_content_generation_comparison_cells);
    append_profile_counter(stream, "row_content_generation_advances", stats.row_content_generation_advances);
    append_profile_counter(
        stream,
        "wide_boundary_repairs_from_text_writes",
        stats.wide_boundary_repairs_from_text_writes);
    append_profile_counter(stream, "dirty_marks_from_text_writes", stats.dirty_marks_from_text_writes);
    append_profile_counter(stream, "line_wraps_from_text_writes", stats.line_wraps_from_text_writes);
    append_profile_counter(
        stream,
        "scrollback_appends_from_text_writes",
        stats.scrollback_appends_from_text_writes);
    append_profile_counter(stream, "render_snapshot_requests", stats.render_snapshot_requests);
    append_profile_counter(stream, "render_snapshots_constructed", stats.render_snapshots_constructed);
    append_profile_counter(stream, "render_snapshot_rows_visited", stats.render_snapshot_rows_visited);
    append_profile_counter(
        stream,
        "render_snapshot_rows_materialized",
        stats.render_snapshot_rows_materialized);
    append_profile_counter(stream, "render_snapshot_rows_borrowed", stats.render_snapshot_rows_borrowed);
    append_profile_counter(stream, "render_snapshot_rows_owned", stats.render_snapshot_rows_owned);
    append_profile_counter(stream, "render_snapshot_cells_scanned", stats.render_snapshot_cells_scanned);
    append_profile_counter(stream, "render_snapshot_cells_emitted", stats.render_snapshot_cells_emitted);
    append_profile_counter(
        stream,
        "render_snapshot_compact_empty_text_cells",
        stats.render_snapshot_compact_empty_text_cells);
    append_profile_counter(
        stream,
        "render_snapshot_compact_ascii_text_cells",
        stats.render_snapshot_compact_ascii_text_cells);
    append_profile_counter(
        stream,
        "render_snapshot_inline_single_bmp_text_cells",
        stats.render_snapshot_inline_single_bmp_text_cells);
    append_profile_counter(
        stream,
        "render_snapshot_fallback_qstring_copies",
        stats.render_snapshot_fallback_qstring_copies);
    append_profile_counter(
        stream,
        "render_snapshot_fallback_text_code_units_copied",
        stats.render_snapshot_fallback_text_code_units_copied);
    append_profile_counter(
        stream,
        "render_snapshot_fallback_printable_ascii_copies",
        stats.render_snapshot_fallback_printable_ascii_copies);
    append_profile_counter(
        stream,
        "render_snapshot_fallback_other_ascii_copies",
        stats.render_snapshot_fallback_other_ascii_copies);
    append_profile_counter(
        stream,
        "render_snapshot_fallback_single_non_ascii_copies",
        stats.render_snapshot_fallback_single_non_ascii_copies);
    append_profile_counter(
        stream,
        "render_snapshot_fallback_multi_text_copies",
        stats.render_snapshot_fallback_multi_text_copies);
    append_profile_counter(
        stream,
        "render_snapshot_unoccupied_cells_skipped",
        stats.render_snapshot_unoccupied_cells_skipped);
    append_profile_counter(
        stream,
        "render_snapshot_dirty_rows_requested",
        stats.render_snapshot_dirty_rows_requested);
    append_profile_counter(
        stream,
        "render_snapshot_dirty_rows_visible",
        stats.render_snapshot_dirty_rows_visible);
    append_profile_counter(
        stream,
        "render_snapshot_full_repaint_fallbacks",
        stats.render_snapshot_full_repaint_fallbacks);
    append_profile_counter(
        stream,
        "render_snapshot_viewport_fallbacks",
        stats.render_snapshot_viewport_fallbacks);
    append_profile_counter(
        stream,
        "render_snapshot_zero_dirty_publications",
        stats.render_snapshot_zero_dirty_publications);
    append_profile_counter(
        stream,
        "max_render_snapshot_rows_visited",
        stats.max_render_snapshot_rows_visited);
    append_profile_counter(
        stream,
        "max_render_snapshot_cells_emitted",
        stats.max_render_snapshot_cells_emitted);
    append_profile_counter(
        stream,
        "max_render_snapshot_fallback_text_units_per_cell",
        stats.max_render_snapshot_fallback_text_units_per_cell);
}

void append_session_profile_stats_text(
    QTextStream&                              stream,
    const term::Terminal_session_profile_stats& stats)
{
    stream << "session_profile_stats\n";
    stream << "  enabled=" << (stats.enabled ? "true" : "false") << '\n';
    append_profile_counter(stream, "render_snapshot_requests", stats.render_snapshot_requests);
    append_profile_counter(stream, "render_snapshots_constructed", stats.render_snapshots_constructed);
    append_profile_counter(stream, "render_snapshot_publications", stats.render_snapshot_publications);
    append_profile_counter(stream, "content_snapshot_publications", stats.content_snapshot_publications);
    append_profile_counter(stream, "selection_snapshot_publications", stats.selection_snapshot_publications);
    append_profile_counter(stream, "geometry_snapshot_publications", stats.geometry_snapshot_publications);
    append_profile_counter(
        stream,
        "public_projection_scroll_requests",
        stats.public_projection_scroll_requests);
    append_profile_counter(
        stream,
        "public_projection_scroll_publications",
        stats.public_projection_scroll_publications);
    append_profile_counter(stream, "dirty_coalescing_attempts", stats.dirty_coalescing_attempts);
    append_profile_counter(stream, "dirty_coalescing_applied", stats.dirty_coalescing_applied);
    append_profile_counter(stream, "zero_dirty_snapshot_publications", stats.zero_dirty_snapshot_publications);
    append_profile_counter(
        stream,
        "snapshots_superseded_before_render",
        stats.snapshots_superseded_before_render);
    append_profile_counter(stream, "snapshots_marked_rendered", stats.snapshots_marked_rendered);
    append_profile_counter(
        stream,
        "max_unrendered_snapshot_generations",
        stats.max_unrendered_snapshot_generations);
}

template<typename Renderer_stats>
void append_text_layout_stats_text(
    QTextStream&           stream,
    const Renderer_stats&  stats)
{
    append_profile_counter(
        stream,
        "qt_text_layout_calls",
        static_cast<std::uint64_t>(stats.qt_text_layout_calls));
    append_profile_counter(
        stream,
        "text_layout_runs_single_code_unit",
        static_cast<std::uint64_t>(stats.text_layout_runs_single_code_unit));
    append_profile_counter(
        stream,
        "text_layout_runs_multi_code_unit",
        static_cast<std::uint64_t>(stats.text_layout_runs_multi_code_unit));
    append_profile_counter(
        stream,
        "text_layout_runs_all_space",
        static_cast<std::uint64_t>(stats.text_layout_runs_all_space));
    append_profile_counter(
        stream,
        "text_layout_runs_printable_ascii",
        static_cast<std::uint64_t>(stats.text_layout_runs_printable_ascii));
    append_profile_counter(
        stream,
        "text_layout_runs_printable_ascii_with_space",
        static_cast<std::uint64_t>(stats.text_layout_runs_printable_ascii_with_space));
    append_profile_counter(
        stream,
        "text_layout_runs_other_ascii",
        static_cast<std::uint64_t>(stats.text_layout_runs_other_ascii));
    append_profile_counter(
        stream,
        "text_layout_runs_non_ascii",
        static_cast<std::uint64_t>(stats.text_layout_runs_non_ascii));
    append_profile_counter(
        stream,
        "text_layout_runs_clipped",
        static_cast<std::uint64_t>(stats.text_layout_runs_clipped));
    append_profile_counter(
        stream,
        "text_layout_runs_ascii_layout_font",
        static_cast<std::uint64_t>(stats.text_layout_runs_ascii_layout_font));
    append_profile_counter(
        stream,
        "text_layout_runs_force_blended_order",
        static_cast<std::uint64_t>(stats.text_layout_runs_force_blended_order));
    append_profile_counter(
        stream,
        "text_layout_runs_with_hyperlink",
        static_cast<std::uint64_t>(stats.text_layout_runs_with_hyperlink));
    append_profile_counter(
        stream,
        "text_layout_runs_with_decoration",
        static_cast<std::uint64_t>(stats.text_layout_runs_with_decoration));
    append_profile_counter(
        stream,
        "text_layout_runs_mixed_ascii_non_ascii",
        static_cast<std::uint64_t>(stats.text_layout_runs_mixed_ascii_non_ascii));
    append_profile_counter(
        stream,
        "text_layout_runs_pure_non_ascii",
        static_cast<std::uint64_t>(stats.text_layout_runs_pure_non_ascii));
    append_profile_counter(
        stream,
        "text_layout_runs_plain_unclipped",
        static_cast<std::uint64_t>(stats.text_layout_runs_plain_unclipped));
    append_profile_counter(
        stream,
        "text_layout_runs_plain_unclipped_ascii_font",
        static_cast<std::uint64_t>(stats.text_layout_runs_plain_unclipped_ascii_font));
    append_profile_counter(
        stream,
        "text_layout_runs_all_space_plain_unclipped",
        static_cast<std::uint64_t>(stats.text_layout_runs_all_space_plain_unclipped));
    append_profile_counter(
        stream,
        "text_layout_runs_printable_ascii_plain_unclipped",
        static_cast<std::uint64_t>(stats.text_layout_runs_printable_ascii_plain_unclipped));
    append_profile_counter(
        stream,
        "text_layout_runs_non_ascii_plain_unclipped",
        static_cast<std::uint64_t>(stats.text_layout_runs_non_ascii_plain_unclipped));
    append_profile_counter(
        stream,
        "text_layout_runs_mixed_ascii_non_ascii_plain_unclipped",
        static_cast<std::uint64_t>(
            stats.text_layout_runs_mixed_ascii_non_ascii_plain_unclipped));
    append_profile_counter(
        stream,
        "text_layout_runs_pure_non_ascii_plain_unclipped",
        static_cast<std::uint64_t>(stats.text_layout_runs_pure_non_ascii_plain_unclipped));
    append_profile_counter(
        stream,
        "text_layout_runs_fast_space_candidate",
        static_cast<std::uint64_t>(stats.text_layout_runs_fast_space_candidate));
    append_profile_counter(
        stream,
        "text_layout_runs_fast_ascii_candidate",
        static_cast<std::uint64_t>(stats.text_layout_runs_fast_ascii_candidate));
    append_profile_counter(
        stream,
        "text_layout_runs_fast_ascii_no_space_candidate",
        static_cast<std::uint64_t>(stats.text_layout_runs_fast_ascii_no_space_candidate));
    append_profile_counter(
        stream,
        "text_layout_runs_fast_ascii_single_candidate",
        static_cast<std::uint64_t>(stats.text_layout_runs_fast_ascii_single_candidate));
    append_profile_counter(
        stream,
        "text_layout_runs_fast_ascii_multi_candidate",
        static_cast<std::uint64_t>(stats.text_layout_runs_fast_ascii_multi_candidate));
    append_profile_counter(
        stream,
        "text_ascii_replacement_runs_screened",
        static_cast<std::uint64_t>(stats.text_ascii_replacement_runs_screened));
    append_profile_counter(
        stream,
        "text_ascii_replacement_runs_eligible",
        static_cast<std::uint64_t>(stats.text_ascii_replacement_runs_eligible));
    append_profile_counter(
        stream,
        "text_ascii_replacement_runs_attempted",
        static_cast<std::uint64_t>(stats.text_ascii_replacement_runs_attempted));
    append_profile_counter(
        stream,
        "text_ascii_replacement_runs_trusted_fast_path",
        static_cast<std::uint64_t>(stats.text_ascii_replacement_runs_trusted_fast_path));
    append_profile_counter(
        stream,
        "text_ascii_replacement_runs_succeeded",
        static_cast<std::uint64_t>(stats.text_ascii_replacement_runs_succeeded));
    append_profile_counter(
        stream,
        "text_ascii_replacement_runs_all_space_succeeded",
        static_cast<std::uint64_t>(stats.text_ascii_replacement_runs_all_space_succeeded));
    if constexpr (requires { stats.text_ascii_replacement_add_glyphs_calls; }) {
        append_profile_counter(
            stream,
            "text_ascii_replacement_add_glyphs_calls",
            static_cast<std::uint64_t>(stats.text_ascii_replacement_add_glyphs_calls));
    }
    append_profile_counter(
        stream,
        "text_ascii_replacement_runs_fallback",
        static_cast<std::uint64_t>(stats.text_ascii_replacement_runs_fallback));
    append_profile_counter(
        stream,
        "text_ascii_replacement_runs_rejected_clipped",
        static_cast<std::uint64_t>(stats.text_ascii_replacement_runs_rejected_clipped));
    append_profile_counter(
        stream,
        "text_ascii_replacement_runs_rejected_force_blended_order",
        static_cast<std::uint64_t>(
            stats.text_ascii_replacement_runs_rejected_force_blended_order));
    append_profile_counter(
        stream,
        "text_ascii_replacement_runs_rejected_decoration",
        static_cast<std::uint64_t>(stats.text_ascii_replacement_runs_rejected_decoration));
    append_profile_counter(
        stream,
        "text_ascii_replacement_runs_rejected_hyperlink",
        static_cast<std::uint64_t>(stats.text_ascii_replacement_runs_rejected_hyperlink));
    append_profile_counter(
        stream,
        "text_ascii_replacement_runs_rejected_non_printable_ascii",
        static_cast<std::uint64_t>(
            stats.text_ascii_replacement_runs_rejected_non_printable_ascii));
    append_profile_counter(
        stream,
        "text_ascii_replacement_runs_rejected_non_ascii",
        static_cast<std::uint64_t>(stats.text_ascii_replacement_runs_rejected_non_ascii));
    append_profile_counter(
        stream,
        "text_ascii_replacement_runs_rejected_geometry",
        static_cast<std::uint64_t>(stats.text_ascii_replacement_runs_rejected_geometry));
    append_profile_counter(
        stream,
        "text_ascii_replacement_runs_rejected_unsupported_font",
        static_cast<std::uint64_t>(
            stats.text_ascii_replacement_runs_rejected_unsupported_font));
    append_profile_counter(
        stream,
        "text_ascii_replacement_runs_rejected_internal_node",
        static_cast<std::uint64_t>(stats.text_ascii_replacement_runs_rejected_internal_node));
    append_profile_counter(
        stream,
        "text_ascii_replacement_runs_rejected_glyph_mapping",
        static_cast<std::uint64_t>(stats.text_ascii_replacement_runs_rejected_glyph_mapping));
    append_profile_counter(
        stream,
        "text_layout_code_units",
        static_cast<std::uint64_t>(stats.text_layout_code_units));
    append_profile_counter(
        stream,
        "text_layout_space_code_units",
        static_cast<std::uint64_t>(stats.text_layout_space_code_units));
    append_profile_counter(
        stream,
        "text_layout_printable_ascii_code_units",
        static_cast<std::uint64_t>(stats.text_layout_printable_ascii_code_units));
    append_profile_counter(
        stream,
        "text_layout_other_ascii_code_units",
        static_cast<std::uint64_t>(stats.text_layout_other_ascii_code_units));
    append_profile_counter(
        stream,
        "text_layout_non_ascii_code_units",
        static_cast<std::uint64_t>(stats.text_layout_non_ascii_code_units));
    append_profile_counter(
        stream,
        "text_layout_plain_unclipped_code_units",
        static_cast<std::uint64_t>(stats.text_layout_plain_unclipped_code_units));
    append_profile_counter(
        stream,
        "text_layout_all_space_plain_unclipped_code_units",
        static_cast<std::uint64_t>(stats.text_layout_all_space_plain_unclipped_code_units));
    append_profile_counter(
        stream,
        "text_layout_printable_ascii_plain_unclipped_code_units",
        static_cast<std::uint64_t>(
            stats.text_layout_printable_ascii_plain_unclipped_code_units));
    append_profile_counter(
        stream,
        "text_layout_non_ascii_plain_unclipped_code_units",
        static_cast<std::uint64_t>(stats.text_layout_non_ascii_plain_unclipped_code_units));
    append_profile_counter(
        stream,
        "text_layout_fast_space_candidate_code_units",
        static_cast<std::uint64_t>(stats.text_layout_fast_space_candidate_code_units));
    append_profile_counter(
        stream,
        "text_layout_fast_ascii_candidate_code_units",
        static_cast<std::uint64_t>(stats.text_layout_fast_ascii_candidate_code_units));
    append_profile_counter(
        stream,
        "text_ascii_replacement_code_units_screened",
        static_cast<std::uint64_t>(stats.text_ascii_replacement_code_units_screened));
    append_profile_counter(
        stream,
        "text_ascii_replacement_code_units_eligible",
        static_cast<std::uint64_t>(stats.text_ascii_replacement_code_units_eligible));
    append_profile_counter(
        stream,
        "text_ascii_replacement_code_units_attempted",
        static_cast<std::uint64_t>(stats.text_ascii_replacement_code_units_attempted));
    append_profile_counter(
        stream,
        "text_ascii_replacement_code_units_trusted_fast_path",
        static_cast<std::uint64_t>(
            stats.text_ascii_replacement_code_units_trusted_fast_path));
    append_profile_counter(
        stream,
        "text_ascii_replacement_code_units_succeeded",
        static_cast<std::uint64_t>(stats.text_ascii_replacement_code_units_succeeded));
    append_profile_counter(
        stream,
        "text_ascii_replacement_code_units_fallback",
        static_cast<std::uint64_t>(stats.text_ascii_replacement_code_units_fallback));
}

template<typename Renderer_stats>
void append_renderer_stats_text(
    QTextStream&                 stream,
    const Renderer_stats&        stats)
{
    stream << "last_renderer_stats\n";
    stream << "  paint_completed=" << (stats.paint_completed ? "true" : "false") << '\n';
    append_renderer_frame_stats_text(stream, stats.frame);
    append_profile_counter(
        stream,
        "text_content_rebuilds",
        static_cast<std::uint64_t>(stats.text_content_rebuilds));
    append_profile_counter(
        stream,
        "text_content_reused",
        static_cast<std::uint64_t>(stats.text_content_reused));
    append_profile_counter(
        stream,
        "text_content_removed",
        static_cast<std::uint64_t>(stats.text_content_removed));
    append_profile_counter(
        stream,
        "text_content_failures",
        static_cast<std::uint64_t>(stats.text_content_failures));
    append_profile_counter(
        stream,
        "atlas_work_created",
        static_cast<std::uint64_t>(stats.atlas_work_created));
    if constexpr (requires { stats.atlas_work_reused; }) {
        append_profile_counter(
            stream,
            "atlas_work_reused",
            static_cast<std::uint64_t>(stats.atlas_work_reused));
    }
    append_profile_counter(
        stream,
        "text_cache_entry_child_nodes_cleared_for_replacement",
        static_cast<std::uint64_t>(
            stats.text_cache_entry_child_nodes_cleared_for_replacement));
    append_profile_counter(
        stream,
        "text_cache_entry_child_nodes_cleared_for_removal",
        static_cast<std::uint64_t>(
            stats.text_cache_entry_child_nodes_cleared_for_removal));
    append_profile_counter(
        stream,
        "text_cache_entry_max_child_nodes_cleared",
        static_cast<std::uint64_t>(stats.text_cache_entry_max_child_nodes_cleared));
    append_text_layout_stats_text(stream, stats);
    append_profile_counter(
        stream,
        "text_groups_considered",
        static_cast<std::uint64_t>(stats.text_groups_considered));
    append_profile_counter(
        stream,
        "text_groups_dirty",
        static_cast<std::uint64_t>(stats.text_groups_dirty));
    append_profile_counter(
        stream,
        "text_groups_clean",
        static_cast<std::uint64_t>(stats.text_groups_clean));
    append_profile_counter(
        stream,
        "text_clean_reuse_skips",
        static_cast<std::uint64_t>(stats.text_clean_reuse_skips));
    append_profile_counter(
        stream,
        "text_resource_descriptor_reuses",
        static_cast<std::uint64_t>(stats.text_resource_descriptor_reuses));
    if constexpr (requires { stats.text_resource_descriptor_builds; }) {
        append_profile_counter(
            stream,
            "text_resource_descriptor_builds",
            static_cast<std::uint64_t>(stats.text_resource_descriptor_builds));
    }
    if constexpr (requires { stats.text_resource_descriptor_builds_avoided; }) {
        append_profile_counter(
            stream,
            "text_resource_descriptor_builds_avoided",
            static_cast<std::uint64_t>(stats.text_resource_descriptor_builds_avoided));
    }
    if constexpr (requires { stats.text_resource_descriptor_clean_reuse_skips; }) {
        append_profile_counter(
            stream,
            "text_resource_descriptor_clean_reuse_skips",
            static_cast<std::uint64_t>(stats.text_resource_descriptor_clean_reuse_skips));
    }
    append_profile_counter(
        stream,
        "text_key_builds",
        static_cast<std::uint64_t>(stats.text_key_builds));
    append_profile_counter(
        stream,
        "text_dirty_row_ranges",
        static_cast<std::uint64_t>(stats.text_dirty_row_ranges));
    append_profile_counter(
        stream,
        "text_dirty_rows",
        static_cast<std::uint64_t>(stats.text_dirty_rows));
    append_profile_counter(
        stream,
        "text_runs_considered",
        static_cast<std::uint64_t>(stats.text_runs_considered));
    append_profile_counter(
        stream,
        "text_coalescing_candidate_groups",
        static_cast<std::uint64_t>(stats.text_coalescing_candidate_groups));
    append_profile_counter(
        stream,
        "text_coalescing_enabled_groups",
        static_cast<std::uint64_t>(stats.text_coalescing_enabled_groups));
    append_profile_counter(
        stream,
        "text_resource_rows_with_runs",
        static_cast<std::uint64_t>(stats.text_resource_rows_with_runs));
    append_profile_counter(
        stream,
        "text_resource_max_runs_after_coalescing_per_row",
        static_cast<std::uint64_t>(stats.text_resource_max_runs_after_coalescing_per_row));
    append_profile_counter(
        stream,
        "text_resource_runs_before_coalescing",
        static_cast<std::uint64_t>(stats.text_resource_runs_before_coalescing));
    append_profile_counter(
        stream,
        "text_resource_runs_after_coalescing",
        static_cast<std::uint64_t>(stats.text_resource_runs_after_coalescing));
    append_profile_counter(
        stream,
        "text_dirty_descriptor_identical_rows",
        static_cast<std::uint64_t>(stats.text_dirty_descriptor_identical_rows));
    append_profile_counter(
        stream,
        "text_key_match_reuses",
        static_cast<std::uint64_t>(stats.text_key_match_reuses));
    append_profile_counter(
        stream,
        "text_dirty_rows_rebuilt",
        static_cast<std::uint64_t>(stats.text_dirty_rows_rebuilt));
    append_profile_counter(
        stream,
        "text_clean_rows_rebuilt",
        static_cast<std::uint64_t>(stats.text_clean_rows_rebuilt));
    if constexpr (requires { stats.text_dirty_rebuilds_without_old_slot; }) {
        append_profile_counter(
            stream,
            "text_dirty_rebuilds_without_old_slot",
            static_cast<std::uint64_t>(stats.text_dirty_rebuilds_without_old_slot));
    }
    if constexpr (requires { stats.text_dirty_rebuilds_with_frame_key_mismatch; }) {
        append_profile_counter(
            stream,
            "text_dirty_rebuilds_with_frame_key_mismatch",
            static_cast<std::uint64_t>(stats.text_dirty_rebuilds_with_frame_key_mismatch));
    }
    if constexpr (requires { stats.text_dirty_rebuilds_with_descriptor_ineligible; }) {
        append_profile_counter(
            stream,
            "text_dirty_rebuilds_with_descriptor_ineligible",
            static_cast<std::uint64_t>(stats.text_dirty_rebuilds_with_descriptor_ineligible));
    }
    if constexpr (requires { stats.text_dirty_rebuilds_with_old_descriptor_missing; }) {
        append_profile_counter(
            stream,
            "text_dirty_rebuilds_with_old_descriptor_missing",
            static_cast<std::uint64_t>(
                stats.text_dirty_rebuilds_with_old_descriptor_missing));
    }
    if constexpr (requires { stats.text_dirty_rebuilds_with_descriptor_mismatch; }) {
        append_profile_counter(
            stream,
            "text_dirty_rebuilds_with_descriptor_mismatch",
            static_cast<std::uint64_t>(stats.text_dirty_rebuilds_with_descriptor_mismatch));
    }
    if constexpr (requires { stats.text_descriptor_mismatch_run_count; }) {
        append_profile_counter(
            stream,
            "text_descriptor_mismatch_run_count",
            static_cast<std::uint64_t>(stats.text_descriptor_mismatch_run_count));
    }
    if constexpr (requires { stats.text_descriptor_mismatch_text; }) {
        append_profile_counter(
            stream,
            "text_descriptor_mismatch_text",
            static_cast<std::uint64_t>(stats.text_descriptor_mismatch_text));
    }
    if constexpr (requires { stats.text_descriptor_mismatch_foreground; }) {
        append_profile_counter(
            stream,
            "text_descriptor_mismatch_foreground",
            static_cast<std::uint64_t>(stats.text_descriptor_mismatch_foreground));
    }
    if constexpr (requires { stats.text_descriptor_mismatch_geometry; }) {
        append_profile_counter(
            stream,
            "text_descriptor_mismatch_geometry",
            static_cast<std::uint64_t>(stats.text_descriptor_mismatch_geometry));
    }
    if constexpr (requires { stats.text_descriptor_mismatch_baseline; }) {
        append_profile_counter(
            stream,
            "text_descriptor_mismatch_baseline",
            static_cast<std::uint64_t>(stats.text_descriptor_mismatch_baseline));
    }
    if constexpr (requires { stats.text_dirty_rebuilds_with_key_mismatch; }) {
        append_profile_counter(
            stream,
            "text_dirty_rebuilds_with_key_mismatch",
            static_cast<std::uint64_t>(stats.text_dirty_rebuilds_with_key_mismatch));
    }
    append_profile_counter(
        stream,
        "rect_resource_rects_before_coalescing",
        static_cast<std::uint64_t>(stats.rect_resource_rects_before_coalescing));
    append_profile_counter(
        stream,
        "rect_resource_rects_after_coalescing",
        static_cast<std::uint64_t>(stats.rect_resource_rects_after_coalescing));
    append_profile_counter(
        stream,
        "text_cache_entries_created",
        static_cast<std::uint64_t>(stats.text_cache_entries_created));
    append_profile_counter(
        stream,
        "text_cache_entries_replaced",
        static_cast<std::uint64_t>(stats.text_cache_entries_replaced));
    stream
        << "  text_wrapper_order_rebuilt="
        << (stats.text_wrapper_order_rebuilt ? "true" : "false") << '\n';
    stream
        << "  background_layer_rebuilt="
        << (stats.background_layer_rebuilt ? "true" : "false") << '\n';
    stream
        << "  selection_layer_rebuilt="
        << (stats.selection_layer_rebuilt ? "true" : "false") << '\n';
    stream
        << "  graphic_layer_rebuilt="
        << (stats.graphic_layer_rebuilt ? "true" : "false") << '\n';
    stream
        << "  decoration_layer_rebuilt="
        << (stats.decoration_layer_rebuilt ? "true" : "false") << '\n';
    stream
        << "  cursor_layer_rebuilt="
        << (stats.cursor_layer_rebuilt ? "true" : "false") << '\n';
    stream
        << "  cursor_text_layer_rebuilt="
        << (stats.cursor_text_layer_rebuilt ? "true" : "false") << '\n';
    stream
        << "  overlay_layer_rebuilt="
        << (stats.overlay_layer_rebuilt ? "true" : "false") << '\n';
    append_profile_counter(
        stream,
        "background_rows_rebuilt",
        static_cast<std::uint64_t>(stats.background_rows_rebuilt));
    append_profile_counter(
        stream,
        "background_rows_reused",
        static_cast<std::uint64_t>(stats.background_rows_reused));
    append_profile_counter(
        stream,
        "background_row_clean_reuse_skips",
        static_cast<std::uint64_t>(stats.background_row_clean_reuse_skips));
    append_profile_counter(
        stream,
        "background_rows_removed",
        static_cast<std::uint64_t>(stats.background_rows_removed));
    append_profile_counter(
        stream,
        "background_row_cache_fallbacks",
        static_cast<std::uint64_t>(stats.background_row_cache_fallbacks));
    append_profile_counter(
        stream,
        "selection_rows_rebuilt",
        static_cast<std::uint64_t>(stats.selection_rows_rebuilt));
    append_profile_counter(
        stream,
        "selection_rows_reused",
        static_cast<std::uint64_t>(stats.selection_rows_reused));
    append_profile_counter(
        stream,
        "selection_row_clean_reuse_skips",
        static_cast<std::uint64_t>(stats.selection_row_clean_reuse_skips));
    append_profile_counter(
        stream,
        "selection_rows_removed",
        static_cast<std::uint64_t>(stats.selection_rows_removed));
    append_profile_counter(
        stream,
        "selection_row_cache_fallbacks",
        static_cast<std::uint64_t>(stats.selection_row_cache_fallbacks));
    append_profile_counter(
        stream,
        "decoration_rows_rebuilt",
        static_cast<std::uint64_t>(stats.decoration_rows_rebuilt));
    append_profile_counter(
        stream,
        "decoration_rows_reused",
        static_cast<std::uint64_t>(stats.decoration_rows_reused));
    append_profile_counter(
        stream,
        "decoration_row_clean_reuse_skips",
        static_cast<std::uint64_t>(stats.decoration_row_clean_reuse_skips));
    append_profile_counter(
        stream,
        "decoration_rows_removed",
        static_cast<std::uint64_t>(stats.decoration_rows_removed));
    append_profile_counter(
        stream,
        "decoration_row_cache_fallbacks",
        static_cast<std::uint64_t>(stats.decoration_row_cache_fallbacks));
    append_profile_counter(
        stream,
        "graphic_rect_rows_rebuilt",
        static_cast<std::uint64_t>(stats.graphic_rect_rows_rebuilt));
    append_profile_counter(
        stream,
        "graphic_rect_rows_reused",
        static_cast<std::uint64_t>(stats.graphic_rect_rows_reused));
    append_profile_counter(
        stream,
        "graphic_rect_row_clean_reuse_skips",
        static_cast<std::uint64_t>(stats.graphic_rect_row_clean_reuse_skips));
    append_profile_counter(
        stream,
        "graphic_rect_rows_removed",
        static_cast<std::uint64_t>(stats.graphic_rect_rows_removed));
    append_profile_counter(
        stream,
        "graphic_rect_row_cache_fallbacks",
        static_cast<std::uint64_t>(stats.graphic_rect_row_cache_fallbacks));
    append_profile_counter(
        stream,
        "graphic_arc_rows_rebuilt",
        static_cast<std::uint64_t>(stats.graphic_arc_rows_rebuilt));
    append_profile_counter(
        stream,
        "graphic_arc_rows_reused",
        static_cast<std::uint64_t>(stats.graphic_arc_rows_reused));
    append_profile_counter(
        stream,
        "graphic_arc_row_clean_reuse_skips",
        static_cast<std::uint64_t>(stats.graphic_arc_row_clean_reuse_skips));
    append_profile_counter(
        stream,
        "graphic_arc_rows_removed",
        static_cast<std::uint64_t>(stats.graphic_arc_rows_removed));
    append_profile_counter(
        stream,
        "graphic_arc_row_cache_fallbacks",
        static_cast<std::uint64_t>(stats.graphic_arc_row_cache_fallbacks));
}

template<typename Cumulative_renderer_stats>
void append_cumulative_renderer_stats_text(
    QTextStream&                       stream,
    const Cumulative_renderer_stats&   stats)
{
    stream << "cumulative_renderer_stats\n";
    append_profile_counter(stream, "frames_published",       stats.frames_published);
    append_profile_counter(stream, "paint_completed_frames", stats.paint_completed_frames);
    append_profile_counter(stream, "root_reused_frames",     stats.root_reused_frames);
    append_renderer_frame_stats_text(stream, stats.frame);
    append_profile_counter(stream, "text_content_rebuilds",   stats.text_content_rebuilds);
    append_profile_counter(stream, "text_content_reused",     stats.text_content_reused);
    append_profile_counter(stream, "text_content_removed",    stats.text_content_removed);
    append_profile_counter(stream, "text_content_failures",   stats.text_content_failures);
    append_profile_counter(stream, "atlas_work_created", stats.atlas_work_created);
    if constexpr (requires { stats.atlas_work_reused; }) {
        append_profile_counter(stream, "atlas_work_reused", stats.atlas_work_reused);
    }
    append_profile_counter(
        stream,
        "text_cache_entry_child_nodes_cleared_for_replacement",
        stats.text_cache_entry_child_nodes_cleared_for_replacement);
    append_profile_counter(
        stream,
        "text_cache_entry_child_nodes_cleared_for_removal",
        stats.text_cache_entry_child_nodes_cleared_for_removal);
    append_profile_counter(
        stream,
        "text_cache_entry_max_child_nodes_cleared",
        stats.text_cache_entry_max_child_nodes_cleared);
    append_text_layout_stats_text(stream, stats);
    append_profile_counter(stream, "text_groups_considered", stats.text_groups_considered);
    append_profile_counter(stream, "text_groups_dirty",      stats.text_groups_dirty);
    append_profile_counter(stream, "text_groups_clean",      stats.text_groups_clean);
    append_profile_counter(stream, "text_clean_reuse_skips", stats.text_clean_reuse_skips);
    append_profile_counter(
        stream,
        "text_resource_descriptor_reuses",
        stats.text_resource_descriptor_reuses);
    if constexpr (requires { stats.text_resource_descriptor_builds; }) {
        append_profile_counter(
            stream,
            "text_resource_descriptor_builds",
            stats.text_resource_descriptor_builds);
    }
    if constexpr (requires { stats.text_resource_descriptor_builds_avoided; }) {
        append_profile_counter(
            stream,
            "text_resource_descriptor_builds_avoided",
            stats.text_resource_descriptor_builds_avoided);
    }
    if constexpr (requires { stats.text_resource_descriptor_clean_reuse_skips; }) {
        append_profile_counter(
            stream,
            "text_resource_descriptor_clean_reuse_skips",
            stats.text_resource_descriptor_clean_reuse_skips);
    }
    append_profile_counter(stream, "text_key_builds",       stats.text_key_builds);
    append_profile_counter(stream, "text_dirty_row_ranges", stats.text_dirty_row_ranges);
    append_profile_counter(stream, "text_dirty_rows",       stats.text_dirty_rows);
    append_profile_counter(stream, "text_runs_considered",  stats.text_runs_considered);
    append_profile_counter(
        stream,
        "text_coalescing_candidate_groups",
        stats.text_coalescing_candidate_groups);
    append_profile_counter(
        stream,
        "text_coalescing_enabled_groups",
        stats.text_coalescing_enabled_groups);
    append_profile_counter(
        stream,
        "text_resource_rows_with_runs",
        stats.text_resource_rows_with_runs);
    append_profile_counter(
        stream,
        "text_resource_max_runs_after_coalescing_per_row",
        stats.text_resource_max_runs_after_coalescing_per_row);
    append_profile_counter(
        stream,
        "text_resource_runs_before_coalescing",
        stats.text_resource_runs_before_coalescing);
    append_profile_counter(
        stream,
        "text_resource_runs_after_coalescing",
        stats.text_resource_runs_after_coalescing);
    append_profile_counter(
        stream,
        "text_dirty_descriptor_identical_rows",
        stats.text_dirty_descriptor_identical_rows);
    append_profile_counter(
        stream,
        "text_key_match_reuses",
        stats.text_key_match_reuses);
    append_profile_counter(
        stream,
        "text_dirty_rows_rebuilt",
        stats.text_dirty_rows_rebuilt);
    append_profile_counter(
        stream,
        "text_clean_rows_rebuilt",
        stats.text_clean_rows_rebuilt);
    if constexpr (requires { stats.text_dirty_rebuilds_without_old_slot; }) {
        append_profile_counter(
            stream,
            "text_dirty_rebuilds_without_old_slot",
            stats.text_dirty_rebuilds_without_old_slot);
    }
    if constexpr (requires { stats.text_dirty_rebuilds_with_frame_key_mismatch; }) {
        append_profile_counter(
            stream,
            "text_dirty_rebuilds_with_frame_key_mismatch",
            stats.text_dirty_rebuilds_with_frame_key_mismatch);
    }
    if constexpr (requires { stats.text_dirty_rebuilds_with_descriptor_ineligible; }) {
        append_profile_counter(
            stream,
            "text_dirty_rebuilds_with_descriptor_ineligible",
            stats.text_dirty_rebuilds_with_descriptor_ineligible);
    }
    if constexpr (requires { stats.text_dirty_rebuilds_with_old_descriptor_missing; }) {
        append_profile_counter(
            stream,
            "text_dirty_rebuilds_with_old_descriptor_missing",
            stats.text_dirty_rebuilds_with_old_descriptor_missing);
    }
    if constexpr (requires { stats.text_dirty_rebuilds_with_descriptor_mismatch; }) {
        append_profile_counter(
            stream,
            "text_dirty_rebuilds_with_descriptor_mismatch",
            stats.text_dirty_rebuilds_with_descriptor_mismatch);
    }
    if constexpr (requires { stats.text_descriptor_mismatch_run_count; }) {
        append_profile_counter(
            stream,
            "text_descriptor_mismatch_run_count",
            stats.text_descriptor_mismatch_run_count);
    }
    if constexpr (requires { stats.text_descriptor_mismatch_text; }) {
        append_profile_counter(
            stream,
            "text_descriptor_mismatch_text",
            stats.text_descriptor_mismatch_text);
    }
    if constexpr (requires { stats.text_descriptor_mismatch_foreground; }) {
        append_profile_counter(
            stream,
            "text_descriptor_mismatch_foreground",
            stats.text_descriptor_mismatch_foreground);
    }
    if constexpr (requires { stats.text_descriptor_mismatch_geometry; }) {
        append_profile_counter(
            stream,
            "text_descriptor_mismatch_geometry",
            stats.text_descriptor_mismatch_geometry);
    }
    if constexpr (requires { stats.text_descriptor_mismatch_baseline; }) {
        append_profile_counter(
            stream,
            "text_descriptor_mismatch_baseline",
            stats.text_descriptor_mismatch_baseline);
    }
    if constexpr (requires { stats.text_dirty_rebuilds_with_key_mismatch; }) {
        append_profile_counter(
            stream,
            "text_dirty_rebuilds_with_key_mismatch",
            stats.text_dirty_rebuilds_with_key_mismatch);
    }
    append_profile_counter(
        stream,
        "rect_resource_rects_before_coalescing",
        stats.rect_resource_rects_before_coalescing);
    append_profile_counter(
        stream,
        "rect_resource_rects_after_coalescing",
        stats.rect_resource_rects_after_coalescing);
    append_profile_counter(
        stream,
        "text_cache_entries_created",
        stats.text_cache_entries_created);
    append_profile_counter(
        stream,
        "text_cache_entries_replaced",
        stats.text_cache_entries_replaced);
    append_profile_counter(
        stream,
        "text_wrapper_order_rebuilds",
        stats.text_wrapper_order_rebuilds);
    append_profile_counter(stream, "background_layer_rebuilds", stats.background_layer_rebuilds);
    append_profile_counter(stream, "selection_layer_rebuilds",  stats.selection_layer_rebuilds);
    append_profile_counter(stream, "graphic_layer_rebuilds",    stats.graphic_layer_rebuilds);
    append_profile_counter(stream, "decoration_layer_rebuilds", stats.decoration_layer_rebuilds);
    append_profile_counter(stream, "cursor_layer_rebuilds",     stats.cursor_layer_rebuilds);
    append_profile_counter(stream, "cursor_text_layer_rebuilds", stats.cursor_text_layer_rebuilds);
    append_profile_counter(stream, "overlay_layer_rebuilds",     stats.overlay_layer_rebuilds);
    append_profile_counter(stream, "background_rows_rebuilt",    stats.background_rows_rebuilt);
    append_profile_counter(stream, "background_rows_reused",     stats.background_rows_reused);
    append_profile_counter(
        stream,
        "background_row_clean_reuse_skips",
        stats.background_row_clean_reuse_skips);
    append_profile_counter(stream, "background_rows_removed", stats.background_rows_removed);
    append_profile_counter(
        stream,
        "background_row_cache_fallbacks",
        stats.background_row_cache_fallbacks);
    append_profile_counter(stream, "selection_rows_rebuilt", stats.selection_rows_rebuilt);
    append_profile_counter(stream, "selection_rows_reused",  stats.selection_rows_reused);
    append_profile_counter(
        stream,
        "selection_row_clean_reuse_skips",
        stats.selection_row_clean_reuse_skips);
    append_profile_counter(stream, "selection_rows_removed", stats.selection_rows_removed);
    append_profile_counter(
        stream,
        "selection_row_cache_fallbacks",
        stats.selection_row_cache_fallbacks);
    append_profile_counter(stream, "decoration_rows_rebuilt", stats.decoration_rows_rebuilt);
    append_profile_counter(stream, "decoration_rows_reused",  stats.decoration_rows_reused);
    append_profile_counter(
        stream,
        "decoration_row_clean_reuse_skips",
        stats.decoration_row_clean_reuse_skips);
    append_profile_counter(stream, "decoration_rows_removed", stats.decoration_rows_removed);
    append_profile_counter(
        stream,
        "decoration_row_cache_fallbacks",
        stats.decoration_row_cache_fallbacks);
    append_profile_counter(stream, "graphic_rect_rows_rebuilt", stats.graphic_rect_rows_rebuilt);
    append_profile_counter(stream, "graphic_rect_rows_reused",  stats.graphic_rect_rows_reused);
    append_profile_counter(
        stream,
        "graphic_rect_row_clean_reuse_skips",
        stats.graphic_rect_row_clean_reuse_skips);
    append_profile_counter(stream, "graphic_rect_rows_removed", stats.graphic_rect_rows_removed);
    append_profile_counter(
        stream,
        "graphic_rect_row_cache_fallbacks",
        stats.graphic_rect_row_cache_fallbacks);
    append_profile_counter(stream, "graphic_arc_rows_rebuilt", stats.graphic_arc_rows_rebuilt);
    append_profile_counter(stream, "graphic_arc_rows_reused",  stats.graphic_arc_rows_reused);
    append_profile_counter(
        stream,
        "graphic_arc_row_clean_reuse_skips",
        stats.graphic_arc_row_clean_reuse_skips);
    append_profile_counter(stream, "graphic_arc_rows_removed", stats.graphic_arc_rows_removed);
    append_profile_counter(
        stream,
        "graphic_arc_row_cache_fallbacks",
        stats.graphic_arc_row_cache_fallbacks);
}

void append_surface_geometry_profile_text(
    QTextStream&               stream,
    const VNM_TerminalSurface& surface)
{
    const term::Qt_grid_metrics_provider metrics_provider(
        term::vnm_terminal_font(surface.font_family(), surface.font_size()),
        surface.window() != nullptr ? surface.window()->devicePixelRatio() : 1.0);
    const term::terminal_cell_metrics_t cell_metrics =
        metrics_provider.cell_metrics();
    const QQuickWindow* const window = surface.window();

    stream << "surface_geometry\n";
    stream << "  rows=" << surface.rows() << '\n';
    stream << "  columns=" << surface.columns() << '\n';
    stream << "  surface_width=" << surface.width() << '\n';
    stream << "  surface_height=" << surface.height() << '\n';
    stream << "  cell_width=" << cell_metrics.width << '\n';
    stream << "  cell_height=" << cell_metrics.height << '\n';
    stream << "  font_family=" << profile_string_literal(surface.font_family()) << '\n';
    stream << "  font_size=" << surface.font_size() << '\n';
    if (window != nullptr) {
        stream << "  window_width=" << window->width() << '\n';
        stream << "  window_height=" << window->height() << '\n';
        stream << "  device_pixel_ratio=" << window->devicePixelRatio() << '\n';
    }
}

void append_slow_text_layout_diagnostics_text(
    QTextStream&           stream,
    const term::terminal_text_layout_slow_diagnostics_t&
                           diagnostics)
{
    stream
        << "slow_text_layouts"
        << " threshold_ns="   << static_cast<qulonglong>(diagnostics.threshold_ns)
        << " slow_calls="     << static_cast<qulonglong>(diagnostics.slow_call_count)
        << " stored_samples=" << static_cast<qulonglong>(diagnostics.samples.size())
        << '\n';

    int index = 0;
    for (const term::terminal_text_layout_slow_diagnostic_t& sample :
        diagnostics.samples)
    {
        stream
            << "  sample index="    << index
            << " duration_ns="      << static_cast<qulonglong>(sample.duration_ns)
            << " text_utf16_units=" << sample.text_utf16_units
            << " text_codepoints="  << sample.text_codepoints
            << " text_hash="        << static_cast<qulonglong>(sample.text_hash)
            << " row="              << sample.row
            << " logical_row="      << sample.logical_row
            << " column="           << sample.column
            << " style_id="         << sample.style_id
            << " hyperlink_id="     << static_cast<qulonglong>(sample.hyperlink_id)
            << " rect_width="       << sample.rect_width
            << " rect_height="      << sample.rect_height
            << " ascii_only="       << (sample.ascii_only ? "true" : "false")
            << " printable_ascii_only="
            << (sample.printable_ascii_only ? "true" : "false")
            << " has_control_codepoint="
            << (sample.has_control_codepoint ? "true" : "false")
            << " clipped="          << (sample.clipped ? "true" : "false")
            << " force_blended_order="
            << (sample.force_blended_order ? "true" : "false")
            << " ascii_layout_font="
            << (sample.ascii_layout_font ? "true" : "false")
            << " line_has_text="    << (sample.line_has_text ? "true" : "false")
            << " font_family="      << profile_string_literal(sample.font_family)
            << " font_style_name="
            << profile_string_literal(sample.font_style_name)
            << " resolved_font_family="
            << profile_string_literal(sample.resolved_font_family)
            << " resolved_font_style_name="
            << profile_string_literal(sample.resolved_font_style_name)
            << " font_point_size="  << sample.font_point_size
            << " font_pixel_size="  << sample.font_pixel_size
            << " font_weight="      << sample.font_weight
            << " font_italic="      << (sample.font_italic ? "true" : "false")
            << " codepoints="
            << profile_string_literal(sample.codepoint_sample)
            << " text_preview_truncated="
            << (sample.text_preview_truncated ? "true" : "false")
            << " text_preview="
            << profile_string_literal(sample.text_preview)
            << '\n';
        ++index;
    }
}

void append_qsg_atlas_profile_text(
    QTextStream&                         stream,
    const term::Qsg_atlas_frame_report&  report)
{
    const term::Glyph_coverage_counts& coverage =
        report.frame_build.glyph_coverage;

    stream << "qsg_atlas\n";
    stream << "  renderer=atlas\n";
    stream << "  sampler_mode="
        << QString::fromLatin1(
            term::qsg_atlas_sampler_mode_name(report.render.glyph_sampler_mode))
        << '\n';
    append_profile_counter(stream, "capture_count", report.capture_count);
    append_profile_counter(stream, "prepare_count", report.prepare_count);
    append_profile_counter(stream, "render_count", report.render_count);
    append_profile_counter(stream, "capture_sequence", report.capture_sequence);
    append_profile_counter(
        stream,
        "captured_snapshot_sequence",
        report.captured_snapshot_sequence);
    append_profile_counter(stream, "captured_font_epoch", report.captured_font_epoch);
    stream
        << "  command_buffer_non_null="
        << (report.command_buffer_non_null ? "true" : "false") << '\n';
    stream
        << "  render_target_non_null="
        << (report.render_target_non_null ? "true" : "false") << '\n';
    stream << "  rhi_non_null=" << (report.rhi_non_null ? "true" : "false") << '\n';
    stream << "  drew=" << (report.drew ? "true" : "false") << '\n';
    append_profile_bool(
        stream,
        "coverage_texture_created",
        report.coverage_texture_created);
    append_profile_bool(
        stream,
        "coverage_upload_recorded",
        report.coverage_upload_recorded);
    append_profile_counter(stream, "rasterized_glyphs", report.rasterized_glyphs);
    append_profile_counter(stream, "atlas_page_count", report.atlas_page_count);
    append_profile_counter(
        stream,
        "max_glyph_instance_page",
        static_cast<std::uint64_t>(std::max(
            0,
            report.frame_build.max_glyph_instance_page)));
    const term::Qsg_atlas_producer_summary& producer = report.producer;
    stream << "  producer\n";
    append_profile_counter(
        stream,
        "text_runs_considered",
        producer.text_runs_considered);
    append_profile_counter(stream, "text_runs_empty", producer.text_runs_empty);
    append_profile_counter(
        stream,
        "shape_cache_lookups",
        producer.shape_cache_lookups);
    append_profile_counter(stream, "shape_cache_hits", producer.shape_cache_hits);
    append_profile_counter(
        stream,
        "shape_cache_misses",
        producer.shape_cache_misses);
    append_profile_counter(
        stream,
        "shape_cache_inserts",
        producer.shape_cache_inserts);
    append_profile_counter(stream, "shape_cache_pruned", producer.shape_cache_pruned);
    append_profile_counter(
        stream,
        "shape_cache_entries",
        producer.shape_cache_entries);
    append_profile_counter(stream, "shaped_runs_built", producer.shaped_runs_built);
    append_profile_counter(
        stream,
        "shaped_runs_reused",
        producer.shaped_runs_reused);
    append_profile_counter(
        stream,
        "shaped_glyph_records_built",
        producer.shaped_glyph_records_built);
    append_profile_counter(
        stream,
        "shaped_glyph_records_reused",
        producer.shaped_glyph_records_reused);
    append_profile_counter(
        stream,
        "presentation_run_scans",
        producer.presentation_run_scans);
    append_profile_counter(
        stream,
        "presentation_source_scans",
        producer.presentation_source_scans);
    append_profile_counter(
        stream,
        "presentation_fast_text_runs",
        producer.presentation_fast_text_runs);
    append_profile_counter(
        stream,
        "presentation_emoji_runs",
        producer.presentation_emoji_runs);
    append_profile_counter(
        stream,
        "slot_resolutions_built",
        producer.slot_resolutions_built);
    append_profile_counter(
        stream,
        "slot_resolutions_reused",
        producer.slot_resolutions_reused);
    append_profile_counter(stream, "simple_path_attempts", producer.simple_path_attempts);
    append_profile_counter(stream, "simple_path_used", producer.simple_path_used);
    append_profile_counter(stream, "simple_path_fallbacks", producer.simple_path_fallbacks);
    const term::Qsg_atlas_warm_lazy_summary& warm_lazy = report.warm_lazy;
    stream << "  warm_lazy\n";
    append_profile_bool(stream, "warm_completed", warm_lazy.warm_completed);
    append_profile_counter(stream, "warm_epoch", warm_lazy.warm_epoch);
    append_profile_counter(stream, "warm_seed_strings", warm_lazy.warm_seed_strings);
    append_profile_counter(
        stream,
        "warm_shaped_glyph_records",
        warm_lazy.warm_shaped_glyph_records);
    append_profile_counter(
        stream,
        "warm_covered_glyph_records",
        warm_lazy.warm_covered_glyph_records);
    append_profile_counter(
        stream,
        "warm_skipped_glyph_records",
        warm_lazy.warm_skipped_glyph_records);
    append_profile_counter(
        stream,
        "warm_environment_skipped_glyph_records",
        warm_lazy.warm_environment_skipped_glyph_records);
    append_profile_counter(
        stream,
        "warm_failed_glyph_records",
        warm_lazy.warm_failed_glyph_records);
    append_profile_counter(
        stream,
        "warm_missing_string_indexes",
        warm_lazy.warm_missing_string_indexes);
    append_profile_counter(
        stream,
        "warm_invalid_string_indexes",
        warm_lazy.warm_invalid_string_indexes);
    append_profile_counter(
        stream,
        "warm_unsupported_images",
        warm_lazy.warm_unsupported_images);
    append_profile_counter(stream, "warm_cache_hits", warm_lazy.warm_cache_hits);
    append_profile_counter(
        stream,
        "warm_insert_attempts",
        warm_lazy.warm_insert_attempts);
    append_profile_counter(stream, "warm_inserts", warm_lazy.warm_inserts);
    append_profile_counter(
        stream,
        "warm_failed_inserts",
        warm_lazy.warm_failed_inserts);
    stream << "  warm_elapsed_ms=" << warm_lazy.warm_elapsed_ms << '\n';
    append_profile_bool(stream, "warm_page_pressure", warm_lazy.warm_page_pressure);
    append_profile_counter(
        stream,
        "lazy_insert_attempts",
        warm_lazy.lazy_insert_attempts);
    append_profile_counter(stream, "lazy_inserts", warm_lazy.lazy_inserts);
    append_profile_counter(
        stream,
        "lazy_failed_inserts",
        warm_lazy.lazy_failed_inserts);
    stream << "  lazy_elapsed_ms=" << warm_lazy.lazy_elapsed_ms << '\n';
    append_profile_counter(
        stream,
        "lazy_max_insert_us",
        warm_lazy.lazy_max_insert_us);
    append_profile_counter(stream, "lazy_frames", warm_lazy.lazy_frames);
    append_profile_counter(
        stream,
        "incomplete_frames",
        warm_lazy.incomplete_frames);
    stream << "  placement\n";
    append_profile_counter(
        stream,
        "snapped_origin_failures",
        static_cast<std::uint64_t>(report.frame_build.snapped_origin_failures));
    stream << "  misses\n";
    append_profile_counter(
        stream,
        "glyph_missed_instances",
        static_cast<std::uint64_t>(report.frame_build.glyph_missed_instances));
    append_profile_counter(
        stream,
        "glyph_coverage_failures",
        static_cast<std::uint64_t>(report.frame_build.glyph_coverage_failures));
    append_profile_counter(
        stream,
        "glyph_atlas_insert_failures",
        static_cast<std::uint64_t>(report.frame_build.glyph_atlas_insert_failures));
    if (report.frame_build.first_glyph_miss.valid) {
        const term::Qsg_atlas_glyph_miss_diagnostic& miss =
            report.frame_build.first_glyph_miss;
        stream << "  first_glyph_miss\n";
        stream << "  cause="
            << QString::fromLatin1(
                term::qsg_atlas_glyph_miss_cause_name(miss.cause))
            << '\n';
        stream << "  coverage_kind="
            << QString::fromLatin1(
                term::qsg_atlas_glyph_coverage_kind_name(
                    miss.image.coverage_kind))
            << '\n';
        stream << "  presentation="
            << QString::fromLatin1(
                term::qsg_atlas_glyph_image_presentation_name(
                    miss.image.presentation))
            << '\n';
        append_profile_counter(
            stream,
            "glyph_index",
            static_cast<std::uint64_t>(miss.image.glyph_index));
        stream << "  fallback_face_id="
            << profile_string_literal(miss.image.fallback_face_id)
            << '\n';
        append_profile_counter(
            stream,
            "source_format",
            static_cast<std::uint64_t>(miss.image.source_format));
        append_profile_counter(
            stream,
            "source_string_start",
            static_cast<std::uint64_t>(miss.image.source_string_start));
        append_profile_counter(
            stream,
            "source_string_end",
            static_cast<std::uint64_t>(miss.image.source_string_end));
        append_profile_counter(
            stream,
            "atlas_page_count",
            static_cast<std::uint64_t>(miss.atlas_page_count));
        append_profile_counter(
            stream,
            "atlas_page_budget",
            static_cast<std::uint64_t>(miss.atlas_page_budget));
    }
    stream << "  coverage\n";
    append_profile_counter(
        stream,
        "grayscale_masks",
        static_cast<std::uint64_t>(coverage.grayscale_masks));
    append_profile_counter(
        stream,
        "lcd_rgb_masks",
        static_cast<std::uint64_t>(coverage.lcd_rgb_masks));
    append_profile_counter(
        stream,
        "lcd_bgr_masks",
        static_cast<std::uint64_t>(coverage.lcd_bgr_masks));
    append_profile_counter(
        stream,
        "color_images",
        static_cast<std::uint64_t>(coverage.color_images));
    append_profile_counter(
        stream,
        "ambiguous_images",
        static_cast<std::uint64_t>(coverage.ambiguous_images));
    append_profile_counter(
        stream,
        "unsupported_images",
        static_cast<std::uint64_t>(coverage.unsupported_images));
    append_profile_counter(
        stream,
        "missed_images",
        static_cast<std::uint64_t>(coverage.missed_images));
    stream << "  buffer_upload\n";
    append_profile_counter(
        stream,
        "atlas_page_budget",
        report.render.atlas_page_budget);
    append_profile_counter(
        stream,
        "atlas_budget_bytes",
        report.render.atlas_budget_bytes);
    append_profile_counter(stream, "atlas_used_bytes", report.render.atlas_used_bytes);
    append_profile_counter(
        stream,
        "atlas_failed_inserts",
        report.render.atlas_failed_inserts);
    append_profile_counter(
        stream,
        "shaped_text_runs",
        static_cast<std::uint64_t>(report.render.shaped_text_runs));
    append_profile_counter(
        stream,
        "shaped_glyph_records",
        static_cast<std::uint64_t>(report.render.shaped_glyph_records));
    append_profile_counter(
        stream,
        "shaped_missing_string_indexes",
        static_cast<std::uint64_t>(
            report.render.shaped_missing_string_indexes));
    append_profile_counter(
        stream,
        "shaped_invalid_string_indexes",
        static_cast<std::uint64_t>(
            report.render.shaped_invalid_string_indexes));
    append_profile_bool(
        stream,
        "atlas_page_pressure",
        report.render.atlas_page_pressure);
    stream << "  render\n";
    append_profile_counter(stream, "draw_calls", report.render.draw_calls);
    append_profile_counter(stream, "rect_draw_calls", report.render.rect_draw_calls);
    append_profile_counter(stream, "glyph_draw_calls", report.render.glyph_draw_calls);
    append_profile_counter(
        stream,
        "rect_buffer_uploaded_bytes",
        report.render.rect_buffer.uploaded_bytes);
    append_profile_counter(
        stream,
        "glyph_buffer_uploaded_bytes",
        report.render.glyph_buffer.uploaded_bytes);
    stream << "  capabilities\n";
    append_profile_bool(
        stream,
        "glyph_shader_package_available",
        report.render.glyph_shader_package_available);
    append_profile_bool(
        stream,
        "dual_source_probe_shader_package_available",
        report.render.dual_source_probe_shader_package_available);
    append_profile_bool(
        stream,
        "dual_source_blend_factors_available",
        report.render.dual_source_blend_factors_available);
    append_profile_bool(
        stream,
        "dual_source_blend_factors_runtime_probe",
        report.render.dual_source_blend_factors_runtime_probe);
}

bool write_profile_text(
    const QString&                     path,
    VNM_TerminalSurface&               surface,
    const term::Hierarchical_profiler& gui_profiler,
    QString*                           out_error)
{
    const term::Render_profile_snapshot_t render_profile =
        term::VNM_TerminalSurface_render_bridge::render_profiler_snapshot(surface);
    const term::Terminal_screen_model_dirty_row_stats dirty_row_stats =
        term::VNM_TerminalSurface_render_bridge::dirty_row_stats(surface);
    const term::Terminal_screen_model_dirty_row_timeline dirty_row_timeline =
        term::VNM_TerminalSurface_render_bridge::dirty_row_timeline(surface);
    const term::Terminal_screen_model_profile_stats model_profile_stats =
        term::VNM_TerminalSurface_render_bridge::model_profile_stats(surface);
    const term::Terminal_session_profile_stats session_profile_stats =
        term::VNM_TerminalSurface_render_bridge::session_profile_stats(surface);
    const term::terminal_renderer_stats_t renderer_stats =
        term::VNM_TerminalSurface_render_bridge::last_renderer_stats(surface);
    const term::terminal_renderer_cumulative_stats_t cumulative_renderer_stats =
        term::VNM_TerminalSurface_render_bridge::cumulative_renderer_stats(surface);
    const term::Qsg_atlas_frame_report atlas_report =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    const term::Profile_timeline_snapshot gui_timeline = gui_profiler.timeline_snapshot();

    QString text;
    QTextStream stream(&text);
    stream << "vnm_terminal example terminal profile\n";
    stream << "format=1\n";
    stream << "time_unit=ns\n\n";
    append_surface_geometry_profile_text(stream, surface);
    stream << '\n';
    append_dirty_row_stats_text(stream, dirty_row_stats);
    stream << '\n';
    append_dirty_row_timeline_text(stream, dirty_row_timeline);
    stream << '\n';
    append_model_profile_stats_text(stream, model_profile_stats);
    stream << '\n';
    append_session_profile_stats_text(stream, session_profile_stats);
    stream << '\n';
    append_renderer_stats_text(stream, renderer_stats);
    stream << '\n';
    append_cumulative_renderer_stats_text(stream, cumulative_renderer_stats);
    stream << '\n';
    append_qsg_atlas_profile_text(stream, atlas_report);
    stream << '\n';
    append_slow_text_layout_diagnostics_text(stream, render_profile.slow_text_layouts);
    stream << "\ngui_thread\n";
    append_profile_node_text(stream, gui_profiler.root_snapshot(), 1);
    stream << '\n';
    append_profile_timeline_text(stream, QStringLiteral("gui_thread"), gui_timeline);
    stream << "\nrender_thread sequence=" << static_cast<qulonglong>(render_profile.sequence)
        << '\n';
    append_profile_node_text(stream, render_profile.root, 1);
    stream << '\n';
    append_profile_timeline_text(
        stream,
        QStringLiteral("render_thread"),
        render_profile.timeline);
    stream.flush();

    const QString absolute_path = QFileInfo(path).absoluteFilePath();
    QFile file(absolute_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        *out_error = QStringLiteral("could not write profile text %1: %2")
            .arg(absolute_path, file.errorString());
        return false;
    }

    const QByteArray profile_bytes = text.toUtf8();
    if (file.write(profile_bytes) != profile_bytes.size()) {
        *out_error = QStringLiteral("could not write profile text %1: %2")
            .arg(absolute_path, file.errorString());
        return false;
    }

    return true;
}
#endif

template<typename Value>
void insert_json_counter(
    QJsonObject&  object,
    const char*   name,
    Value         value)
{
    object.insert(
        QString::fromLatin1(name),
        QString::number(static_cast<qulonglong>(value)));
}

constexpr const char* k_runtime_frame_rate_elapsed_basis =
    "app_exec_elapsed_ms_including_process_startup_excluding_profile_write";
constexpr const char* k_paint_completed_frame_counter_path =
    "renderer.paint_completed_frames";
constexpr const char* k_qsg_atlas_render_frame_counter_path =
    "qsg_atlas.render_count";

struct renderer_frame_evidence_t
{
    const char*   counter_path = k_paint_completed_frame_counter_path;
    std::uint64_t frame_count  = 0U;
};

double frames_per_second(
    std::uint64_t frame_count,
    qint64        elapsed_ms)
{
    return elapsed_ms > 0
        ? static_cast<double>(frame_count) * 1000.0 / static_cast<double>(elapsed_ms)
        : 0.0;
}

renderer_frame_evidence_t renderer_frame_evidence(
    const term::terminal_renderer_cumulative_stats_t& cumulative_stats,
    const term::Qsg_atlas_frame_report&               atlas_report)
{
    if (atlas_report.render_count > 0U) {
        return {
            k_qsg_atlas_render_frame_counter_path,
            atlas_report.render_count,
        };
    }

    return {
        k_paint_completed_frame_counter_path,
        cumulative_stats.paint_completed_frames,
    };
}

QJsonObject renderer_frame_evidence_json(
    const renderer_frame_evidence_t& evidence,
    qint64                           elapsed_ms)
{
    QJsonObject object;
    object.insert(
        QStringLiteral("counter_path"),
        QString::fromLatin1(evidence.counter_path));
    insert_json_counter(object, "frame_count", evidence.frame_count);
    object.insert(
        QStringLiteral("frames_per_second"),
        frames_per_second(evidence.frame_count, elapsed_ms));
    object.insert(
        QStringLiteral("elapsed_basis"),
        QString::fromLatin1(k_runtime_frame_rate_elapsed_basis));
    return object;
}

QJsonObject startup_metrics_json(
    const Runtime_state&              state,
    const renderer_frame_evidence_t&  frame_evidence)
{
    QJsonObject object;
    object.insert(QStringLiteral("first_output_elapsed_ms"), state.first_output_elapsed_ms);
    object.insert(QStringLiteral("output_seen"), state.output_seen);
    object.insert(
        QStringLiteral("visible_first_frame_completed"),
        frame_evidence.frame_count > 0U);
    object.insert(
        QStringLiteral("visible_first_frame_counter_path"),
        QString::fromLatin1(frame_evidence.counter_path));
    return object;
}

QJsonObject profiling_measurement_json(const metrics_timing_t& timing)
{
    QJsonObject object;
    object.insert(
        QStringLiteral("compiled"),
        static_cast<bool>(VNM_TERMINAL_PROFILING_ENABLED));
    object.insert(QStringLiteral("profile_text_requested"), timing.profile_text_requested);
    object.insert(QStringLiteral("profile_write_elapsed_ms"), timing.profile_write_elapsed_ms);
    object.insert(QStringLiteral("elapsed_ms_excludes_profile_write"), true);
    return object;
}

QJsonObject atlas_buffer_summary_json(
    const term::Qsg_atlas_buffer_update_summary& summary)
{
    QJsonObject object;
    insert_json_counter(object, "rhi_frames_in_flight", summary.rhi_frames_in_flight);
    insert_json_counter(object, "rhi_frame_slot", summary.rhi_frame_slot);
    insert_json_counter(object, "instance_count", summary.instance_count);
    insert_json_counter(
        object,
        "active_instance_count",
        summary.active_instance_count);
    insert_json_counter(object, "instance_bytes", summary.instance_bytes);
    insert_json_counter(object, "buffer_bytes", summary.buffer_bytes);
    insert_json_counter(object, "dirty_rows", summary.dirty_rows);
    insert_json_counter(object, "seeded_slots", summary.seeded_slots);
    insert_json_counter(object, "full_uploads", summary.full_uploads);
    insert_json_counter(object, "partial_uploads", summary.partial_uploads);
    insert_json_counter(object, "uploaded_bytes", summary.uploaded_bytes);
    object.insert(QStringLiteral("full_upload"), summary.full_upload);
    object.insert(QStringLiteral("partial_upload"), summary.partial_upload);
    object.insert(QStringLiteral("skipped_upload"), summary.skipped_upload);
    object.insert(
        QStringLiteral("rotating_slot_seed_upload"),
        summary.rotating_slot_seed_upload);
    object.insert(
        QStringLiteral("buffer_recreated_upload"),
        summary.buffer_recreated_upload);
    object.insert(
        QStringLiteral("instance_layout_changed_upload"),
        summary.instance_layout_changed_upload);
    object.insert(QStringLiteral("full_repaint_upload"), summary.full_repaint_upload);
    object.insert(QStringLiteral("non_dirty_state_upload"), summary.non_dirty_state_upload);
    object.insert(QStringLiteral("row_stable_layout"), summary.row_stable_layout);
    return object;
}

QJsonObject glyph_coverage_counts_json(
    const term::Qsg_atlas_frame_build_summary& summary)
{
    const term::Glyph_coverage_counts& counts = summary.glyph_coverage;

    QJsonObject object;
    insert_json_counter(object, "grayscale_masks", counts.grayscale_masks);
    insert_json_counter(object, "lcd_rgb_masks", counts.lcd_rgb_masks);
    insert_json_counter(object, "lcd_bgr_masks", counts.lcd_bgr_masks);
    insert_json_counter(object, "color_images", counts.color_images);
    insert_json_counter(object, "ambiguous_images", counts.ambiguous_images);
    insert_json_counter(object, "unsupported_images", counts.unsupported_images);
    insert_json_counter(object, "missed_images", counts.missed_images);
    return object;
}

QJsonObject atlas_first_glyph_miss_json(
    const term::Qsg_atlas_glyph_miss_diagnostic& miss)
{
    QJsonObject object;
    object.insert(QStringLiteral("valid"), miss.valid);
    object.insert(
        QStringLiteral("cause"),
        QString::fromLatin1(term::qsg_atlas_glyph_miss_cause_name(miss.cause)));
    object.insert(
        QStringLiteral("coverage_kind"),
        QString::fromLatin1(
            term::qsg_atlas_glyph_coverage_kind_name(
                miss.image.coverage_kind)));
    object.insert(
        QStringLiteral("presentation"),
        QString::fromLatin1(
            term::qsg_atlas_glyph_image_presentation_name(
                miss.image.presentation)));
    object.insert(
        QStringLiteral("source_format"),
        static_cast<int>(miss.image.source_format));
    object.insert(QStringLiteral("source_width"), miss.image.source_size.width());
    object.insert(QStringLiteral("source_height"), miss.image.source_size.height());
    insert_json_counter(object, "glyph_index", miss.image.glyph_index);
    object.insert(QStringLiteral("fallback_face_id"), miss.image.fallback_face_id);
    insert_json_counter(object, "text_run_index", miss.image.text_run_index);
    insert_json_counter(object, "glyph_run_index", miss.image.glyph_run_index);
    insert_json_counter(
        object,
        "glyph_index_in_run",
        miss.image.glyph_index_in_run);
    insert_json_counter(
        object,
        "source_string_start",
        miss.image.source_string_start);
    insert_json_counter(object, "source_string_end", miss.image.source_string_end);
    object.insert(QStringLiteral("tile_width"), miss.tile_size.width());
    object.insert(QStringLiteral("tile_height"), miss.tile_size.height());
    insert_json_counter(object, "tile_bytes_per_line", miss.tile_bytes_per_line);
    insert_json_counter(object, "atlas_page_count", miss.atlas_page_count);
    insert_json_counter(object, "atlas_page_budget", miss.atlas_page_budget);
    object.insert(QStringLiteral("atlas_page_width"), miss.atlas_page_size.width());
    object.insert(
        QStringLiteral("atlas_page_height"),
        miss.atlas_page_size.height());
    return object;
}

QJsonObject atlas_capabilities_json(const term::Qsg_atlas_render_summary& summary)
{
    QJsonObject object;
    object.insert(
        QStringLiteral("glyph_shader_package_available"),
        summary.glyph_shader_package_available);
    object.insert(
        QStringLiteral("dual_source_probe_shader_package_available"),
        summary.dual_source_probe_shader_package_available);
    object.insert(
        QStringLiteral("dual_source_blend_factors_available"),
        summary.dual_source_blend_factors_available);
    object.insert(
        QStringLiteral("dual_source_blend_factors_runtime_probe"),
        summary.dual_source_blend_factors_runtime_probe);
    return object;
}

QJsonObject atlas_render_summary_json(
    const term::Qsg_atlas_render_summary& summary)
{
    QJsonObject object;
    object.insert(
        QStringLiteral("rect_buffer"),
        atlas_buffer_summary_json(summary.rect_buffer));
    object.insert(
        QStringLiteral("glyph_buffer"),
        atlas_buffer_summary_json(summary.glyph_buffer));
    insert_json_counter(
        object,
        "shaped_text_runs",
        summary.shaped_text_runs);
    insert_json_counter(
        object,
        "shaped_glyph_records",
        summary.shaped_glyph_records);
    insert_json_counter(
        object,
        "shaped_missing_string_indexes",
        summary.shaped_missing_string_indexes);
    insert_json_counter(
        object,
        "shaped_invalid_string_indexes",
        summary.shaped_invalid_string_indexes);
    insert_json_counter(
        object,
        "glyph_buffer_instances",
        summary.glyph_buffer_instances);
    insert_json_counter(
        object,
        "glyph_text_row_capacity",
        summary.glyph_text_row_capacity);
    insert_json_counter(
        object,
        "glyph_cursor_text_row_capacity",
        summary.glyph_cursor_text_row_capacity);
    insert_json_counter(
        object,
        "background_rects_before_coalescing",
        summary.background_rects_before_coalescing);
    insert_json_counter(
        object,
        "background_rects_after_coalescing",
        summary.background_rects_after_coalescing);
    insert_json_counter(
        object,
        "background_rects_coalesced",
        summary.background_rects_coalesced);
    insert_json_counter(object, "rect_draw_calls", summary.rect_draw_calls);
    insert_json_counter(object, "glyph_draw_calls", summary.glyph_draw_calls);
    insert_json_counter(object, "draw_calls", summary.draw_calls);
    insert_json_counter(object, "atlas_page_count", summary.atlas_page_count);
    insert_json_counter(object, "atlas_page_budget", summary.atlas_page_budget);
    insert_json_counter(object, "atlas_page_bytes", summary.atlas_page_bytes);
    insert_json_counter(object, "atlas_allocated_bytes", summary.atlas_allocated_bytes);
    insert_json_counter(object, "atlas_budget_bytes", summary.atlas_budget_bytes);
    insert_json_counter(object, "atlas_used_bytes", summary.atlas_used_bytes);
    insert_json_counter(object, "atlas_failed_inserts", summary.atlas_failed_inserts);
    object.insert(QStringLiteral("atlas_page_pressure"), summary.atlas_page_pressure);
    object.insert(
        QStringLiteral("coverage_texture_uploaded"),
        summary.coverage_texture_uploaded);
    object.insert(
        QStringLiteral("coverage_texture_skipped"),
        summary.coverage_texture_skipped);
    object.insert(
        QStringLiteral("full_dirty_range_reupload"),
        summary.full_dirty_range_reupload);
    object.insert(
        QStringLiteral("public_projection_full_reupload"),
        summary.public_projection_full_reupload);
    object.insert(QStringLiteral("scroll_full_reupload"), summary.scroll_full_reupload);
    object.insert(
        QStringLiteral("non_dirty_selection_invalidation"),
        summary.non_dirty_selection_invalidation);
    object.insert(
        QStringLiteral("non_dirty_cursor_invalidation"),
        summary.non_dirty_cursor_invalidation);
    object.insert(
        QStringLiteral("non_dirty_preedit_invalidation"),
        summary.non_dirty_preedit_invalidation);
    object.insert(
        QStringLiteral("non_dirty_options_invalidation"),
        summary.non_dirty_options_invalidation);
    object.insert(
        QStringLiteral("non_dirty_visual_bell_invalidation"),
        summary.non_dirty_visual_bell_invalidation);
    object.insert(QStringLiteral("font_epoch_invalidation"), summary.font_epoch_invalidation);
    return object;
}

QJsonObject atlas_producer_summary_json(
    const term::Qsg_atlas_producer_summary& summary)
{
    QJsonObject object;
    insert_json_counter(object, "text_runs_considered", summary.text_runs_considered);
    insert_json_counter(object, "text_runs_empty", summary.text_runs_empty);
    insert_json_counter(object, "shape_cache_lookups", summary.shape_cache_lookups);
    insert_json_counter(object, "shape_cache_hits", summary.shape_cache_hits);
    insert_json_counter(object, "shape_cache_misses", summary.shape_cache_misses);
    insert_json_counter(object, "shape_cache_inserts", summary.shape_cache_inserts);
    insert_json_counter(object, "shape_cache_pruned", summary.shape_cache_pruned);
    insert_json_counter(object, "shape_cache_entries", summary.shape_cache_entries);
    insert_json_counter(object, "shaped_runs_built", summary.shaped_runs_built);
    insert_json_counter(object, "shaped_runs_reused", summary.shaped_runs_reused);
    insert_json_counter(
        object,
        "shaped_glyph_records_built",
        summary.shaped_glyph_records_built);
    insert_json_counter(
        object,
        "shaped_glyph_records_reused",
        summary.shaped_glyph_records_reused);
    insert_json_counter(
        object,
        "presentation_run_scans",
        summary.presentation_run_scans);
    insert_json_counter(
        object,
        "presentation_source_scans",
        summary.presentation_source_scans);
    insert_json_counter(
        object,
        "presentation_fast_text_runs",
        summary.presentation_fast_text_runs);
    insert_json_counter(
        object,
        "presentation_emoji_runs",
        summary.presentation_emoji_runs);
    insert_json_counter(object, "slot_resolutions_built", summary.slot_resolutions_built);
    insert_json_counter(object, "slot_resolutions_reused", summary.slot_resolutions_reused);
    insert_json_counter(object, "simple_path_attempts", summary.simple_path_attempts);
    insert_json_counter(object, "simple_path_used", summary.simple_path_used);
    insert_json_counter(object, "simple_path_fallbacks", summary.simple_path_fallbacks);
    return object;
}

QJsonObject atlas_warm_lazy_summary_json(
    const term::Qsg_atlas_warm_lazy_summary& summary)
{
    QJsonObject object;
    object.insert(QStringLiteral("warm_completed"), summary.warm_completed);
    insert_json_counter(object, "warm_epoch", summary.warm_epoch);
    insert_json_counter(object, "warm_seed_strings", summary.warm_seed_strings);
    insert_json_counter(
        object,
        "warm_shaped_glyph_records",
        summary.warm_shaped_glyph_records);
    insert_json_counter(
        object,
        "warm_covered_glyph_records",
        summary.warm_covered_glyph_records);
    insert_json_counter(
        object,
        "warm_skipped_glyph_records",
        summary.warm_skipped_glyph_records);
    insert_json_counter(
        object,
        "warm_environment_skipped_glyph_records",
        summary.warm_environment_skipped_glyph_records);
    insert_json_counter(
        object,
        "warm_failed_glyph_records",
        summary.warm_failed_glyph_records);
    insert_json_counter(
        object,
        "warm_missing_string_indexes",
        summary.warm_missing_string_indexes);
    insert_json_counter(
        object,
        "warm_invalid_string_indexes",
        summary.warm_invalid_string_indexes);
    insert_json_counter(
        object,
        "warm_unsupported_images",
        summary.warm_unsupported_images);
    insert_json_counter(object, "warm_cache_hits", summary.warm_cache_hits);
    insert_json_counter(
        object,
        "warm_insert_attempts",
        summary.warm_insert_attempts);
    insert_json_counter(object, "warm_inserts", summary.warm_inserts);
    insert_json_counter(
        object,
        "warm_failed_inserts",
        summary.warm_failed_inserts);
    object.insert(QStringLiteral("warm_elapsed_ms"), summary.warm_elapsed_ms);
    object.insert(QStringLiteral("warm_page_pressure"), summary.warm_page_pressure);
    insert_json_counter(
        object,
        "lazy_insert_attempts",
        summary.lazy_insert_attempts);
    insert_json_counter(object, "lazy_inserts", summary.lazy_inserts);
    insert_json_counter(
        object,
        "lazy_failed_inserts",
        summary.lazy_failed_inserts);
    object.insert(QStringLiteral("lazy_elapsed_ms"), summary.lazy_elapsed_ms);
    insert_json_counter(
        object,
        "lazy_max_insert_us",
        summary.lazy_max_insert_us);
    insert_json_counter(object, "lazy_frames", summary.lazy_frames);
    insert_json_counter(object, "incomplete_frames", summary.incomplete_frames);
    return object;
}

QJsonObject qsg_atlas_metrics_json(const term::Qsg_atlas_frame_report& report)
{
    QJsonObject object;
    object.insert(QStringLiteral("renderer"), QStringLiteral("atlas"));

    insert_json_counter(object, "capture_count", report.capture_count);
    insert_json_counter(object, "prepare_count", report.prepare_count);
    insert_json_counter(object, "render_count", report.render_count);
    insert_json_counter(object, "capture_sequence", report.capture_sequence);
    insert_json_counter(
        object,
        "captured_snapshot_sequence",
        report.captured_snapshot_sequence);
    insert_json_counter(object, "captured_font_epoch", report.captured_font_epoch);
    object.insert(QStringLiteral("command_buffer_non_null"), report.command_buffer_non_null);
    object.insert(QStringLiteral("render_target_non_null"), report.render_target_non_null);
    object.insert(QStringLiteral("rhi_non_null"), report.rhi_non_null);
    object.insert(QStringLiteral("drew"), report.drew);
    object.insert(QStringLiteral("coverage_texture_created"), report.coverage_texture_created);
    object.insert(QStringLiteral("coverage_upload_recorded"), report.coverage_upload_recorded);
    object.insert(QStringLiteral("raw_font_rasterized"), report.raw_font_rasterized);
    insert_json_counter(object, "rasterized_glyphs", report.rasterized_glyphs);
    insert_json_counter(object, "atlas_page_count", report.atlas_page_count);
    insert_json_counter(
        object,
        "max_glyph_instance_page",
        std::max(0, report.frame_build.max_glyph_instance_page));
    insert_json_counter(
        object,
        "snapped_origin_failures",
        report.frame_build.snapped_origin_failures);
    insert_json_counter(
        object,
        "glyph_missed_instances",
        report.frame_build.glyph_missed_instances);
    insert_json_counter(
        object,
        "glyph_coverage_failures",
        report.frame_build.glyph_coverage_failures);
    insert_json_counter(
        object,
        "glyph_atlas_insert_failures",
        report.frame_build.glyph_atlas_insert_failures);
    object.insert(
        QStringLiteral("coverage"),
        glyph_coverage_counts_json(report.frame_build));
    object.insert(
        QStringLiteral("first_glyph_miss"),
        atlas_first_glyph_miss_json(report.frame_build.first_glyph_miss));
    object.insert(
        QStringLiteral("sampler_mode"),
        QString::fromLatin1(
            term::qsg_atlas_sampler_mode_name(report.render.glyph_sampler_mode)));
    object.insert(
        QStringLiteral("capabilities"),
        atlas_capabilities_json(report.render));
    object.insert(
        QStringLiteral("producer"),
        atlas_producer_summary_json(report.producer));
    object.insert(
        QStringLiteral("warm_lazy"),
        atlas_warm_lazy_summary_json(report.warm_lazy));
    object.insert(QStringLiteral("buffer_upload"), atlas_render_summary_json(report.render));
    return object;
}

QJsonObject surface_geometry_json(const VNM_TerminalSurface& surface)
{
    QJsonObject object;
    object.insert(QStringLiteral("rows"), surface.rows());
    object.insert(QStringLiteral("columns"), surface.columns());
    object.insert(QStringLiteral("surface_width"), surface.width());
    object.insert(QStringLiteral("surface_height"), surface.height());
    object.insert(QStringLiteral("font_family"), surface.font_family());
    object.insert(QStringLiteral("font_size"), surface.font_size());
    if (surface.window() != nullptr) {
        object.insert(QStringLiteral("window_width"), surface.window()->width());
        object.insert(QStringLiteral("window_height"), surface.window()->height());
        object.insert(
            QStringLiteral("device_pixel_ratio"),
            surface.window()->devicePixelRatio());
    }
    return object;
}

template<typename Simple_content_stats>
void insert_renderer_simple_content_stats(
    QJsonObject&                  object,
    const Simple_content_stats&   stats)
{
    insert_json_counter(object, "cells_considered", stats.cells_considered);
    insert_json_counter(object, "eligible_cells", stats.eligible_cells);
    insert_json_counter(
        object,
        "eligible_after_all_gates_cells",
        stats.eligible_after_all_gates_cells);
    insert_json_counter(object, "rows_with_eligible_cells", stats.rows_with_eligible_cells);
    insert_json_counter(object, "styles_with_eligible_cells", stats.styles_with_eligible_cells);
    insert_json_counter(object, "dirty_eligible_cells", stats.dirty_eligible_cells);
    insert_json_counter(object, "clean_eligible_cells", stats.clean_eligible_cells);
    insert_json_counter(object, "text_category_empty_cells", stats.text_category_empty_cells);
    insert_json_counter(
        object,
        "text_category_printable_ascii_cells",
        stats.text_category_printable_ascii_cells);
    insert_json_counter(
        object,
        "text_category_other_ascii_cells",
        stats.text_category_other_ascii_cells);
    insert_json_counter(
        object,
        "text_category_non_ascii_cells",
        stats.text_category_non_ascii_cells);
    insert_json_counter(object, "route_none_cells", stats.route_none_cells);
    insert_json_counter(object, "route_fast_text_cells", stats.route_fast_text_cells);
    insert_json_counter(
        object,
        "route_qt_text_layout_cells",
        stats.route_qt_text_layout_cells);
    insert_json_counter(object, "route_fallback_cells", stats.route_fallback_cells);
    insert_json_counter(object, "rejection_none_cells", stats.rejection_none_cells);
    insert_json_counter(
        object,
        "rejection_empty_text_cells",
        stats.rejection_empty_text_cells);
    insert_json_counter(
        object,
        "rejection_invalid_grid_cells",
        stats.rejection_invalid_grid_cells);
    insert_json_counter(
        object,
        "rejection_invalid_position_cells",
        stats.rejection_invalid_position_cells);
    insert_json_counter(
        object,
        "rejection_invalid_style_id_cells",
        stats.rejection_invalid_style_id_cells);
    insert_json_counter(
        object,
        "rejection_wide_continuation_cells",
        stats.rejection_wide_continuation_cells);
    insert_json_counter(
        object,
        "rejection_invalid_display_width_cells",
        stats.rejection_invalid_display_width_cells);
    insert_json_counter(
        object,
        "rejection_invalid_text_encoding_cells",
        stats.rejection_invalid_text_encoding_cells);
    insert_json_counter(
        object,
        "rejection_invalid_text_width_cells",
        stats.rejection_invalid_text_width_cells);
    insert_json_counter(
        object,
        "rejection_multi_cell_text_cells",
        stats.rejection_multi_cell_text_cells);
    insert_json_counter(
        object,
        "rejection_non_printable_ascii_cells",
        stats.rejection_non_printable_ascii_cells);
    insert_json_counter(
        object,
        "rejection_non_ascii_text_cells",
        stats.rejection_non_ascii_text_cells);
    insert_json_counter(
        object,
        "rejection_decoration_cells",
        stats.rejection_decoration_cells);
    insert_json_counter(object, "rejection_hyperlink_cells", stats.rejection_hyperlink_cells);
}

template<typename Frame_stats>
void insert_renderer_frame_stats(
    QJsonObject&        object,
    const Frame_stats&  stats)
{
    QJsonObject simple_content;
    insert_renderer_simple_content_stats(simple_content, stats.simple_content);

    insert_json_counter(object, "visible_rows", stats.visible_rows);
    insert_json_counter(object, "dirty_rows", stats.dirty_rows);
    insert_json_counter(object, "full_dirty_rows", stats.full_dirty_rows);
    insert_json_counter(object, "cell_pass_input_cells", stats.cell_pass_input_cells);
    insert_json_counter(
        object,
        "cell_pass_classification_calls",
        stats.cell_pass_classification_calls);
    insert_json_counter(object, "packed_pass_input_cells", stats.packed_pass_input_cells);
    insert_json_counter(object, "packed_pass_cells_scanned", stats.packed_pass_cells_scanned);
    insert_json_counter(
        object,
        "packed_pass_classification_calls",
        stats.packed_pass_classification_calls);
    insert_json_counter(
        object,
        "packed_text_sidecars_enabled",
        stats.packed_text_sidecars_enabled);
    insert_json_counter(
        object,
        "packed_text_sidecars_disabled",
        stats.packed_text_sidecars_disabled);
    insert_json_counter(
        object,
        "packed_text_disabled_cells_skipped",
        stats.packed_text_disabled_cells_skipped);
    insert_json_counter(object, "packed_cells_appended", stats.packed_cells_appended);
    insert_json_counter(object, "dirty_row_lookup_count", stats.dirty_row_lookup_count);
    insert_json_counter(object, "cells_considered", stats.cells_considered);
    insert_json_counter(object, "cells_skipped_invalid", stats.cells_skipped_invalid);
    insert_json_counter(
        object,
        "cells_skipped_wide_continuation",
        stats.cells_skipped_wide_continuation);
    insert_json_counter(object, "cells_rendered", stats.cells_rendered);
    insert_json_counter(object, "text_cells_empty", stats.text_cells_empty);
    insert_json_counter(
        object,
        "text_cells_rendered_as_text",
        stats.text_cells_rendered_as_text);
    insert_json_counter(
        object,
        "text_cells_printable_ascii",
        stats.text_cells_printable_ascii);
    insert_json_counter(object, "text_cells_other_ascii", stats.text_cells_other_ascii);
    insert_json_counter(object, "text_cells_non_ascii", stats.text_cells_non_ascii);
    insert_json_counter(object, "text_cells_simple_ascii", stats.text_cells_simple_ascii);
    insert_json_counter(object, "text_cells_single_width", stats.text_cells_single_width);
    insert_json_counter(object, "text_cells_multi_width", stats.text_cells_multi_width);
    insert_json_counter(
        object,
        "text_cells_with_decorations",
        stats.text_cells_with_decorations);
    insert_json_counter(object, "text_cells_with_hyperlink", stats.text_cells_with_hyperlink);
    insert_json_counter(object, "text_style_changes", stats.text_style_changes);
    insert_json_counter(object, "text_distinct_styles", stats.text_distinct_styles);
    insert_json_counter(object, "background_rects_emitted", stats.background_rects_emitted);
    insert_json_counter(object, "selection_rects_emitted", stats.selection_rects_emitted);
    insert_json_counter(object, "graphic_rects_emitted", stats.graphic_rects_emitted);
    insert_json_counter(object, "graphic_arcs_emitted", stats.graphic_arcs_emitted);
    insert_json_counter(object, "text_runs_emitted", stats.text_runs_emitted);
    insert_json_counter(
        object,
        "cursor_text_runs_emitted",
        stats.cursor_text_runs_emitted);
    insert_json_counter(
        object,
        "decoration_rects_emitted",
        stats.decoration_rects_emitted);
    insert_json_counter(object, "cursor_rects_emitted", stats.cursor_rects_emitted);
    insert_json_counter(object, "overlay_rects_emitted", stats.overlay_rects_emitted);
    insert_json_counter(object, "packed_rows", stats.packed_rows);
    insert_json_counter(object, "packed_text_spans", stats.packed_text_spans);
    insert_json_counter(object, "packed_text_cells", stats.packed_text_cells);
    if constexpr (requires { stats.packed_text_ascii_direct_cells; }) {
        insert_json_counter(
            object,
            "packed_text_ascii_direct_cells",
            stats.packed_text_ascii_direct_cells);
    }
    if constexpr (requires { stats.packed_text_ascii_direct_bytes; }) {
        insert_json_counter(
            object,
            "packed_text_ascii_direct_bytes",
            stats.packed_text_ascii_direct_bytes);
    }
    if constexpr (requires { stats.packed_text_utf8_cells; }) {
        insert_json_counter(object, "packed_text_utf8_cells", stats.packed_text_utf8_cells);
    }
    if constexpr (requires { stats.packed_text_utf8_input_units; }) {
        insert_json_counter(
            object,
            "packed_text_utf8_input_code_units",
            stats.packed_text_utf8_input_units);
    }
    if constexpr (requires { stats.packed_text_utf8_output_bytes; }) {
        insert_json_counter(
            object,
            "packed_text_utf8_output_bytes",
            stats.packed_text_utf8_output_bytes);
    }
    insert_json_counter(object, "packed_payload_bytes", stats.packed_payload_bytes);
    object.insert(QStringLiteral("simple_content"), simple_content);
}

void insert_text_layout_stats_json(
    QJsonObject&                                      object,
    const term::terminal_renderer_cumulative_stats_t& stats)
{
    insert_json_counter(object, "qt_text_layout_calls", stats.qt_text_layout_calls);
    insert_json_counter(
        object,
        "text_layout_runs_single_code_unit",
        stats.text_layout_runs_single_code_unit);
    insert_json_counter(
        object,
        "text_layout_runs_multi_code_unit",
        stats.text_layout_runs_multi_code_unit);
    insert_json_counter(
        object,
        "text_layout_runs_all_space",
        stats.text_layout_runs_all_space);
    insert_json_counter(
        object,
        "text_layout_runs_printable_ascii",
        stats.text_layout_runs_printable_ascii);
    insert_json_counter(
        object,
        "text_layout_runs_printable_ascii_with_space",
        stats.text_layout_runs_printable_ascii_with_space);
    insert_json_counter(
        object,
        "text_layout_runs_other_ascii",
        stats.text_layout_runs_other_ascii);
    insert_json_counter(
        object,
        "text_layout_runs_non_ascii",
        stats.text_layout_runs_non_ascii);
    insert_json_counter(
        object,
        "text_layout_runs_clipped",
        stats.text_layout_runs_clipped);
    insert_json_counter(
        object,
        "text_layout_runs_ascii_layout_font",
        stats.text_layout_runs_ascii_layout_font);
    insert_json_counter(
        object,
        "text_layout_runs_force_blended_order",
        stats.text_layout_runs_force_blended_order);
    insert_json_counter(
        object,
        "text_layout_runs_with_hyperlink",
        stats.text_layout_runs_with_hyperlink);
    insert_json_counter(
        object,
        "text_layout_runs_with_decoration",
        stats.text_layout_runs_with_decoration);
    insert_json_counter(
        object,
        "text_layout_runs_mixed_ascii_non_ascii",
        stats.text_layout_runs_mixed_ascii_non_ascii);
    insert_json_counter(
        object,
        "text_layout_runs_pure_non_ascii",
        stats.text_layout_runs_pure_non_ascii);
    insert_json_counter(
        object,
        "text_layout_runs_plain_unclipped",
        stats.text_layout_runs_plain_unclipped);
    insert_json_counter(
        object,
        "text_layout_runs_plain_unclipped_ascii_font",
        stats.text_layout_runs_plain_unclipped_ascii_font);
    insert_json_counter(
        object,
        "text_layout_runs_all_space_plain_unclipped",
        stats.text_layout_runs_all_space_plain_unclipped);
    insert_json_counter(
        object,
        "text_layout_runs_printable_ascii_plain_unclipped",
        stats.text_layout_runs_printable_ascii_plain_unclipped);
    insert_json_counter(
        object,
        "text_layout_runs_non_ascii_plain_unclipped",
        stats.text_layout_runs_non_ascii_plain_unclipped);
    insert_json_counter(
        object,
        "text_layout_runs_mixed_ascii_non_ascii_plain_unclipped",
        stats.text_layout_runs_mixed_ascii_non_ascii_plain_unclipped);
    insert_json_counter(
        object,
        "text_layout_runs_pure_non_ascii_plain_unclipped",
        stats.text_layout_runs_pure_non_ascii_plain_unclipped);
    insert_json_counter(
        object,
        "text_layout_runs_fast_space_candidate",
        stats.text_layout_runs_fast_space_candidate);
    insert_json_counter(
        object,
        "text_layout_runs_fast_ascii_candidate",
        stats.text_layout_runs_fast_ascii_candidate);
    insert_json_counter(
        object,
        "text_layout_runs_fast_ascii_no_space_candidate",
        stats.text_layout_runs_fast_ascii_no_space_candidate);
    insert_json_counter(
        object,
        "text_layout_runs_fast_ascii_single_candidate",
        stats.text_layout_runs_fast_ascii_single_candidate);
    insert_json_counter(
        object,
        "text_layout_runs_fast_ascii_multi_candidate",
        stats.text_layout_runs_fast_ascii_multi_candidate);
    insert_json_counter(
        object,
        "text_ascii_replacement_runs_screened",
        stats.text_ascii_replacement_runs_screened);
    insert_json_counter(
        object,
        "text_ascii_replacement_runs_eligible",
        stats.text_ascii_replacement_runs_eligible);
    insert_json_counter(
        object,
        "text_ascii_replacement_runs_attempted",
        stats.text_ascii_replacement_runs_attempted);
    insert_json_counter(
        object,
        "text_ascii_replacement_runs_trusted_fast_path",
        stats.text_ascii_replacement_runs_trusted_fast_path);
    insert_json_counter(
        object,
        "text_ascii_replacement_runs_succeeded",
        stats.text_ascii_replacement_runs_succeeded);
    insert_json_counter(
        object,
        "text_ascii_replacement_runs_all_space_succeeded",
        stats.text_ascii_replacement_runs_all_space_succeeded);
    if constexpr (requires { stats.text_ascii_replacement_add_glyphs_calls; }) {
        insert_json_counter(
            object,
            "text_ascii_replacement_add_glyphs_calls",
            stats.text_ascii_replacement_add_glyphs_calls);
    }
    insert_json_counter(
        object,
        "text_ascii_replacement_runs_fallback",
        stats.text_ascii_replacement_runs_fallback);
    insert_json_counter(
        object,
        "text_ascii_replacement_runs_rejected_clipped",
        stats.text_ascii_replacement_runs_rejected_clipped);
    insert_json_counter(
        object,
        "text_ascii_replacement_runs_rejected_force_blended_order",
        stats.text_ascii_replacement_runs_rejected_force_blended_order);
    insert_json_counter(
        object,
        "text_ascii_replacement_runs_rejected_decoration",
        stats.text_ascii_replacement_runs_rejected_decoration);
    insert_json_counter(
        object,
        "text_ascii_replacement_runs_rejected_hyperlink",
        stats.text_ascii_replacement_runs_rejected_hyperlink);
    insert_json_counter(
        object,
        "text_ascii_replacement_runs_rejected_non_printable_ascii",
        stats.text_ascii_replacement_runs_rejected_non_printable_ascii);
    insert_json_counter(
        object,
        "text_ascii_replacement_runs_rejected_non_ascii",
        stats.text_ascii_replacement_runs_rejected_non_ascii);
    insert_json_counter(
        object,
        "text_ascii_replacement_runs_rejected_geometry",
        stats.text_ascii_replacement_runs_rejected_geometry);
    insert_json_counter(
        object,
        "text_ascii_replacement_runs_rejected_unsupported_font",
        stats.text_ascii_replacement_runs_rejected_unsupported_font);
    insert_json_counter(
        object,
        "text_ascii_replacement_runs_rejected_internal_node",
        stats.text_ascii_replacement_runs_rejected_internal_node);
    insert_json_counter(
        object,
        "text_ascii_replacement_runs_rejected_glyph_mapping",
        stats.text_ascii_replacement_runs_rejected_glyph_mapping);
    insert_json_counter(object, "text_layout_code_units", stats.text_layout_code_units);
    insert_json_counter(
        object,
        "text_layout_space_code_units",
        stats.text_layout_space_code_units);
    insert_json_counter(
        object,
        "text_layout_printable_ascii_code_units",
        stats.text_layout_printable_ascii_code_units);
    insert_json_counter(
        object,
        "text_layout_other_ascii_code_units",
        stats.text_layout_other_ascii_code_units);
    insert_json_counter(
        object,
        "text_layout_non_ascii_code_units",
        stats.text_layout_non_ascii_code_units);
    insert_json_counter(
        object,
        "text_layout_plain_unclipped_code_units",
        stats.text_layout_plain_unclipped_code_units);
    insert_json_counter(
        object,
        "text_layout_all_space_plain_unclipped_code_units",
        stats.text_layout_all_space_plain_unclipped_code_units);
    insert_json_counter(
        object,
        "text_layout_printable_ascii_plain_unclipped_code_units",
        stats.text_layout_printable_ascii_plain_unclipped_code_units);
    insert_json_counter(
        object,
        "text_layout_non_ascii_plain_unclipped_code_units",
        stats.text_layout_non_ascii_plain_unclipped_code_units);
    insert_json_counter(
        object,
        "text_layout_fast_space_candidate_code_units",
        stats.text_layout_fast_space_candidate_code_units);
    insert_json_counter(
        object,
        "text_layout_fast_ascii_candidate_code_units",
        stats.text_layout_fast_ascii_candidate_code_units);
    insert_json_counter(
        object,
        "text_ascii_replacement_code_units_screened",
        stats.text_ascii_replacement_code_units_screened);
    insert_json_counter(
        object,
        "text_ascii_replacement_code_units_eligible",
        stats.text_ascii_replacement_code_units_eligible);
    insert_json_counter(
        object,
        "text_ascii_replacement_code_units_attempted",
        stats.text_ascii_replacement_code_units_attempted);
    insert_json_counter(
        object,
        "text_ascii_replacement_code_units_trusted_fast_path",
        stats.text_ascii_replacement_code_units_trusted_fast_path);
    insert_json_counter(
        object,
        "text_ascii_replacement_code_units_succeeded",
        stats.text_ascii_replacement_code_units_succeeded);
    insert_json_counter(
        object,
        "text_ascii_replacement_code_units_fallback",
        stats.text_ascii_replacement_code_units_fallback);
}

QJsonObject terminal_metrics_json(
    const VNM_TerminalSurface&  surface,
    const Runtime_state&        state,
    const metrics_timing_t&     timing,
    int                         app_result)
{
    const term::terminal_renderer_cumulative_stats_t cumulative_stats =
        term::VNM_TerminalSurface_render_bridge::cumulative_renderer_stats(surface);
    const term::Qsg_atlas_frame_report atlas_report =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
    const renderer_frame_evidence_t frame_evidence =
        renderer_frame_evidence(cumulative_stats, atlas_report);

    QJsonObject frame;
    insert_renderer_frame_stats(frame, cumulative_stats.frame);

    QJsonObject renderer;
    insert_json_counter(renderer, "frames_published", cumulative_stats.frames_published);
    insert_json_counter(
        renderer,
        "paint_completed_frames",
        cumulative_stats.paint_completed_frames);
    insert_json_counter(renderer, "root_reused_frames", cumulative_stats.root_reused_frames);
    insert_json_counter(renderer, "text_content_rebuilds", cumulative_stats.text_content_rebuilds);
    insert_json_counter(renderer, "text_content_reused", cumulative_stats.text_content_reused);
    insert_json_counter(renderer, "text_content_removed", cumulative_stats.text_content_removed);
    insert_json_counter(renderer, "text_content_failures", cumulative_stats.text_content_failures);
    insert_json_counter(
        renderer,
        "atlas_work_created",
        cumulative_stats.atlas_work_created);
    if constexpr (requires { cumulative_stats.atlas_work_reused; }) {
        insert_json_counter(
            renderer,
            "atlas_work_reused",
            cumulative_stats.atlas_work_reused);
    }
    insert_json_counter(
        renderer,
        "text_cache_entry_child_nodes_cleared_for_replacement",
        cumulative_stats.text_cache_entry_child_nodes_cleared_for_replacement);
    insert_json_counter(
        renderer,
        "text_cache_entry_child_nodes_cleared_for_removal",
        cumulative_stats.text_cache_entry_child_nodes_cleared_for_removal);
    insert_json_counter(
        renderer,
        "text_cache_entry_max_child_nodes_cleared",
        cumulative_stats.text_cache_entry_max_child_nodes_cleared);
    insert_json_counter(renderer, "route_fast_text_cells", cumulative_stats.route_fast_text_cells);
    insert_json_counter(
        renderer,
        "route_qt_text_layout_runs",
        cumulative_stats.route_qt_text_layout_runs);
    insert_json_counter(renderer, "route_fallback_cells", cumulative_stats.route_fallback_cells);
    insert_text_layout_stats_json(renderer, cumulative_stats);
    if constexpr (requires { cumulative_stats.text_resource_descriptor_builds; }) {
        insert_json_counter(
            renderer,
            "text_resource_descriptor_builds",
            cumulative_stats.text_resource_descriptor_builds);
    }
    if constexpr (requires { cumulative_stats.text_resource_descriptor_builds_avoided; }) {
        insert_json_counter(
            renderer,
            "text_resource_descriptor_builds_avoided",
            cumulative_stats.text_resource_descriptor_builds_avoided);
    }
    insert_json_counter(renderer, "qsg_nodes_created", cumulative_stats.qsg_nodes_created);
    insert_json_counter(renderer, "qsg_nodes_replaced", cumulative_stats.qsg_nodes_replaced);
    insert_json_counter(renderer, "qsg_nodes_destroyed", cumulative_stats.qsg_nodes_destroyed);
    insert_json_counter(
        renderer,
        "background_qsg_nodes_created",
        cumulative_stats.background_qsg_nodes_created);
    insert_json_counter(
        renderer,
        "background_qsg_nodes_replaced",
        cumulative_stats.background_qsg_nodes_replaced);
    insert_json_counter(
        renderer,
        "background_qsg_nodes_destroyed",
        cumulative_stats.background_qsg_nodes_destroyed);
    insert_json_counter(
        renderer,
        "text_groups_considered",
        cumulative_stats.text_groups_considered);
    insert_json_counter(renderer, "text_groups_dirty", cumulative_stats.text_groups_dirty);
    insert_json_counter(renderer, "text_groups_clean", cumulative_stats.text_groups_clean);
    insert_json_counter(
        renderer,
        "text_clean_reuse_skips",
        cumulative_stats.text_clean_reuse_skips);
    insert_json_counter(
        renderer,
        "text_resource_descriptor_reuses",
        cumulative_stats.text_resource_descriptor_reuses);
    insert_json_counter(renderer, "text_key_match_reuses", cumulative_stats.text_key_match_reuses);
    insert_json_counter(renderer, "text_key_builds", cumulative_stats.text_key_builds);
    insert_json_counter(renderer, "text_key_bytes", cumulative_stats.text_key_bytes);
    insert_json_counter(renderer, "rect_key_builds", cumulative_stats.rect_key_builds);
    insert_json_counter(renderer, "rect_key_bytes", cumulative_stats.rect_key_bytes);
    insert_json_counter(renderer, "cache_key_builds", cumulative_stats.cache_key_builds);
    insert_json_counter(renderer, "cache_key_bytes", cumulative_stats.cache_key_bytes);
    insert_json_counter(
        renderer,
        "text_dirty_row_ranges",
        cumulative_stats.text_dirty_row_ranges);
    insert_json_counter(renderer, "text_dirty_rows", cumulative_stats.text_dirty_rows);
    renderer.insert(
        QString::fromLatin1(
            renderer_text_resource_dirty_row_metric_name(cumulative_stats)),
        QString::number(
            static_cast<qulonglong>(
                renderer_text_resource_dirty_row_metric_value(cumulative_stats))));
    insert_json_counter(
        renderer,
        "text_runs_considered",
        cumulative_stats.text_runs_considered);
    insert_json_counter(
        renderer,
        "text_coalescing_candidate_groups",
        cumulative_stats.text_coalescing_candidate_groups);
    insert_json_counter(
        renderer,
        "text_coalescing_enabled_groups",
        cumulative_stats.text_coalescing_enabled_groups);
    insert_json_counter(
        renderer,
        "text_resource_rows_with_runs",
        cumulative_stats.text_resource_rows_with_runs);
    insert_json_counter(
        renderer,
        "text_resource_max_runs_after_coalescing_per_row",
        cumulative_stats.text_resource_max_runs_after_coalescing_per_row);
    insert_json_counter(
        renderer,
        "text_resource_runs_before_coalescing",
        cumulative_stats.text_resource_runs_before_coalescing);
    insert_json_counter(
        renderer,
        "text_resource_runs_after_coalescing",
        cumulative_stats.text_resource_runs_after_coalescing);
    insert_json_counter(
        renderer,
        "text_dirty_descriptor_identical_rows",
        cumulative_stats.text_dirty_descriptor_identical_rows);
    insert_json_counter(
        renderer,
        "text_dirty_rows_rebuilt",
        cumulative_stats.text_dirty_rows_rebuilt);
    insert_json_counter(
        renderer,
        "text_clean_rows_rebuilt",
        cumulative_stats.text_clean_rows_rebuilt);
    insert_json_counter(
        renderer,
        "rect_resource_rects_before_coalescing",
        cumulative_stats.rect_resource_rects_before_coalescing);
    insert_json_counter(
        renderer,
        "rect_resource_rects_after_coalescing",
        cumulative_stats.rect_resource_rects_after_coalescing);
    insert_json_counter(
        renderer,
        "background_row_rects_before_coalescing",
        cumulative_stats.background_row_rects_before_coalescing);
    insert_json_counter(
        renderer,
        "background_row_rects_after_coalescing",
        cumulative_stats.background_row_rects_after_coalescing);
    insert_json_counter(
        renderer,
        "background_batched_rects",
        cumulative_stats.background_batched_rects);
    insert_json_counter(
        renderer,
        "background_batched_vertices",
        cumulative_stats.background_batched_vertices);
    insert_json_counter(
        renderer,
        "selection_batched_rects",
        cumulative_stats.selection_batched_rects);
    insert_json_counter(
        renderer,
        "selection_batched_vertices",
        cumulative_stats.selection_batched_vertices);
    insert_json_counter(
        renderer,
        "graphic_batched_rects",
        cumulative_stats.graphic_batched_rects);
    insert_json_counter(
        renderer,
        "graphic_batched_vertices",
        cumulative_stats.graphic_batched_vertices);
    insert_json_counter(
        renderer,
        "decoration_batched_rects",
        cumulative_stats.decoration_batched_rects);
    insert_json_counter(
        renderer,
        "decoration_batched_vertices",
        cumulative_stats.decoration_batched_vertices);
    insert_json_counter(
        renderer,
        "text_cache_entries_created",
        cumulative_stats.text_cache_entries_created);
    insert_json_counter(
        renderer,
        "text_cache_entries_replaced",
        cumulative_stats.text_cache_entries_replaced);
    insert_json_counter(
        renderer,
        "text_cache_entries_removed",
        cumulative_stats.text_cache_entries_removed);
    insert_json_counter(
        renderer,
        "text_wrapper_order_rebuilds",
        cumulative_stats.text_wrapper_order_rebuilds);
    insert_json_counter(
        renderer,
        "background_layer_rebuilds",
        cumulative_stats.background_layer_rebuilds);
    insert_json_counter(
        renderer,
        "selection_layer_rebuilds",
        cumulative_stats.selection_layer_rebuilds);
    insert_json_counter(
        renderer,
        "graphic_layer_rebuilds",
        cumulative_stats.graphic_layer_rebuilds);
    insert_json_counter(
        renderer,
        "decoration_layer_rebuilds",
        cumulative_stats.decoration_layer_rebuilds);
    insert_json_counter(
        renderer,
        "cursor_layer_rebuilds",
        cumulative_stats.cursor_layer_rebuilds);
    insert_json_counter(
        renderer,
        "cursor_text_layer_rebuilds",
        cumulative_stats.cursor_text_layer_rebuilds);
    insert_json_counter(renderer, "overlay_layer_rebuilds", cumulative_stats.overlay_layer_rebuilds);
    insert_json_counter(renderer, "row_cache_hits", cumulative_stats.row_cache_hits);
    insert_json_counter(
        renderer,
        "row_cache_clean_skips",
        cumulative_stats.row_cache_clean_skips);
    insert_json_counter(
        renderer,
        "background_rows_rebuilt",
        cumulative_stats.background_rows_rebuilt);
    insert_json_counter(
        renderer,
        "background_rows_reused",
        cumulative_stats.background_rows_reused);
    insert_json_counter(
        renderer,
        "background_row_clean_reuse_skips",
        cumulative_stats.background_row_clean_reuse_skips);
    insert_json_counter(
        renderer,
        "background_rows_removed",
        cumulative_stats.background_rows_removed);
    insert_json_counter(
        renderer,
        "background_row_cache_fallbacks",
        cumulative_stats.background_row_cache_fallbacks);
    insert_json_counter(
        renderer,
        "selection_rows_rebuilt",
        cumulative_stats.selection_rows_rebuilt);
    insert_json_counter(
        renderer,
        "selection_rows_reused",
        cumulative_stats.selection_rows_reused);
    insert_json_counter(
        renderer,
        "selection_row_clean_reuse_skips",
        cumulative_stats.selection_row_clean_reuse_skips);
    insert_json_counter(
        renderer,
        "selection_rows_removed",
        cumulative_stats.selection_rows_removed);
    insert_json_counter(
        renderer,
        "selection_row_cache_fallbacks",
        cumulative_stats.selection_row_cache_fallbacks);
    insert_json_counter(
        renderer,
        "decoration_rows_rebuilt",
        cumulative_stats.decoration_rows_rebuilt);
    insert_json_counter(
        renderer,
        "decoration_rows_reused",
        cumulative_stats.decoration_rows_reused);
    insert_json_counter(
        renderer,
        "decoration_row_clean_reuse_skips",
        cumulative_stats.decoration_row_clean_reuse_skips);
    insert_json_counter(
        renderer,
        "decoration_rows_removed",
        cumulative_stats.decoration_rows_removed);
    insert_json_counter(
        renderer,
        "decoration_row_cache_fallbacks",
        cumulative_stats.decoration_row_cache_fallbacks);
    insert_json_counter(
        renderer,
        "graphic_rect_rows_rebuilt",
        cumulative_stats.graphic_rect_rows_rebuilt);
    insert_json_counter(
        renderer,
        "graphic_rect_rows_reused",
        cumulative_stats.graphic_rect_rows_reused);
    insert_json_counter(
        renderer,
        "graphic_rect_row_clean_reuse_skips",
        cumulative_stats.graphic_rect_row_clean_reuse_skips);
    insert_json_counter(
        renderer,
        "graphic_rect_rows_removed",
        cumulative_stats.graphic_rect_rows_removed);
    insert_json_counter(
        renderer,
        "graphic_rect_row_cache_fallbacks",
        cumulative_stats.graphic_rect_row_cache_fallbacks);
    insert_json_counter(
        renderer,
        "graphic_arc_rows_rebuilt",
        cumulative_stats.graphic_arc_rows_rebuilt);
    insert_json_counter(
        renderer,
        "graphic_arc_rows_reused",
        cumulative_stats.graphic_arc_rows_reused);
    insert_json_counter(
        renderer,
        "graphic_arc_row_clean_reuse_skips",
        cumulative_stats.graphic_arc_row_clean_reuse_skips);
    insert_json_counter(
        renderer,
        "graphic_arc_rows_removed",
        cumulative_stats.graphic_arc_rows_removed);
    insert_json_counter(
        renderer,
        "graphic_arc_row_cache_fallbacks",
        cumulative_stats.graphic_arc_row_cache_fallbacks);
    renderer.insert(QStringLiteral("frame"), frame);

    QJsonObject root;
    root.insert(QStringLiteral("schema"), QStringLiteral("vnm_terminal_runtime_metrics_v2"));
    root.insert(QStringLiteral("elapsed_ms"), timing.app_elapsed_ms);
    root.insert(QStringLiteral("app_result"), app_result);
    root.insert(QStringLiteral("process_exit_code"), state.process_exit_code);
    root.insert(QStringLiteral("process_exit_reason"), enum_key(state.process_exit_reason));
    root.insert(QStringLiteral("backend_error_count"), state.backend_error_count);
    root.insert(QStringLiteral("output_seen"), state.output_seen);
    root.insert(QStringLiteral("process_exited"), state.process_exited);
    root.insert(QStringLiteral("timeout_expired"), state.timeout_expired);
    root.insert(
        QStringLiteral("paint_frames_per_second"),
        frames_per_second(cumulative_stats.paint_completed_frames, timing.app_elapsed_ms));
    root.insert(
        QStringLiteral("paint_frames_per_second_elapsed_basis"),
        QString::fromLatin1(k_runtime_frame_rate_elapsed_basis));
    root.insert(
        QStringLiteral("renderer_frame_evidence"),
        renderer_frame_evidence_json(frame_evidence, timing.app_elapsed_ms));
    root.insert(
        QStringLiteral("startup"),
        startup_metrics_json(state, frame_evidence));
    root.insert(QStringLiteral("profiling"), profiling_measurement_json(timing));
    root.insert(QStringLiteral("surface_geometry"), surface_geometry_json(surface));
    root.insert(QStringLiteral("renderer"), renderer);
    root.insert(QStringLiteral("qsg_atlas"), qsg_atlas_metrics_json(atlas_report));

    return root;
}

bool write_metrics_json(
    const QString&              path,
    const VNM_TerminalSurface&  surface,
    const Runtime_state&        state,
    const metrics_timing_t&     timing,
    int                         app_result,
    QString*                    out_error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *out_error = QStringLiteral("could not write metrics JSON %1: %2")
            .arg(path, file.errorString());
        return false;
    }

    const QByteArray json = QJsonDocument(
        terminal_metrics_json(
            surface,
            state,
            timing,
            app_result))
            .toJson(QJsonDocument::Indented);
    if (file.write(json) != json.size()) {
        *out_error = QStringLiteral("could not write metrics JSON %1: %2")
            .arg(path, file.errorString());
        return false;
    }

    return true;
}

}

#ifndef VNM_TERMINAL_APP_NO_MAIN
int main(int argc, char** argv)
{
    const QStringList arguments = raw_arguments(argc, argv);
    request_vsync_surface_format();

    Qt_arguments qt_arguments = make_qt_arguments(argc, argv);
    QGuiApplication app(qt_arguments.argc, qt_arguments.argv.data());
    QCoreApplication::setOrganizationName(QStringLiteral("Varinomics"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("varinomics.com"));
    QCoreApplication::setApplicationName(QStringLiteral("vnm_terminal"));
    QCoreApplication::setApplicationVersion(QStringLiteral(VNM_TERMINAL_VERSION_STRING));
    const QIcon app_icon(
        QStringLiteral(
            ":/vnm_terminal/vnm_terminal.ico"));
    QGuiApplication::setWindowIcon(app_icon);

    Parse_result parse_result = parse_arguments(arguments);
    if (parse_result.help_requested) {
        print_usage();
        return 0;
    }

    if (!parse_result.error.isEmpty()) {
        print_error(parse_result.error);
        print_usage();
        return k_exit_usage_error;
    }

    App_options options = std::move(parse_result.options);
    if (!validate_capture_paths(&options, &parse_result.error)) {
        print_error(parse_result.error);
        return k_exit_usage_error;
    }

    if (!options.backend_output_capture_path.isEmpty() &&
        !prepare_capture_file(
            QStringLiteral("--capture-output"),
            options.backend_output_capture_path, &parse_result.error))
    {
        print_error(parse_result.error);
        return k_exit_usage_error;
    }
    if (!options.transcript_capture_path.isEmpty() &&
        !prepare_capture_file(
            QStringLiteral("--capture-transcript"),
            options.transcript_capture_path, &parse_result.error))
    {
        print_error(parse_result.error);
        return k_exit_usage_error;
    }
    if (!options.metrics_json_path.isEmpty() &&
        !prepare_capture_file(
            QStringLiteral("--metrics-json"),
            options.metrics_json_path, &parse_result.error))
    {
        print_error(parse_result.error);
        return k_exit_usage_error;
    }
#if VNM_TERMINAL_PROFILING_ENABLED
    if (!options.profile_text_path.isEmpty() &&
        !prepare_profile_text_file(options.profile_text_path, &parse_result.error))
    {
        print_error(parse_result.error);
        return k_exit_usage_error;
    }
#endif

    const bool persistence_enabled = terminal_window_persistence_enabled();
    if (persistence_enabled) {
        QSettings settings;
        apply_persisted_terminal_window_state(
            load_persisted_terminal_window_state(settings),
            &options);
    }

    QQmlEngine chrome_engine;

    QQuickWindow window;
    window.setTitle(default_window_title());
    window.setIcon(app_icon);
    window.setColor(options.custom_titlebar
        ? chrome::terminal_chrome_background_color(window.isActive())
        : QColor(9, 12, 16));
    window.resize(options.window_size);
    if (options.window_position.has_value()) {
        window.setPosition(*options.window_position);
    }
    if (options.custom_titlebar) {
        window.setFlags(window.flags() | Qt::FramelessWindowHint);
    }

    std::unique_ptr<chrome::Terminal_qml_chrome> titlebar;
    if (options.custom_titlebar) {
        titlebar = std::make_unique<chrome::Terminal_qml_chrome>(chrome_engine, window);
        if (!titlebar->is_valid()) {
            print_error(QStringLiteral("failed to create shared window chrome: %1")
                .arg(titlebar->error_string()));
            return k_exit_start_failed;
        }
    }
    auto* titlebar_ptr = titlebar.get();

    auto* surface = new VNM_TerminalSurface(window.contentItem());
    term::VNM_TerminalSurface_render_bridge::set_selection_trace_enabled(
        *surface,
        options.selection_trace_enabled);
#if VNM_TERMINAL_PROFILING_ENABLED
    std::unique_ptr<term::Hierarchical_profiler> gui_profiler;
    std::unique_ptr<term::Active_profiler_binding> gui_profiler_binding;
    std::shared_ptr<term::Hierarchical_profiler> render_profiler;
    if (!options.profile_text_path.isEmpty()) {
        gui_profiler = std::make_unique<term::Hierarchical_profiler>();
        gui_profiler_binding =
            std::make_unique<term::Active_profiler_binding>(gui_profiler.get());
        render_profiler = std::make_shared<term::Hierarchical_profiler>();
        term::VNM_TerminalSurface_render_bridge::set_render_profiler(
            *surface,
            render_profiler);
        term::VNM_TerminalSurface_render_bridge::set_dirty_row_stats_enabled(
            *surface,
            true);
    }
#endif
    auto* scrollbar = new chrome::Terminal_scrollbar(window.contentItem());
    scrollbar->set_surface(surface);
    scrollbar->set_wheel_trace_enabled(options.wheel_trace_enabled);
    surface->set_font_family(options.font_family);
    surface->set_font_size(options.font_size);
    surface->set_color_theme(options.theme);
    surface->set_wheel_event_policy(
        VNM_TerminalSurface::Wheel_event_policy::LOCAL_SCROLLBACK_FIRST);
    apply_scrollback_limit_option(*surface, options);
#if defined(Q_OS_MACOS)
    surface->set_copy_shortcut_policy(VNM_TerminalSurface::Copy_shortcut_policy::TERMINAL_INPUT);
#endif
    surface->set_alternate_screen_wheel_policy(options.alternate_screen_wheel_policy);
    apply_synchronized_output_scroll_policy_option(*surface, options);
    apply_primary_repaint_recovery_option(*surface, options);
    surface->set_backend_output_capture_path(options.backend_output_capture_path);
    surface->set_transcript_capture_path(options.transcript_capture_path);
    surface->set_transcript_snapshot_diagnostics(options.transcript_snapshot_diagnostics);
    surface->set_transcript_timing_diagnostics(options.transcript_timing_diagnostics);
    surface->set_wheel_trace_enabled(options.wheel_trace_enabled);
    install_wheel_delivery_indicator_filter(
        *surface,
        *scrollbar,
        titlebar_ptr,
        options.wheel_trace_enabled);
    const bool custom_titlebar_enabled = options.custom_titlebar;
    std::optional<Persisted_terminal_window_state> latest_restorable_window_state =
        restorable_terminal_window_state(window, *surface);
    const auto remember_restorable_window_state =
        [&window, surface, &latest_restorable_window_state] {
            const std::optional<Persisted_terminal_window_state> state =
                restorable_terminal_window_state(window, *surface);
            if (state.has_value()) {
                latest_restorable_window_state = *state;
            }
        };

    apply_terminal_shell_geometry(
        window,
        *surface,
        *scrollbar,
        titlebar_ptr,
        custom_titlebar_enabled);
    window.installEventFilter(new Terminal_shortcut_filter(surface));

    connect_terminal_metadata_to_chrome(*surface, window, titlebar_ptr);
    QObject::connect(
        surface,
        &VNM_TerminalSurface::clipboard_write_requested,
        surface,
        [surface](
            quint64 request_id,
            const QString& target_selection,
            const QByteArray& payload)
        {
            // Respond during the signal delivery so the single pending host
            // request slot in VNM_TerminalSurface cannot be superseded.
            Q_UNUSED(payload);
            handle_clipboard_write_request(
                *surface,
                request_id,
                target_selection);
        });
    QObject::connect(
        surface,
        &VNM_TerminalSurface::text_area_resize_requested,
        &window,
        [&window, surface](int rows, int columns) {
            (void)resize_window_for_text_area_request(
                window,
                *surface,
                rows,
                columns);
        });

    QObject::connect(
        &window,
        &QQuickWindow::widthChanged,
        surface,
        [
            titlebar_ptr,
            &window,
            surface,
            scrollbar,
            custom_titlebar_enabled,
            remember_restorable_window_state
        ] {
            apply_terminal_shell_geometry(
                window,
                *surface,
                *scrollbar,
                titlebar_ptr,
                custom_titlebar_enabled);
            remember_restorable_window_state();
        });
    QObject::connect(
        &window,
        &QQuickWindow::heightChanged,
        surface,
        [
            titlebar_ptr,
            &window,
            surface,
            scrollbar,
            custom_titlebar_enabled,
            remember_restorable_window_state
        ] {
            apply_terminal_shell_geometry(
                window,
                *surface,
                *scrollbar,
                titlebar_ptr,
                custom_titlebar_enabled);
            remember_restorable_window_state();
        });
    QObject::connect(
        &window,
        &QWindow::xChanged,
        surface,
        [remember_restorable_window_state](int) {
            remember_restorable_window_state();
        });
    QObject::connect(
        &window,
        &QWindow::yChanged,
        surface,
        [remember_restorable_window_state](int) {
            remember_restorable_window_state();
        });

    if (titlebar_ptr != nullptr) {
        auto sync_titlebar_state = [titlebar_ptr, &window] {
            sync_chrome_window_state(*titlebar_ptr, window);
        };
        auto sync_titlebar_state_and_geometry =
            [
                titlebar_ptr,
                &window,
                surface,
                scrollbar,
                custom_titlebar_enabled,
                remember_restorable_window_state
            ] {
                sync_chrome_window_state(*titlebar_ptr, window);
                apply_terminal_shell_geometry(
                    window,
                    *surface,
                    *scrollbar,
                    titlebar_ptr,
                    custom_titlebar_enabled);
                remember_restorable_window_state();
            };
        QObject::connect(
            &window,
            &QWindow::activeChanged,
            titlebar_ptr,
            [titlebar_ptr, &window] {
                sync_chrome_window_state(*titlebar_ptr, window);
            });
        QObject::connect(
            &window,
            &QWindow::windowStateChanged,
            titlebar_ptr,
            [sync_titlebar_state_and_geometry](Qt::WindowState) {
                sync_titlebar_state_and_geometry();
            });
        sync_titlebar_state();
    }
    else {
        QObject::connect(
            &window,
            &QWindow::windowStateChanged,
            surface,
            [remember_restorable_window_state](Qt::WindowState) {
                remember_restorable_window_state();
            });
    }

    QObject::connect(
        &app,
        &QCoreApplication::aboutToQuit,
        surface,
        [
            persistence_enabled,
            surface,
            &window,
            &latest_restorable_window_state
        ] {
            if (!persistence_enabled) {
                return;
            }

            const std::optional<Persisted_terminal_window_state> current_state =
                restorable_terminal_window_state(window, *surface);
            Persisted_terminal_window_state state =
                current_state.value_or(
                    latest_restorable_window_state.value_or(
                        Persisted_terminal_window_state{}));
            state.font_size = surface->font_size();
            state.maximized = window.windowStates().testFlag(Qt::WindowMaximized);

            QSettings settings;
            save_persisted_terminal_window_state(settings, state);
        });

    Runtime_state state;
    QElapsedTimer startup_elapsed_timer;
    startup_elapsed_timer.start();

    QObject::connect(
        surface,
        &VNM_TerminalSurface::backend_error,
        surface,
        [&state](VNM_TerminalSurface::Backend_error_code code, const QString& message) {
            ++state.backend_error_count;
            print_error(QStringLiteral("backend error [%1]: %2")
                .arg(enum_key(code), message));
        });
    QObject::connect(
        surface,
        &VNM_TerminalSurface::output_activity,
        surface,
        [&state, &startup_elapsed_timer] {
            if (!state.output_seen) {
                state.first_output_elapsed_ms = startup_elapsed_timer.elapsed();
            }
            state.output_seen = true;
        });
    QObject::connect(
        surface,
        &VNM_TerminalSurface::process_exited,
        surface,
        [&options, &state, &window](
            VNM_TerminalSurface::Exit_reason reason,
            int exit_code)
        {
            state.process_exited      = true;
            state.process_exit_reason = reason;
            state.process_exit_code   = exit_code;

            if (!options.keep_open_after_process_exits) {
                QCoreApplication::exit(
                    state.timeout_expired
                        ? k_exit_timeout
                        : app_status_after_process_exit(options, state));
            }
        });

    QTimer timeout_timer(&app);
    QTimer timeout_force_exit_timer(&app);
    timeout_timer.setSingleShot(true);
    timeout_force_exit_timer.setSingleShot(true);
    QObject::connect(
        &timeout_timer,
        &QTimer::timeout,
        &app,
        [&options, &state, surface, &timeout_force_exit_timer] {
            if (state.process_exited) {
                QCoreApplication::exit(app_status_after_process_exit(options, state));
                return;
            }

            state.timeout_expired = true;
            print_error(QStringLiteral("timeout after %1 ms").arg(*options.timeout_ms));
            if (!surface->terminate_process()) {
                QCoreApplication::exit(k_exit_timeout);
                return;
            }

            timeout_force_exit_timer.start(k_timeout_force_exit_grace_ms);
        });
    QObject::connect(
        &timeout_force_exit_timer,
        &QTimer::timeout,
        &app,
        [] {
            QCoreApplication::exit(k_exit_timeout);
        });

    window.show();
    if (options.restore_maximized_window_state) {
        window.setWindowState(Qt::WindowMaximized);
    }
    surface->forceActiveFocus();

    QTimer::singleShot(0, &app, [&options, &state, surface, &timeout_timer] {
        if (!surface->start_process(options.command, options.working_directory)) {
            if (state.backend_error_count == 0) {
                print_error(QStringLiteral("failed to start terminal process"));
            }

            QCoreApplication::exit(k_exit_start_failed);
            return;
        }

        if (options.timeout_ms.has_value()) {
            timeout_timer.start(*options.timeout_ms);
        }
    });

    QElapsedTimer app_elapsed_timer;
    app_elapsed_timer.start();
    int app_result = app.exec();
    metrics_timing_t metrics_timing;
    metrics_timing.app_elapsed_ms         = app_elapsed_timer.elapsed();
    metrics_timing.profile_text_requested = !options.profile_text_path.isEmpty();
#if VNM_TERMINAL_PROFILING_ENABLED
    if (!options.profile_text_path.isEmpty() && gui_profiler != nullptr) {
        QString profile_error;
        QElapsedTimer profile_write_timer;
        profile_write_timer.start();
        if (!write_profile_text(
                options.profile_text_path, *surface, *gui_profiler, &profile_error))
        {
            print_error(profile_error);
            if (app_result == 0) {
                app_result = k_exit_usage_error;
            }
        }
        metrics_timing.profile_write_elapsed_ms = profile_write_timer.elapsed();
    }
#endif

    if (!options.metrics_json_path.isEmpty()) {
        QString metrics_error;
        if (!write_metrics_json(
                options.metrics_json_path,
                *surface,
                state,
                metrics_timing,
                app_result,
                &metrics_error))
        {
            print_error(metrics_error);
            if (app_result == 0) {
                app_result = k_exit_usage_error;
            }
        }
    }

    if (app_result == 0 && options.require_output && !state.output_seen) {
        print_error(QStringLiteral("required terminal output activity was not observed"));
        return k_exit_no_output;
    }

    return app_result;
}
#endif
