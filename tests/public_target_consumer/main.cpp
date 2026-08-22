#include "vnm_terminal/app_support/app_settings.h"
#include "vnm_terminal/app_support/app_shortcuts.h"
#include "vnm_terminal/app_support/qml_chrome.h"
#include "vnm_terminal/app_support/terminal_scrollbar.h"
#include "vnm_terminal/app_support/terminal_search_bar.h"
#include "vnm_terminal/app_support/terminal_settings_controller.h"
#include "vnm_terminal/app_support/terminal_settings_window.h"
#include "vnm_terminal/default_shell.h"

#include <QQmlEngine>
#include <QQuickWindow>
#include <QtGlobal>

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

    // Keep this build-only branch opaque so the final link pulls app support's
    // packaged qml-chrome runtime dependency without running any GUI code.
    if (qEnvironmentVariableIsSet(
            "VNM_TERMINAL_PUBLIC_TARGET_CONSUMER_INSTANTIATE_CHROME")) {
        QQmlEngine          engine;
        QQuickWindow        window;
        Terminal_qml_chrome chrome(engine, window);
        return chrome.is_valid() ? 0 : 1;
    }

    return vnm_terminal::default_shell_argv().isEmpty() ? 0 : 0;
}
