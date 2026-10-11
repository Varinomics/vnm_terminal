#include "vnm_terminal/app_support/terminal_search_bar.h"

#include "vnm_terminal/app_support/app_shortcuts.h"
#include "vnm_terminal/app_support/qml_chrome.h"

#include "qml_component_error_string.h"

#include "vnm_terminal/vnm_terminal_surface.h"

#include <vnm_font_namespace.h>

#include <QCoreApplication>
#include <QFont>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QUrl>
#include <QVariant>

// Search can be used without constructing the terminal's chrome or settings.
extern int qInitResources_qmlcache_vnm_terminal_app_support();

namespace vnm_terminal::terminal_app {

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
    // The chevrons are Font Awesome 4 codepoints, so the bar registers that
    // face itself rather than naming a family and relying on some other library
    // in the process having registered one. Registration marks the family name,
    // and the marked name is what the QML component binds to: a hand-written copy
    // of it drifts from the binary, and a face the host has installed under the
    // unmarked name would merge with it.
    const vnm_fonts::Registered_font icon_font =
        vnm_fonts::register_shipped_font(vnm_fonts::Shipped_font::FONT_AWESOME_4);
    if (!icon_font.is_valid()) {
        m_error_string = icon_font.error;
        return;
    }
    m_icon_font_family = icon_font.family;

    m_context->setContextProperty(QStringLiteral("terminalSurface"), &surface);
    m_context->setContextProperty(QStringLiteral("searchBar"), this);

    QQmlComponent component(&engine);
    qInitResources_qmlcache_vnm_terminal_app_support();
    component.loadUrl(
        QUrl(QStringLiteral("qrc:/vnm_terminal/terminal_search_bar.qml")));
    if (!component.isReady()) {
        m_error_string = detail::qml_component_error_string(component);
        return;
    }

    m_root_object.reset(component.create(m_context.get()));
    if (m_root_object == nullptr) {
        m_error_string = detail::qml_component_error_string(component);
        return;
    }

    m_root_item = qobject_cast<QQuickItem*>(m_root_object.get());
    if (m_root_item == nullptr) {
        m_error_string = QStringLiteral("terminal search QML root is not a QQuickItem");
        m_root_object.reset();
        return;
    }

    m_query_item = m_root_item->findChild<QQuickItem*>(
        QStringLiteral("terminal_search_query_input"));
    m_result_item = m_root_item->findChild<QQuickItem*>(
        QStringLiteral("terminal_search_result_text"));
    if (m_query_item == nullptr || m_result_item == nullptr) {
        m_error_string = QStringLiteral("terminal search QML is missing required text items");
        m_root_item = nullptr;
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
    return
        m_root_item   != nullptr &&
        m_query_item  != nullptr &&
        m_result_item != nullptr;
}

bool Terminal_search_bar::is_visible() const
{
    return m_root_item != nullptr && m_root_item->isVisible();
}

QString Terminal_search_bar::error_string() const
{
    return m_error_string;
}

QString Terminal_search_bar::icon_font_family() const
{
    return m_icon_font_family;
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
        case VNM_TerminalSurface::Search_result_state::SEARCHING:
            return QStringLiteral("Searching...");
        case VNM_TerminalSurface::Search_result_state::NO_MATCH:
            return QStringLiteral("No matches");
        case VNM_TerminalSurface::Search_result_state::MATCH:
            return QStringLiteral("%1 of %2")
                .arg(m_surface->current_search_match())
                .arg(m_surface->search_match_count());
        default:
            return {};
    }
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

void Terminal_search_bar::set_text_font_family(const QString& font_family)
{
    if (m_query_item == nullptr || m_result_item == nullptr) {
        return;
    }

    QFont query_font = m_query_item->property("font").value<QFont>();
    query_font.setFamily(font_family);
    m_query_item->setProperty("font", QVariant::fromValue(query_font));

    QFont result_font = m_result_item->property("font").value<QFont>();
    result_font.setFamily(font_family);
    m_result_item->setProperty("font", QVariant::fromValue(result_font));
}

bool Terminal_search_bar::focus_query()
{
    if (m_query_item == nullptr || m_query_item->hasActiveFocus()) {
        return false;
    }

    m_query_item->forceActiveFocus(Qt::OtherFocusReason);
    return m_query_item->hasActiveFocus();
}

void Terminal_search_bar::commit_text(const QString& text)
{
    if (m_query_item == nullptr) {
        return;
    }

    QInputMethodEvent event;
    event.setCommitString(text);
    QCoreApplication::sendEvent(m_query_item, &event);
}

void Terminal_search_bar::send_key_event(QKeyEvent& event)
{
    if (m_query_item != nullptr &&
        (event.type() == QEvent::KeyPress || event.type() == QEvent::KeyRelease))
    {
        QCoreApplication::sendEvent(m_query_item, &event);
    }
}

void Terminal_search_bar::send_key_press(
    int                   key,
    Qt::KeyboardModifiers modifiers,
    const QString&        text)
{
    QKeyEvent event(QEvent::KeyPress, key, modifiers, text);
    send_key_event(event);
}

void Terminal_search_bar::send_key_release(
    int                   key,
    Qt::KeyboardModifiers modifiers)
{
    QKeyEvent event(QEvent::KeyRelease, key, modifiers, QString{});
    send_key_event(event);
}

void Terminal_search_bar::show_search()
{
    if (m_root_item == nullptr || m_query_item == nullptr || m_surface == nullptr) {
        return;
    }

    m_root_item->setProperty("query_text", m_surface->search_query());
    if (!m_root_item->isVisible()) {
        m_root_item->setVisible(true);
        emit visibility_changed(true);
    }

    m_query_item->forceActiveFocus(Qt::ShortcutFocusReason);
    QMetaObject::invokeMethod(m_query_item, "selectAll");
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

bool Terminal_search_bar::apply_shortcut(int key, Qt::KeyboardModifiers modifiers)
{
    if (m_surface == nullptr) {
        return false;
    }

    const Search_shortcut_action action = terminal_search_shortcut_action(
        key,
        modifiers,
        is_visible(),
        !m_surface->search_query().isEmpty());
    apply_shortcut_action(action);
    return action != Search_shortcut_action::NONE;
}

void Terminal_search_bar::apply_shortcut_action(Search_shortcut_action action)
{
    switch (action) {
        case Search_shortcut_action::SHOW:
            show_search();
            return;
        case Search_shortcut_action::NEXT:
            if (m_surface != nullptr) {
                (void)m_surface->search_next();
            }
            return;
        case Search_shortcut_action::PREVIOUS:
            if (m_surface != nullptr) {
                (void)m_surface->search_previous();
            }
            return;
        case Search_shortcut_action::DISMISS:
            dismiss_search();
            return;
        case Search_shortcut_action::NONE:
        default:
            return;
    }
}

bool Terminal_search_bar::commit_visible_text(const QString& text)
{
    if (!is_visible()) {
        return false;
    }

    (void)focus_query();
    if (!text.isEmpty()) {
        commit_text(text);
    }
    return true;
}

bool Terminal_search_bar::send_visible_key_event(QKeyEvent& event)
{
    if (!is_visible()) {
        return false;
    }

    (void)focus_query();
    send_key_event(event);
    return true;
}

} // namespace vnm_terminal::terminal_app
