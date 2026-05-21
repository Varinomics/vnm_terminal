#pragma once

#include <QFont>
#include <QString>

namespace vnm_terminal::internal {

constexpr int k_vnm_terminal_default_font_pixel_size = 13;

QString vnm_terminal_default_monospace_font_family();
bool vnm_terminal_default_monospace_font_loaded();
QFont vnm_terminal_font(QString family, qreal pixel_size);

}
