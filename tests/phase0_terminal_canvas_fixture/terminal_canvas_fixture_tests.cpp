#include "vnm_terminal/internal/terminal_canvas_fixture_contract.h"
#include "helpers/test_check.h"

#include <QCoreApplication>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

struct command_result_t
{
    int                        exit_code = 0;
    std::string                output;
};

struct protocol_record_t
{
    std::string                kind;
    std::string                label;
    std::vector<std::string>   values;
};

bool is_hex_string(const std::string& text);

class Fake_transport
{
public:
    bool process(const protocol_record_t& record)
    {
        if (record.kind == "checkpoint") {
            return record.label == "startup";
        }

        if (record.kind == "output") {
            return process_output(record);
        }

        if (record.kind == "expect-input") {
            return process_expected_input(record);
        }

        if (record.kind == "resize") {
            return process_resize(record);
        }

        if (record.kind == "repeat-output") {
            return process_repeat_output(record);
        }

        if (record.kind == "exit") {
            return process_exit(record);
        }

        return false;
    }

    bool complete() const
    {
        return
            m_entered_alternate    &&
            m_left_alternate       &&
            m_prompt_editing_seen  &&
            m_resize_seen          &&
            m_bracketed_paste_seen &&
            m_focus_seen           &&
            m_mouse_seen           &&
            m_reply_seen           &&
            m_high_volume_seen     &&
            m_exit_seen;
    }

private:
    static bool contains_hex(const std::string& text, const std::string& needle)
    {
        return text.find(needle) != std::string::npos;
    }

    static int parse_int(const std::string& text)
    {
        return std::stoi(text);
    }

    bool process_output(const protocol_record_t& record)
    {
        if (record.values.size() != 1U || !is_hex_string(record.values[0])) {
            return false;
        }

        const std::string& payload = record.values[0];
        m_output_bytes += payload.size() / 2U;

        if (record.label == "enter-alternate-screen") {
            m_entered_alternate = payload == "1b5b3f3130343968";
            return m_entered_alternate;
        }

        if (record.label == term::k_terminal_canvas_fixture_enable_input_modes_label) {
            m_bracketed_paste_enabled = contains_hex(payload, "1b5b3f3230303468");
            m_focus_enabled           = contains_hex(payload, "1b5b3f3130303468");
            m_mouse_enabled           =
                contains_hex(payload, "1b5b3f3130303068") ||
                contains_hex(payload, "1b5b3f3130303268") ||
                contains_hex(payload, "1b5b3f3130303368");
            m_sgr_mouse_enabled       = contains_hex(payload, "1b5b3f3130303668");
            return
                m_bracketed_paste_enabled &&
                m_focus_enabled           &&
                m_mouse_enabled           &&
                m_sgr_mouse_enabled;
        }

        if (record.label == "reply-handling") {
            m_reply_queries_seen =
                contains_hex(payload, "1b5b63")            &&
                contains_hex(payload, "1b5b366e")          &&
                contains_hex(payload, "1b5b3f323030342470");
            return m_reply_queries_seen;
        }

        if (record.label == "leave-alternate-screen") {
            m_left_alternate = payload == "1b5b3f313034396c";
            return m_left_alternate;
        }

        return !payload.empty();
    }

    bool process_expected_input(const protocol_record_t& record)
    {
        if (record.values.size() != 1U || !is_hex_string(record.values[0])) {
            return false;
        }

        const std::string& payload = record.values[0];

        if (record.label == "prompt-editing-keys") {
            m_prompt_editing_seen =
                contains_hex(payload, "1b5b44") &&
                contains_hex(payload, "7f")     &&
                contains_hex(payload, "0d");
            return m_prompt_editing_seen;
        }

        if (record.label == "bracketed-paste") {
            m_bracketed_paste_seen =
                m_bracketed_paste_enabled             &&
                contains_hex(payload, "1b5b3230307e") &&
                contains_hex(payload, "1b5b3230317e");
            return m_bracketed_paste_seen;
        }

        if (record.label == "focus-reporting") {
            m_focus_seen =
                m_focus_enabled &&
                payload == "1b5b491b5b4f";
            return m_focus_seen;
        }

        if (record.label == "mouse-sgr-1006") {
            m_mouse_seen =
                m_mouse_enabled                &&
                m_sgr_mouse_enabled            &&
                contains_hex(payload, "1b5b3c");
            return m_mouse_seen;
        }

        if (record.label == "reply-handling") {
            m_reply_seen =
                m_reply_queries_seen                           &&
                contains_hex(payload, "1b5b3f313b3263")        &&
                contains_hex(payload, "1b5b32343b383052")      &&
                contains_hex(payload, "1b5b3f323030343b312479");
            return m_reply_seen;
        }

        return false;
    }

    bool process_resize(const protocol_record_t& record)
    {
        if (record.values.size() != 2U) {
            return false;
        }

        m_resize_seen = parse_int(record.values[0]) == 33 &&
            parse_int(record.values[1]) == 120;
        return m_resize_seen;
    }

    bool process_repeat_output(const protocol_record_t& record)
    {
        if (record.values.size() != 2U || !is_hex_string(record.values[1])) {
            return false;
        }

        const int repeat_count = parse_int(record.values[0]);
        m_high_volume_seen = record.label == "high-volume-streaming" &&
            repeat_count >= 4096 &&
            !record.values[1].empty();
        if (m_high_volume_seen) {
            m_output_bytes += static_cast<std::size_t>(repeat_count) *
                (record.values[1].size() / 2U);
        }
        return m_high_volume_seen;
    }

    bool process_exit(const protocol_record_t& record)
    {
        if (record.values.size() != 1U) {
            return false;
        }

        m_exit_seen = record.label == "clean-exit" && parse_int(record.values[0]) == 0;
        return m_exit_seen;
    }

    bool           m_entered_alternate       = false;
    bool           m_left_alternate          = false;
    bool           m_bracketed_paste_enabled = false;
    bool           m_focus_enabled           = false;
    bool           m_mouse_enabled           = false;
    bool           m_sgr_mouse_enabled       = false;
    bool           m_prompt_editing_seen     = false;
    bool           m_resize_seen             = false;
    bool           m_bracketed_paste_seen    = false;
    bool           m_focus_seen              = false;
    bool           m_mouse_seen              = false;
    bool           m_reply_queries_seen      = false;
    bool           m_reply_seen              = false;
    bool           m_high_volume_seen        = false;
    bool           m_exit_seen               = false;
    std::size_t    m_output_bytes            = 0U;
};

using vnm_terminal::test_helpers::check;

command_result_t run_fixture(const QString& fixture_path, const QStringList& arguments)
{
    command_result_t result;

    QProcess process;
    process.setProgram(fixture_path);
    process.setArguments(arguments);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();

    if (!process.waitForStarted(5000)) {
        result.exit_code = -1;
        return result;
    }

    if (!process.waitForFinished(5000)) {
        process.kill();
        process.waitForFinished(5000);
        result.exit_code = -1;
        result.output    = process.readAllStandardOutput().toStdString();
        return result;
    }

    result.exit_code = process.exitCode();
    result.output    = process.readAllStandardOutput().toStdString();
    return result;
}

bool is_hex_string(const std::string& text)
{
    if (text.empty() || (text.size() % 2U) != 0U) {
        return false;
    }

    for (char ch : text) {
        const bool digit = ch >= '0' && ch <= '9';
        const bool lower = ch >= 'a' && ch <= 'f';
        const bool upper = ch >= 'A' && ch <= 'F';
        if (!digit && !lower && !upper) {
            return false;
        }
    }

    return true;
}

std::vector<std::string> split_lines(const std::string& text)
{
    std::vector<std::string> lines;
    std::istringstream in(text);
    std::string line;

    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            lines.push_back(line);
        }
    }

    return lines;
}

std::vector<protocol_record_t> parse_records(
    const std::vector<std::string>&    lines,
    std::vector<std::string>&          errors)
{
    std::vector<protocol_record_t> records;

    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i < 2U) {
            continue;
        }

        const std::string& line = lines[i];
        std::istringstream in(line);
        std::string prefix;
        in >> prefix;

        if (prefix != "record") {
            errors.push_back("unexpected protocol line: " + line);
            continue;
        }

        protocol_record_t record;
        in >> record.kind;
        in >> record.label;

        std::string value;
        while (in >> value) {
            record.values.push_back(value);
        }

        if (record.kind.empty() || record.label.empty()) {
            errors.push_back("record line is missing kind or label: " + line);
            continue;
        }

        records.push_back(std::move(record));
    }

    return records;
}

std::vector<std::string> contract_values(
    const term::terminal_canvas_fixture_record_t& contract_record)
{
    switch (contract_record.kind) {
        case term::Terminal_canvas_fixture_record_kind::OUTPUT:
        case term::Terminal_canvas_fixture_record_kind::EXPECT_INPUT:
            return {std::string(contract_record.payload_hex)};

        case term::Terminal_canvas_fixture_record_kind::RESIZE:
            return {
                std::to_string(contract_record.rows),
                std::to_string(contract_record.columns),
            };

        case term::Terminal_canvas_fixture_record_kind::REPEAT_OUTPUT:
            return {
                std::to_string(contract_record.repeat_count),
                std::string(contract_record.payload_hex),
            };

        case term::Terminal_canvas_fixture_record_kind::EXIT:
            return {std::to_string(contract_record.exit_code)};

        case term::Terminal_canvas_fixture_record_kind::CHECKPOINT:
            return {};
    }

    return {};
}

bool validate_exact_contract_records(const std::vector<protocol_record_t>& records)
{
    bool ok = true;
    const std::vector<term::terminal_canvas_fixture_record_t>& contract_records =
        term::terminal_canvas_fixture_contract_script();

    ok &= check(records.size() == contract_records.size(), "fixture emits exact record count");
    if (records.size() != contract_records.size()) {
        return false;
    }

    for (std::size_t i = 0; i < records.size(); ++i) {
        const protocol_record_t& record = records[i];
        const term::terminal_canvas_fixture_record_t& contract_record = contract_records[i];

        ok &= check(record.kind ==
            std::string(term::terminal_canvas_fixture_kind_name(contract_record.kind)),
            "fixture record kind matches contract order");
        ok &= check(record.label == std::string(contract_record.label),
            "fixture record label matches contract order");
        ok &= check(record.values == contract_values(contract_record),
            "fixture record values match contract");
    }

    return ok;
}

bool validate_fake_transport(const std::vector<protocol_record_t>& records)
{
    bool ok = true;
    Fake_transport transport;

    for (const protocol_record_t& record : records) {
        ok &= check(transport.process(record), "fake transport accepts record");
    }

    ok &= check(transport.complete(), "fake transport completed fixture scenario");
    return ok;
}

bool validate_list_command(const QString& fixture_path)
{
    const command_result_t result = run_fixture(fixture_path, {"--list"});
    return
        check(result.exit_code == 0,
            "fixture --list exits successfully")
        &&
        check(result.output.find(term::terminal_canvas_fixture_scenario_name()) != std::string::npos,
            "fixture --list includes scenario");
}

bool validate_unknown_scenario(const QString& fixture_path)
{
    const command_result_t result = run_fixture(fixture_path, {"--scenario", "missing"});
    return check(result.exit_code != 0, "fixture rejects unknown scenario");
}

bool validate_scenario_output(const QString& fixture_path)
{
    bool ok = true;
    const command_result_t result = run_fixture(
        fixture_path,
        {
            "--scenario",
            QString::fromLatin1(
                term::terminal_canvas_fixture_scenario_name().data(),
                static_cast<qsizetype>(
                    term::terminal_canvas_fixture_scenario_name().size())),
        });
    ok &= check(result.exit_code == 0, "fixture scenario exits successfully");

    const std::vector<std::string> lines = split_lines(result.output);
    ok &= check(lines.size() >= 4U, "fixture scenario prints protocol records");
    if (lines.size() < 2U) {
        return false;
    }

    ok &= check(lines[0] == "vnm-fixture-protocol 1", "fixture protocol header");
    ok &= check(lines[1] ==
        "scenario " + std::string(term::terminal_canvas_fixture_scenario_name()),
        "fixture scenario header");

    std::vector<std::string> errors;
    const std::vector<protocol_record_t> records = parse_records(lines, errors);
    for (const std::string& error : errors) {
        std::cerr << "FAIL: " << error << '\n';
        ok = false;
    }
    ok &= check(lines.size() == records.size() + 2U, "fixture emits no stray lines");

    for (const protocol_record_t& record : records) {
        if (record.kind == "output" || record.kind == "expect-input") {
            ok &= check(record.values.size() == 1U, "byte record has one payload");
            if (record.values.size() == 1U) {
                ok &= check(is_hex_string(record.values[0]), "byte record payload is hex");
            }
        }
        else
        if (record.kind == "resize") {
            ok &= check(record.values.size() == 2U, "resize record has rows and columns");
        }
        else
        if (record.kind == "repeat-output") {
            ok &= check(record.values.size() == 2U, "repeat output has count and payload");
            if (record.values.size() == 2U) {
                ok &= check(std::stoi(record.values[0]) >= 4096,
                    "high-volume fixture has meaningful repeat count");
                ok &= check(is_hex_string(record.values[1]), "repeat payload is hex");
            }
        }
        else
        if (record.kind == "exit") {
            ok &= check(record.values.size() == 1U, "exit record has code");
        }
        else {
            ok &= check(record.kind == "checkpoint", "record kind is known");
        }
    }

    ok &= validate_exact_contract_records(records);
    ok &= validate_fake_transport(records);
    return ok;
}

}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    if (argc != 2) {
        std::cerr << "usage: terminal_canvas_fixture_tests <fixture-executable>\n";
        return 2;
    }

    const QString fixture_path = QString::fromLocal8Bit(argv[1]);
    bool ok = true;
    ok &= validate_list_command(fixture_path);
    ok &= validate_unknown_scenario(fixture_path);
    ok &= validate_scenario_output(fixture_path);

    return ok ? 0 : 1;
}
