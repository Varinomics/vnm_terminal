#pragma once

#include <QByteArrayView>

namespace vnm_terminal::internal {

struct Terminal_utf8_scan_state
{
    int            continuation_remaining = 0;
    unsigned char  next_minimum           = 0x80U;
    unsigned char  next_maximum           = 0xbfU;
};

inline void reset_utf8_scan_state(Terminal_utf8_scan_state& state)
{
    state = {};
}

inline bool utf8_scan_start_sequence(
    unsigned char              byte,
    Terminal_utf8_scan_state&  state)
{
    reset_utf8_scan_state(state);

    if (byte >= 0xc2U && byte <= 0xdfU) {
        state.continuation_remaining = 1;
        return true;
    }

    if (byte == 0xe0U) {
        state.continuation_remaining = 2;
        state.next_minimum           = 0xa0U;
        return true;
    }

    if ((byte >= 0xe1U && byte <= 0xecU) || (byte >= 0xeeU && byte <= 0xefU)) {
        state.continuation_remaining = 2;
        return true;
    }

    if (byte == 0xedU) {
        state.continuation_remaining = 2;
        state.next_maximum           = 0x9fU;
        return true;
    }

    if (byte == 0xf0U) {
        state.continuation_remaining = 3;
        state.next_minimum           = 0x90U;
        return true;
    }

    if (byte >= 0xf1U && byte <= 0xf3U) {
        state.continuation_remaining = 3;
        return true;
    }

    if (byte == 0xf4U) {
        state.continuation_remaining = 3;
        state.next_maximum           = 0x8fU;
        return true;
    }

    return false;
}

inline bool utf8_scan_consumes_byte(
    unsigned char              byte,
    Terminal_utf8_scan_state&  state)
{
    if (state.continuation_remaining > 0) {
        if (byte >= state.next_minimum && byte <= state.next_maximum) {
            --state.continuation_remaining;
            state.next_minimum = 0x80U;
            state.next_maximum = 0xbfU;
            return true;
        }

        reset_utf8_scan_state(state);
        return false;
    }

    return utf8_scan_start_sequence(byte, state);
}

inline Terminal_utf8_scan_state utf8_scan_state_after(
    QByteArrayView             bytes,
    Terminal_utf8_scan_state   initial_state)
{
    Terminal_utf8_scan_state state = initial_state;
    for (const char byte : bytes) {
        utf8_scan_consumes_byte(static_cast<unsigned char>(byte), state);
    }

    return state;
}

}
