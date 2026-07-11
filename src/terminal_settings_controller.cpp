#include "terminal_settings_controller.h"

#if defined(VNM_TERMINAL_BUILD_PROVENANCE_HEADER)
#include VNM_TERMINAL_BUILD_PROVENANCE_HEADER
#endif

#include <QFontDatabase>

#ifndef VNM_TERMINAL_BUILD_PROVENANCE_TEXT
#define VNM_TERMINAL_BUILD_PROVENANCE_TEXT \
    "Build date: unknown\n"                \
    "vnm_terminal: unknown\n"              \
    "vnm_terminal_surface: unknown\n"      \
    "vnm_qml_chrome: unknown"
#endif

namespace settings = vnm_terminal::terminal_app;

settings::Terminal_settings_controller::Terminal_settings_controller(QObject* parent)
:
    QObject(parent)
{}

QString settings::Terminal_settings_controller::build_provenance_text() const
{
    return QStringLiteral(VNM_TERMINAL_BUILD_PROVENANCE_TEXT);
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
