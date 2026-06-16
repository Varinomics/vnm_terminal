#include "terminal_settings_controller.h"

#include <QFontDatabase>

namespace settings = vnm_terminal::terminal_app;

settings::Terminal_settings_controller::Terminal_settings_controller(QObject* parent)
:
    QObject(parent)
{}

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
