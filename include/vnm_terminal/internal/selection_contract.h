#pragma once

#include <QString>
#include <QByteArray>
#include <cstddef>
#include <cstdint>
#include <span>

namespace vnm_terminal::internal {

enum class Terminal_selection_mode
{
    NONE,
    NORMAL,
    WORD,
    LINE,
};

enum class Terminal_selection_result_code
{
    OK,
    NO_SELECTION,
    INVALID_RANGE,
};

enum class Terminal_osc52_policy
{
    DENY,
    REQUEST,
    ALLOW,
};

enum class Terminal_clipboard_response_decision
{
    DENY,
    ALLOW,
};

struct terminal_grid_position_t
{
    int                        row    = 0;
    int                        column = 0;
};

struct Terminal_selection_range
{
    terminal_grid_position_t   start;
    terminal_grid_position_t   end;
    Terminal_selection_mode    mode   = Terminal_selection_mode::NORMAL;
};

inline bool operator==(
    terminal_grid_position_t lhs,
    terminal_grid_position_t rhs)
{
    return lhs.row == rhs.row && lhs.column == rhs.column;
}

inline bool operator!=(
    terminal_grid_position_t lhs,
    terminal_grid_position_t rhs)
{
    return !(lhs == rhs);
}

inline bool operator==(
    const Terminal_selection_range& lhs,
    const Terminal_selection_range& rhs)
{
    return lhs.start == rhs.start && lhs.end == rhs.end && lhs.mode == rhs.mode;
}

inline bool operator!=(
    const Terminal_selection_range& lhs,
    const Terminal_selection_range& rhs)
{
    return !(lhs == rhs);
}

struct Terminal_selection_result
{
    Terminal_selection_result_code code = Terminal_selection_result_code::OK;
    QString                        text;
};

struct Terminal_osc52_write_request
{
    std::uint64_t  request_id       = 0U;
    QString        target_selection = QStringLiteral("clipboard");
    QByteArray     decoded_payload;
    std::size_t    raw_payload_size = 0U;
    QString        source_sequence;
};

struct Terminal_osc52_write_response
{
    std::uint64_t  request_id       = 0U;
    Terminal_clipboard_response_decision decision =
        Terminal_clipboard_response_decision::DENY;
};

class Selection_contract_controller
{
public:
    bool                            has_selection() const { return m_has_selection; }
    const Terminal_selection_range& range()         const { return m_range;         }

    void begin(terminal_grid_position_t anchor);
    void extend(terminal_grid_position_t extent);
    void clear();
    void set_range(Terminal_selection_range range);
    bool apply_scrollback_eviction(int evicted_rows);

    Terminal_selection_result selected_text(std::span<const QString> logical_rows) const;

private:
    bool                       m_has_selection = false;
    Terminal_selection_range   m_range;
};

}
