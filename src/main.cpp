#include "frameless_resize_filter.h"
#include "terminal_scrollbar.h"
#include "window_chrome.h"

#include "vnm_terminal/vnm_terminal_surface.h"

#include "vnm_terminal/internal/hierarchical_profiler.h"
#include "vnm_terminal/internal/qt_grid_metrics_provider.h"
#include "vnm_terminal/internal/vnm_terminal_font.h"
#if VNM_TERMINAL_PROFILING_ENABLED
#include "vnm_terminal/internal/vnm_terminal_surface_render_bridge.h"
#endif

#include <QByteArray>
#include <QClipboard>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QIODevice>
#include <QKeyEvent>
#include <QMetaEnum>
#include <QObject>
#include <QPointF>
#include <QQuickWindow>
#include <QRectF>
#include <QSGRendererInterface>
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

namespace {

namespace term   = vnm_terminal::internal;
namespace chrome = vnm_terminal::terminal_app;

constexpr int   k_exit_usage_error             = 2;
constexpr int   k_exit_start_failed            = 3;
constexpr int   k_exit_process_failed          = 4;
constexpr int   k_exit_timeout                 = 5;
constexpr int   k_exit_no_output               = 6;
constexpr int   k_timeout_force_exit_grace_ms  = 5000;
constexpr qreal k_custom_titlebar_height       = 32.0;
constexpr qreal k_terminal_scrollbar_width     = 12.0;
constexpr int   k_text_area_resize_max_rows    = 512;
constexpr int   k_text_area_resize_max_columns = 512;

constexpr qreal k_text_area_resize_max_window_axis = 8192.0;

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
    QString            profile_text_path;
    QString            font_family = term::vnm_terminal_default_monospace_font_family();
    qreal              font_size   = term::k_vnm_terminal_default_font_pixel_size;
    QString            theme       = QStringLiteral("default");
    QSize              window_size = QSize(900, 600);
    VNM_TerminalSurface::Alternate_screen_wheel_policy alternate_screen_wheel_policy =
        VNM_TerminalSurface::Alternate_screen_wheel_policy::MOUSE_REPORTING_FIRST;
    std::optional<int> timeout_ms;
    bool               shell_requested               = false;
    bool               keep_open_after_process_exits = false;
    bool               require_output                = false;
    bool               custom_titlebar               = k_custom_titlebar_default_enabled;
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
    VNM_TerminalSurface::Exit_reason process_exit_reason =
        VNM_TerminalSurface::Exit_reason::EXITED;
    bool               output_seen     = false;
    bool               process_exited  = false;
    bool               timeout_expired = false;
};

struct Terminal_shell_geometry
{
    QRectF             chrome_rect;
    QRectF             terminal_rect;
    QRectF             scrollbar_rect;
};

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
        const bool paste_shortcut =
            key_event->key() == Qt::Key_V &&
            (modifiers == Qt::ControlModifier ||
                modifiers == (Qt::ControlModifier | Qt::ShiftModifier));
        if (!paste_shortcut) {
            return false;
        }

        if (!m_surface->hasActiveFocus()) {
            return false;
        }

        return paste_clipboard_text();
    }

private:
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

Terminal_shell_geometry terminal_shell_geometry(
    const QSizeF&  window_size,
    bool           custom_titlebar,
    bool           resize_border_active = true)
{
    const qreal width  = std::max<qreal>(0.0, window_size.width());
    const qreal height = std::max<qreal>(0.0, window_size.height());

    const auto split_terminal_area = [](const QRectF& area, Terminal_shell_geometry* geometry) {
        const qreal scrollbar_width = std::min(k_terminal_scrollbar_width, area.width());
        geometry->terminal_rect  = QRectF(
            area.left(),
            area.top(),
            std::max<qreal>(0.0, area.width() - scrollbar_width),
            area.height());
        geometry->scrollbar_rect = QRectF(
            area.right() - scrollbar_width,
            area.top(),
            scrollbar_width,
            area.height());
    };

    Terminal_shell_geometry geometry;
    if (!custom_titlebar) {
        split_terminal_area(QRectF(0.0, 0.0, width, height), &geometry);
        return geometry;
    }

    const qreal border = resize_border_active
        ? chrome::k_default_frameless_resize_border_width
        : 0.0;
    const qreal titlebar_height  = std::min(k_custom_titlebar_height, height);
    const qreal horizontal_inset = std::min(border, width / 2.0);
    const qreal terminal_width_available =
        std::max<qreal>(0.0, width - horizontal_inset * 2.0);
    const qreal terminal_height_available = std::max<qreal>(0.0, height - titlebar_height);
    const qreal bottom_inset              = std::min(border, terminal_height_available);

    geometry.chrome_rect = QRectF(0.0, 0.0, width, titlebar_height);
    split_terminal_area(
        QRectF(
            horizontal_inset,
            titlebar_height,
            terminal_width_available,
            std::max<qreal>(0.0, terminal_height_available - bottom_inset)),
        &geometry);
    return geometry;
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
    chrome::Terminal_window_chrome* titlebar,
    bool                           custom_titlebar)
{
    const Terminal_shell_geometry geometry = terminal_shell_geometry(
        QSizeF(window.width(), window.height()),
        custom_titlebar,
        custom_titlebar_resize_border_active(window));

    if (titlebar != nullptr) {
        titlebar->setPosition(geometry.chrome_rect.topLeft());
        titlebar->setSize(geometry.chrome_rect.size());
    }

    surface.setPosition(geometry.terminal_rect.topLeft());
    surface.setSize(geometry.terminal_rect.size());
    scrollbar.setPosition(geometry.scrollbar_rect.topLeft());
    scrollbar.setSize(geometry.scrollbar_rect.size());
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

std::vector<QRectF> window_chrome_button_rects(
    const chrome::Terminal_window_chrome& titlebar)
{
    std::vector<QRectF> rects;
    const chrome::Window_chrome_layout layout = titlebar.chrome_layout();
    const QPointF offset(titlebar.x(), titlebar.y());
    rects.reserve(layout.buttons.size());
    for (const chrome::Window_chrome_button_geometry& button : layout.buttons) {
        rects.push_back(button.rect.translated(offset));
    }
    return rects;
}

QString visible_terminal_title(QString terminal_title)
{
    terminal_title = terminal_title.trimmed();
    return terminal_title.isEmpty() ? default_window_title() : terminal_title;
}

void sync_terminal_title(
    QQuickWindow&                  window,
    chrome::Terminal_window_chrome* titlebar,
    const QString&                 terminal_title)
{
    const QString visible_title = visible_terminal_title(terminal_title);
    window.setTitle(visible_title);
    if (titlebar != nullptr) {
        titlebar->set_terminal_title(visible_title);
    }
}

void connect_terminal_metadata_to_window_chrome(
    VNM_TerminalSurface&           surface,
    QQuickWindow&                  window,
    chrome::Terminal_window_chrome* titlebar)
{
    QObject::connect(
        &surface,
        &VNM_TerminalSurface::terminal_title_changed,
        &window,
        [titlebar, &window, &surface] {
            sync_terminal_title(window, titlebar, surface.terminal_title());
        });
    sync_terminal_title(window, titlebar, surface.terminal_title());

    if (titlebar != nullptr) {
        QObject::connect(
            &surface,
            &VNM_TerminalSurface::terminal_icon_name_changed,
            titlebar,
            [titlebar, &surface] {
                titlebar->set_terminal_icon_name(surface.terminal_icon_name());
            });
        titlebar->set_terminal_icon_name(surface.terminal_icon_name());
    }
}

void sync_chrome_window_state(
    chrome::Terminal_window_chrome& titlebar,
    QQuickWindow&                  window)
{
    titlebar.set_window_active(window.isActive());
    titlebar.set_window_maximized(
        window.windowStates().testFlag(Qt::WindowMaximized) ||
        window.windowStates().testFlag(Qt::WindowFullScreen));
    window.setColor(chrome::window_chrome_background_color(window.isActive()));
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
        << "  --window-size <width>x<height>  window size in logical pixels\n"
#if defined(_WIN32) || defined(__linux__)
        << "  --native-titlebar               use the platform titlebar instead of built-in chrome\n"
#endif
        << "  --alternate-wheel <mode>        alternate-screen wheel: mouse(default), cursor, or page\n"
        << "  --capture-output <path>         write raw backend output bytes to a file\n"
#if VNM_TERMINAL_PROFILING_ENABLED
        << "  --profile-text <path>           write profile and dirty-row diagnostics\n"
#endif
        << "  --software-renderer             use the Qt software scene graph\n"
        << "  --keep-open-after-process-exits leave the window open after the child exits\n"
        << "  --timeout-ms <n>                fail if the run is still active after n ms\n"
        << "  --require-output                fail if no terminal output activity is observed\n"
        << "  --help                          show this help\n"
        << "\n"
        << "interactions:\n"
        << "  mouse-reporting apps receive unmodified mouse drags; Shift-drag selects locally\n"
        << "  Ctrl+C copies selected text, otherwise sends Ctrl+C; "
        << "Ctrl+V/Ctrl+Shift+V paste clipboard text\n"
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
#elif defined(__linux__)
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

bool prepare_backend_output_capture_file(
    const QString& path,
    QString*       out_error)
{
    if (path.trimmed().isEmpty()) {
        *out_error = QStringLiteral("--capture-output requires a non-empty path");
        return false;
    }

    const QFileInfo file_info(path);
    const QDir parent_dir = file_info.absoluteDir();
    if (!parent_dir.exists()) {
        *out_error = QStringLiteral("--capture-output parent directory does not exist: %1")
            .arg(parent_dir.absolutePath());
        return false;
    }

    QFile file(file_info.absoluteFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *out_error = QStringLiteral("--capture-output could not open %1: %2")
            .arg(file_info.absoluteFilePath(), file.errorString());
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

        if (argument_is(argument, "--software-renderer")) {
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

bool has_software_renderer_argument(int argc, char** argv)
{
    for (int index = 1; index < argc; ++index) {
        const QByteArray argument(argv[index]);
        if (argument == "--") {
            return false;
        }

        if (argument == "--software-renderer") {
            return true;
        }
    }

    return false;
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

template<typename Frame_stats>
void append_renderer_frame_stats_text(
    QTextStream&       stream,
    const Frame_stats& stats)
{
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
        "frame_text_cells_rendered_as_graphic",
        static_cast<std::uint64_t>(stats.text_cells_rendered_as_graphic));
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
        "frame_cursor_graphic_rects_emitted",
        static_cast<std::uint64_t>(stats.cursor_graphic_rects_emitted));
    append_profile_counter(
        stream,
        "frame_cursor_graphic_arcs_emitted",
        static_cast<std::uint64_t>(stats.cursor_graphic_arcs_emitted));
    append_profile_counter(
        stream,
        "frame_overlay_rects_emitted",
        static_cast<std::uint64_t>(stats.overlay_rects_emitted));
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

void append_renderer_stats_text(
    QTextStream&                           stream,
    const term::terminal_renderer_stats_t& stats)
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
        "text_leaf_nodes_created",
        static_cast<std::uint64_t>(stats.text_leaf_nodes_created));
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
        "text_resource_runs_before_coalescing",
        static_cast<std::uint64_t>(stats.text_resource_runs_before_coalescing));
    append_profile_counter(
        stream,
        "text_resource_runs_after_coalescing",
        static_cast<std::uint64_t>(stats.text_resource_runs_after_coalescing));
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
        << "  cursor_graphic_layer_rebuilt="
        << (stats.cursor_graphic_layer_rebuilt ? "true" : "false") << '\n';
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

void append_cumulative_renderer_stats_text(
    QTextStream&           stream,
    const term::terminal_renderer_cumulative_stats_t&
                           stats)
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
    append_profile_counter(stream, "text_leaf_nodes_created", stats.text_leaf_nodes_created);
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
    append_profile_counter(stream, "text_groups_considered", stats.text_groups_considered);
    append_profile_counter(stream, "text_groups_dirty",      stats.text_groups_dirty);
    append_profile_counter(stream, "text_groups_clean",      stats.text_groups_clean);
    append_profile_counter(stream, "text_clean_reuse_skips", stats.text_clean_reuse_skips);
    append_profile_counter(
        stream,
        "text_resource_descriptor_reuses",
        stats.text_resource_descriptor_reuses);
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
        "text_resource_runs_before_coalescing",
        stats.text_resource_runs_before_coalescing);
    append_profile_counter(
        stream,
        "text_resource_runs_after_coalescing",
        stats.text_resource_runs_after_coalescing);
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
    append_profile_counter(
        stream,
        "cursor_graphic_layer_rebuilds",
        stats.cursor_graphic_layer_rebuilds);
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
    const term::terminal_renderer_stats_t renderer_stats = term::VNM_TerminalSurface_render_bridge::last_renderer_stats(
        surface);
    const term::terminal_renderer_cumulative_stats_t cumulative_renderer_stats =
        term::VNM_TerminalSurface_render_bridge::cumulative_renderer_stats(surface);
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
    append_renderer_stats_text(stream, renderer_stats);
    stream << '\n';
    append_cumulative_renderer_stats_text(stream, cumulative_renderer_stats);
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

}

#ifndef VNM_TERMINAL_APP_NO_MAIN
int main(int argc, char** argv)
{
    const QStringList arguments = raw_arguments(argc, argv);
    request_vsync_surface_format();
    if (has_software_renderer_argument(argc, argv)) {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    }

    Qt_arguments qt_arguments = make_qt_arguments(argc, argv);
    QGuiApplication app(qt_arguments.argc, qt_arguments.argv.data());
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
    if (!options.backend_output_capture_path.isEmpty() &&
        !prepare_backend_output_capture_file(
            options.backend_output_capture_path, &parse_result.error))
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

    QQuickWindow window;
    window.setTitle(default_window_title());
    window.setIcon(app_icon);
    window.setColor(options.custom_titlebar
        ? chrome::window_chrome_background_color(window.isActive())
        : QColor(9, 12, 16));
    window.resize(options.window_size);
    if (options.custom_titlebar) {
        window.setFlags(window.flags() | Qt::FramelessWindowHint);
    }

    auto* titlebar = options.custom_titlebar
        ? new chrome::Terminal_window_chrome(window.contentItem())
        : nullptr;
    auto* surface = new VNM_TerminalSurface(window.contentItem());
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
    surface->set_font_family(options.font_family);
    surface->set_font_size(options.font_size);
    surface->set_color_theme(options.theme);
    surface->set_alternate_screen_wheel_policy(options.alternate_screen_wheel_policy);
    surface->set_backend_output_capture_path(options.backend_output_capture_path);
    const bool custom_titlebar_enabled = options.custom_titlebar;

    apply_terminal_shell_geometry(
        window,
        *surface,
        *scrollbar,
        titlebar,
        custom_titlebar_enabled);
    window.installEventFilter(new Terminal_shortcut_filter(surface));
    if (titlebar != nullptr) {
        auto* resize_filter = new chrome::Frameless_resize_filter(&window, &window);
        resize_filter->set_button_exclusion_rects_provider([titlebar] {
            return window_chrome_button_rects(*titlebar);
        });
        window.installEventFilter(resize_filter);
    }

    connect_terminal_metadata_to_window_chrome(*surface, window, titlebar);
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
        [titlebar, &window, surface, scrollbar, custom_titlebar_enabled] {
            apply_terminal_shell_geometry(
                window,
                *surface,
                *scrollbar,
                titlebar,
                custom_titlebar_enabled);
        });
    QObject::connect(
        &window,
        &QQuickWindow::heightChanged,
        surface,
        [titlebar, &window, surface, scrollbar, custom_titlebar_enabled] {
            apply_terminal_shell_geometry(
                window,
                *surface,
                *scrollbar,
                titlebar,
                custom_titlebar_enabled);
        });

    if (titlebar != nullptr) {
        auto sync_titlebar_state = [titlebar, &window] {
            sync_chrome_window_state(*titlebar, window);
        };
        auto sync_titlebar_state_and_geometry =
            [titlebar, &window, surface, scrollbar, custom_titlebar_enabled] {
                sync_chrome_window_state(*titlebar, window);
                apply_terminal_shell_geometry(
                    window,
                    *surface,
                    *scrollbar,
                    titlebar,
                    custom_titlebar_enabled);
            };
        QObject::connect(
            &window,
            &QWindow::activeChanged,
            titlebar,
            sync_titlebar_state);
        QObject::connect(
            &window,
            &QWindow::windowStateChanged,
            titlebar,
            [sync_titlebar_state_and_geometry](Qt::WindowState) {
                sync_titlebar_state_and_geometry();
            });
        sync_titlebar_state();
    }

    Runtime_state state;

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
        [&state] {
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

    int app_result = app.exec();
#if VNM_TERMINAL_PROFILING_ENABLED
    if (!options.profile_text_path.isEmpty() && gui_profiler != nullptr) {
        QString profile_error;
        if (!write_profile_text(
                options.profile_text_path, *surface, *gui_profiler, &profile_error))
        {
            print_error(profile_error);
            if (app_result == 0) {
                app_result = k_exit_usage_error;
            }
        }
    }
#endif

    if (app_result == 0 && options.require_output && !state.output_seen) {
        print_error(QStringLiteral("required terminal output activity was not observed"));
        return k_exit_no_output;
    }

    return app_result;
}
#endif
