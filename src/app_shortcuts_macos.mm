#include "vnm_terminal/app_support/app_shortcuts.h"

#include <QGuiApplication>
#include <QQuickWindow>

#import <AppKit/AppKit.h>

namespace vnm_terminal::terminal_app {

bool Terminal_shortcut_filter::nativeEventFilter(
    const QByteArray& type, void* message, qintptr*)
{
    if (type != QByteArrayLiteral("mac_generic_NSEvent") || !host_shortcuts_have_focus()) {
        return false;
    }

    NSEvent* event = static_cast<NSEvent*>(message);
    if (event.type != NSEventTypeKeyDown) {
        return false;
    }
    constexpr NSEventModifierFlags shortcut_mask = NSEventModifierFlagCommand |
        NSEventModifierFlagControl | NSEventModifierFlagOption | NSEventModifierFlagShift;
    if ((event.modifierFlags & shortcut_mask) != NSEventModifierFlagCommand ||
        ![event.charactersIgnoringModifiers isEqualToString:@","])
    {
        return false;
    }

    QQuickWindow* window = m_surface->window();
    if (QGuiApplication::focusWindow() != window) {
        return false;
    }
    NSView* view = reinterpret_cast<NSView*>(window->winId());
    NSWindow* native_window = view.window;
    if (native_window == nil || event.window != native_window || NSApp.keyWindow != native_window) {
        return false;
    }

    // AppKit consumes Command+comma at Qt's disabled Settings menu placeholder.
    // Send the original event through the owned window so Cocoa still performs
    // its normal Qt key translation before the focused shortcut filter runs.
    [native_window sendEvent:event];
    return true;
}

} // namespace vnm_terminal::terminal_app
