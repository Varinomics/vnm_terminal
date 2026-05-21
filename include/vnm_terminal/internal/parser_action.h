#pragma once

#include "vnm_terminal/internal/selection_contract.h"
#include "vnm_terminal/internal/terminal_style.h"
#include <QString>
#include <QByteArray>
#include <variant>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace vnm_terminal::internal {

constexpr std::size_t k_osc_payload_limit_bytes              = 1024U * 1024U;
constexpr std::size_t k_dcs_payload_limit_bytes              = 1024U * 1024U;
constexpr std::size_t k_apc_payload_limit_bytes              = 1024U * 1024U;
constexpr std::size_t k_pm_payload_limit_bytes               = 1024U * 1024U;
constexpr std::size_t k_sos_payload_limit_bytes              = 1024U * 1024U;
constexpr std::size_t k_control_sequence_pending_limit_bytes = 4096U;
constexpr std::size_t k_csi_parameter_group_limit            = 128U;
constexpr std::size_t k_csi_parameter_atom_limit             = 256U;
constexpr std::size_t k_csi_parameter_digit_limit            = 9U;
constexpr std::size_t k_title_scalar_limit                   = 4096U;

enum class Parser_action_kind
{
    SCREEN_MUTATION,
    STYLE_MUTATION,
    CONTROL_SEQUENCE,
    TERMINAL_REPLY,
    TERMINAL_QUERY,
    DIAGNOSTIC,
    NOTIFICATION,
    HOST_REQUEST,
};

enum class Parser_sequence_family
{
    NONE,
    PRINTABLE,
    ESC,
    CSI,
    OSC,
    DCS,
    APC,
    PM,
    SOS,
};

inline bool is_string_family(Parser_sequence_family family)
{
    return
        family == Parser_sequence_family::OSC ||
        family == Parser_sequence_family::DCS ||
        family == Parser_sequence_family::APC ||
        family == Parser_sequence_family::PM  ||
        family == Parser_sequence_family::SOS;
}

inline std::size_t payload_limit_for_family(Parser_sequence_family family)
{
    switch (family) {
        case Parser_sequence_family::OSC: return k_osc_payload_limit_bytes;
        case Parser_sequence_family::DCS: return k_dcs_payload_limit_bytes;
        case Parser_sequence_family::APC: return k_apc_payload_limit_bytes;
        case Parser_sequence_family::PM:  return k_pm_payload_limit_bytes;
        case Parser_sequence_family::SOS: return k_sos_payload_limit_bytes;
        case Parser_sequence_family::NONE:
        case Parser_sequence_family::PRINTABLE:
        case Parser_sequence_family::ESC:
        case Parser_sequence_family::CSI:
            return 0U;
    }

    return 0U;
}

inline QString source_name_for_family(Parser_sequence_family family)
{
    switch (family) {
        case Parser_sequence_family::OSC:       return QStringLiteral("OSC");
        case Parser_sequence_family::DCS:       return QStringLiteral("DCS");
        case Parser_sequence_family::APC:       return QStringLiteral("APC");
        case Parser_sequence_family::PM:        return QStringLiteral("PM");
        case Parser_sequence_family::SOS:       return QStringLiteral("SOS");
        case Parser_sequence_family::CSI:       return QStringLiteral("CSI");
        case Parser_sequence_family::ESC:       return QStringLiteral("ESC");
        case Parser_sequence_family::NONE:
        case Parser_sequence_family::PRINTABLE: return QStringLiteral("unknown");
    }

    return QStringLiteral("unknown");
}

enum class Parser_string_terminator
{
    NONE,
    BEL,
    ST_7BIT,
    ST_8BIT,
    RECOVERY,
    END_OF_INPUT,
};

enum class Parser_control_sequence_action
{
    DISPATCH,
    IGNORE,
    DISCARD,
};

enum class Parser_recovery_strategy
{
    NONE,
    IGNORE_BYTE,
    DISCARD_SEQUENCE,
    DISCARD_STRING,
    RESET_TO_GROUND,
};

enum class Screen_mutation_kind
{
    PRINT_TEXT,
    CARRIAGE_RETURN,
    LINE_FEED,
    BACKSPACE,
    HORIZONTAL_TAB,
    SET_TITLE,
    SET_ICON_NAME,
    BELL,
    SET_HYPERLINK,
};

enum class Parser_diagnostic_code
{
    MALFORMED_INPUT,
    PAYLOAD_LIMIT_EXCEEDED,
    TITLE_LIMIT_EXCEEDED,
    UNSUPPORTED_SEQUENCE,
    CLIPBOARD_READ_DENIED,
    CLIPBOARD_WRITE_DENIED,
};

enum class Parser_notification_kind
{
    OUTPUT_ACTIVITY,
    TITLE_CHANGED,
    ICON_NAME_CHANGED,
    BELL_REQUESTED,
    TEXT_AREA_RESIZE_REQUESTED,
};

enum class Terminal_reply_kind
{
    RAW,
    DA1,
    DA2,
    DSR_CURSOR_POSITION,
    DECRQM,
    OSC_QUERY,
    TEXT_AREA_SIZE,
};

enum class Terminal_sgr_operation_kind
{
    RESET_ALL,
    SET_ATTRIBUTES,
    CLEAR_ATTRIBUTES,
    SET_FOREGROUND,
    SET_BACKGROUND,
};

enum class Terminal_color_query_kind
{
    DEFAULT_FOREGROUND,
    DEFAULT_BACKGROUND,
    CURSOR,
    PALETTE_INDEX,
};

struct Screen_print_text_mutation
{
    QString    text;
    int        row    = 0;
    int        column = 0;
};

struct Screen_carriage_return_mutation
{};

struct Screen_line_feed_mutation
{};

struct Screen_backspace_mutation
{};

struct Screen_horizontal_tab_mutation
{};

struct Screen_set_title_mutation
{
    QString    title;
};

struct Screen_set_icon_name_mutation
{
    QString    icon_name;
};

struct Screen_bell_mutation
{};

struct Screen_set_hyperlink_mutation
{
    QByteArray identity_key;
};

using Screen_mutation = std::variant<
    Screen_print_text_mutation,
    Screen_carriage_return_mutation,
    Screen_line_feed_mutation,
    Screen_backspace_mutation,
    Screen_horizontal_tab_mutation,
    Screen_set_title_mutation,
    Screen_set_icon_name_mutation,
    Screen_bell_mutation,
    Screen_set_hyperlink_mutation>;

struct Terminal_sgr_operation
{
    Terminal_sgr_operation_kind    kind       = Terminal_sgr_operation_kind::RESET_ALL;
    std::uint16_t                  attributes = 0U;
    Terminal_color_ref             color;
};

struct Terminal_sgr_sequence
{
    std::vector<Terminal_sgr_operation> operations;
    QByteArray                          raw_parameters;
};

struct Terminal_color_query
{
    Terminal_color_query_kind      kind          = Terminal_color_query_kind::DEFAULT_FOREGROUND;
    int                            palette_index = 0;
    QString                        source_sequence;
};

struct Parser_control_sequence
{
    Parser_sequence_family         family     = Parser_sequence_family::NONE;
    Parser_control_sequence_action action     = Parser_control_sequence_action::DISPATCH;
    std::vector<int>               parameters;
    QByteArray                     private_marker;
    QByteArray                     intermediates;
    QByteArray                     final_bytes;
    QByteArray                     payload;
    Parser_string_terminator       terminator = Parser_string_terminator::NONE;
    QByteArray                     raw_bytes;
};

struct Terminal_reply
{
    QByteArray                     wire_bytes;
    QString                        source_sequence;
    Terminal_reply_kind            kind          = Terminal_reply_kind::RAW;
    Parser_sequence_family         source_family = Parser_sequence_family::NONE;
};

struct Parser_payload_diagnostic
{
    Parser_diagnostic_code         code             = Parser_diagnostic_code::MALFORMED_INPUT;
    QString                        source_sequence;
    std::size_t                    raw_payload_size = 0U;
    std::size_t                    limit_bytes      = 0U;
    Parser_sequence_family         family           = Parser_sequence_family::NONE;
    Parser_recovery_strategy       recovery         = Parser_recovery_strategy::NONE;
};

struct Parser_notification
{
    Parser_notification_kind       kind    = Parser_notification_kind::OUTPUT_ACTIVITY;
    QString                        text;
    int                            rows    = 0;
    int                            columns = 0;
};

struct Parser_action
{
    std::variant<
        Screen_mutation,
        Terminal_sgr_sequence,
        Parser_control_sequence,
        Terminal_reply,
        Terminal_color_query,
        Parser_payload_diagnostic,
        Parser_notification,
        Terminal_osc52_write_request
    > payload = Screen_mutation{};
};

inline Screen_mutation_kind screen_mutation_kind(const Screen_mutation& mutation)
{
    struct visitor_t
    {
        Screen_mutation_kind operator()(const Screen_print_text_mutation&) const
        {
            return Screen_mutation_kind::PRINT_TEXT;
        }

        Screen_mutation_kind operator()(const Screen_carriage_return_mutation&) const
        {
            return Screen_mutation_kind::CARRIAGE_RETURN;
        }

        Screen_mutation_kind operator()(const Screen_line_feed_mutation&) const
        {
            return Screen_mutation_kind::LINE_FEED;
        }

        Screen_mutation_kind operator()(const Screen_backspace_mutation&) const
        {
            return Screen_mutation_kind::BACKSPACE;
        }

        Screen_mutation_kind operator()(const Screen_horizontal_tab_mutation&) const
        {
            return Screen_mutation_kind::HORIZONTAL_TAB;
        }

        Screen_mutation_kind operator()(const Screen_set_title_mutation&) const
        {
            return Screen_mutation_kind::SET_TITLE;
        }

        Screen_mutation_kind operator()(const Screen_set_icon_name_mutation&) const
        {
            return Screen_mutation_kind::SET_ICON_NAME;
        }

        Screen_mutation_kind operator()(const Screen_bell_mutation&) const
        {
            return Screen_mutation_kind::BELL;
        }

        Screen_mutation_kind operator()(const Screen_set_hyperlink_mutation&) const
        {
            return Screen_mutation_kind::SET_HYPERLINK;
        }
    };

    return std::visit(visitor_t{}, mutation);
}

inline Parser_action_kind parser_action_kind(const Parser_action& action)
{
    struct visitor_t
    {
        Parser_action_kind operator()(const Screen_mutation&) const
        {
            return Parser_action_kind::SCREEN_MUTATION;
        }

        Parser_action_kind operator()(const Terminal_sgr_sequence&) const
        {
            return Parser_action_kind::STYLE_MUTATION;
        }

        Parser_action_kind operator()(const Parser_control_sequence&) const
        {
            return Parser_action_kind::CONTROL_SEQUENCE;
        }

        Parser_action_kind operator()(const Terminal_reply&) const
        {
            return Parser_action_kind::TERMINAL_REPLY;
        }

        Parser_action_kind operator()(const Terminal_color_query&) const
        {
            return Parser_action_kind::TERMINAL_QUERY;
        }

        Parser_action_kind operator()(const Parser_payload_diagnostic&) const
        {
            return Parser_action_kind::DIAGNOSTIC;
        }

        Parser_action_kind operator()(const Parser_notification&) const
        {
            return Parser_action_kind::NOTIFICATION;
        }

        Parser_action_kind operator()(const Terminal_osc52_write_request&) const
        {
            return Parser_action_kind::HOST_REQUEST;
        }
    };

    return std::visit(visitor_t{}, action.payload);
}

inline Parser_action make_print_text_action(QString text, int row, int column)
{
    return {
        Screen_mutation{Screen_print_text_mutation{std::move(text), row, column}},
    };
}

inline Parser_action make_sgr_action(Terminal_sgr_sequence sequence)
{
    return {
        std::move(sequence),
    };
}

inline Parser_action make_color_query_action(
    Terminal_color_query_kind  kind,
    int                        palette_index,
    QString                    source_sequence)
{
    return {
        Terminal_color_query{kind, palette_index, std::move(source_sequence)},
    };
}

inline Parser_action make_escape_dispatch_action(QByteArray final_bytes, QByteArray raw_bytes = {})
{
    return {
        Parser_control_sequence{
            Parser_sequence_family::ESC,
            Parser_control_sequence_action::DISPATCH,
            {},
            {},
            {},
            std::move(final_bytes),
            {},
            Parser_string_terminator::NONE,
            std::move(raw_bytes),
        },
    };
}

inline QByteArray encode_csi_parameter_payload(const std::vector<int>& parameters)
{
    QByteArray payload;
    for (std::size_t i = 0; i < parameters.size(); ++i) {
        if (i > 0U) {
            payload.append(';');
        }
        payload.append(QByteArray::number(parameters[i]));
    }
    return payload;
}

inline Parser_action make_csi_control_sequence_action(
    Parser_control_sequence_action action,
    std::vector<int>               parameters,
    QByteArray                     final_bytes,
    QByteArray                     private_marker = {},
    QByteArray                     intermediates = {},
    QByteArray                     raw_bytes = {})
{
    QByteArray payload = encode_csi_parameter_payload(parameters);
    return {
        Parser_control_sequence{
            Parser_sequence_family::CSI,
            action,
            std::move(parameters),
            std::move(private_marker),
            std::move(intermediates),
            std::move(final_bytes),
            std::move(payload),
            Parser_string_terminator::NONE,
            std::move(raw_bytes),
        },
    };
}

inline Parser_action make_csi_dispatch_action(
    std::vector<int>   parameters,
    QByteArray         final_bytes,
    QByteArray         private_marker = {},
    QByteArray         intermediates = {},
    QByteArray         raw_bytes = {})
{
    return
        make_csi_control_sequence_action(
            Parser_control_sequence_action::DISPATCH,
            std::move(parameters),
            std::move(final_bytes),
            std::move(private_marker),
            std::move(intermediates),
            std::move(raw_bytes));
}

inline Parser_action make_string_sequence_action(
    Parser_sequence_family     family,
    QByteArray                 payload,
    Parser_string_terminator   terminator,
    QByteArray                 raw_bytes = {})
{
    return {
        Parser_control_sequence{
            family,
            Parser_control_sequence_action::DISPATCH,
            {},
            {},
            {},
            {},
            std::move(payload),
            terminator,
            std::move(raw_bytes),
        },
    };
}

inline Parser_sequence_family source_family_for_reply_kind(Terminal_reply_kind kind)
{
    switch (kind) {
        case Terminal_reply_kind::RAW:
            return Parser_sequence_family::NONE;
        case Terminal_reply_kind::OSC_QUERY:
            return Parser_sequence_family::OSC;
        case Terminal_reply_kind::DA1:
        case Terminal_reply_kind::DA2:
        case Terminal_reply_kind::DSR_CURSOR_POSITION:
        case Terminal_reply_kind::DECRQM:
        case Terminal_reply_kind::TEXT_AREA_SIZE:
            return Parser_sequence_family::CSI;
    }

    return Parser_sequence_family::NONE;
}

inline Parser_action make_terminal_reply_action(
    Terminal_reply_kind    kind,
    QByteArray             wire_bytes,
    QString                source_sequence)
{
    return {
        Terminal_reply{
            std::move(wire_bytes),
            std::move(source_sequence),
            kind,
            source_family_for_reply_kind(kind),
        },
    };
}

inline Parser_action make_da1_reply_action(QByteArray wire_bytes)
{
    return make_terminal_reply_action(
        Terminal_reply_kind::DA1, std::move(wire_bytes), QStringLiteral("DA1"));
}

inline Parser_action make_da2_reply_action(QByteArray wire_bytes)
{
    return make_terminal_reply_action(
        Terminal_reply_kind::DA2, std::move(wire_bytes), QStringLiteral("DA2"));
}

inline Parser_action make_dsr_cursor_position_reply_action(int row, int column)
{
    return
        make_terminal_reply_action(
            Terminal_reply_kind::DSR_CURSOR_POSITION,
            QByteArray("\x1b[") + QByteArray::number(row) + ';' + QByteArray::number(column) + 'R',
            QStringLiteral("DSR cursor position"));
}

inline Parser_action make_decrqm_reply_action(int mode, int status)
{
    return
        make_terminal_reply_action(
            Terminal_reply_kind::DECRQM,
            QByteArray("\x1b[?") + QByteArray::number(mode) + ';' + QByteArray::number(status) + "$y",
            QStringLiteral("DECRQM"));
}

inline Parser_action make_text_area_size_reply_action(int rows, int columns)
{
    return
        make_terminal_reply_action(
            Terminal_reply_kind::TEXT_AREA_SIZE,
            QByteArray("\x1b[8;") + QByteArray::number(rows) + ';' + QByteArray::number(columns) + 't',
            QStringLiteral("CSI 18 t"));
}

inline Parser_action make_osc_query_reply_action(QByteArray wire_bytes, QString source_sequence)
{
    return {
        Terminal_reply{
            std::move(wire_bytes),
            std::move(source_sequence),
            Terminal_reply_kind::OSC_QUERY,
            Parser_sequence_family::OSC,
        },
    };
}

inline Parser_action make_osc52_write_request_action(
    std::uint64_t          request_id,
    QString                target_selection,
    QByteArray             decoded_payload,
    std::size_t            raw_payload_size,
    QString                source_sequence)
{
    return {
        Terminal_osc52_write_request{
            request_id,
            std::move(target_selection),
            std::move(decoded_payload),
            raw_payload_size,
            std::move(source_sequence),
        },
    };
}

inline Parser_action make_payload_limit_diagnostic(
    QString                source_sequence,
    std::size_t            raw_payload_size,
    std::size_t            limit_bytes,
    Parser_sequence_family family = Parser_sequence_family::NONE)
{
    return {
        Parser_payload_diagnostic{
            Parser_diagnostic_code::PAYLOAD_LIMIT_EXCEEDED,
            std::move(source_sequence),
            raw_payload_size,
            limit_bytes,
            family,
            Parser_recovery_strategy::DISCARD_STRING,
        },
    };
}

inline Parser_action make_string_family_payload_limit_diagnostic(
    Parser_sequence_family family,
    std::size_t            raw_payload_size)
{
    return
        make_payload_limit_diagnostic(
            source_name_for_family(family),
            raw_payload_size,
            payload_limit_for_family(family),
            family);
}

inline Parser_action make_dcs_payload_limit_diagnostic(std::size_t raw_payload_size)
{
    return make_string_family_payload_limit_diagnostic(
        Parser_sequence_family::DCS,
        raw_payload_size);
}

inline Parser_action make_osc_payload_limit_diagnostic(std::size_t raw_payload_size)
{
    return make_string_family_payload_limit_diagnostic(
        Parser_sequence_family::OSC,
        raw_payload_size);
}

inline Parser_action make_apc_payload_limit_diagnostic(std::size_t raw_payload_size)
{
    return make_string_family_payload_limit_diagnostic(
        Parser_sequence_family::APC,
        raw_payload_size);
}

inline Parser_action make_pm_payload_limit_diagnostic(std::size_t raw_payload_size)
{
    return make_string_family_payload_limit_diagnostic(
        Parser_sequence_family::PM,
        raw_payload_size);
}

inline Parser_action make_sos_payload_limit_diagnostic(std::size_t raw_payload_size)
{
    return make_string_family_payload_limit_diagnostic(
        Parser_sequence_family::SOS,
        raw_payload_size);
}

inline Parser_action make_title_limit_diagnostic(
    std::size_t                raw_title_scalar_count,
    QString                    source_sequence = QStringLiteral("OSC title"))
{
    return {
        Parser_payload_diagnostic{
            Parser_diagnostic_code::TITLE_LIMIT_EXCEEDED,
            std::move(source_sequence),
            raw_title_scalar_count,
            k_title_scalar_limit,
            Parser_sequence_family::OSC,
            Parser_recovery_strategy::DISCARD_STRING,
        },
    };
}

inline Parser_action make_malformed_recovery_diagnostic(
    QString                    source_sequence,
    Parser_sequence_family     family,
    Parser_recovery_strategy   recovery)
{
    return {
        Parser_payload_diagnostic{
            Parser_diagnostic_code::MALFORMED_INPUT,
            std::move(source_sequence),
            0U,
            0U,
            family,
            recovery,
        },
    };
}

inline bool is_diagnostic_action(const Parser_action& action)
{
    return parser_action_kind(action) == Parser_action_kind::DIAGNOSTIC;
}

}
