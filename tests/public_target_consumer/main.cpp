#include "vnm_terminal/app_support/app_settings.h"
#include "vnm_terminal/app_support/app_shortcuts.h"
#include "vnm_terminal/app_support/qml_chrome.h"
#include "vnm_terminal/app_support/terminal_scrollbar.h"
#include "vnm_terminal/app_support/terminal_search_bar.h"
#include "vnm_terminal/app_support/terminal_settings_controller.h"
#include "vnm_terminal/app_support/terminal_settings_window.h"
#include "vnm_terminal/default_shell.h"

#include <type_traits>

int main()
{
    using namespace vnm_terminal::terminal_app;
    static_assert(std::is_default_constructible_v<Terminal_settings_snapshot>);
    static_assert(std::is_base_of_v<QObject, Terminal_shortcut_filter>);
    static_assert(std::is_base_of_v<QObject, Terminal_qml_chrome>);
    static_assert(std::is_base_of_v<QQuickPaintedItem, Terminal_scrollbar>);
    static_assert(std::is_base_of_v<QObject, Terminal_search_bar>);
    static_assert(std::is_base_of_v<QObject, Terminal_settings_controller>);
    static_assert(std::is_base_of_v<QObject, Terminal_settings_window>);
    return vnm_terminal::default_shell_argv().isEmpty() ? 0 : 0;
}
