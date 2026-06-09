#pragma once

#include <QObject>
#include <QPointer>
#include <QStringList>

class VNM_TerminalSurface;

namespace vnm_terminal::terminal_app {

// Bridges the settings panel QML to the live terminal surface. The panel binds
// its controls directly to the surface's QML properties (colorScheme, fontSize,
// textRendererMode, scrollbackLimit, fontFamily) for immediate apply; this
// controller adds the helpers QML cannot derive itself (the monospace font
// list) and persists appearance changes to QSettings as they happen.
class Terminal_settings_controller final : public QObject
{
    Q_OBJECT

public:
    explicit Terminal_settings_controller(
        VNM_TerminalSurface& surface,
        QObject*             parent = nullptr);

    Q_INVOKABLE QStringList available_font_families() const;

private:
    void connect_persistence();
    void persist_appearance() const;

    QPointer<VNM_TerminalSurface> m_surface;
};

} // namespace vnm_terminal::terminal_app
