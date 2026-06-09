#include "terminal_settings_controller.h"

#include "app_settings.h"

#include "vnm_terminal/vnm_terminal_surface.h"

#include <QFontDatabase>
#include <QLatin1String>
#include <QSettings>

namespace settings = vnm_terminal::terminal_app;

settings::Terminal_settings_controller::Terminal_settings_controller(
    VNM_TerminalSurface& surface,
    QObject*             parent)
:
    QObject(parent),
    m_surface(&surface)
{
    connect_persistence();
}

QStringList settings::Terminal_settings_controller::available_font_families() const
{
    QStringList families;
    const QStringList all_families = QFontDatabase::families();
    for (const QString& family : all_families) {
        if (QFontDatabase::isFixedPitch(family)) {
            families.push_back(family);
        }
    }
    return families;
}

void settings::Terminal_settings_controller::connect_persistence()
{
    if (m_surface == nullptr || !terminal_window_persistence_enabled()) {
        return;
    }

    const auto persist = [this] { persist_appearance(); };
    QObject::connect(m_surface, &VNM_TerminalSurface::color_scheme_changed,       this, persist);
    QObject::connect(m_surface, &VNM_TerminalSurface::font_family_changed,        this, persist);
    QObject::connect(m_surface, &VNM_TerminalSurface::font_size_changed,          this, persist);
    QObject::connect(m_surface, &VNM_TerminalSurface::text_renderer_mode_changed, this, persist);
    QObject::connect(m_surface, &VNM_TerminalSurface::scrollback_limit_changed,   this, persist);
}

void settings::Terminal_settings_controller::persist_appearance() const
{
    if (m_surface == nullptr) {
        return;
    }

    QSettings settings;
    settings.beginGroup(QLatin1String(k_appearance_settings_group));
    settings.setValue(QLatin1String(k_appearance_color_scheme), m_surface->color_scheme());
    settings.setValue(QLatin1String(k_appearance_font_family),  m_surface->font_family());
    settings.setValue(
        QLatin1String(k_appearance_text_renderer_mode),
        static_cast<int>(m_surface->text_renderer_mode()));
    settings.setValue(QLatin1String(k_appearance_scrollback_limit), m_surface->scrollback_limit());
    settings.endGroup();

    // font_size stays in the window group so it shares the startup read/write
    // path with the persisted window state.
    settings.beginGroup(QLatin1String(k_window_settings_group));
    settings.setValue(QLatin1String(k_window_settings_font_size), m_surface->font_size());
    settings.endGroup();

    settings.sync();
}
