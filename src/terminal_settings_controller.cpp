#include "terminal_settings_controller.h"

#include "app_settings.h"

#include "vnm_terminal/vnm_terminal_surface.h"

#include <QFontDatabase>
#include <QLatin1String>
#include <QSettings>

namespace settings = vnm_terminal::terminal_app;

namespace {

// Long enough to coalesce a live font-size drag into one disk sync, short
// enough that the settings hit disk well before the user could exit.
constexpr int k_persist_debounce_ms = 400;

}

settings::Terminal_settings_controller::Terminal_settings_controller(
    VNM_TerminalSurface& surface,
    QObject*             parent)
:
    QObject(parent),
    m_surface(&surface)
{
    m_persist_debounce_timer.setSingleShot(true);
    m_persist_debounce_timer.setInterval(k_persist_debounce_ms);
    QObject::connect(
        &m_persist_debounce_timer,
        &QTimer::timeout,
        this,
        [this] { persist_appearance(); });
    connect_persistence();
}

settings::Terminal_settings_controller::~Terminal_settings_controller()
{
    // A change inside the debounce window must not be lost to app exit.
    if (m_persist_debounce_timer.isActive()) {
        persist_appearance();
    }
}

QStringList settings::Terminal_settings_controller::available_font_families() const
{
    QStringList families;
    const QStringList all_families = QFontDatabase::families();
    for (const QString& family : all_families) {
        // Monospace only, and scalable only: legacy bitmap fonts (Fixedsys,
        // Terminal, 8514oem, ...) have no outlines, so they cannot be MSDF-baked
        // and render poorly when zoomed. Excluding them also keeps the picker
        // free of fonts the user does not want.
        if (QFontDatabase::isFixedPitch(family) &&
            QFontDatabase::isSmoothlyScalable(family))
        {
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

    const auto persist = [this] { m_persist_debounce_timer.start(); };
    QObject::connect(m_surface, &VNM_TerminalSurface::color_scheme_changed,                  this, persist);
    QObject::connect(m_surface, &VNM_TerminalSurface::font_family_changed,                   this, persist);
    QObject::connect(m_surface, &VNM_TerminalSurface::font_size_changed,                     this, persist);
    QObject::connect(m_surface, &VNM_TerminalSurface::text_renderer_mode_changed,            this, persist);
    QObject::connect(m_surface, &VNM_TerminalSurface::lcd_subpixel_order_changed,            this, persist);
    QObject::connect(m_surface, &VNM_TerminalSurface::row_timestamp_tooltip_enabled_changed, this, persist);
    QObject::connect(m_surface, &VNM_TerminalSurface::scrollback_limit_changed,              this, persist);
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
    settings.setValue(
        QLatin1String(k_appearance_lcd_subpixel_order),
        static_cast<int>(m_surface->lcd_subpixel_order()));
    settings.setValue(
        QLatin1String(k_appearance_row_timestamp_tooltip),
        m_surface->row_timestamp_tooltip_enabled());
    settings.setValue(QLatin1String(k_appearance_scrollback_limit), m_surface->scrollback_limit());
    settings.endGroup();

    // font_size stays in the window group so it shares the startup read/write
    // path with the persisted window state.
    settings.beginGroup(QLatin1String(k_window_settings_group));
    settings.setValue(QLatin1String(k_window_settings_font_size), m_surface->font_size());
    settings.endGroup();

    settings.sync();
}
