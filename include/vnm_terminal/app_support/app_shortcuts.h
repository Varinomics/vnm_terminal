#pragma once

#include "vnm_terminal/vnm_terminal_surface.h"

#include <QObject>
#include <QPointer>
#include <Qt>

#if defined(Q_OS_MACOS)
#include <QAbstractNativeEventFilter>
#endif

class QEvent;
class QQuickItem;

namespace vnm_terminal::terminal_app {

enum class Paste_shortcut_policy
{
    DISABLED,
    CTRL_SHIFT_V,
    CTRL_V_AND_CTRL_SHIFT_V,
    PLATFORM_DEFAULT,
};

enum class Search_shortcut_action
{
    NONE,
    SHOW,
    NEXT,
    PREVIOUS,
    DISMISS,
};

// Decide whether a key press should paste under the given policy. The caller
// pre-masks modifiers to Ctrl|Shift|Alt|Meta; the predicate masks too so it is
// safe to call with raw modifiers. PLATFORM_DEFAULT includes macOS Cmd+V; the
// explicit Ctrl-combo policies are literal and do not honor Cmd+V.
// macOS modifiers follow the host's AA_MacDontSwapCtrlAndMeta convention.
bool paste_shortcut_should_paste(
    Paste_shortcut_policy   policy,
    int                     key,
    Qt::KeyboardModifiers   modifiers);

Search_shortcut_action terminal_search_shortcut_action(
    int                     key,
    Qt::KeyboardModifiers   modifiers,
    bool                    search_ui_visible,
    bool                    query_active);

// Windows console applications bind clipboard-image paste to Alt+V and receive
// Ctrl+V only as text, so a paste of an image-only clipboard reaches them as
// this key press. The target is expected to be the terminal surface.
void send_image_paste_shortcut(QObject& target);

class Terminal_shortcut_filter final : public QObject
#if defined(Q_OS_MACOS)
    , private QAbstractNativeEventFilter
#endif
{
    Q_OBJECT

public:
    explicit Terminal_shortcut_filter(
        VNM_TerminalSurface*  surface,
        Paste_shortcut_policy paste_policy = Paste_shortcut_policy::PLATFORM_DEFAULT);

    void set_search_ui_root(QQuickItem* root_item);
    void set_search_ui_visible(bool visible);

signals:
    void settings_requested();
    void search_shortcut_requested(Search_shortcut_action action);

protected:
    bool eventFilter(QObject*, QEvent* event) override;

private:
#if defined(Q_OS_MACOS)
    bool nativeEventFilter(const QByteArray& type, void* message, qintptr* result) override;
#endif
    bool host_shortcuts_have_focus() const;
    bool copy_selected_text();
    bool paste_clipboard_text();

    VNM_TerminalSurface*   m_surface      = nullptr;
    Paste_shortcut_policy  m_paste_policy = Paste_shortcut_policy::PLATFORM_DEFAULT;
    QPointer<QQuickItem>   m_search_ui_root;
    bool                   m_search_ui_visible = false;
};

} // namespace vnm_terminal::terminal_app
