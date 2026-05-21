#include "vnm_terminal/internal/terminal_input_encoder.h"

#include "vnm_terminal/internal/hierarchical_profiler.h"
#include <QKeyEvent>
#include <Qt>
#include <QtGlobal>
#include <array>
#include <utility>

namespace vnm_terminal::internal {

namespace {

constexpr Qt::KeyboardModifiers k_terminal_modifier_mask =
    Qt::ShiftModifier | Qt::AltModifier | Qt::ControlModifier;

constexpr ushort k_c0_control_max = 0x001fU;
constexpr ushort k_del_control    = 0x007fU;
constexpr ushort k_c1_control_min = 0x0080U;
constexpr ushort k_c1_control_max = 0x009fU;

QString sanitize_paste_text(QString text)
{
    QString sanitized;
    sanitized.reserve(text.size());

    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar  ch   = text.at(i);
        const ushort code = ch.unicode();
        if (code == u'\r') {
            sanitized.append(QChar(u'\n'));
            if (i + 1 < text.size() && text.at(i + 1) == QChar(u'\n')) {
                ++i;
            }
            continue;
        }

        if (code == u'\n' || code == u'\t') {
            sanitized.append(ch);
            continue;
        }

        if (code <= k_c0_control_max ||
            code == k_del_control    ||
            (code >= k_c1_control_min && code <= k_c1_control_max))
        {
            continue;
        }

        sanitized.append(ch);
    }

    return sanitized;
}

bool should_frame_paste(
    Terminal_input_mode_state      modes,
    Terminal_paste_framing_policy  framing_policy)
{
    switch (framing_policy) {
        case Terminal_paste_framing_policy::DISABLED:               return false;
        case Terminal_paste_framing_policy::APPLICATION_CONTROLLED: return modes.bracketed_paste;
        case Terminal_paste_framing_policy::ENABLED:                return true;
    }

    Q_UNREACHABLE();
    return false;
}

QByteArray ss3(char final_byte)
{
    QByteArray bytes = QByteArrayLiteral("\x1bO");
    bytes.append(final_byte);
    return bytes;
}

QByteArray csi_final(int first_parameter, int modifier_parameter, char final_byte)
{
    QByteArray bytes = QByteArrayLiteral("\x1b[");
    bytes.append(QByteArray::number(first_parameter));
    bytes.append(';');
    bytes.append(QByteArray::number(modifier_parameter));
    bytes.append(final_byte);
    return bytes;
}

QByteArray csi_tilde(int first_parameter, int modifier_parameter = 0)
{
    QByteArray bytes = QByteArrayLiteral("\x1b[");
    bytes.append(QByteArray::number(first_parameter));
    if (modifier_parameter > 0) {
        bytes.append(';');
        bytes.append(QByteArray::number(modifier_parameter));
    }
    bytes.append('~');
    return bytes;
}

int mouse_modifier_bits(Qt::KeyboardModifiers modifiers)
{
    int bits = 0;
    if ((modifiers & Qt::ShiftModifier)   != Qt::NoModifier) { bits += 4;  }
    if ((modifiers & Qt::AltModifier)     != Qt::NoModifier) { bits += 8;  }
    if ((modifiers & Qt::ControlModifier) != Qt::NoModifier) { bits += 16; }
    return bits;
}

int mouse_button_code(Terminal_mouse_button button)
{
    switch (button) {
        case Terminal_mouse_button::LEFT:       return 0;
        case Terminal_mouse_button::MIDDLE:     return 1;
        case Terminal_mouse_button::RIGHT:      return 2;
        case Terminal_mouse_button::NONE:       return 3;
        case Terminal_mouse_button::WHEEL_UP:   return 64;
        case Terminal_mouse_button::WHEEL_DOWN: return 65;
    }

    Q_UNREACHABLE();
    return 3;
}

bool is_reportable_button(Terminal_mouse_button button)
{
    return
        button == Terminal_mouse_button::LEFT   ||
        button == Terminal_mouse_button::MIDDLE ||
        button == Terminal_mouse_button::RIGHT;
}

QByteArray sgr_mouse_report(const Terminal_mouse_event& event, int button_code, char final_byte)
{
    QByteArray bytes = QByteArrayLiteral("\x1b[<");
    bytes.append(QByteArray::number(button_code + mouse_modifier_bits(event.modifiers)));
    bytes.append(';');
    bytes.append(QByteArray::number(event.column + 1));
    bytes.append(';');
    bytes.append(QByteArray::number(event.row + 1));
    bytes.append(final_byte);
    return bytes;
}

Qt::KeyboardModifiers terminal_modifiers(const QKeyEvent& event)
{
    return event.modifiers() & k_terminal_modifier_mask;
}

bool has_terminal_modifiers(const QKeyEvent& event)
{
    return terminal_modifiers(event) != Qt::NoModifier;
}

int modifier_parameter(const QKeyEvent& event)
{
    int parameter = 1;
    const Qt::KeyboardModifiers modifiers = event.modifiers();
    if ((modifiers & Qt::ShiftModifier)   != Qt::NoModifier) { parameter += 1; }
    if ((modifiers & Qt::AltModifier)     != Qt::NoModifier) { parameter += 2; }
    if ((modifiers & Qt::ControlModifier) != Qt::NoModifier) { parameter += 4; }
    return parameter;
}

QByteArray alt_prefixed(QByteArray bytes, const QKeyEvent& event)
{
    if (!bytes.isEmpty() && (event.modifiers() & Qt::AltModifier) != Qt::NoModifier) {
        bytes.prepend('\x1b');
    }
    return bytes;
}

QByteArray control_key_bytes(const QKeyEvent& event)
{
    if ((event.modifiers() & Qt::ControlModifier) == Qt::NoModifier) {
        return {};
    }

    const int key = event.key();
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        return QByteArray(1, static_cast<char>(key - Qt::Key_A + 1));
    }

    switch (key) {
        case Qt::Key_At:
        case Qt::Key_Space:        return QByteArray(1, '\0');
        case Qt::Key_BracketLeft:  return QByteArray(1, '\x1b');
        case Qt::Key_Backslash:    return QByteArray(1, '\x1c');
        case Qt::Key_BracketRight: return QByteArray(1, '\x1d');
        case Qt::Key_AsciiCircum:  return QByteArray(1, '\x1e');
        case Qt::Key_Underscore:   return QByteArray(1, '\x1f');
        default:                   return {};
    }
}

QByteArray special_key_bytes(const QKeyEvent& event)
{
    switch (event.key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
            return alt_prefixed(QByteArray(1, '\r'), event);
        case Qt::Key_Tab:
            if ((event.modifiers() & Qt::ShiftModifier) != Qt::NoModifier) {
                return {};
            }
            return alt_prefixed(QByteArray(1, '\t'), event);
        case Qt::Key_Backspace:
            if ((event.modifiers() & Qt::ControlModifier) != Qt::NoModifier) {
                return alt_prefixed(QByteArray(1, '\b'), event);
            }
            return alt_prefixed(QByteArray(1, '\x7f'), event);
        case Qt::Key_Escape:
            return alt_prefixed(QByteArray(1, '\x1b'), event);
        default:
            return {};
    }
}

QByteArray arrow_key_bytes(const QKeyEvent& event, bool application_cursor_keys)
{
    char final_byte = '\0';
    switch (event.key()) {
        case Qt::Key_Up:    final_byte = 'A'; break;
        case Qt::Key_Down:  final_byte = 'B'; break;
        case Qt::Key_Right: final_byte = 'C'; break;
        case Qt::Key_Left:  final_byte = 'D'; break;
        default:            return {};
    }

    if (has_terminal_modifiers(event)) {
        return csi_final(1, modifier_parameter(event), final_byte);
    }

    if (application_cursor_keys) {
        return ss3(final_byte);
    }
    return QByteArrayLiteral("\x1b[") + final_byte;
}

QByteArray navigation_key_bytes(const QKeyEvent& event, bool application_cursor_keys)
{
    if (event.key()                                 == Qt::Key_Backtab ||
        (event.key() == Qt::Key_Tab &&
         (event.modifiers() & Qt::ShiftModifier) != Qt::NoModifier))
    {
        const Qt::KeyboardModifiers modifiers = terminal_modifiers(event);
        if (modifiers == Qt::NoModifier ||
            modifiers == Qt::ShiftModifier)
        {
            return QByteArrayLiteral("\x1b[Z");
        }
        return csi_final(1, modifier_parameter(event), 'Z');
    }

    switch (event.key()) {
        case Qt::Key_Home:
            if (has_terminal_modifiers(event)) {
                return csi_final(1, modifier_parameter(event), 'H');
            }
            return application_cursor_keys ? ss3('H') : QByteArrayLiteral("\x1b[H");
        case Qt::Key_End:
            if (has_terminal_modifiers(event)) {
                return csi_final(1, modifier_parameter(event), 'F');
            }
            return application_cursor_keys ? ss3('F') : QByteArrayLiteral("\x1b[F");
        case Qt::Key_Insert:
            return has_terminal_modifiers(event)
                ? csi_tilde(2, modifier_parameter(event))
                : csi_tilde(2);
        case Qt::Key_Delete:
            return has_terminal_modifiers(event)
                ? csi_tilde(3, modifier_parameter(event))
                : csi_tilde(3);
        case Qt::Key_PageUp:
            return has_terminal_modifiers(event)
                ? csi_tilde(5, modifier_parameter(event))
                : csi_tilde(5);
        case Qt::Key_PageDown:
            return has_terminal_modifiers(event)
                ? csi_tilde(6, modifier_parameter(event))
                : csi_tilde(6);
        default:
            return {};
    }
}

QByteArray function_key_bytes(const QKeyEvent& event)
{
    const int key = event.key();
    if (key < Qt::Key_F1 || key > Qt::Key_F12) {
        return {};
    }

    static constexpr std::array<char, 4> k_f1_to_f4_final_bytes = {'P', 'Q', 'R', 'S'};
    static constexpr std::array<int, 8> k_f5_to_f12_parameters = {
        15, 17, 18, 19, 20, 21, 23, 24,
    };

    if (key <= Qt::Key_F4) {
        const char final_byte =
            k_f1_to_f4_final_bytes[static_cast<std::size_t>(key - Qt::Key_F1)];
        return has_terminal_modifiers(event)
            ? csi_final(1, modifier_parameter(event), final_byte)
            : ss3(final_byte);
    }

    const int parameter =
        k_f5_to_f12_parameters[static_cast<std::size_t>(key - Qt::Key_F5)];
    return has_terminal_modifiers(event)
        ? csi_tilde(parameter, modifier_parameter(event))
        : csi_tilde(parameter);
}

bool is_application_keypad_key(int key)
{
    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        return true;
    }

    switch (key) {
        case Qt::Key_Period:
        case Qt::Key_Minus:
        case Qt::Key_Comma:
        case Qt::Key_Plus:
        case Qt::Key_Asterisk:
        case Qt::Key_Slash:
        case Qt::Key_Enter:
        case Qt::Key_Equal:
            return true;
        default:
            return false;
    }
}

QByteArray application_keypad_bytes(const QKeyEvent& event)
{
    if ((event.modifiers() & Qt::KeypadModifier) == Qt::NoModifier ||
        terminal_modifiers(event)                != Qt::NoModifier)
    {
        return {};
    }

    const int key = event.key();
    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        return ss3(static_cast<char>('p' + key - Qt::Key_0));
    }

    switch (key) {
        case Qt::Key_Period:   return ss3('n');
        case Qt::Key_Minus:    return ss3('m');
        case Qt::Key_Comma:    return ss3('l');
        case Qt::Key_Plus:     return ss3('k');
        case Qt::Key_Asterisk: return ss3('j');
        case Qt::Key_Slash:    return ss3('o');
        case Qt::Key_Enter:    return ss3('M');
        case Qt::Key_Equal:    return ss3('X');
        default:               return {};
    }
}

QByteArray printable_text_bytes(const QKeyEvent& event)
{
    const QString text = event.text();
    if (text.isEmpty()) {
        return {};
    }

    if ((event.modifiers() & Qt::ControlModifier) != Qt::NoModifier) {
        return {};
    }

    return alt_prefixed(text.toUtf8(), event);
}

QByteArray ctrl_alt_printable_text_bytes(const QKeyEvent& event)
{
    if ((event.modifiers() & Qt::ControlModifier) == Qt::NoModifier ||
        (event.modifiers() & Qt::AltModifier)     == Qt::NoModifier)
    {
        return {};
    }

    const QString text = event.text();
    if (text.isEmpty()) {
        return {};
    }

    const QChar first_character = text.front();
    if (first_character.unicode() < 0x20U || first_character.unicode() == 0x7fU) {
        return {};
    }

    return text.toUtf8();
}

}

QByteArray encode_terminal_key_event(
    const QKeyEvent&           event,
    Terminal_input_mode_state  modes)
{
    VNM_TERMINAL_PROFILE_SCOPE("encode_terminal_key_event");

    if (modes.application_keypad) {
        const QByteArray keypad_bytes = application_keypad_bytes(event);
        if (!keypad_bytes.isEmpty()) {
            return keypad_bytes;
        }
    }

    const QByteArray navigation_bytes =
        navigation_key_bytes(event, modes.application_cursor_keys);
    if (!navigation_bytes.isEmpty()) {
        return navigation_bytes;
    }

    const QByteArray arrow_bytes = arrow_key_bytes(event, modes.application_cursor_keys);
    if (!arrow_bytes.isEmpty()) {
        return arrow_bytes;
    }

    const QByteArray function_bytes = function_key_bytes(event);
    if (!function_bytes.isEmpty()) {
        return function_bytes;
    }

    const QByteArray special_bytes = special_key_bytes(event);
    if (!special_bytes.isEmpty()) {
        return special_bytes;
    }

    const QByteArray ctrl_alt_printable_bytes = ctrl_alt_printable_text_bytes(event);
    if (!ctrl_alt_printable_bytes.isEmpty()) {
        return ctrl_alt_printable_bytes;
    }

    const QByteArray control_bytes = control_key_bytes(event);
    if (!control_bytes.isEmpty()) {
        return alt_prefixed(control_bytes, event);
    }

    return printable_text_bytes(event);
}

bool terminal_key_event_may_encode(const QKeyEvent& event)
{
    if (!encode_terminal_key_event(event, {}).isEmpty()) {
        return true;
    }

    return
        (event.modifiers() & Qt::KeypadModifier) != Qt::NoModifier &&
         terminal_modifiers(event) == Qt::NoModifier               &&
         is_application_keypad_key(event.key());
}

QByteArray encode_terminal_mouse_event(
    Terminal_mouse_event       event,
    Terminal_input_mode_state  modes)
{
    VNM_TERMINAL_PROFILE_SCOPE("encode_terminal_mouse_event");

    // This phase only emits SGR 1006 mouse reports. Legacy coordinate encodings
    // are deliberately unsupported until the backend/input contract needs them.
    if (!modes.sgr_mouse_encoding ||
        modes.mouse_tracking == Terminal_input_mouse_tracking_mode::NONE ||
        modes.mouse_tracking == Terminal_input_mouse_tracking_mode::X10  ||
        event.row            <  0                                        ||
        event.column         <  0)
    {
        return {};
    }

    switch (event.kind) {
        case Terminal_mouse_event_kind::PRESS:
            if (!is_reportable_button(event.button)) {
                return {};
            }
            return sgr_mouse_report(event, mouse_button_code(event.button), 'M');

        case Terminal_mouse_event_kind::RELEASE:
            if (!is_reportable_button(event.button)) {
                return {};
            }
            return sgr_mouse_report(event, mouse_button_code(event.button), 'm');

        case Terminal_mouse_event_kind::DRAG:
            if (modes.mouse_tracking != Terminal_input_mouse_tracking_mode::BUTTON_EVENT &&
                modes.mouse_tracking != Terminal_input_mouse_tracking_mode::ANY_EVENT)
            {
                return {};
            }
            if (!is_reportable_button(event.button)) {
                return {};
            }
            return sgr_mouse_report(event, mouse_button_code(event.button) + 32, 'M');

        case Terminal_mouse_event_kind::MOVE:
            if (modes.mouse_tracking != Terminal_input_mouse_tracking_mode::ANY_EVENT) {
                return {};
            }
            return sgr_mouse_report(event, 35, 'M');

        case Terminal_mouse_event_kind::WHEEL:
            if (event.button != Terminal_mouse_button::WHEEL_UP &&
                event.button != Terminal_mouse_button::WHEEL_DOWN)
            {
                return {};
            }
            return sgr_mouse_report(event, mouse_button_code(event.button), 'M');
    }

    Q_UNREACHABLE();
    return {};
}

QByteArray encode_terminal_paste_text(
    QString                        text,
    Terminal_input_mode_state      modes,
    Terminal_paste_framing_policy  framing_policy)
{
    VNM_TERMINAL_PROFILE_SCOPE("encode_terminal_paste_text");

    const QByteArray body = sanitize_paste_text(std::move(text)).toUtf8();
    if (body.isEmpty() || !should_frame_paste(modes, framing_policy)) {
        return body;
    }

    QByteArray bytes;
    bytes.reserve(
        QByteArrayLiteral("\x1b[200~").size() +
        body.size() +
        QByteArrayLiteral("\x1b[201~").size());
    bytes.append(QByteArrayLiteral("\x1b[200~"));
    bytes.append(body);
    bytes.append(QByteArrayLiteral("\x1b[201~"));
    return bytes;
}

}
