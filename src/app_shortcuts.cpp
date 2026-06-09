#include "app_shortcuts.h"

#include <QClipboard>
#include <QEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QString>
#include <QtGlobal>

namespace vnm_terminal::terminal_app {

bool paste_shortcut_should_paste(
    Paste_shortcut_policy   policy,
    int                     key,
    Qt::KeyboardModifiers   modifiers)
{
    if (key != Qt::Key_V) {
        return false;
    }

    const Qt::KeyboardModifiers masked =
        modifiers &
        (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier);
    const bool ctrl_shift_v = masked == (Qt::ControlModifier | Qt::ShiftModifier);
    const bool ctrl_or_ctrl_shift_v =
        masked == Qt::ControlModifier || ctrl_shift_v;

    switch (policy) {
        case Paste_shortcut_policy::DISABLED:                return false;
        case Paste_shortcut_policy::CTRL_SHIFT_V:            return ctrl_shift_v;
        case Paste_shortcut_policy::CTRL_V_AND_CTRL_SHIFT_V: return ctrl_or_ctrl_shift_v;
        case Paste_shortcut_policy::PLATFORM_DEFAULT:
#if defined(Q_OS_MACOS)
            return ctrl_or_ctrl_shift_v || masked == Qt::MetaModifier;
#else
            return ctrl_or_ctrl_shift_v;
#endif
        default:                                             return false;
    }
}

Terminal_shortcut_filter::Terminal_shortcut_filter(
    VNM_TerminalSurface*  surface,
    Paste_shortcut_policy paste_policy)
:
    QObject(surface),
    m_surface(surface),
    m_paste_policy(paste_policy)
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
    const bool paste_shortcut =
        paste_shortcut_should_paste(m_paste_policy, key_event->key(), modifiers);
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
