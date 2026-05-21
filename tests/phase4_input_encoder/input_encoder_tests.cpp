#include "vnm_terminal/internal/terminal_input_encoder.h"
#include "helpers/test_check.h"

#include <QCoreApplication>
#include <QKeyEvent>
#include <iostream>
#include <string>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

bool check_bytes_equal(
    const QByteArray&  actual,
    const QByteArray&  expected,
    const std::string& message)
{
    if (actual != expected) {
        std::cerr << "FAIL: " << message
            << " expected=" << expected.toHex(' ').constData()
            << " actual="   << actual.toHex(' ').constData() << '\n';
        return false;
    }

    return true;
}

bool check_bytes_equal(const QByteArray& actual, const QByteArray& expected, const char* message)
{
    return check_bytes_equal(actual, expected, std::string(message));
}

QByteArray bytes_from_hex(const char* hex)
{
    return QByteArray::fromHex(QByteArray(hex));
}

QByteArray framed_paste(QByteArray body)
{
    QByteArray bytes = QByteArrayLiteral("\x1b[200~");
    bytes += body;
    bytes += QByteArrayLiteral("\x1b[201~");
    return bytes;
}

QByteArray encode(
    int                                key,
    Qt::KeyboardModifiers              modifiers,
    QString                            text = {},
    term::Terminal_input_mode_state    modes = {})
{
    QKeyEvent event(QEvent::KeyPress, key, modifiers, text);
    return term::encode_terminal_key_event(event, modes);
}

bool may_encode(
    int                    key,
    Qt::KeyboardModifiers  modifiers,
    QString                text = {})
{
    QKeyEvent event(QEvent::KeyPress, key, modifiers, text);
    return term::terminal_key_event_may_encode(event);
}

QByteArray sgr_mouse_report(int button_code, int row, int column, char final_byte)
{
    QByteArray bytes = QByteArrayLiteral("\x1b[<");
    bytes += QByteArray::number(button_code);
    bytes += ';';
    bytes += QByteArray::number(column + 1);
    bytes += ';';
    bytes += QByteArray::number(row + 1);
    bytes += final_byte;
    return bytes;
}

bool test_control_and_altgr()
{
    bool ok = true;

    ok &= check_bytes_equal(
        encode(Qt::Key_At, Qt::ControlModifier),
        bytes_from_hex("00"),
        "Ctrl+@ maps to NUL");
    ok &= check_bytes_equal(
        encode(Qt::Key_Space, Qt::ControlModifier),
        bytes_from_hex("00"),
        "Ctrl+Space maps to NUL");
    ok &= check_bytes_equal(
        encode(Qt::Key_Backspace, Qt::ControlModifier),
        bytes_from_hex("08"),
        "Ctrl+Backspace maps to BS");
    ok &= check_bytes_equal(
        encode(Qt::Key_K, Qt::ControlModifier),
        bytes_from_hex("0b"),
        "Ctrl+K maps to VT for shell line editing");
    ok &= check_bytes_equal(
        encode(Qt::Key_Backspace, Qt::ControlModifier | Qt::AltModifier),
        bytes_from_hex("1b08"),
        "Ctrl+Alt+Backspace prefixes BS with ESC");
    ok &= check_bytes_equal(
        encode(
            Qt::Key_E,
            Qt::ControlModifier | Qt::AltModifier,
            QString::fromUtf8("\xe2\x82\xac")),
        bytes_from_hex("e282ac"),
        "Ctrl+Alt printable text preserves UTF-8 layout text");
    ok &= check_bytes_equal(
        encode(
            Qt::Key_C,
            Qt::ControlModifier | Qt::AltModifier,
            QString(QChar(0x0003))),
        bytes_from_hex("1b03"),
        "Ctrl+Alt platform C0 text falls through to Alt-prefixed control");
    ok &= check_bytes_equal(
        encode(Qt::Key_Left, Qt::ControlModifier | Qt::AltModifier),
        bytes_from_hex("1b5b313b3744"),
        "Ctrl+Alt navigation keeps modifier-aware arrow encoding");
    ok &= check_bytes_equal(
        encode(Qt::Key_F5, Qt::ControlModifier | Qt::AltModifier),
        bytes_from_hex("1b5b31353b377e"),
        "Ctrl+Alt function key keeps modifier-aware function encoding");

    return ok;
}

bool test_cursor_and_navigation_modes()
{
    bool ok = true;

    term::Terminal_input_mode_state modes;
    ok &= check_bytes_equal(
        encode(Qt::Key_Home, Qt::NoModifier, {}, modes),
        bytes_from_hex("1b5b48"),
        "normal Home writes CSI H");
    ok &= check_bytes_equal(
        encode(Qt::Key_End, Qt::NoModifier, {}, modes),
        bytes_from_hex("1b5b46"),
        "normal End writes CSI F");

    modes.application_cursor_keys  = true;
    ok                            &= check_bytes_equal(
        encode(Qt::Key_Home, Qt::NoModifier, {}, modes),
        bytes_from_hex("1b4f48"),
        "application cursor Home writes SS3 H");
    ok                            &= check_bytes_equal(
        encode(Qt::Key_End, Qt::NoModifier, {}, modes),
        bytes_from_hex("1b4f46"),
        "application cursor End writes SS3 F");
    ok                            &= check_bytes_equal(
        encode(Qt::Key_Home, Qt::ShiftModifier, {}, modes),
        bytes_from_hex("1b5b313b3248"),
        "modified Home keeps CSI modifier form in application cursor mode");

    ok &= check_bytes_equal(
        encode(Qt::Key_Backtab, Qt::NoModifier),
        bytes_from_hex("1b5b5a"),
        "Backtab writes CSI Z");
    ok &= check_bytes_equal(
        encode(Qt::Key_Backtab, Qt::ShiftModifier),
        bytes_from_hex("1b5b5a"),
        "Shift+Backtab writes CSI Z");
    ok &= check_bytes_equal(
        encode(Qt::Key_Tab, Qt::ShiftModifier, QStringLiteral("\t")),
        bytes_from_hex("1b5b5a"),
        "Shift+Tab writes CSI Z");
    ok &= check_bytes_equal(
        encode(Qt::Key_Tab, Qt::ShiftModifier | Qt::ControlModifier, QStringLiteral("\t")),
        bytes_from_hex("1b5b313b365a"),
        "Ctrl+Shift+Tab writes CSI 1;6 Z");
    ok &= check_bytes_equal(
        encode(Qt::Key_Tab, Qt::ShiftModifier | Qt::AltModifier, QStringLiteral("\t")),
        bytes_from_hex("1b5b313b345a"),
        "Alt+Shift+Tab writes CSI 1;4 Z");

    return ok;
}

bool test_keypad_policy()
{
    bool ok = true;

    term::Terminal_input_mode_state modes;
    modes.application_keypad = true;

    ok &= check_bytes_equal(
        encode(Qt::Key_5, Qt::KeypadModifier, QStringLiteral("5"), modes),
        bytes_from_hex("1b4f75"),
        "unmodified application keypad digit uses SS3 encoding");
    ok &= check_bytes_equal(
        encode(
            Qt::Key_5,
            Qt::KeypadModifier | Qt::ShiftModifier,
            QStringLiteral("5"),
            modes),
        bytes_from_hex("35"),
        "modified keypad digit falls through to printable text");
    ok &= check_bytes_equal(
        encode(
            Qt::Key_Plus,
            Qt::KeypadModifier | Qt::AltModifier,
            QStringLiteral("+"),
            modes),
        bytes_from_hex("1b2b"),
        "modified keypad operator preserves Alt printable behavior");

    return ok;
}

bool test_key_preclassification()
{
    bool ok = true;

    ok &= check(!may_encode(Qt::Key_Shift, Qt::ShiftModifier),
        "modifier-only key is classified as unencodable");
    ok &= check(!may_encode(Qt::Key_unknown, Qt::NoModifier),
        "unknown key without text is classified as unencodable");
    ok &= check(may_encode(Qt::Key_unknown, Qt::NoModifier, QStringLiteral("x")),
        "unknown key with text is classified as encodable");
    ok &= check(may_encode(Qt::Key_Left, Qt::NoModifier),
        "cursor key is classified as encodable");
    ok &= check(may_encode(Qt::Key_5, Qt::KeypadModifier),
        "keypad key can depend on live application keypad mode");

    return ok;
}

bool test_paste_framing_policy()
{
    bool ok = true;

    term::Terminal_input_mode_state modes;
    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            QStringLiteral("abc"),
            modes,
            term::Terminal_paste_framing_policy::DISABLED),
        QByteArrayLiteral("abc"),
        "disabled paste policy never frames");
    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            QStringLiteral("abc"),
            modes,
            term::Terminal_paste_framing_policy::APPLICATION_CONTROLLED),
        QByteArrayLiteral("abc"),
        "application-controlled paste policy stays unframed without terminal mode");

    modes.bracketed_paste = true;
    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            QStringLiteral("abc"),
            modes,
            term::Terminal_paste_framing_policy::APPLICATION_CONTROLLED),
        QByteArrayLiteral("\x1b[200~abc\x1b[201~"),
        "application-controlled paste policy frames when terminal mode is set");

    modes.bracketed_paste = false;
    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            QStringLiteral("abc"),
            modes,
            term::Terminal_paste_framing_policy::ENABLED),
        QByteArrayLiteral("\x1b[200~abc\x1b[201~"),
        "enabled paste policy always frames");

    modes.bracketed_paste  = true;
    ok                    &= check_bytes_equal(
        term::encode_terminal_paste_text(
            {},
            modes,
            term::Terminal_paste_framing_policy::DISABLED),
        {},
        "disabled empty paste produces no bytes");
    ok                    &= check_bytes_equal(
        term::encode_terminal_paste_text(
            {},
            modes,
            term::Terminal_paste_framing_policy::APPLICATION_CONTROLLED),
        {},
        "application-controlled empty paste produces no frame");
    ok                    &= check_bytes_equal(
        term::encode_terminal_paste_text(
            {},
            modes,
            term::Terminal_paste_framing_policy::ENABLED),
        {},
        "enabled empty paste produces no frame");

    return ok;
}

bool test_paste_sanitization()
{
    bool ok = true;

    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            QStringLiteral("a\r\nb\rc\n"),
            {},
            term::Terminal_paste_framing_policy::DISABLED),
        QByteArrayLiteral("a\nb\nc\n"),
        "paste sanitization normalizes CRLF and lone CR to LF");
    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            QStringLiteral("\r"),
            {},
            term::Terminal_paste_framing_policy::DISABLED),
        QByteArrayLiteral("\n"),
        "paste sanitization normalizes trailing lone CR");
    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            QStringLiteral("\r\r"),
            {},
            term::Terminal_paste_framing_policy::DISABLED),
        QByteArrayLiteral("\n\n"),
        "paste sanitization normalizes consecutive lone CRs");
    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            QStringLiteral("\r\n\r\n"),
            {},
            term::Terminal_paste_framing_policy::DISABLED),
        QByteArrayLiteral("\n\n"),
        "paste sanitization normalizes consecutive CRLF pairs");

    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            QStringLiteral(" \t\nkept  "),
            {},
            term::Terminal_paste_framing_policy::DISABLED),
        QByteArrayLiteral(" \t\nkept  "),
        "paste sanitization preserves spaces tabs LF and trailing spaces");

    QString controls;
    controls.append(QChar(0x0000));
    controls.append(QChar(0x0001));
    controls.append(QChar(0x001b));
    controls.append(QStringLiteral("A"));
    controls.append(QChar(0x007f));
    controls.append(QChar(0x0085));
    controls.append(QStringLiteral("B"));
    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            controls,
            {},
            term::Terminal_paste_framing_policy::DISABLED),
        QByteArrayLiteral("AB"),
        "paste sanitization strips NUL ESC DEL C0 and C1 controls");

    QString control_boundaries;
    control_boundaries.append(QChar(0x0009));
    control_boundaries.append(QChar(0x000a));
    control_boundaries.append(QChar(0x000d));
    control_boundaries.append(QChar(0x001f));
    control_boundaries.append(QChar(0x0020));
    control_boundaries.append(QChar(0x007f));
    control_boundaries.append(QChar(0x009f));
    control_boundaries.append(QChar(0x00a0));
    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            control_boundaries,
            {},
            term::Terminal_paste_framing_policy::DISABLED),
        QByteArrayLiteral("\t\n\n \xc2\xa0"),
        "paste sanitization preserves tab LF space NBSP and strips control boundaries");

    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            controls,
            {},
            term::Terminal_paste_framing_policy::ENABLED),
        framed_paste(QByteArrayLiteral("AB")),
        "framed paste sanitizes the body without stripping the frame");

    QString delimiter_injection = QStringLiteral("\x1b[201~harm");
    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            delimiter_injection,
            {},
            term::Terminal_paste_framing_policy::ENABLED),
        framed_paste(QByteArrayLiteral("[201~harm")),
        "framed paste strips ESC from an embedded end delimiter");

    QString c1_csi_injection;
    c1_csi_injection.append(QChar(0x009b));
    c1_csi_injection.append(QStringLiteral("201~harm"));
    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            c1_csi_injection,
            {},
            term::Terminal_paste_framing_policy::ENABLED),
        framed_paste(QByteArrayLiteral("201~harm")),
        "framed paste strips C1 CSI from an embedded end delimiter");

    QString stripped_only;
    stripped_only.append(QChar(0x0000));
    stripped_only.append(QChar(0x001b));
    stripped_only.append(QChar(0x007f));
    stripped_only.append(QChar(0x009b));
    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            stripped_only,
            {},
            term::Terminal_paste_framing_policy::ENABLED),
        {},
        "paste sanitization that removes the whole body produces no frame");

    const QString unicode_text = QString::fromUtf8("lambda \xce\xbb euro \xe2\x82\xac");
    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            unicode_text,
            {},
            term::Terminal_paste_framing_policy::DISABLED),
        unicode_text.toUtf8(),
        "paste sanitization preserves UTF-8 text");

    const QString non_bmp_text = QString::fromUtf8("emoji \xf0\x9f\x98\x80");
    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            non_bmp_text,
            {},
            term::Terminal_paste_framing_policy::DISABLED),
        non_bmp_text.toUtf8(),
        "paste sanitization preserves non-BMP UTF-8 text");

    return ok;
}

bool test_sgr_mouse_reporting()
{
    bool ok = true;

    term::Terminal_input_mode_state modes;
    modes.sgr_mouse_encoding = true;

    ok &= check_bytes_equal(
        term::encode_terminal_mouse_event({
            term::Terminal_mouse_event_kind::PRESS,
            term::Terminal_mouse_button::LEFT,
            3,
            4,
            Qt::NoModifier,
        }, modes),
        {},
        "mouse tracking disabled emits no bytes");

    modes.mouse_tracking = term::Terminal_input_mouse_tracking_mode::X10;
    ok &= check_bytes_equal(
        term::encode_terminal_mouse_event({
            term::Terminal_mouse_event_kind::PRESS,
            term::Terminal_mouse_button::LEFT,
            3,
            4,
            Qt::NoModifier,
        }, modes),
        {},
        "X10 mouse tracking emits no SGR bytes");

    modes.mouse_tracking      = term::Terminal_input_mouse_tracking_mode::NORMAL;
    modes.sgr_mouse_encoding  = false;
    ok                       &= check_bytes_equal(
        term::encode_terminal_mouse_event({
            term::Terminal_mouse_event_kind::PRESS,
            term::Terminal_mouse_button::LEFT,
            3,
            4,
            Qt::NoModifier,
        }, modes),
        {},
        "non-SGR mouse mode emits no bytes");

    modes.sgr_mouse_encoding  = true;
    ok                       &= check_bytes_equal(
        term::encode_terminal_mouse_event({
            term::Terminal_mouse_event_kind::PRESS,
            term::Terminal_mouse_button::LEFT,
            3,
            4,
            Qt::NoModifier,
        }, modes),
        QByteArrayLiteral("\x1b[<0;5;4M"),
        "normal SGR left press uses one-based wire coordinates");
    ok                       &= check_bytes_equal(
        term::encode_terminal_mouse_event({
            term::Terminal_mouse_event_kind::RELEASE,
            term::Terminal_mouse_button::LEFT,
            3,
            4,
            Qt::NoModifier,
        }, modes),
        QByteArrayLiteral("\x1b[<0;5;4m"),
        "normal SGR left release uses final m");
    ok                       &= check_bytes_equal(
        term::encode_terminal_mouse_event({
            term::Terminal_mouse_event_kind::PRESS,
            term::Terminal_mouse_button::RIGHT,
            0,
            0,
            Qt::ShiftModifier | Qt::AltModifier | Qt::ControlModifier,
        }, modes),
        QByteArrayLiteral("\x1b[<30;1;1M"),
        "SGR mouse modifier bits add Shift Alt Ctrl");
    ok                       &= check_bytes_equal(
        term::encode_terminal_mouse_event({
            term::Terminal_mouse_event_kind::DRAG,
            term::Terminal_mouse_button::LEFT,
            2,
            3,
            Qt::NoModifier,
        }, modes),
        {},
        "normal tracking does not report drag");

    modes.mouse_tracking = term::Terminal_input_mouse_tracking_mode::BUTTON_EVENT;
    ok &= check_bytes_equal(
        term::encode_terminal_mouse_event({
            term::Terminal_mouse_event_kind::DRAG,
            term::Terminal_mouse_button::LEFT,
            2,
            3,
            Qt::NoModifier,
        }, modes),
        QByteArrayLiteral("\x1b[<32;4;3M"),
        "button-event tracking reports drag");

    modes.mouse_tracking  = term::Terminal_input_mouse_tracking_mode::ANY_EVENT;
    ok                   &= check_bytes_equal(
        term::encode_terminal_mouse_event({
            term::Terminal_mouse_event_kind::MOVE,
            term::Terminal_mouse_button::NONE,
            1,
            2,
            Qt::NoModifier,
        }, modes),
        QByteArrayLiteral("\x1b[<35;3;2M"),
        "any-event tracking reports passive motion");
    ok                   &= check_bytes_equal(
        term::encode_terminal_mouse_event({
            term::Terminal_mouse_event_kind::WHEEL,
            term::Terminal_mouse_button::WHEEL_UP,
            0,
            0,
            Qt::NoModifier,
        }, modes),
        QByteArrayLiteral("\x1b[<64;1;1M"),
        "SGR mouse reports wheel up");
    ok                   &= check_bytes_equal(
        term::encode_terminal_mouse_event({
            term::Terminal_mouse_event_kind::WHEEL,
            term::Terminal_mouse_button::WHEEL_DOWN,
            0,
            0,
            Qt::NoModifier,
        }, modes),
        QByteArrayLiteral("\x1b[<65;1;1M"),
        "SGR mouse reports wheel down");

    return ok;
}

bool test_sgr_mouse_modifier_button_matrix()
{
    struct mouse_modifier_case_t
    {
        Qt::KeyboardModifiers  modifiers;
        int                    wire_bits = 0;
        const char*            label     = "";
    };

    struct mouse_button_case_t
    {
        term::Terminal_mouse_button button;
        int                         wire_code = 0;
        const char*                 label     = "";
    };

    static const std::vector<mouse_modifier_case_t> k_modifier_cases = {
        { Qt::NoModifier, 0, "none" },
        { Qt::ShiftModifier, 4, "shift" },
        { Qt::AltModifier, 8, "alt" },
        { Qt::ControlModifier, 16, "ctrl" },
        {
            Qt::ShiftModifier | Qt::AltModifier | Qt::ControlModifier,
            28,
            "shift-alt-ctrl",
        },
    };
    static const std::vector<mouse_button_case_t> k_button_cases = {
        { term::Terminal_mouse_button::LEFT,   0, "left"   },
        { term::Terminal_mouse_button::MIDDLE, 1, "middle" },
        { term::Terminal_mouse_button::RIGHT,  2, "right"  },
    };

    term::Terminal_input_mode_state modes;
    modes.mouse_tracking     = term::Terminal_input_mouse_tracking_mode::NORMAL;
    modes.sgr_mouse_encoding = true;

    bool ok = true;
    for (const mouse_modifier_case_t& modifier_case : k_modifier_cases) {
        for (const mouse_button_case_t& button_case : k_button_cases) {
            const int wire_code = button_case.wire_code + modifier_case.wire_bits;
            const std::string label =
                std::string("SGR mouse ") + modifier_case.label + ' ' + button_case.label;

            ok &= check_bytes_equal(
                term::encode_terminal_mouse_event({
                    term::Terminal_mouse_event_kind::PRESS,
                    button_case.button,
                    5,
                    7,
                    modifier_case.modifiers,
                }, modes),
                sgr_mouse_report(wire_code, 5, 7, 'M'),
                label + " press");
            ok &= check_bytes_equal(
                term::encode_terminal_mouse_event({
                    term::Terminal_mouse_event_kind::RELEASE,
                    button_case.button,
                    5,
                    7,
                    modifier_case.modifiers,
                }, modes),
                sgr_mouse_report(wire_code, 5, 7, 'm'),
                label + " release");
        }
    }

    return ok;
}

bool test_mouse_tracking_mode_matrix()
{
    struct mouse_tracking_case_t
    {
        term::Terminal_input_mouse_tracking_mode tracking_mode;
        term::Terminal_mouse_event_kind          event_kind;
        term::Terminal_mouse_button              button;
        bool                                     emits      = false;
        int                                      wire_code  = 0;
        char                                     final_byte = 'M';
        const char*                              label      = "";
    };

    static const std::vector<mouse_tracking_case_t> k_tracking_cases = {
        {
            term::Terminal_input_mouse_tracking_mode::NORMAL,
            term::Terminal_mouse_event_kind::PRESS,
            term::Terminal_mouse_button::LEFT,
            true,
            0,
            'M',
            "normal press",
        },
        {
            term::Terminal_input_mouse_tracking_mode::NORMAL,
            term::Terminal_mouse_event_kind::RELEASE,
            term::Terminal_mouse_button::LEFT,
            true,
            0,
            'm',
            "normal release",
        },
        {
            term::Terminal_input_mouse_tracking_mode::NORMAL,
            term::Terminal_mouse_event_kind::DRAG,
            term::Terminal_mouse_button::LEFT,
            false,
            0,
            'M',
            "normal drag",
        },
        {
            term::Terminal_input_mouse_tracking_mode::NORMAL,
            term::Terminal_mouse_event_kind::MOVE,
            term::Terminal_mouse_button::NONE,
            false,
            0,
            'M',
            "normal move",
        },
        {
            term::Terminal_input_mouse_tracking_mode::NORMAL,
            term::Terminal_mouse_event_kind::WHEEL,
            term::Terminal_mouse_button::WHEEL_UP,
            true,
            64,
            'M',
            "normal wheel",
        },
        {
            term::Terminal_input_mouse_tracking_mode::BUTTON_EVENT,
            term::Terminal_mouse_event_kind::DRAG,
            term::Terminal_mouse_button::LEFT,
            true,
            32,
            'M',
            "button-event drag",
        },
        {
            term::Terminal_input_mouse_tracking_mode::BUTTON_EVENT,
            term::Terminal_mouse_event_kind::MOVE,
            term::Terminal_mouse_button::NONE,
            false,
            0,
            'M',
            "button-event move",
        },
        {
            term::Terminal_input_mouse_tracking_mode::ANY_EVENT,
            term::Terminal_mouse_event_kind::DRAG,
            term::Terminal_mouse_button::LEFT,
            true,
            32,
            'M',
            "any-event drag",
        },
        {
            term::Terminal_input_mouse_tracking_mode::ANY_EVENT,
            term::Terminal_mouse_event_kind::MOVE,
            term::Terminal_mouse_button::NONE,
            true,
            35,
            'M',
            "any-event move",
        },
    };

    bool ok = true;
    for (const mouse_tracking_case_t& tracking_case : k_tracking_cases) {
        term::Terminal_input_mode_state modes;
        modes.mouse_tracking     = tracking_case.tracking_mode;
        modes.sgr_mouse_encoding = true;

        const QByteArray expected = tracking_case.emits
            ? sgr_mouse_report(tracking_case.wire_code, 1, 2, tracking_case.final_byte)
            : QByteArray();
        ok &= check_bytes_equal(
            term::encode_terminal_mouse_event({
                tracking_case.event_kind,
                tracking_case.button,
                1,
                2,
                Qt::NoModifier,
            }, modes),
            expected,
            std::string("SGR mouse tracking matrix ") + tracking_case.label);

        modes.sgr_mouse_encoding = false;
        ok &= check_bytes_equal(
            term::encode_terminal_mouse_event({
                tracking_case.event_kind,
                tracking_case.button,
                1,
                2,
                Qt::NoModifier,
            }, modes),
            {},
            std::string("legacy mouse encoding unsupported for ") + tracking_case.label);
    }

    return ok;
}

}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    bool ok = true;
    ok &= test_control_and_altgr();
    ok &= test_cursor_and_navigation_modes();
    ok &= test_keypad_policy();
    ok &= test_key_preclassification();
    ok &= test_paste_framing_policy();
    ok &= test_paste_sanitization();
    ok &= test_sgr_mouse_reporting();
    ok &= test_sgr_mouse_modifier_button_matrix();
    ok &= test_mouse_tracking_mode_matrix();
    return ok ? 0 : 1;
}
