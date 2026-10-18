#include "vnm_terminal/app_support/app_shortcuts.h"
#include "vnm_terminal/internal/vnm_terminal_surface_render_bridge.h"
#include "vnm_terminal/terminal_renderer_surface_format.h"
#include "helpers/test_check.h"

#include <QByteArray>
#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QPoint>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSizeF>
#include <QString>
#include <QTest>
#include <QUrl>

#define __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES 0
#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>

#include <iostream>
#include <initializer_list>
#include <memory>
#include <utility>

namespace app = vnm_terminal::terminal_app;
namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

class Input_backend final : public term::Terminal_backend
{
public:
    term::Terminal_backend_result start(
        const term::Terminal_launch_config&,
        term::Terminal_backend_callbacks callbacks) override
    {
        m_callbacks = std::move(callbacks);
        m_running = true;
        m_callbacks.output_received(QByteArrayLiteral("alpha beta\r\n"));
        return term::backend_accept();
    }

    term::Terminal_backend_result write(QByteArray bytes) override
    {
        m_input += bytes;
        return term::backend_accept();
    }

    term::Terminal_backend_result resize(term::Terminal_backend_resize_request) override
    {
        return term::backend_accept();
    }

    term::Terminal_backend_result set_output_paused(bool) override { return term::backend_accept(); }
    term::Terminal_backend_result interrupt() override           { return terminate(); }

    term::Terminal_backend_result terminate() override
    {
        if (m_running) {
            m_running = false;
            m_callbacks.process_exited({term::Terminal_exit_reason::TERMINATED, 0});
        }
        return term::backend_accept();
    }

    QByteArray take_input() { return std::exchange(m_input, {}); }

private:
    term::Terminal_backend_callbacks m_callbacks;
    QByteArray                       m_input;
    bool                             m_running = false;
};

class Native_key_observer final : public QObject
{
public:
    int count() const { return m_count; }

    bool matches(int key, Qt::KeyboardModifiers modifiers, unsigned short native_key,
        NSEventModifierFlags native_modifiers) const
    {
        return m_spontaneous && m_key == key && m_modifiers == modifiers &&
            m_native_key == native_key && m_native_modifiers == native_modifiers;
    }

protected:
    bool eventFilter(QObject*, QEvent* event) override
    {
        if (event->type() == QEvent::KeyPress) {
            const auto& key_event = *static_cast<QKeyEvent*>(event);
            ++m_count;
            m_key              = key_event.key();
            m_modifiers        = key_event.modifiers();
            m_native_key       = key_event.nativeVirtualKey();
            m_native_modifiers = key_event.nativeModifiers();
            m_spontaneous      = event->spontaneous();
            std::cout << "native key=" << m_key << " qt=" << m_modifiers.toInt()
                      << " cocoa=" << m_native_modifiers << " spontaneous=" << m_spontaneous
                      << " text=" << key_event.text().toUtf8().toHex().constData() << '\n';
        }
        return false;
    }

private:
    int                   m_count            = 0;
    int                   m_key              = 0;
    Qt::KeyboardModifiers m_modifiers;
    quint32               m_native_key       = 0;
    quint32               m_native_modifiers = 0;
    bool                  m_spontaneous      = false;
};

Qt::KeyboardModifiers qt_modifiers(NSEventModifierFlags flags)
{
    const bool literal = QCoreApplication::testAttribute(Qt::AA_MacDontSwapCtrlAndMeta);
    Qt::KeyboardModifiers result;
    if (flags & NSEventModifierFlagCommand)    result |= literal ? Qt::MetaModifier : Qt::ControlModifier;
    if (flags & NSEventModifierFlagControl)    result |= literal ? Qt::ControlModifier : Qt::MetaModifier;
    if (flags & NSEventModifierFlagShift)      result |= Qt::ShiftModifier;
    if (flags & NSEventModifierFlagNumericPad) result |= Qt::KeypadModifier;
    return result;
}

void dispatch_native_key(QQuickWindow& window,
    unsigned short native_key, NSEventModifierFlags flags,
    NSString* characters, NSString* unmodified)
{
    NSView* view = reinterpret_cast<NSView*>(window.winId());
    NSWindow* native_window = view.window;
    @autoreleasepool {
        for (NSEventType type : {NSEventTypeKeyDown, NSEventTypeKeyUp}) {
            NSEvent* event = [NSEvent keyEventWithType:type
                location:NSZeroPoint modifierFlags:flags
                timestamp:NSProcessInfo.processInfo.systemUptime
                windowNumber:native_window.windowNumber context:nil
                characters:characters charactersIgnoringModifiers:unmodified
                isARepeat:NO keyCode:native_key];
            [NSApp sendEvent:event];
        }
    }
    QCoreApplication::processEvents();
}

bool send_native_key(QQuickWindow& window, Native_key_observer& observer,
    int key, unsigned short native_key, NSEventModifierFlags flags,
    NSString* characters, NSString* unmodified)
{
    const int before = observer.count();
    dispatch_native_key(window, native_key, flags, characters, unmodified);
    return check(observer.count() == before + 1 &&
        observer.matches(key, qt_modifiers(flags), native_key, flags),
        "AppKit delivers one spontaneous Qt key press with the physical Cocoa modifiers");
}

bool select_terminal_text(QQuickWindow& window, VNM_TerminalSurface& surface)
{
    surface.clear_selection();
    const auto metrics = term::VNM_TerminalSurface_render_bridge::cell_metrics(surface);
    const QPoint start(qRound(metrics.width * 1.5), qRound(metrics.height * 0.5));
    const QPoint end(qRound(metrics.width * 4.5), qRound(metrics.height * 0.5));
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(&window, end);
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, end);
    return check(surface.selected_text() == QStringLiteral("lpha"),
        "terminal selection fixture contains lpha");
}

bool test_keyboard()
{
    QQuickWindow window;
    window.resize(440, 240);
    VNM_TerminalSurface surface(window.contentItem());
    surface.setSize(QSizeF(440, 240));
    // Match the standalone application's existing macOS copy policy.
    surface.set_copy_shortcut_policy(VNM_TerminalSurface::Copy_shortcut_policy::TERMINAL_INPUT);
    auto backend = std::make_unique<Input_backend>();
    Input_backend* input = backend.get();
    if (!check(term::VNM_TerminalSurface_render_bridge::start_backend_terminal(
            surface, std::move(backend), {QStringLiteral("native-keyboard")}).accepted,
            "terminal session starts"))
    {
        return false;
    }
    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(surface);

    app::Terminal_shortcut_filter shortcuts(&surface);
    Native_key_observer observer;
    window.installEventFilter(&shortcuts);
    window.installEventFilter(&observer);
    window.show();
    NSView* view = reinterpret_cast<NSView*>(window.winId());
    [NSApp activateIgnoringOtherApps:YES];
    [view.window makeKeyAndOrderFront:nil];
    [view.window makeFirstResponder:view];
    surface.forceActiveFocus();
    if (!check(QTest::qWaitForWindowActive(&window), "Cocoa terminal window becomes active")) {
        return false;
    }

    bool ok = check(surface.hasActiveFocus(), "terminal owns keyboard focus");
    QClipboard* clipboard = QGuiApplication::clipboard();
    const auto send = [&](int key, unsigned short code, NSEventModifierFlags flags,
                         NSString* text, NSString* unmodified) {
        ok &= send_native_key(window, observer, key, code, flags, text, unmodified);
    };
    const auto expect_input = [&](const QByteArray& expected, const char* description) {
        const QByteArray actual = input->take_input();
        if (actual != expected) {
            std::cerr << description << ": actual=" << actual.toHex().constData()
                      << " expected=" << expected.toHex().constData() << '\n';
        }
        ok &= check(actual == expected, description);
    };

    ok &= select_terminal_text(window, surface);
    clipboard->setText(QStringLiteral("copy sentinel"));
    send(Qt::Key_C, kVK_ANSI_C, NSEventModifierFlagCommand, @"c", @"c");
    ok &= check(clipboard->text() == QStringLiteral("lpha"), "Command+C copies terminal selection");
    expect_input({}, "Command+C sends no terminal bytes");

    ok &= select_terminal_text(window, surface);
    clipboard->setText(QStringLiteral("control sentinel"));
    send(Qt::Key_C, kVK_ANSI_C, NSEventModifierFlagControl, @"\003", @"c");
    expect_input(QByteArray(1, '\x03'), "Control+C with selection sends ETX");
    ok &= check(clipboard->text() == QStringLiteral("control sentinel"), "Control+C does not copy");
    surface.clear_selection();
    send(Qt::Key_C, kVK_ANSI_C, NSEventModifierFlagControl, @"\003", @"c");
    expect_input(QByteArray(1, '\x03'), "Control+C without selection sends ETX");
    send(Qt::Key_D, kVK_ANSI_D, NSEventModifierFlagControl, @"\004", @"d");
    expect_input(QByteArray(1, '\x04'), "Control+D sends EOT");
    send(Qt::Key_Z, kVK_ANSI_Z, NSEventModifierFlagControl, @"\032", @"z");
    expect_input(QByteArray(1, '\x1a'), "Control+Z sends SUB");

    int settings_requests = 0;
    QObject::connect(&shortcuts, &app::Terminal_shortcut_filter::settings_requested,
        &window, [&] { ++settings_requests; });
    send(Qt::Key_Comma, kVK_ANSI_Comma, NSEventModifierFlagCommand, @",", @",");
    ok &= check(settings_requests == 1, "Command+comma requests Settings");
    expect_input({}, "Command+comma sends no terminal bytes");
    const int before_control_comma = settings_requests;
    send(Qt::Key_Comma, kVK_ANSI_Comma, NSEventModifierFlagControl, @",", @",");
    ok &= check(settings_requests == before_control_comma, "Control+comma does not request Settings");
    (void)input->take_input();

    clipboard->setText(QStringLiteral("native paste"));
    send(Qt::Key_V, kVK_ANSI_V, NSEventModifierFlagCommand, @"v", @"v");
    expect_input(QByteArrayLiteral("native paste"), "default Command+V pastes clipboard text");

    // The preservation oracle is the same native chord delivered without the
    // host filter, not an invented policy for otherwise unclaimed Command keys.
    constexpr NSEventModifierFlags unclaimed_flags[] = {
        NSEventModifierFlagCommand,
        NSEventModifierFlagCommand | NSEventModifierFlagControl,
    };
    for (const auto flags : unclaimed_flags)
    {
        window.removeEventFilter(&shortcuts);
        send(Qt::Key_K, kVK_ANSI_K, flags,
            flags & NSEventModifierFlagControl ? @"\013" : @"k", @"k");
        const QByteArray original = input->take_input();
        window.installEventFilter(&shortcuts);
        window.removeEventFilter(&observer);
        window.installEventFilter(&observer);
        send(Qt::Key_K, kVK_ANSI_K, flags,
            flags & NSEventModifierFlagControl ? @"\013" : @"k", @"k");
        expect_input(original, "unclaimed Command chord retains its terminal bytes");
    }
    window.removeEventFilter(&shortcuts);
    constexpr NSEventModifierFlags arrow_flags = NSEventModifierFlagCommand |
        NSEventModifierFlagFunction | NSEventModifierFlagNumericPad;
    send(Qt::Key_Left, kVK_LeftArrow, arrow_flags, @"\uF702", @"\uF702");
    const QByteArray original_arrow = input->take_input();
    window.installEventFilter(&shortcuts);
    window.removeEventFilter(&observer);
    window.installEventFilter(&observer);
    send(Qt::Key_Left, kVK_LeftArrow, arrow_flags, @"\uF702", @"\uF702");
    expect_input(original_arrow, "unclaimed Command+Left retains its terminal bytes");

    window.removeEventFilter(&shortcuts);
    for (const auto policy : {app::Paste_shortcut_policy::CTRL_SHIFT_V,
             app::Paste_shortcut_policy::CTRL_V_AND_CTRL_SHIFT_V})
    {
        app::Terminal_shortcut_filter explicit_paste(&surface, policy);
        window.installEventFilter(&explicit_paste);
        window.removeEventFilter(&observer);
        window.installEventFilter(&observer);
        send(Qt::Key_V, kVK_ANSI_V, NSEventModifierFlagCommand, @"v", @"v");
        ok &= check(input->take_input() != QByteArrayLiteral("native paste"),
            "literal Control paste policies do not claim Command+V");
        send(Qt::Key_V, kVK_ANSI_V, NSEventModifierFlagControl | NSEventModifierFlagShift,
            @"\026", @"V");
        expect_input(QByteArrayLiteral("native paste"), "literal Control+Shift+V pastes");
        if (policy == app::Paste_shortcut_policy::CTRL_V_AND_CTRL_SHIFT_V) {
            send(Qt::Key_V, kVK_ANSI_V, NSEventModifierFlagControl, @"\026", @"v");
            expect_input(QByteArrayLiteral("native paste"), "literal Control+V pastes");
        }
    }
    window.installEventFilter(&shortcuts);
    window.removeEventFilter(&observer);
    window.installEventFilter(&observer);

    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData("import QtQuick\nTextInput { text: 'editor selection'; width: 400; height: 40 }", QUrl());
    std::unique_ptr<QObject> object(component.create());
    auto* editor = qobject_cast<QQuickItem*>(object.get());
    if (!check(editor != nullptr, "native text-field fixture loads")) {
        std::cerr << component.errorString().toStdString() << '\n';
        return false;
    }
    editor->setParentItem(window.contentItem());
    editor->forceActiveFocus();
    QMetaObject::invokeMethod(editor, "selectAll");
    clipboard->setText(QStringLiteral("editor sentinel"));
    send(Qt::Key_C, kVK_ANSI_C, NSEventModifierFlagCommand, @"c", @"c");
    ok &= check(clipboard->text() == QStringLiteral("editor selection"),
        "Command+C keeps the native text-field copy action");
    expect_input({}, "text-field copy sends no terminal bytes");

    const int before_editor_comma = settings_requests;
    dispatch_native_key(window, kVK_ANSI_Comma, NSEventModifierFlagCommand, @",", @",");
    ok &= check(settings_requests == before_editor_comma,
        "Command+comma in an unrelated text field does not open terminal Settings");
    expect_input({}, "text-field Command+comma sends no terminal bytes");

    shortcuts.set_search_ui_root(editor);
    send(Qt::Key_Comma, kVK_ANSI_Comma, NSEventModifierFlagCommand, @",", @",");
    ok &= check(settings_requests == before_editor_comma + 1,
        "Command+comma in the owned search field requests Settings");
    expect_input({}, "search-field Command+comma sends no terminal bytes");
    shortcuts.set_search_ui_root(nullptr);

    surface.forceActiveFocus();
    QQuickWindow other_window;
    other_window.resize(240, 120);
    other_window.show();
    NSView* other_view = reinterpret_cast<NSView*>(other_window.winId());
    [other_view.window makeKeyAndOrderFront:nil];
    [other_view.window makeFirstResponder:other_view];
    if (!check(QTest::qWaitForWindowActive(&other_window), "unrelated Cocoa window becomes active")) {
        return false;
    }
    const int before_other_comma = settings_requests;
    dispatch_native_key(other_window, kVK_ANSI_Comma, NSEventModifierFlagCommand, @",", @",");
    ok &= check(settings_requests == before_other_comma,
        "Command+comma in another window does not open terminal Settings");
    expect_input({}, "another window's Command+comma sends no terminal bytes");

    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && QByteArray(argv[1]) == "--literal-modifiers") {
        QCoreApplication::setAttribute(Qt::AA_MacDontSwapCtrlAndMeta);
    }
    QGuiApplication application(argc, argv);
    vnm_terminal::apply_terminal_renderer_minimum_surface_format();
    if (!check(QGuiApplication::platformName() == QStringLiteral("cocoa"),
            "native keyboard regression requires the Cocoa platform"))
    {
        return 1;
    }
    return test_keyboard() ? 0 : 1;
}
