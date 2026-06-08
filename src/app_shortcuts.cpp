#include "app_shortcuts.h"

#include <QClipboard>
#include <QEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QString>
#include <QtGlobal>

namespace vnm_terminal::terminal_app {

Terminal_shortcut_filter::Terminal_shortcut_filter(VNM_TerminalSurface* surface)
:
    QObject(surface),
    m_surface(surface)
{}

bool Terminal_shortcut_filter::eventFilter(QObject*, QEvent* event)
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

bool Terminal_shortcut_filter::copy_selected_text()
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

bool Terminal_shortcut_filter::paste_clipboard_text()
{
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (clipboard != nullptr) {
        m_surface->paste_text(clipboard->text());
        return true;
    }

    return false;
}

} // namespace vnm_terminal::terminal_app
