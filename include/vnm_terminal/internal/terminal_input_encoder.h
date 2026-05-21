#pragma once

#include "vnm_terminal/internal/terminal_input_mode.h"
#include <QByteArray>
#include <QString>
#include <Qt>

class QKeyEvent;

namespace vnm_terminal::internal {

enum class Terminal_paste_framing_policy
{
    DISABLED,
    APPLICATION_CONTROLLED,
    ENABLED,
};

enum class Terminal_mouse_event_kind
{
    PRESS,
    RELEASE,
    DRAG,
    MOVE,
    WHEEL,
};

enum class Terminal_mouse_button
{
    LEFT,
    MIDDLE,
    RIGHT,
    NONE,
    WHEEL_UP,
    WHEEL_DOWN,
};

struct Terminal_mouse_event
{
    Terminal_mouse_event_kind  kind      = Terminal_mouse_event_kind::MOVE;
    Terminal_mouse_button      button    = Terminal_mouse_button::NONE;
    int                        row       = 0;
    int                        column    = 0;
    Qt::KeyboardModifiers      modifiers = Qt::NoModifier;
};

QByteArray encode_terminal_key_event(
    const QKeyEvent&               event,
    Terminal_input_mode_state      modes);

bool terminal_key_event_may_encode(
    const QKeyEvent&               event);

QByteArray encode_terminal_mouse_event(
    Terminal_mouse_event           event,
    Terminal_input_mode_state      modes);

QByteArray encode_terminal_paste_text(
    QString                        text,
    Terminal_input_mode_state      modes,
    Terminal_paste_framing_policy  framing_policy);

}
