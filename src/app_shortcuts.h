#pragma once

#include "vnm_terminal/vnm_terminal_surface.h"

#include <QObject>

class QEvent;

namespace vnm_terminal::terminal_app {

class Terminal_shortcut_filter final : public QObject
{
public:
    explicit Terminal_shortcut_filter(VNM_TerminalSurface* surface);

protected:
    bool eventFilter(QObject*, QEvent* event) override;

private:
    bool copy_selected_text();
    bool paste_clipboard_text();

    VNM_TerminalSurface* m_surface = nullptr;
};

} // namespace vnm_terminal::terminal_app
