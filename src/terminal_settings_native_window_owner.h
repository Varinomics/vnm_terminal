#pragma once

#include <QtGlobal>

namespace vnm_terminal::terminal_app::detail {

#ifdef Q_OS_WIN
void set_terminal_settings_native_window_owner(
    quintptr settings_window_id,
    quintptr owner_window_id);
#endif

} // namespace vnm_terminal::terminal_app::detail
