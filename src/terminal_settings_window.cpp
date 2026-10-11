#include "vnm_terminal/app_support/terminal_settings_window.h"

#include "qml_component_error_string.h"
#include "terminal_settings_native_window_owner.h"

#include "vnm_terminal/app_support/terminal_settings_controller.h"
#include "vnm_terminal/app_support/terminal_settings_model.h"

#include "vnm_qml_chrome/vnm_qml_chrome_runtime.h"
#include "vnm_qml_chrome/vnm_system_window.h"

#include "vnm_terminal/vnm_terminal_surface.h"

#include <vnm_font_namespace.h>

#include <QPoint>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QRect>
#include <QScreen>
#include <QUrl>
#include <QWindow>
#include <Qt>

#include <utility>

// Settings can be used without constructing the terminal's chrome or search.
extern int qInitResources_qmlcache_vnm_terminal_app_support();

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace settings = vnm_terminal::terminal_app;

#ifdef Q_OS_WIN
namespace vnm_terminal::terminal_app::detail {

void set_terminal_settings_native_window_owner(
    quintptr settings_window_id,
    quintptr owner_window_id)
{
    const HWND settings_window = reinterpret_cast<HWND>(settings_window_id);
    if (settings_window == nullptr) {
        return;
    }

    const HWND owner_window = reinterpret_cast<HWND>(owner_window_id);
    SetWindowLongPtrW(
        settings_window,
        GWLP_HWNDPARENT,
        reinterpret_cast<LONG_PTR>(owner_window));
}

} // namespace vnm_terminal::terminal_app::detail
#endif

namespace {

int clamp_axis(int value, int min_value, int max_value)
{
    if (max_value < min_value) {
        return min_value;
    }

    return qBound(min_value, value, max_value);
}

QScreen* screen_for_anchor(const QWindow* anchor, const QRect& anchor_geometry)
{
    if (anchor != nullptr && anchor->screen() != nullptr) {
        return anchor->screen();
    }

    if (!anchor_geometry.isEmpty()) {
        if (QScreen* screen = QGuiApplication::screenAt(anchor_geometry.center())) {
            return screen;
        }
    }

    return QGuiApplication::primaryScreen();
}

#ifdef Q_OS_WIN
struct Window_title_search
{
    const QString* title = nullptr;
    QRect geometry;
    HWND hwnd = nullptr;
};

struct Native_window_anchor
{
    quintptr id = 0;
    QRect geometry;
};

BOOL CALLBACK find_visible_window_with_title(HWND hwnd, LPARAM user_data)
{
    auto* search = reinterpret_cast<Window_title_search*>(user_data);
    if (search == nullptr || search->title == nullptr || !IsWindowVisible(hwnd)) {
        return TRUE;
    }

    wchar_t title_buffer[256] = {};
    if (GetWindowTextW(hwnd, title_buffer, 256) <= 0) {
        return TRUE;
    }
    if (QString::fromWCharArray(title_buffer) != *search->title) {
        return TRUE;
    }

    RECT rect{};
    if (GetWindowRect(hwnd, &rect) &&
        rect.right > rect.left &&
        rect.bottom > rect.top)
    {
        search->hwnd = hwnd;
        search->geometry = QRect(
            QPoint(rect.left, rect.top),
            QPoint(rect.right - 1, rect.bottom - 1));
        return FALSE;
    }

    return TRUE;
}

bool valid_native_anchor(
    HWND  candidate,
    HWND  settings_window,
    QRect* geometry)
{
    if (candidate == nullptr ||
        candidate == settings_window ||
        !IsWindow(candidate) ||
        !IsWindowVisible(candidate) ||
        GetAncestor(candidate, GA_ROOT) != candidate)
    {
        return false;
    }

    RECT rect{};
    if (!GetWindowRect(candidate, &rect) ||
        rect.right <= rect.left ||
        rect.bottom <= rect.top)
    {
        return false;
    }

    *geometry = QRect(
        QPoint(rect.left, rect.top),
        QPoint(rect.right - 1, rect.bottom - 1));
    return true;
}

Native_window_anchor resolve_native_window_anchor(
    QWindow&       settings_window,
    quintptr       requested_anchor_id,
    const QString& preferred_window_title)
{
    const HWND settings_hwnd = reinterpret_cast<HWND>(settings_window.winId());
    const HWND requested_hwnd = reinterpret_cast<HWND>(requested_anchor_id);
    Native_window_anchor anchor;
    if (valid_native_anchor(
            requested_hwnd,
            settings_hwnd,
            &anchor.geometry))
    {
        anchor.id = requested_anchor_id;
        return anchor;
    }

    if (!preferred_window_title.isEmpty()) {
        Window_title_search search;
        search.title = &preferred_window_title;
        EnumWindows(
            find_visible_window_with_title,
            reinterpret_cast<LPARAM>(&search));
        if (valid_native_anchor(
                search.hwnd,
                settings_hwnd,
                &anchor.geometry))
        {
            anchor.id = reinterpret_cast<quintptr>(search.hwnd);
        }
    }
    return anchor;
}

void show_window_above_anchor(QWindow& window, quintptr anchor_id)
{
    const HWND hwnd = reinterpret_cast<HWND>(window.winId());
    if (hwnd == nullptr) {
        return;
    }

    settings::detail::set_terminal_settings_native_window_owner(
        reinterpret_cast<quintptr>(hwnd),
        anchor_id);

    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetWindowPos(
        hwnd,
        HWND_TOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetWindowPos(
        hwnd,
        HWND_NOTOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    BringWindowToTop(hwnd);
}
#endif

QRect foreground_anchor_geometry()
{
#ifdef Q_OS_WIN
    const HWND foreground_window = GetForegroundWindow();
    RECT rect{};
    if (foreground_window != nullptr &&
        GetWindowRect(foreground_window, &rect) &&
        rect.right > rect.left &&
        rect.bottom > rect.top)
    {
        return QRect(
            QPoint(rect.left, rect.top),
            QPoint(rect.right - 1, rect.bottom - 1));
    }
#endif

    QScreen* screen = QGuiApplication::primaryScreen();
    return screen != nullptr ? screen->availableGeometry() : QRect{};
}

// A self-contained frameless QML Window styled with the shared VNM_Chrome
// titlebar so it visually belongs to the terminal. Its preference controls bind
// to a live surface or settings model; the controller supplies the font list.
//
// The dialog styles every control itself (dialog-local inline `component`
// definitions over QtQuick.Controls.Basic, carrying the S_ prefix) because the
// controls and chrome follow the host application's dark/light selection,
// independently of the terminal's color scheme. The form keeps its natural
// height when the screen can contain it; on a smaller screen its body scrolls below the fixed
// titlebar.
} // namespace

settings::Terminal_settings_window::Terminal_settings_window(
    QQmlEngine&                   engine,
    VNM_TerminalSurface&          surface,
    Terminal_settings_controller& controller,
    bool                          interaction_diagnostics_unlocked,
    QObject*                      parent)
:
    QObject(parent)
{
    initialize(engine, surface, controller, true, interaction_diagnostics_unlocked);
}

settings::Terminal_settings_window::Terminal_settings_window(
    QQmlEngine&                   engine,
    Terminal_settings_model&      model,
    Terminal_settings_controller& controller,
    bool                          interaction_diagnostics_unlocked,
    QObject*                      parent)
:
    QObject(parent)
{
    initialize(engine, model, controller, false, interaction_diagnostics_unlocked);
    QObject::connect(this, &Terminal_settings_window::font_family_selected,
        &model, &Terminal_settings_model::request_font_family);
}

void settings::Terminal_settings_window::initialize(
    QQmlEngine&                   engine,
    QObject&                      preferences,
    Terminal_settings_controller& controller,
    bool                          has_live_surface,
    bool                          interaction_diagnostics_unlocked)
{
    if (!vnm_init_qml_chrome_runtime(engine)) {
        m_error_string = QStringLiteral("failed to initialize vnm_qml_chrome runtime");
        return;
    }
    m_system_window = engine.singletonInstance<vnm_qml_chrome::System_window*>(
        "VNM_Chrome", "VNM_system_window");

    const vnm_fonts::Registered_font icon_font =
        vnm_fonts::register_shipped_font(
            vnm_fonts::Shipped_font::FONT_AWESOME_7_FREE_SOLID);
    if (!icon_font.is_valid()) {
        m_error_string = icon_font.error;
        return;
    }

    auto* context = new QQmlContext(engine.rootContext(), this);
    context->setContextProperty(QStringLiteral("surface"), &preferences);
    context->setContextProperty(QStringLiteral("hasLiveSurface"), has_live_surface);
    context->setContextProperty(QStringLiteral("settings"), &controller);
    context->setContextProperty(
        QStringLiteral("settingsIconFontFamily"),
        icon_font.family);
    context->setContextProperty(
        QStringLiteral("interactionDiagnosticsUnlocked"),
        interaction_diagnostics_unlocked);

    QQmlComponent component(&engine);
    qInitResources_qmlcache_vnm_terminal_app_support();
    component.loadUrl(
        QUrl(QStringLiteral("qrc:/vnm_terminal/terminal_settings_window.qml")));
    if (!component.isReady()) {
        m_error_string = settings::detail::qml_component_error_string(component);
        return;
    }

    m_root_object.reset(component.create(context));
    if (m_root_object == nullptr) {
        m_error_string = settings::detail::qml_component_error_string(component);
        return;
    }

    m_window = qobject_cast<QQuickWindow*>(m_root_object.get());
    if (m_window == nullptr) {
        m_error_string = QStringLiteral("settings window QML root is not a Window");
        m_root_object.reset();
        return;
    }

    // Closing only hides the window. Qt keeps a hidden window's graphics device
    // and swapchain by default, and nothing presents on them while hidden, so a
    // GPU reset would go unnoticed until reopening resizes a lost swapchain.
    // Releasing both on hide makes every reopen start from a fresh device.
    m_window->setPersistentGraphics(false);
    m_window->setPersistentSceneGraph(false);

    QObject::connect(
        m_root_object.get(),
        SIGNAL(font_family_selected(QString)),
        this,
        SIGNAL(font_family_selected(QString)));
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
    QObject::connect(
        m_window,
        &QWindow::screenChanged,
        this,
        [this](QScreen* screen) {
            if (m_window == nullptr || !m_window->isVisible() || screen == nullptr) {
                return;
            }

            watch_available_geometry(screen);
            apply_available_geometry(screen->availableGeometry());
            clamp_to_available_geometry(screen->availableGeometry());
        });

    const auto clamp_resized_window = [this](int) {
        if (m_window != nullptr &&
            m_window->isVisible() &&
            m_available_geometry_screen != nullptr)
        {
            clamp_to_available_geometry(
                m_available_geometry_screen->availableGeometry());
        }
    };
    QObject::connect(m_window, &QWindow::widthChanged, this, clamp_resized_window);
    QObject::connect(m_window, &QWindow::heightChanged, this, clamp_resized_window);
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
    if (m_window == nullptr) {
        return;
    }

    QObject::disconnect(m_transient_parent_screen_connection);
    m_window->setTransientParent(parent);
    if (parent != nullptr) {
        m_transient_parent_screen_connection = QObject::connect(
            parent,
            &QWindow::screenChanged,
            this,
            [this](QScreen*) {
                if (m_window != nullptr && m_window->isVisible()) {
                    m_positioned = false;
                    place_within_anchor();
                }
            });
    }
}

void settings::Terminal_settings_window::set_dark_mode(bool dark_mode)
{
    if (m_window != nullptr) {
        m_window->setProperty("dark_mode", dark_mode);
    }
}

void settings::Terminal_settings_window::set_fallback_anchor_window_title(const QString& title)
{
    m_fallback_anchor_window_title = title;
}

void settings::Terminal_settings_window::set_native_anchor_id_provider(
    Native_anchor_id_provider provider)
{
    m_native_anchor_id_provider = std::move(provider);
}

void settings::Terminal_settings_window::show_window()
{
    if (m_window == nullptr) {
        return;
    }

#ifdef Q_OS_WIN
    const quintptr requested_anchor_id = m_native_anchor_id_provider
        ? m_native_anchor_id_provider()
        : 0;
    const Native_window_anchor native_anchor = resolve_native_window_anchor(
        *m_window,
        requested_anchor_id,
        m_fallback_anchor_window_title);
    place_within_anchor(native_anchor.geometry);
#else
    place_within_anchor();
#endif

    if (!m_window->isVisible()) {
        m_window->show();
    }

#ifdef Q_OS_WIN
    show_window_above_anchor(*m_window, native_anchor.id);
#endif
    m_window->raise();
    m_window->requestActivate();
}

void settings::Terminal_settings_window::apply_available_geometry(
    const QRect& available_geometry)
{
    if (m_window == nullptr || available_geometry.isEmpty()) {
        return;
    }

    m_window->setProperty(
        "available_width_limit",
        available_geometry.width());
    m_window->setProperty(
        "available_height_limit",
        available_geometry.height());

    const int bounded_width = qBound(
        1,
        m_window->width(),
        available_geometry.width());
    const int bounded_height = qBound(
        1,
        m_window->height(),
        available_geometry.height());
    if (bounded_width != m_window->width() || bounded_height != m_window->height()) {
        m_window->resize(bounded_width, bounded_height);
    }
}

void settings::Terminal_settings_window::clamp_to_available_geometry(
    const QRect& available_geometry)
{
    if (m_window == nullptr || available_geometry.isEmpty()) {
        return;
    }

    const int width = qMin(m_window->width(), available_geometry.width());
    const int height = qMin(m_window->height(), available_geometry.height());
    QPoint top_left = m_window->position();
    top_left.setX(clamp_axis(
        top_left.x(),
        available_geometry.left(),
        available_geometry.left() + available_geometry.width() - width));
    top_left.setY(clamp_axis(
        top_left.y(),
        available_geometry.top(),
        available_geometry.top() + available_geometry.height() - height));
    m_window->setPosition(top_left);
}

void settings::Terminal_settings_window::place_within_anchor(
    const QRect& native_anchor_geometry)
{
    if (m_window == nullptr) {
        return;
    }

    const bool native_anchor_selected = !native_anchor_geometry.isEmpty();
    const QWindow* anchor = native_anchor_selected
        ? nullptr
        : m_window->transientParent();
    const QRect anchor_geometry = native_anchor_selected
        ? native_anchor_geometry
        : (anchor != nullptr && anchor->isVisible()
            ? anchor->geometry()
            : foreground_anchor_geometry());
    if (anchor_geometry.isEmpty()) {
        return;
    }

    QScreen* screen = screen_for_anchor(anchor, anchor_geometry);
    if (screen == nullptr || screen->availableGeometry().isEmpty()) {
        return;
    }

    const QRect available_geometry = screen->availableGeometry();
    watch_available_geometry(screen);
    apply_available_geometry(available_geometry);

    const int width = qMax(1, m_window->width());
    const int height = qMax(1, m_window->height());
    QPoint top_left = m_positioned
        ? m_window->position()
        : anchor_geometry.center() - QPoint(width / 2, height / 2);

    top_left.setX(clamp_axis(
        top_left.x(),
        available_geometry.left(),
        available_geometry.left() + available_geometry.width() - width));
    top_left.setY(clamp_axis(
        top_left.y(),
        available_geometry.top(),
        available_geometry.top() + available_geometry.height() - height));

    m_window->setPosition(top_left);
    m_positioned = true;
}

void settings::Terminal_settings_window::watch_available_geometry(QScreen* screen)
{
    if (m_available_geometry_screen == screen) {
        return;
    }

    QObject::disconnect(m_available_geometry_connection);
    m_available_geometry_screen = screen;
    if (screen != nullptr) {
        m_available_geometry_connection = QObject::connect(
            screen,
            &QScreen::availableGeometryChanged,
            this,
            [this](const QRect& available_geometry) {
                if (m_window != nullptr && m_window->isVisible()) {
                    apply_available_geometry(available_geometry);
                    clamp_to_available_geometry(available_geometry);
                }
            });
    }
}

void settings::Terminal_settings_window::handle_close_requested()
{
    if (m_window != nullptr) {
        m_window->hide();
    }
}

void settings::Terminal_settings_window::handle_move_requested()
{
    if (m_window != nullptr && m_system_window != nullptr) {
        (void)m_system_window->start_system_move(m_window);
    }
}

void settings::Terminal_settings_window::handle_resize_requested(int edges)
{
    if (m_window != nullptr && m_system_window != nullptr) {
        (void)m_system_window->start_system_resize(m_window, edges);
    }
}
