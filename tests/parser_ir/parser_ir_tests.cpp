#include "vnm_terminal/internal/parser_action.h"
#include "helpers/test_check.h"

#include <QByteArray>
#include <QString>
#include <iostream>
#include <variant>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

term::Parser_action dispatch_parser_fixture(QByteArray bytes)
{
    const QByteArray osc_8bit_st_prefix = QByteArray("\x1b]0;title", 9);

    if (bytes == QByteArrayLiteral("text")) {
        return term::make_print_text_action(QStringLiteral("text"), 0, 0);
    }

    if (bytes == QByteArray("\x1b" "c", 2)) {
        return term::make_escape_dispatch_action(QByteArrayLiteral("c"), bytes);
    }

    if (bytes == QByteArray("\x1b[6n", 4)) {
        return term::make_csi_dispatch_action({6}, QByteArrayLiteral("n"), {}, {}, bytes);
    }

    if (bytes == QByteArray("\x1b[?25h", 6)) {
        return term::make_csi_dispatch_action(
            {25}, QByteArrayLiteral("h"), QByteArrayLiteral("?"), {}, bytes);
    }

    if (bytes == QByteArray("\x1b[!p", 4)) {
        return term::make_csi_dispatch_action(
            {}, QByteArrayLiteral("p"), {}, QByteArrayLiteral("!"), bytes);
    }

    if (bytes == QByteArray("\x1b[1;4;31m", 9)) {
        return term::make_csi_dispatch_action(
            {1, 4, 31}, QByteArrayLiteral("m"), {}, {}, bytes);
    }

    if (bytes == QByteArray("\x1b]0;title\a", 10)) {
        return
            term::make_string_sequence_action(
                term::Parser_sequence_family::OSC,
                QByteArrayLiteral("0;title"),
                term::Parser_string_terminator::BEL,
                bytes);
    }

    if (bytes == QByteArray("\x1b]0;title\x1b\\", 11)) {
        return
            term::make_string_sequence_action(
                term::Parser_sequence_family::OSC,
                QByteArrayLiteral("0;title"),
                term::Parser_string_terminator::ST_7BIT,
                bytes);
    }

    if (bytes.size()                             == osc_8bit_st_prefix.size() + 1 &&
        bytes.startsWith(osc_8bit_st_prefix)                                      &&
        static_cast<unsigned char>(bytes.back()) == 0x9cU)
    {
        return
            term::make_string_sequence_action(
                term::Parser_sequence_family::OSC,
                QByteArrayLiteral("0;title"),
                term::Parser_string_terminator::ST_8BIT,
                bytes);
    }

    if (bytes.startsWith(QByteArray("\x1b[", 2))) {
        return
            term::make_malformed_recovery_diagnostic(
                QStringLiteral("malformed CSI"),
                term::Parser_sequence_family::CSI,
                term::Parser_recovery_strategy::RESET_TO_GROUND);
    }

    return
        term::make_malformed_recovery_diagnostic(
            QStringLiteral("malformed ESC"),
            term::Parser_sequence_family::ESC,
            term::Parser_recovery_strategy::RESET_TO_GROUND);
}

term::Parser_action dispatch_dcs_size_fixture(std::size_t payload_size)
{
    if (payload_size > term::k_dcs_payload_limit_bytes) {
        return term::make_dcs_payload_limit_diagnostic(payload_size);
    }

    return term::make_string_sequence_action(
        term::Parser_sequence_family::DCS,
        {},
        term::Parser_string_terminator::ST_7BIT);
}

bool test_printable_and_control_ir()
{
    bool ok = true;

    const term::Parser_action print = dispatch_parser_fixture(QByteArrayLiteral("text"));
    const auto& print_mutation = std::get<term::Screen_print_text_mutation>(
        std::get<term::Screen_mutation>(print.payload));
    ok &= check(term::parser_action_kind(print) == term::Parser_action_kind::SCREEN_MUTATION,
        "print fixture produces screen mutation");
    ok &= check(
        term::screen_mutation_kind(std::get<term::Screen_mutation>(print.payload)) ==
            term::Screen_mutation_kind::PRINT_TEXT,
        "print fixture mutation kind");
    ok &= check(print_mutation.text == QStringLiteral("text"), "print fixture text");

    const term::Parser_action esc          = dispatch_parser_fixture(QByteArray("\x1b" "c", 2));
    const auto&               esc_sequence = std::get<term::Parser_control_sequence>(esc.payload);
    ok &= check(term::parser_action_kind(esc) == term::Parser_action_kind::CONTROL_SEQUENCE,
        "ESC fixture produces control sequence");
    ok &= check(esc_sequence.family == term::Parser_sequence_family::ESC,
        "ESC fixture family");
    ok &= check(esc_sequence.final_bytes == QByteArrayLiteral("c"),
        "ESC fixture final byte");
    ok &= check(esc_sequence.raw_bytes == QByteArray("\x1b" "c", 2), "ESC fixture raw bytes");

    const term::Parser_action csi          = dispatch_parser_fixture(QByteArray("\x1b[6n", 4));
    const auto&               csi_sequence = std::get<term::Parser_control_sequence>(csi.payload);
    ok &= check(csi_sequence.family == term::Parser_sequence_family::CSI,
        "CSI fixture family");
    ok &= check(csi_sequence.parameters.size() == 1U && csi_sequence.parameters[0] == 6,
        "CSI fixture parameters");
    ok &= check(csi_sequence.final_bytes == QByteArrayLiteral("n"), "CSI fixture final byte");
    ok &= check(csi_sequence.raw_bytes == QByteArray("\x1b[6n", 4), "CSI fixture raw bytes");

    const term::Parser_action csi_private =
        dispatch_parser_fixture(QByteArray("\x1b[?25h", 6));
    const auto& csi_private_sequence =
        std::get<term::Parser_control_sequence>(csi_private.payload);
    ok &= check(csi_private_sequence.private_marker == QByteArrayLiteral("?"),
        "CSI fixture private marker");
    ok &= check(csi_private_sequence.parameters.size() == 1U &&
        csi_private_sequence.parameters[0] == 25,
        "CSI fixture private parameters");

    const term::Parser_action csi_intermediate =
        dispatch_parser_fixture(QByteArray("\x1b[!p", 4));
    const auto& csi_intermediate_sequence =
        std::get<term::Parser_control_sequence>(csi_intermediate.payload);
    ok &= check(csi_intermediate_sequence.intermediates == QByteArrayLiteral("!"),
        "CSI fixture intermediates");

    const term::Parser_action csi_multi =
        dispatch_parser_fixture(QByteArray("\x1b[1;4;31m", 9));
    const auto& csi_multi_sequence =
        std::get<term::Parser_control_sequence>(csi_multi.payload);
    ok &= check(csi_multi_sequence.parameters.size() == 3U &&
        csi_multi_sequence.parameters[0] == 1 &&
        csi_multi_sequence.parameters[1] == 4 &&
        csi_multi_sequence.parameters[2] == 31,
        "CSI fixture multiple parameters");

    const term::Parser_action csi_ignore = term::make_csi_control_sequence_action(
        term::Parser_control_sequence_action::IGNORE,
        {},
        QByteArrayLiteral("m"));
    const auto& csi_ignore_sequence =
        std::get<term::Parser_control_sequence>(csi_ignore.payload);
    ok &= check(csi_ignore_sequence.action == term::Parser_control_sequence_action::IGNORE,
        "CSI ignore action");

    const term::Parser_action csi_discard = term::make_csi_control_sequence_action(
        term::Parser_control_sequence_action::DISCARD,
        {},
        QByteArrayLiteral("m"));
    const auto& csi_discard_sequence =
        std::get<term::Parser_control_sequence>(csi_discard.payload);
    ok &= check(csi_discard_sequence.action == term::Parser_control_sequence_action::DISCARD,
        "CSI discard action");

    const term::Parser_action osc_recovery = term::make_string_sequence_action(
        term::Parser_sequence_family::OSC,
        QByteArrayLiteral("0;unterminated"),
        term::Parser_string_terminator::RECOVERY,
        QByteArrayLiteral("\x1b]0;unterminated\x1b["));
    const auto& osc_recovery_sequence =
        std::get<term::Parser_control_sequence>(osc_recovery.payload);
    ok &= check(osc_recovery_sequence.terminator == term::Parser_string_terminator::RECOVERY,
        "OSC recovery terminator");

    const term::Parser_action dcs_end_of_input = term::make_string_sequence_action(
        term::Parser_sequence_family::DCS,
        QByteArrayLiteral("partial"),
        term::Parser_string_terminator::END_OF_INPUT,
        QByteArrayLiteral("\x1bPpartial"));
    const auto& dcs_end_of_input_sequence =
        std::get<term::Parser_control_sequence>(dcs_end_of_input.payload);
    ok &= check(dcs_end_of_input_sequence.terminator ==
        term::Parser_string_terminator::END_OF_INPUT,
        "DCS end-of-input terminator");

    return ok;
}

bool test_string_terminators_and_dcs_discard()
{
    bool ok = true;

    const term::Parser_action osc_bel =
        dispatch_parser_fixture(QByteArray("\x1b]0;title\a", 10));
    const auto& bel_sequence = std::get<term::Parser_control_sequence>(osc_bel.payload);
    ok &= check(bel_sequence.family == term::Parser_sequence_family::OSC,
        "OSC BEL fixture family");
    ok &= check(bel_sequence.terminator == term::Parser_string_terminator::BEL,
        "OSC BEL fixture terminator");
    ok &= check(bel_sequence.payload == QByteArrayLiteral("0;title"),
        "OSC BEL fixture payload");

    const term::Parser_action osc_st =
        dispatch_parser_fixture(QByteArray("\x1b]0;title\x1b\\", 11));
    const auto& st_sequence = std::get<term::Parser_control_sequence>(osc_st.payload);
    ok &= check(st_sequence.terminator == term::Parser_string_terminator::ST_7BIT,
        "OSC ST fixture terminator");

    QByteArray osc_8bit_st = QByteArray("\x1b]0;title", 9);
    osc_8bit_st.append(static_cast<char>(0x9c));
    const term::Parser_action osc_st_8bit = dispatch_parser_fixture(osc_8bit_st);
    const auto& st_8bit_sequence =
        std::get<term::Parser_control_sequence>(osc_st_8bit.payload);
    ok &= check(st_8bit_sequence.terminator == term::Parser_string_terminator::ST_8BIT,
        "OSC 8-bit ST fixture terminator");

    const term::Parser_action dcs =
        dispatch_dcs_size_fixture(term::k_dcs_payload_limit_bytes + 1U);
    const auto& dcs_diagnostic = std::get<term::Parser_payload_diagnostic>(dcs.payload);
    ok &= check(term::parser_action_kind(dcs) == term::Parser_action_kind::DIAGNOSTIC,
        "DCS discard fixture produces diagnostic");
    ok &= check(dcs_diagnostic.family == term::Parser_sequence_family::DCS,
        "DCS discard fixture family");
    ok &= check(dcs_diagnostic.code == term::Parser_diagnostic_code::PAYLOAD_LIMIT_EXCEEDED,
        "DCS discard fixture code");
    ok &= check(dcs_diagnostic.recovery == term::Parser_recovery_strategy::DISCARD_STRING,
        "DCS discard fixture recovery");
    ok &= check(dcs_diagnostic.raw_payload_size == term::k_dcs_payload_limit_bytes + 1U,
        "DCS discard fixture raw size");
    ok &= check(dcs_diagnostic.limit_bytes == term::k_dcs_payload_limit_bytes,
        "DCS discard fixture limit");

    const term::Parser_action dcs_inside_limit =
        dispatch_dcs_size_fixture(term::k_dcs_payload_limit_bytes);
    const auto& dcs_sequence =
        std::get<term::Parser_control_sequence>(dcs_inside_limit.payload);
    ok &= check(dcs_sequence.family == term::Parser_sequence_family::DCS,
        "DCS inside limit remains a string sequence");

    auto check_payload_limit = [&](const term::Parser_action& action,
        term::Parser_sequence_family family,
        std::size_t raw_payload_size,
        std::size_t limit_bytes,
        const char* label) {
        const auto& diagnostic = std::get<term::Parser_payload_diagnostic>(action.payload);
        ok &= check(term::parser_action_kind(action) == term::Parser_action_kind::DIAGNOSTIC, label);
        ok &= check(diagnostic.code == term::Parser_diagnostic_code::PAYLOAD_LIMIT_EXCEEDED,
            label);
        ok &= check(diagnostic.family == family, label);
        ok &= check(diagnostic.recovery == term::Parser_recovery_strategy::DISCARD_STRING,
            label);
        ok &= check(diagnostic.raw_payload_size == raw_payload_size, label);
        ok &= check(diagnostic.limit_bytes == limit_bytes, label);
    };

    check_payload_limit(
        term::make_osc_payload_limit_diagnostic(term::k_osc_payload_limit_bytes + 1U),
        term::Parser_sequence_family::OSC,
        term::k_osc_payload_limit_bytes + 1U,
        term::k_osc_payload_limit_bytes,
        "OSC payload limit diagnostic");
    check_payload_limit(
        term::make_apc_payload_limit_diagnostic(term::k_apc_payload_limit_bytes + 1U),
        term::Parser_sequence_family::APC,
        term::k_apc_payload_limit_bytes + 1U,
        term::k_apc_payload_limit_bytes,
        "APC payload limit diagnostic");
    check_payload_limit(
        term::make_pm_payload_limit_diagnostic(term::k_pm_payload_limit_bytes + 1U),
        term::Parser_sequence_family::PM,
        term::k_pm_payload_limit_bytes + 1U,
        term::k_pm_payload_limit_bytes,
        "PM payload limit diagnostic");
    check_payload_limit(
        term::make_sos_payload_limit_diagnostic(term::k_sos_payload_limit_bytes + 1U),
        term::Parser_sequence_family::SOS,
        term::k_sos_payload_limit_bytes + 1U,
        term::k_sos_payload_limit_bytes,
        "SOS payload limit diagnostic");

    const term::Parser_action title_limit =
        term::make_title_limit_diagnostic(term::k_title_scalar_limit + 1U);
    const auto& title_diagnostic =
        std::get<term::Parser_payload_diagnostic>(title_limit.payload);
    ok &= check(title_diagnostic.code == term::Parser_diagnostic_code::TITLE_LIMIT_EXCEEDED,
        "title limit diagnostic code");
    ok &= check(title_diagnostic.family == term::Parser_sequence_family::OSC,
        "title limit diagnostic family");
    ok &= check(title_diagnostic.limit_bytes == term::k_title_scalar_limit,
        "title limit diagnostic limit");

    return ok;
}

bool test_terminal_reply_ir()
{
    bool ok = true;

    const term::Parser_action da1 =
        term::make_da1_reply_action(QByteArrayLiteral("\x1b[?1;2c"));
    const auto& da1_reply = std::get<term::Terminal_reply>(da1.payload);
    ok &= check(da1_reply.kind == term::Terminal_reply_kind::DA1, "DA1 reply kind");
    ok &= check(da1_reply.wire_bytes == QByteArrayLiteral("\x1b[?1;2c"),
        "DA1 reply bytes");
    ok &= check(da1_reply.source_family == term::Parser_sequence_family::CSI,
        "DA1 reply source family");

    const term::Parser_action da2 =
        term::make_da2_reply_action(QByteArrayLiteral("\x1b[>0;0;0c"));
    const auto& da2_reply = std::get<term::Terminal_reply>(da2.payload);
    ok &= check(da2_reply.kind == term::Terminal_reply_kind::DA2, "DA2 reply kind");
    ok &= check(da2_reply.source_family == term::Parser_sequence_family::CSI,
        "DA2 reply source family");

    const term::Parser_action dsr       = term::make_dsr_cursor_position_reply_action(24, 80);
    const auto&               dsr_reply = std::get<term::Terminal_reply>(dsr.payload);
    ok                                 &= check(dsr_reply.kind == term::Terminal_reply_kind::DSR_CURSOR_POSITION,
        "DSR reply kind");
    ok                                 &= check(dsr_reply.wire_bytes == QByteArrayLiteral("\x1b[24;80R"),
        "DSR reply bytes");

    const term::Parser_action decrqm       = term::make_decrqm_reply_action(2004, 1);
    const auto&               decrqm_reply = std::get<term::Terminal_reply>(decrqm.payload);
    ok                                    &= check(decrqm_reply.kind == term::Terminal_reply_kind::DECRQM,
        "DECRQM reply kind");
    ok                                    &= check(decrqm_reply.wire_bytes == QByteArrayLiteral("\x1b[?2004;1$y"),
        "DECRQM reply bytes");

    const term::Parser_action text_area_size =
        term::make_text_area_size_reply_action(24, 80);
    const auto& text_area_size_reply =
        std::get<term::Terminal_reply>(text_area_size.payload);
    ok &= check(text_area_size_reply.kind == term::Terminal_reply_kind::TEXT_AREA_SIZE,
        "text-area size reply kind");
    ok &= check(text_area_size_reply.wire_bytes == QByteArrayLiteral("\x1b[8;24;80t"),
        "text-area size reply bytes");
    ok &= check(text_area_size_reply.source_family == term::Parser_sequence_family::CSI,
        "text-area size reply source family");

    const term::Parser_action osc = term::make_osc_query_reply_action(
        QByteArrayLiteral("\x1b]10;rgb:ffff/ffff/ffff\x1b\\"),
        QStringLiteral("OSC 10"));
    const auto& osc_reply = std::get<term::Terminal_reply>(osc.payload);
    ok &= check(term::parser_action_kind(osc) == term::Parser_action_kind::TERMINAL_REPLY,
        "OSC query reply action kind");
    ok &= check(osc_reply.kind == term::Terminal_reply_kind::OSC_QUERY,
        "OSC query reply kind");
    ok &= check(osc_reply.wire_bytes ==
        QByteArrayLiteral("\x1b]10;rgb:ffff/ffff/ffff\x1b\\"),
        "OSC query reply bytes");
    ok &= check(osc_reply.source_sequence == QStringLiteral("OSC 10"),
        "OSC query reply source");
    ok &= check(osc_reply.source_family == term::Parser_sequence_family::OSC,
        "OSC query reply source family");

    const term::Parser_action raw = term::make_terminal_reply_action(
        term::Terminal_reply_kind::RAW,
        QByteArrayLiteral("raw"),
        QStringLiteral("raw fixture"));
    const auto& raw_reply = std::get<term::Terminal_reply>(raw.payload);
    ok &= check(raw_reply.source_family == term::Parser_sequence_family::NONE,
        "raw reply source family");

    return ok;
}

bool test_malformed_recovery_ir()
{
    bool                      ok         = true;
    const term::Parser_action malformed  = dispatch_parser_fixture(QByteArray("\x1b[?", 3));
    const auto&               diagnostic = std::get<term::Parser_payload_diagnostic>(malformed.payload);
    ok &= check(term::parser_action_kind(malformed) == term::Parser_action_kind::DIAGNOSTIC,
        "malformed fixture produces diagnostic");
    ok &= check(diagnostic.code == term::Parser_diagnostic_code::MALFORMED_INPUT,
        "malformed fixture code");
    ok &= check(diagnostic.family == term::Parser_sequence_family::CSI,
        "malformed fixture family");
    ok &= check(diagnostic.recovery == term::Parser_recovery_strategy::RESET_TO_GROUND,
        "malformed fixture recovery");

    const term::Parser_action malformed_esc = dispatch_parser_fixture(QByteArray("\x1b", 1));
    const auto& esc_diagnostic =
        std::get<term::Parser_payload_diagnostic>(malformed_esc.payload);
    ok &= check(esc_diagnostic.family == term::Parser_sequence_family::ESC,
        "malformed ESC fixture family");
    ok &= check(esc_diagnostic.recovery == term::Parser_recovery_strategy::RESET_TO_GROUND,
        "malformed ESC fixture recovery");

    const term::Parser_action ignored_byte = term::make_malformed_recovery_diagnostic(
        QStringLiteral("invalid UTF-8 continuation"),
        term::Parser_sequence_family::PRINTABLE,
        term::Parser_recovery_strategy::IGNORE_BYTE);
    const auto& ignored_byte_diagnostic =
        std::get<term::Parser_payload_diagnostic>(ignored_byte.payload);
    ok &= check(ignored_byte_diagnostic.recovery == term::Parser_recovery_strategy::IGNORE_BYTE,
        "ignore-byte recovery strategy");

    const term::Parser_action discarded_sequence = term::make_malformed_recovery_diagnostic(
        QStringLiteral("malformed CSI parameter"),
        term::Parser_sequence_family::CSI,
        term::Parser_recovery_strategy::DISCARD_SEQUENCE);
    const auto& discarded_sequence_diagnostic =
        std::get<term::Parser_payload_diagnostic>(discarded_sequence.payload);
    ok &= check(discarded_sequence_diagnostic.recovery ==
        term::Parser_recovery_strategy::DISCARD_SEQUENCE,
        "discard-sequence recovery strategy");

    return ok;
}

}

int main()
{
    bool ok = true;
    ok &= test_printable_and_control_ir();
    ok &= test_string_terminators_and_dcs_discard();
    ok &= test_terminal_reply_ir();
    ok &= test_malformed_recovery_ir();
    return ok ? 0 : 1;
}
