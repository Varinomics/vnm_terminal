#pragma once

#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>

class VNM_TerminalSurface;

namespace vnm_terminal::terminal_app {

// Bridges the settings panel QML to the live terminal surface. The panel binds
// its controls directly to the surface's QML properties (colorScheme, fontSize,
// textRendererMode, scrollbackLimit, fontFamily) for immediate apply; this
// controller adds the helpers QML cannot derive itself (the monospace font
// list) and persists appearance changes to QSettings shortly after they
// change. Persistence is debounced: rapid live changes (a font-size drag)
// coalesce into one disk sync, and a pending write flushes on destruction.
class Terminal_settings_controller final : public QObject
{
    Q_OBJECT

public:
    explicit Terminal_settings_controller(
        VNM_TerminalSurface& surface,
        QObject*             parent = nullptr);
    ~Terminal_settings_controller() override;

    Q_INVOKABLE QStringList available_font_families() const;

private:
    void connect_persistence();
    void persist_appearance() const;

    QPointer<VNM_TerminalSurface> m_surface;
    QTimer                        m_persist_debounce_timer;
};

} // namespace vnm_terminal::terminal_app
