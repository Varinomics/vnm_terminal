#pragma once

#include <QString>
#include <utility>

namespace vnm_terminal::internal {

enum class Terminal_ime_result_code
{
    OK,
    EMPTY_COMMIT,
};

struct Ime_preedit_state
{
    QString    text;
    int        cursor_position = 0;
    bool       active          = false;
};

inline bool same_ime_preedit_state(
    const Ime_preedit_state&   lhs,
    const Ime_preedit_state&   rhs)
{
    return
        lhs.text            == rhs.text            &&
        lhs.cursor_position == rhs.cursor_position &&
        lhs.active          == rhs.active;
}

inline bool ime_preedit_has_content(const Ime_preedit_state& state)
{
    return state.active || !state.text.isEmpty();
}

struct Ime_commit_event
{
    QString                    text;
};

struct Terminal_ime_result
{
    Terminal_ime_result_code   code = Terminal_ime_result_code::OK;
    Ime_commit_event           commit;
};

class Ime_preedit_controller_contract
{
public:
    const Ime_preedit_state& state() const { return m_state; }

    void set_preedit(QString text, int cursor_position)
    {
        m_state.text            = std::move(text);
        m_state.cursor_position = cursor_position;
        m_state.active          = !m_state.text.isEmpty();
    }

    Terminal_ime_result commit(QString text)
    {
        if (text.isEmpty()) {
            return {Terminal_ime_result_code::EMPTY_COMMIT, {}};
        }

        cancel();
        return {Terminal_ime_result_code::OK, Ime_commit_event{std::move(text)}};
    }

    void cancel()
    {
        m_state = {};
    }

    void cancel_on_focus_loss()
    {
        cancel();
    }

private:
    Ime_preedit_state m_state;
};

}
