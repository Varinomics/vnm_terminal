#include "vnm_terminal/internal/vnm_terminal_font.h"

#include <QFontDatabase>
#include <QResource>
#include <QStringList>
#include <algorithm>
#include <cmath>

static void init_vnm_terminal_resources()
{
    Q_INIT_RESOURCE(vnm_terminal_resources);
}

namespace vnm_terminal::internal {

namespace {

constexpr const char* k_vnm_framework_monospace_resource =
    ":/vnm_terminal/fonts/vnm_framework_monospace.ttf";

struct Default_monospace_font
{
    QString family;
    bool    embedded_resource_loaded = false;
};

Default_monospace_font load_vnm_framework_monospace_font()
{
    init_vnm_terminal_resources();

    const int font_id =
        QFontDatabase::addApplicationFont(QString::fromLatin1(k_vnm_framework_monospace_resource));
    if (font_id < 0) {
        return {QStringLiteral("monospace"), false};
    }

    const QStringList families = QFontDatabase::applicationFontFamilies(font_id);
    if (families.isEmpty()) {
        return {QStringLiteral("monospace"), false};
    }

    return {families.front(), true};
}

const Default_monospace_font& default_monospace_font()
{
    static const Default_monospace_font font = load_vnm_framework_monospace_font();
    return font;
}

}

QString vnm_terminal_default_monospace_font_family()
{
    return default_monospace_font().family;
}

bool vnm_terminal_default_monospace_font_loaded()
{
    return default_monospace_font().embedded_resource_loaded;
}

QFont vnm_terminal_font(QString family, qreal pixel_size)
{
    if (family.trimmed().isEmpty()) {
        family = vnm_terminal_default_monospace_font_family();
    }

    QFont font(family);
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    if (std::isfinite(pixel_size) && pixel_size > 0.0) {
        font.setPixelSize(std::max(1, static_cast<int>(std::round(pixel_size))));
    }
    return font;
}

}
