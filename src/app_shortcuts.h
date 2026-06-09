#pragma once

#include "vnm_terminal/vnm_terminal_surface.h"

#include <QObject>
#include <Qt>

class QEvent;

namespace vnm_terminal::terminal_app {

enum class Paste_shortcut_policy
{
    DISABLED,
    CTRL_SHIFT_V,
    CTRL_V_AND_CTRL_SHIFT_V,
    PLATFORM_DEFAULT,
};

// Decide whether a key press should paste under the given policy. The caller
// pre-masks modifiers to Ctrl|Shift|Alt|Meta; the predicate masks too so it is
// safe to call with raw modifiers. PLATFORM_DEFAULT reproduces the historical
// behavior, including the macOS Cmd+V shortcut; the explicit Ctrl-combo policies
// are literal and do not honor Cmd+V.
bool paste_shortcut_should_paste(
    Paste_shortcut_policy   policy,
    int                     key,
    Qt::KeyboardModifiers   modifiers);

class Terminal_shortcut_filter final : public QObject
{
public:
    explicit Terminal_shortcut_filter(
        VNM_TerminalSurface*  surface,
        Paste_shortcut_policy paste_policy = Paste_shortcut_policy::PLATFORM_DEFAULT);

protected:
    bool eventFilter(QObject*, QEvent* event) override;

private:
    bool copy_selected_text();
    bool paste_clipboard_text();

    VNM_TerminalSurface*  m_surface      = nullptr;
    Paste_shortcut_policy m_paste_policy = Paste_shortcut_policy::PLATFORM_DEFAULT;
};

} // namespace vnm_terminal::terminal_app
