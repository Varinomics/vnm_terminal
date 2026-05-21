#include "vnm_terminal/internal/terminal_screen_model.h"
#include "helpers/test_check.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace term = vnm_terminal::internal;
namespace fs = std::filesystem;

namespace {

struct Expected_diagnostic
{
    term::Parser_diagnostic_code      code;
    QString                           source_sequence;
    term::Parser_sequence_family      family;
    std::size_t                       raw_payload_size;
    std::size_t                       limit_bytes;
    term::Parser_recovery_strategy    recovery;
};

struct Expected_reply
{
    term::Terminal_reply_kind         kind;
    term::Parser_sequence_family      source_family;
    QByteArray                        wire_bytes;
    QString                           source_sequence;
};

struct Test_case
{
    std::string                       name;
    term::Terminal_screen_model_config
                                      config;
    QByteArray                        bytes;
    QByteArray                        expected_visible_text_utf8;
    QByteArray                        expected_title_utf8;
    bool                              has_expected_visible_text_utf8 = false;
    bool                              has_expected_title_utf8 = false;
    bool                              has_expected_cursor = false;
    term::terminal_grid_position_t    expected_cursor;
    bool                              has_expected_wide_cell = false;
    int                               expected_wide_cell_row = 0;
    int                               expected_wide_cell_column = 0;
    int                               expected_wide_cell_width = 0;
    int                               expected_wide_continuation_column = 0;
    QByteArray                        expected_wide_cell_text_utf8;
    std::uint64_t                     chunk_seed = 0U;
    int                               expected_diagnostic_count = -1;
    int                               expected_reply_count = -1;
    bool                              compare_actions = false;
    bool                              exhaustive_byte_chunks = true;
    bool                              has_large_payload_limit_boundaries = false;
    std::size_t                       large_payload_introducer_size = 0U;
    std::size_t                       large_payload_limit_bytes = 0U;
    std::vector<Expected_diagnostic>  expected_diagnostics;
    std::vector<Expected_reply>       expected_replies;
};

struct Chunk_plan
{
    std::string                name;
    std::vector<int>           chunk_sizes;
};

struct Run_result
{
    term::Terminal_render_snapshot                 snapshot;
    QString                                        visible_text;
    QString                                        title;
    QString                                        icon_name;
    term::terminal_grid_position_t                 cursor;
    term::Terminal_buffer_id                       active_buffer   = term::Terminal_buffer_id::PRIMARY;
    int                                            scrollback_size = 0;
    std::vector<std::string>                       actions;
    std::vector<std::string>                       side_effects;
    std::vector<term::Parser_payload_diagnostic>   diagnostics;
    std::vector<term::Terminal_reply>              replies;
};

class Deterministic_rng
{
public:
    explicit Deterministic_rng(std::uint64_t seed)
    :
        m_state(seed == 0U ? 0x9e3779b97f4a7c15ULL : seed)
    {}

    std::uint32_t next_u32()
    {
        m_state = m_state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<std::uint32_t>(m_state >> 32U);
    }

    int bounded(int exclusive_max)
    {
        if (exclusive_max <= 1) {
            return 0;
        }

        return static_cast<int>(next_u32() % static_cast<std::uint32_t>(exclusive_max));
    }

private:
    std::uint64_t m_state;
};

using vnm_terminal::test_helpers::check;

std::string trim_copy(const std::string& text)
{
    std::size_t first = 0U;
    while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first]))) {
        ++first;
    }

    std::size_t last = text.size();
    while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1U]))) {
        --last;
    }

    return text.substr(first, last - first);
}

std::string strip_inline_comment(const std::string& line)
{
    bool in_quote = false;
    bool escaped  = false;

    for (std::size_t i = 0U; i < line.size(); ++i) {
        const char ch = line[i];
        if (escaped) {
            escaped = false;
            continue;
        }

        if (ch == '\\' && in_quote) {
            escaped = true;
            continue;
        }

        if (ch == '"') {
            in_quote = !in_quote;
            continue;
        }

        if (ch == '#' && !in_quote) {
            return line.substr(0U, i);
        }
    }

    return line;
}

std::string unquote_value(std::string value)
{
    value = trim_copy(std::move(value));
    if (value.size() >= 2U && value.front() == '"' && value.back() == '"') {
        return value.substr(1U, value.size() - 2U);
    }

    return value;
}

std::map<std::string, std::string> parse_key_values(const std::string& text)
{
    std::map<std::string, std::string> values;
    std::istringstream in(text);
    std::string line;

    while (std::getline(in, line)) {
        line = trim_copy(strip_inline_comment(line));
        if (line.empty()) {
            continue;
        }

        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }

        const std::string key = trim_copy(line.substr(0U, equals));
        values[key]           = unquote_value(line.substr(equals + 1U));
    }

    return values;
}

std::vector<std::pair<std::string, std::string>> parse_comment_header_entries(
    const std::string& text)
{
    std::vector<std::pair<std::string, std::string>> entries;
    std::istringstream in(text);
    std::string line;

    while (std::getline(in, line)) {
        line = trim_copy(line);
        if (line.empty()) {
            continue;
        }

        if (line.front() != '#') {
            break;
        }

        const std::string comment = trim_copy(line.substr(1U));
        const std::size_t colon   = comment.find(':');
        if (colon == std::string::npos) {
            continue;
        }

        const std::string key = trim_copy(comment.substr(0U, colon));
        entries.push_back({key, trim_copy(comment.substr(colon + 1U))});
    }

    return entries;
}

std::string read_text_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

bool hex_value(char ch, unsigned char& value)
{
    if (ch >= '0' && ch <= '9') {
        value = static_cast<unsigned char>(ch - '0');
        return true;
    }

    if (ch >= 'a' && ch <= 'f') {
        value = static_cast<unsigned char>(ch - 'a' + 10);
        return true;
    }

    if (ch >= 'A' && ch <= 'F') {
        value = static_cast<unsigned char>(ch - 'A' + 10);
        return true;
    }

    return false;
}

bool bytes_from_hex(const std::string& hex, QByteArray& out_bytes, const std::string& label)
{
    out_bytes.clear();
    int pending_nibble = -1;

    for (char ch : hex) {
        if (std::isspace(static_cast<unsigned char>(ch))) {
            continue;
        }

        unsigned char nibble = 0U;
        if (!hex_value(ch, nibble)) {
            std::cerr << "FAIL: " << label << " contains non-hex byte '" << ch << "'\n";
            return false;
        }

        if (pending_nibble < 0) {
            pending_nibble = static_cast<int>(nibble);
            continue;
        }

        const unsigned char byte =
            static_cast<unsigned char>((pending_nibble << 4U) | nibble);
        out_bytes.append(static_cast<char>(byte));
        pending_nibble = -1;
    }

    if (pending_nibble >= 0) {
        std::cerr << "FAIL: " << label << " has an odd number of hex digits\n";
        return false;
    }

    return true;
}

void report_numeric_parse_failure(
    const fs::path&    path,
    const std::string& key,
    const std::string& value,
    const std::string& type)
{
    std::cerr << "FAIL: seed numeric field '" << key << "' in " << path
        << " must be a valid " << type << ", got '" << value << "'\n";
}

bool parse_int_value(
    const std::string& text,
    const fs::path&    path,
    const std::string& key,
    int&               out_value)
{
    try {
        std::size_t consumed = 0U;
        const int   parsed   = std::stoi(text, &consumed, 10);
        if (consumed != text.size()) {
            report_numeric_parse_failure(path, key, text, "integer");
            return false;
        }

        out_value = parsed;
        return true;
    }
    catch (const std::exception&) {
        report_numeric_parse_failure(path, key, text, "integer");
        return false;
    }
}

bool parse_uint64_value(
    const std::string& text,
    const fs::path&    path,
    const std::string& key,
    std::uint64_t&     out_value)
{
    if (!text.empty() && text.front() == '-') {
        report_numeric_parse_failure(path, key, text, "unsigned integer");
        return false;
    }

    try {
        std::size_t         consumed = 0U;
        const std::uint64_t parsed   = static_cast<std::uint64_t>(std::stoull(text, &consumed, 10));
        if (consumed != text.size()) {
            report_numeric_parse_failure(path, key, text, "unsigned integer");
            return false;
        }

        out_value = parsed;
        return true;
    }
    catch (const std::exception&) {
        report_numeric_parse_failure(path, key, text, "unsigned integer");
        return false;
    }
}

int integer_value(
    const std::map<std::string, std::string>&  values,
    const std::string&                         key,
    int                                        fallback,
    const fs::path&                            path,
    bool&                                      ok)
{
    const auto found = values.find(key);
    if (found == values.end()) {
        return fallback;
    }

    int parsed = fallback;
    ok &= parse_int_value(found->second, path, key, parsed);
    return parsed;
}

std::uint64_t uint64_value(
    const std::map<std::string, std::string>&  values,
    const std::string&                         key,
    std::uint64_t                              fallback,
    const fs::path&                            path,
    bool&                                      ok)
{
    const auto found = values.find(key);
    if (found == values.end()) {
        return fallback;
    }

    std::uint64_t parsed = fallback;
    ok &= parse_uint64_value(found->second, path, key, parsed);
    return parsed;
}

bool contains_key(
    const std::map<std::string, std::string>&  values,
    const std::string&                         key)
{
    return values.find(key) != values.end();
}

bool is_indexed_seed_key(
    const std::string&                         key,
    const std::string&                         prefix,
    const std::vector<std::string>&            suffixes)
{
    if (key.rfind(prefix, 0U) != 0U) {
        return false;
    }

    std::size_t       i           = prefix.size();
    const std::size_t first_digit = i;
    while (i < key.size() && std::isdigit(static_cast<unsigned char>(key[i]))) {
        ++i;
    }

    if (i == first_digit) {
        return false;
    }

    const std::string suffix = key.substr(i);
    return std::find(suffixes.begin(), suffixes.end(), suffix) != suffixes.end();
}

bool is_known_seed_key(const std::string& key)
{
    static const std::vector<std::string> k_known_keys = {
        "name",
        "grid_rows",
        "grid_columns",
        "scrollback_limit",
        "tab_width",
        "chunk_seed",
        "byte_stream_hex",
        "expected_visible_text_utf8_hex",
        "expected_title_utf8_hex",
        "expected_cursor_row",
        "expected_cursor_column",
        "expected_wide_cell_row",
        "expected_wide_cell_column",
        "expected_wide_cell_display_width",
        "expected_wide_continuation_column",
        "expected_wide_cell_text_utf8_hex",
        "expected_diagnostic_count",
        "expected_reply_count",
        "compare_actions",
        "exhaustive_byte_chunks",
    };
    static const std::vector<std::string> k_diagnostic_suffixes = {
        "_code",
        "_source_sequence_utf8_hex",
        "_family",
        "_raw_payload_size",
        "_limit_bytes",
        "_recovery",
    };
    static const std::vector<std::string> k_reply_suffixes = {
        "_kind",
        "_source_family",
        "_wire_bytes_hex",
        "_source_sequence_utf8_hex",
    };

    return
        std::find(k_known_keys.begin(), k_known_keys.end(), key) != k_known_keys.end() ||
        is_indexed_seed_key(key, "expected_diagnostic_", k_diagnostic_suffixes)        ||
        is_indexed_seed_key(key, "expected_reply_", k_reply_suffixes);
}

bool validate_seed_keys(
    const std::map<std::string, std::string>&  values,
    const fs::path&                            path)
{
    bool ok = true;
    for (const auto& entry : values) {
        ok &= check(is_known_seed_key(entry.first),
            "seed has unknown key '" + entry.first + "': " + path.string());
    }
    return ok;
}

bool validate_seed_provenance_header(const std::string& text, const fs::path& path)
{
    static const std::map<std::string, std::string> k_required_exact_values = {
        { "vnm-fixture-format",         "1"                      },
        { "vnm-provenance-origin",      "independently-authored" },
        { "vnm-provenance-license",     "BSD-3-Clause"           },
        { "vnm-provenance-independent", "true"                   },
    };
    static const std::vector<std::string> k_required_non_empty_values = {
        "vnm-provenance-generator",
        "vnm-provenance-reviewer",
    };

    const std::vector<std::pair<std::string, std::string>> entries =
        parse_comment_header_entries(text);

    bool ok = true;
    for (const auto& required : k_required_exact_values) {
        std::vector<std::string> values;
        for (const auto& entry : entries) {
            if (entry.first == required.first) {
                values.push_back(entry.second);
            }
        }

        ok &= check(!values.empty(),
            "seed missing provenance header key '" + required.first + "': " + path.string());
        ok &= check(values.size() <= 1U,
            "seed duplicate provenance header key '" + required.first + "': " + path.string());
        for (const std::string& value : values) {
            ok &= check(value == required.second,
                "seed provenance header key '" + required.first + "' has unsupported value '" +
                    value + "': " + path.string());
        }
    }

    for (const std::string& key : k_required_non_empty_values) {
        std::vector<std::string> values;
        for (const auto& entry : entries) {
            if (entry.first == key) {
                values.push_back(entry.second);
            }
        }

        ok &= check(!values.empty(),
            "seed missing provenance header key '" + key + "': " + path.string());
        ok &= check(values.size() <= 1U,
            "seed duplicate provenance header key '" + key + "': " + path.string());
        for (const std::string& value : values) {
            ok &= check(!value.empty(),
                "seed empty provenance header key '" + key + "': " + path.string());
        }
    }

    return ok;
}

bool validate_all_or_none_keys(
    const std::map<std::string, std::string>&  values,
    const std::vector<std::string>&            keys,
    const fs::path&                            path)
{
    bool has_any = false;
    bool has_all = true;
    for (const std::string& key : keys) {
        const bool has_key = contains_key(values, key);
        has_any |= has_key;
        has_all &= has_key;
    }

    if (!has_any || has_all) {
        return true;
    }

    std::ostringstream out;
    out << "seed has partial expectation keys in " << path.string() << ":";
    for (const std::string& key : keys) {
        out << ' ' << key;
    }

    return check(false, out.str());
}

bool validate_seed_has_meaningful_expectation(
    const std::map<std::string, std::string>&  values,
    const fs::path&                            path)
{
    static const std::vector<std::string> k_expectation_keys = {
        "expected_visible_text_utf8_hex",
        "expected_title_utf8_hex",
        "expected_cursor_row",
        "expected_cursor_column",
        "expected_wide_cell_row",
        "expected_wide_cell_column",
        "expected_wide_cell_display_width",
        "expected_wide_continuation_column",
        "expected_wide_cell_text_utf8_hex",
        "expected_diagnostic_count",
        "expected_reply_count",
    };

    for (const std::string& key : k_expectation_keys) {
        if (contains_key(values, key)) {
            return true;
        }
    }

    const auto compare_actions_it = values.find("compare_actions");
    if (compare_actions_it != values.end() && compare_actions_it->second != "0") {
        return true;
    }

    return check(false, "seed must define at least one expectation: " + path.string());
}

std::string indexed_key(const std::string& prefix, int index, const std::string& suffix)
{
    return prefix + std::to_string(index) + suffix;
}

bool validate_indexed_expectation_keys(
    const std::map<std::string, std::string>&  values,
    const fs::path&                            path,
    const std::string&                         count_key,
    const std::string&                         prefix,
    const std::vector<std::string>&            suffixes,
    int                                        count)
{
    bool ok = true;
    for (const auto& entry : values) {
        if (entry.first.rfind(prefix, 0U) != 0U) {
            continue;
        }

        std::size_t i = prefix.size();
        if (i >= entry.first.size() ||
            !std::isdigit(static_cast<unsigned char>(entry.first[i])))
        {
            continue;
        }

        if (!contains_key(values, count_key)) {
            ok &= check(false,
                "seed indexed expectation key '" + entry.first + "' requires " +
                    count_key + ": " + path.string());
            continue;
        }

        while (i < entry.first.size() &&
            std::isdigit(static_cast<unsigned char>(entry.first[i])))
        {
            ++i;
        }

        int index = -1;
        if (!parse_int_value(entry.first.substr(prefix.size(), i - prefix.size()),
            path, entry.first, index))
        {
            ok = false;
            continue;
        }

        ok &= check(index >= 0 && index < count,
            "seed indexed expectation key '" + entry.first + "' is outside " +
                count_key + ": " + path.string());
    }

    if (count <= 0) {
        return ok;
    }

    for (int i = 0; i < count; ++i) {
        for (const std::string& suffix : suffixes) {
            const std::string key = indexed_key(prefix, i, suffix);
            ok &= check(contains_key(values, key),
                "seed missing indexed expectation key '" + key + "': " + path.string());
        }
    }

    return ok;
}

std::string byte_array_hex(const QByteArray& bytes)
{
    const QByteArray hex = bytes.toHex(' ');
    return std::string(hex.constData(), static_cast<std::size_t>(hex.size()));
}

std::string qstring_hex(const QString& text)
{
    return byte_array_hex(text.toUtf8());
}

void append_screen_mutation_payload_summary(
    std::ostringstream&    out,
    const QString&         text = {},
    int                    row = 0,
    int                    column = 0,
    const QByteArray&      hyperlink_identity_key = {})
{
    out << qstring_hex(text) << ':'
        << row << ':'
        << column << ":0:"
        << byte_array_hex(hyperlink_identity_key);
}

void append_screen_mutation_summary(
    std::ostringstream&            out,
    const term::Screen_mutation&   mutation)
{
    out << "screen:" << static_cast<int>(term::screen_mutation_kind(mutation)) << ':';

    struct visitor_t
    {
        std::ostringstream& out;

        void operator()(const term::Screen_print_text_mutation& mutation) const
        {
            append_screen_mutation_payload_summary(
                out,
                mutation.text,
                mutation.row,
                mutation.column);
        }

        void operator()(const term::Screen_set_title_mutation& mutation) const
        {
            append_screen_mutation_payload_summary(out, mutation.title);
        }

        void operator()(const term::Screen_set_icon_name_mutation& mutation) const
        {
            append_screen_mutation_payload_summary(out, mutation.icon_name);
        }

        void operator()(const term::Screen_set_hyperlink_mutation& mutation) const
        {
            append_screen_mutation_payload_summary(out, {}, 0, 0, mutation.identity_key);
        }

        void operator()(const term::Screen_carriage_return_mutation&) const
        {
            append_screen_mutation_payload_summary(out);
        }

        void operator()(const term::Screen_line_feed_mutation&) const
        {
            append_screen_mutation_payload_summary(out);
        }

        void operator()(const term::Screen_backspace_mutation&) const
        {
            append_screen_mutation_payload_summary(out);
        }

        void operator()(const term::Screen_horizontal_tab_mutation&) const
        {
            append_screen_mutation_payload_summary(out);
        }

        void operator()(const term::Screen_bell_mutation&) const
        {
            append_screen_mutation_payload_summary(out);
        }
    };

    std::visit(visitor_t{out}, mutation);
}

bool parser_diagnostic_code_from_int(int value, term::Parser_diagnostic_code& out_code)
{
    switch (value) {
        case 0:
            out_code = term::Parser_diagnostic_code::MALFORMED_INPUT;
            return true;
        case 1:
            out_code = term::Parser_diagnostic_code::PAYLOAD_LIMIT_EXCEEDED;
            return true;
        case 2:
            out_code = term::Parser_diagnostic_code::TITLE_LIMIT_EXCEEDED;
            return true;
        case 3:
            out_code = term::Parser_diagnostic_code::UNSUPPORTED_SEQUENCE;
            return true;
        case 4:
            out_code = term::Parser_diagnostic_code::CLIPBOARD_READ_DENIED;
            return true;
        case 5:
            out_code = term::Parser_diagnostic_code::CLIPBOARD_WRITE_DENIED;
            return true;
    }

    return false;
}

bool parser_sequence_family_from_int(int value, term::Parser_sequence_family& out_family)
{
    switch (value) {
        case 0:
            out_family = term::Parser_sequence_family::NONE;
            return true;
        case 1:
            out_family = term::Parser_sequence_family::PRINTABLE;
            return true;
        case 2:
            out_family = term::Parser_sequence_family::ESC;
            return true;
        case 3:
            out_family = term::Parser_sequence_family::CSI;
            return true;
        case 4:
            out_family = term::Parser_sequence_family::OSC;
            return true;
        case 5:
            out_family = term::Parser_sequence_family::DCS;
            return true;
        case 6:
            out_family = term::Parser_sequence_family::APC;
            return true;
        case 7:
            out_family = term::Parser_sequence_family::PM;
            return true;
        case 8:
            out_family = term::Parser_sequence_family::SOS;
            return true;
    }

    return false;
}

bool parser_recovery_strategy_from_int(int value, term::Parser_recovery_strategy& out_recovery)
{
    switch (value) {
        case 0:
            out_recovery = term::Parser_recovery_strategy::NONE;
            return true;
        case 1:
            out_recovery = term::Parser_recovery_strategy::IGNORE_BYTE;
            return true;
        case 2:
            out_recovery = term::Parser_recovery_strategy::DISCARD_SEQUENCE;
            return true;
        case 3:
            out_recovery = term::Parser_recovery_strategy::DISCARD_STRING;
            return true;
        case 4:
            out_recovery = term::Parser_recovery_strategy::RESET_TO_GROUND;
            return true;
    }

    return false;
}

bool terminal_reply_kind_from_int(int value, term::Terminal_reply_kind& out_kind)
{
    switch (value) {
        case 0:
            out_kind = term::Terminal_reply_kind::RAW;
            return true;
        case 1:
            out_kind = term::Terminal_reply_kind::DA1;
            return true;
        case 2:
            out_kind = term::Terminal_reply_kind::DA2;
            return true;
        case 3:
            out_kind = term::Terminal_reply_kind::DSR_CURSOR_POSITION;
            return true;
        case 4:
            out_kind = term::Terminal_reply_kind::DECRQM;
            return true;
        case 5:
            out_kind = term::Terminal_reply_kind::OSC_QUERY;
            return true;
        case 6:
            out_kind = term::Terminal_reply_kind::TEXT_AREA_SIZE;
            return true;
    }

    return false;
}

QString qstring_from_utf8_hex(
    const std::map<std::string, std::string>&  values,
    const std::string&                         key,
    const fs::path&                            path,
    bool&                                      ok)
{
    QByteArray bytes;
    ok &= bytes_from_hex(values.at(key), bytes, path.string() + "/" + key);
    return QString::fromUtf8(bytes);
}

void append_parameter_list(std::ostringstream& out, const std::vector<int>& parameters)
{
    out << '[';
    for (std::size_t i = 0U; i < parameters.size(); ++i) {
        if (i > 0U) {
            out << ',';
        }
        out << parameters[i];
    }
    out << ']';
}

void append_color_ref(std::ostringstream& out, const term::Terminal_color_ref& color)
{
    out << static_cast<int>(color.kind) << '/'
        << color.palette_index << '/'
        << color.rgba;
}

std::string action_summary(const term::Parser_action& action)
{
    std::ostringstream out;
    out << static_cast<int>(term::parser_action_kind(action)) << ':';

    switch (term::parser_action_kind(action)) {
        case term::Parser_action_kind::SCREEN_MUTATION: {
            const term::Screen_mutation& mutation = std::get<term::Screen_mutation>(action.payload);
            append_screen_mutation_summary(out, mutation);
            break;
    }

        case term::Parser_action_kind::STYLE_MUTATION: {
            const term::Terminal_sgr_sequence& sequence =
                std::get<term::Terminal_sgr_sequence>(action.payload);
            out << "style:" << byte_array_hex(sequence.raw_parameters) << ':';
            for (const term::Terminal_sgr_operation& operation : sequence.operations) {
                out << static_cast<int>(operation.kind) << '/'
                    << operation.attributes << '/';
                append_color_ref(out, operation.color);
                out << ';';
            }
            break;
    }

        case term::Parser_action_kind::CONTROL_SEQUENCE: {
            const term::Parser_control_sequence& sequence =
                std::get<term::Parser_control_sequence>(action.payload);
            out << "control:" << static_cast<int>(sequence.family) << ':'
                << static_cast<int>(sequence.action) << ':';
            append_parameter_list(out, sequence.parameters);
            out << ':' << byte_array_hex(sequence.private_marker)
                << ':' << byte_array_hex(sequence.intermediates)
                << ':' << byte_array_hex(sequence.final_bytes)
                << ':' << byte_array_hex(sequence.payload)
                << ':' << static_cast<int>(sequence.terminator)
                << ':' << byte_array_hex(sequence.raw_bytes);
            break;
    }

        case term::Parser_action_kind::TERMINAL_REPLY: {
            const term::Terminal_reply& reply = std::get<term::Terminal_reply>(action.payload);
            out << "reply:" << static_cast<int>(reply.kind) << ':'
                << static_cast<int>(reply.source_family) << ':'
                << byte_array_hex(reply.wire_bytes) << ':'
                << qstring_hex(reply.source_sequence);
            break;
    }

        case term::Parser_action_kind::TERMINAL_QUERY: {
            const term::Terminal_color_query& query =
                std::get<term::Terminal_color_query>(action.payload);
            out << "query:" << static_cast<int>(query.kind) << ':'
                << query.palette_index << ':'
                << qstring_hex(query.source_sequence);
            break;
    }

        case term::Parser_action_kind::DIAGNOSTIC: {
            const term::Parser_payload_diagnostic& diagnostic = std::get<term::Parser_payload_diagnostic>(
                action.payload);
            out << "diagnostic:" << static_cast<int>(diagnostic.code) << ':'
                << qstring_hex(diagnostic.source_sequence) << ':'
                << diagnostic.raw_payload_size << ':'
                << diagnostic.limit_bytes << ':'
                << static_cast<int>(diagnostic.family) << ':'
                << static_cast<int>(diagnostic.recovery);
            break;
    }

        case term::Parser_action_kind::NOTIFICATION: {
            const term::Parser_notification& notification =
                std::get<term::Parser_notification>(action.payload);
            out << "notification:" << static_cast<int>(notification.kind) << ':'
                << qstring_hex(notification.text);
            break;
    }

        case term::Parser_action_kind::HOST_REQUEST: {
            const term::Terminal_osc52_write_request& request = std::get<term::Terminal_osc52_write_request>(
                action.payload);
            out << "host:" << request.request_id << ':'
                << qstring_hex(request.target_selection) << ':'
                << byte_array_hex(request.decoded_payload) << ':'
                << request.raw_payload_size << ':'
                << qstring_hex(request.source_sequence);
            break;
        }
    }

    return out.str();
}

bool is_side_effect_action_kind(term::Parser_action_kind kind)
{
    switch (kind) {
        case term::Parser_action_kind::TERMINAL_REPLY:
        case term::Parser_action_kind::TERMINAL_QUERY:
        case term::Parser_action_kind::DIAGNOSTIC:
        case term::Parser_action_kind::NOTIFICATION:
        case term::Parser_action_kind::HOST_REQUEST:
            return true;

        case term::Parser_action_kind::SCREEN_MUTATION:
        case term::Parser_action_kind::STYLE_MUTATION:
        case term::Parser_action_kind::CONTROL_SEQUENCE:
            return false;
    }

    return false;
}

int action_kind_count(
    const std::vector<std::string>&        actions,
    term::Parser_action_kind               kind)
{
    const std::string prefix = std::to_string(static_cast<int>(kind)) + ':';
    int count = 0;
    for (const std::string& action : actions) {
        if (action.rfind(prefix, 0U) == 0U) {
            ++count;
        }
    }
    return count;
}

bool action_summaries_equal(
    const std::vector<std::string>&        left,
    const std::vector<std::string>&        right,
    const std::string&                     label)
{
    bool ok = true;

    ok &= check(left.size() == right.size(),
        label + ": side-effect action count differs: " +
            std::to_string(left.size()) + " vs " + std::to_string(right.size()));

    const std::size_t action_count = std::min(left.size(), right.size());
    for (std::size_t i = 0U; i < action_count; ++i) {
        ok &= check(left[i] == right[i],
            label + ": side-effect action differs at index " + std::to_string(i) +
            "\nchunked:  " + left[i] + "\none-shot: " + right[i]);
    }

    return ok;
}

bool color_states_equal(
    const term::Terminal_color_state&      left,
    const term::Terminal_color_state&      right)
{
    return
        left.default_foreground_rgba == right.default_foreground_rgba &&
        left.default_background_rgba == right.default_background_rgba &&
        left.cursor_rgba             == right.cursor_rgba             &&
        left.palette_rgba            == right.palette_rgba;
}

bool modes_equal(
    const term::Terminal_mode_state&       left,
    const term::Terminal_mode_state&       right)
{
    return
        left.application_cursor_keys == right.application_cursor_keys &&
        left.reverse_video           == right.reverse_video           &&
        left.origin_mode             == right.origin_mode             &&
        left.autowrap                == right.autowrap                &&
        left.cursor_visible          == right.cursor_visible          &&
        left.mouse_tracking          == right.mouse_tracking          &&
        left.focus_reporting         == right.focus_reporting         &&
        left.sgr_mouse_encoding      == right.sgr_mouse_encoding      &&
        left.alternate_scroll        == right.alternate_scroll        &&
        left.bracketed_paste         == right.bracketed_paste         &&
        left.synchronized_output     == right.synchronized_output;
}

bool cells_equal(
    const term::Terminal_render_cell&      left,
    const term::Terminal_render_cell&      right)
{
    return
        left.position          == right.position          &&
        left.text              == right.text              &&
        left.hyperlink_id      == right.hyperlink_id      &&
        left.display_width     == right.display_width     &&
        left.wide_continuation == right.wide_continuation &&
        left.style_id          == right.style_id;
}

const term::Terminal_render_cell* find_cell(
    const term::Terminal_render_snapshot&  snapshot,
    int                                    row,
    int                                    column)
{
    for (const term::Terminal_render_cell& cell : snapshot.cells) {
        if (cell.position.row == row && cell.position.column == column) {
            return &cell;
        }
    }

    return nullptr;
}

bool hyperlinks_equal(
    const term::Terminal_render_hyperlink_metadata&    left,
    const term::Terminal_render_hyperlink_metadata&    right)
{
    return
        left.hyperlink_id == right.hyperlink_id &&
        left.identity_key == right.identity_key &&
        left.uri          == right.uri;
}

bool viewport_states_equal(
    const term::Terminal_viewport_state&   left,
    const term::Terminal_viewport_state&   right)
{
    return
        left.active_buffer                  == right.active_buffer    &&
        left.scrollback_rows                == right.scrollback_rows  &&
        left.visible_rows                   == right.visible_rows     &&
        left.offset_from_tail               == right.offset_from_tail &&
        left.follow_tail                    == right.follow_tail      &&
        left.alternate_screen_scroll_policy == right.alternate_screen_scroll_policy;
}

bool snapshots_equivalent(
    const term::Terminal_render_snapshot&  left,
    const term::Terminal_render_snapshot&  right,
    const std::string&                     label)
{
    bool ok = true;

    ok &= check(left.grid_size.rows == right.grid_size.rows &&
        left.grid_size.columns == right.grid_size.columns,
        label + ": grid size changed");
    ok &= check(viewport_states_equal(left.viewport, right.viewport),
        label + ": viewport changed");
    ok &= check(color_states_equal(left.color_state, right.color_state),
        label + ": color state changed");
    ok &= check(left.styles == right.styles, label + ": style table changed");
    ok &= check(left.cells.size() == right.cells.size(), label + ": cell count changed");

    const std::size_t cell_count = std::min(left.cells.size(), right.cells.size());
    for (std::size_t i = 0U; i < cell_count; ++i) {
        ok &= check(cells_equal(left.cells[i], right.cells[i]),
            label + ": cell changed at index " + std::to_string(i));
    }

    ok &= check(left.hyperlinks.size() == right.hyperlinks.size(),
        label + ": hyperlink metadata count changed");
    const std::size_t hyperlink_count =
        std::min(left.hyperlinks.size(), right.hyperlinks.size());
    for (std::size_t i = 0U; i < hyperlink_count; ++i) {
        ok &= check(hyperlinks_equal(left.hyperlinks[i], right.hyperlinks[i]),
            label + ": hyperlink metadata changed at index " + std::to_string(i));
    }

    ok &= check(left.cursor.position == right.cursor.position &&
        left.cursor.shape == right.cursor.shape &&
        left.cursor.visible == right.cursor.visible &&
        left.cursor.blink_enabled == right.cursor.blink_enabled,
        label + ": cursor changed");
    ok &= check(left.ime_preedit.text == right.ime_preedit.text &&
        left.ime_preedit.cursor_position == right.ime_preedit.cursor_position &&
        left.ime_preedit.active == right.ime_preedit.active,
        label + ": IME state changed");
    ok &= check(left.selection_spans.size() == right.selection_spans.size(),
        label + ": selection spans changed");
    ok &= check(modes_equal(left.modes, right.modes), label + ": modes changed");

    return ok;
}

bool validate_ingest_result(
    const Test_case&                           test_case,
    const term::Terminal_screen_model&         model,
    const term::Terminal_screen_model_result&  result,
    std::uint64_t                              sequence,
    const std::string&                         label)
{
    bool ok                 = true;
    int  previous_dirty_row = -1;

    for (int dirty_row : result.dirty_rows) {
        ok                 &= check(dirty_row > previous_dirty_row, label + ": dirty rows must be sorted");
        ok                 &= check(dirty_row >= 0 && dirty_row < test_case.config.grid_size.rows,
            label + ": dirty row out of range");
        previous_dirty_row  = dirty_row;
    }

    ok &= check(result.scrollback_rows == model.scrollback_size(),
        label + ": result scrollback count must match model");
    ok &= check(result.scrollback_rows >= 0 &&
        result.scrollback_rows <= test_case.config.scrollback_limit,
        label + ": scrollback count must stay bounded");

    const term::terminal_grid_position_t cursor = model.cursor_position();
    ok &= check(cursor.row >= 0 && cursor.row < test_case.config.grid_size.rows,
        label + ": cursor row out of range");
    ok &= check(cursor.column >= 0 && cursor.column < test_case.config.grid_size.columns,
        label + ": cursor column out of range");

    const term::Terminal_render_snapshot snapshot = model.render_snapshot(sequence);
    ok &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        label + ": render snapshot must validate");

    return ok;
}

Run_result collect_run_result(
    const term::Terminal_screen_model&             model,
    std::uint64_t                                  sequence,
    std::vector<std::string>                       actions,
    std::vector<std::string>                       side_effects,
    std::vector<term::Parser_payload_diagnostic>   diagnostics,
    std::vector<term::Terminal_reply>              replies)
{
    return {
        model.render_snapshot(sequence),
        model.visible_text(),
        model.title(),
        model.icon_name(),
        model.cursor_position(),
        model.active_buffer_id(),
        model.scrollback_size(),
        std::move(actions),
        std::move(side_effects),
        std::move(diagnostics),
        std::move(replies),
    };
}

bool run_with_chunks(
    const Test_case&   test_case,
    const Chunk_plan&  plan,
    Run_result&        out_result)
{
    term::Terminal_screen_model model(test_case.config);
    std::vector<std::string> actions;
    std::vector<std::string> side_effects;
    std::vector<term::Parser_payload_diagnostic> diagnostics;
    std::vector<term::Terminal_reply> replies;
    std::size_t   offset   = 0U;
    std::uint64_t sequence = 1U;
    bool          ok       = true;

    for (std::size_t chunk_index = 0U; chunk_index < plan.chunk_sizes.size(); ++chunk_index) {
        const int         requested_size = plan.chunk_sizes[chunk_index];
        const std::size_t remaining      = static_cast<std::size_t>(test_case.bytes.size()) - offset;
        const std::size_t chunk_size     =
            std::min<std::size_t>(remaining, static_cast<std::size_t>(requested_size));

        const QByteArrayView chunk(
            test_case.bytes.constData() + static_cast<qsizetype>(offset),
            static_cast<qsizetype>(chunk_size));
        const term::Terminal_screen_model_result result = model.ingest(chunk);
        for (const term::Parser_action& action : result.actions) {
            std::string summary = action_summary(action);
            if (is_side_effect_action_kind(term::parser_action_kind(action))) {
                side_effects.push_back(summary);
            }
            if (term::parser_action_kind(action) == term::Parser_action_kind::DIAGNOSTIC) {
                diagnostics.push_back(std::get<term::Parser_payload_diagnostic>(action.payload));
            }
            if (term::parser_action_kind(action) == term::Parser_action_kind::TERMINAL_REPLY) {
                replies.push_back(std::get<term::Terminal_reply>(action.payload));
            }
            actions.push_back(std::move(summary));
        }

        ok &= validate_ingest_result(
            test_case,
            model,
            result,
            sequence,
            test_case.name + "/" + plan.name + "/" + std::to_string(chunk_index));

        offset += chunk_size;
        ++sequence;
    }

    ok &= check(offset == static_cast<std::size_t>(test_case.bytes.size()),
        test_case.name + "/" + plan.name + ": chunk plan did not consume the stream");

    out_result = collect_run_result(
        model,
        sequence,
        std::move(actions),
        std::move(side_effects),
        std::move(diagnostics),
        std::move(replies));
    ok &= check(term::validate_render_snapshot(out_result.snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        test_case.name + "/" + plan.name + ": final snapshot must validate");
    return ok;
}

std::vector<int> one_chunk_plan(std::size_t byte_count)
{
    return {static_cast<int>(byte_count)};
}

std::vector<int> single_byte_plan(std::size_t byte_count)
{
    return std::vector<int>(byte_count, 1);
}

std::vector<int> deterministic_chunk_plan(
    std::size_t                byte_count,
    std::uint64_t              seed,
    int                        max_chunk_size)
{
    Deterministic_rng rng(seed);
    std::vector<int> chunk_sizes;
    std::size_t offset = 0U;

    while (offset < byte_count) {
        const std::size_t remaining = byte_count - offset;
        const int bounded_max =
            std::max(1, std::min<int>(max_chunk_size, static_cast<int>(remaining)));
        const int chunk_size = 1 + rng.bounded(bounded_max);
        chunk_sizes.push_back(chunk_size);
        offset += static_cast<std::size_t>(chunk_size);
    }

    if (byte_count > 1U && chunk_sizes.size() == 1U) {
        chunk_sizes = {1, static_cast<int>(byte_count - 1U)};
    }

    return chunk_sizes;
}

std::vector<int> chunk_plan_from_boundaries(
    std::size_t                byte_count,
    std::vector<std::size_t>   boundaries,
    const std::string&         label,
    bool&                      ok)
{
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());

    std::vector<int> chunk_sizes;
    std::size_t offset = 0U;
    for (std::size_t boundary : boundaries) {
        if (boundary <= offset || boundary >= byte_count) {
            continue;
        }

        chunk_sizes.push_back(static_cast<int>(boundary - offset));
        offset = boundary;
    }

    if (offset < byte_count) {
        chunk_sizes.push_back(static_cast<int>(byte_count - offset));
    }

    ok &= check(chunk_sizes.size() > 1U,
        label + ": boundary chunk plan degenerated to one-shot");
    return chunk_sizes;
}

void append_large_payload_chunk_plans(
    const Test_case&           test_case,
    std::vector<Chunk_plan>&   plans,
    bool&                      ok)
{
    const std::size_t byte_count = static_cast<std::size_t>(test_case.bytes.size());
    if (byte_count <= 1U) {
        return;
    }

    std::vector<std::size_t> coarse_boundaries = {1U, 2U, byte_count / 2U};
    if (byte_count >= 2U) { coarse_boundaries.push_back(byte_count - 1U); }
    if (byte_count >= 3U) { coarse_boundaries.push_back(byte_count - 2U); }

    plans.push_back({
        "coarse-boundary",
        chunk_plan_from_boundaries(
            byte_count,
            std::move(coarse_boundaries),
            test_case.name + "/coarse-boundary",
            ok),
    });

    if (!test_case.has_large_payload_limit_boundaries) {
        return;
    }

    const std::size_t exact_payload_limit =
        test_case.large_payload_introducer_size + test_case.large_payload_limit_bytes;
    const std::size_t over_limit_payload = exact_payload_limit + 1U;

    if (byte_count > over_limit_payload) {
        plans.push_back({
            "payload-limit-boundary",
            chunk_plan_from_boundaries(
                byte_count,
                { exact_payload_limit, over_limit_payload, over_limit_payload + 2U },
                test_case.name + "/payload-limit-boundary",
                ok),
        });

        plans.push_back({
            "payload-limit-byte-splits",
            chunk_plan_from_boundaries(
                byte_count,
                {
                    1U,
                    test_case.large_payload_introducer_size,
                    exact_payload_limit - 1U,
                    exact_payload_limit,
                    over_limit_payload,
                    over_limit_payload + 1U,
                    over_limit_payload + 2U,
                },
                test_case.name + "/payload-limit-byte-splits",
                ok),
        });
    }
}

std::vector<Chunk_plan> chunk_plans_for_case(const Test_case& test_case, bool& ok)
{
    const std::size_t byte_count = static_cast<std::size_t>(test_case.bytes.size());
    std::vector<Chunk_plan> plans;
    plans.push_back({"one-shot", one_chunk_plan(byte_count)});

    if (byte_count <= 1U) {
        return plans;
    }

    if (!test_case.exhaustive_byte_chunks || byte_count > 65536U) {
        append_large_payload_chunk_plans(test_case, plans, ok);
        return plans;
    }

    if (test_case.exhaustive_byte_chunks && byte_count <= 4096U) {
        plans.push_back({"single-byte", single_byte_plan(byte_count)});
    }

    plans.push_back({
        "lcg-small",
        deterministic_chunk_plan(byte_count, test_case.chunk_seed ^ 0x4f1bbcdc5d2f13a1ULL, 7),
    });
    plans.push_back({
        "lcg-wide",
        deterministic_chunk_plan(byte_count, test_case.chunk_seed ^ 0x8d58ac26afe12e47ULL, 251),
    });

    return plans;
}

bool check_expected_diagnostics(
    const Test_case&   test_case,
    const Run_result&  result,
    const std::string& label)
{
    if (test_case.expected_diagnostics.empty()) {
        return true;
    }

    bool ok = true;
    ok &= check(result.diagnostics.size() == test_case.expected_diagnostics.size(),
        label + ": diagnostic identity count differs from expectation");

    const std::size_t diagnostic_count =
        std::min(result.diagnostics.size(), test_case.expected_diagnostics.size());
    for (std::size_t i = 0U; i < diagnostic_count; ++i) {
        const Expected_diagnostic&             expected         = test_case.expected_diagnostics[i];
        const term::Parser_payload_diagnostic& actual           = result.diagnostics[i];
        const std::string                      diagnostic_label = label + ": diagnostic " + std::to_string(i);

        ok &= check(actual.code == expected.code, diagnostic_label + " code differs");
        ok &= check(actual.source_sequence == expected.source_sequence,
            diagnostic_label + " source sequence differs");
        ok &= check(actual.family == expected.family, diagnostic_label + " family differs");
        ok &= check(actual.raw_payload_size == expected.raw_payload_size,
            diagnostic_label + " raw payload size differs");
        ok &= check(actual.limit_bytes == expected.limit_bytes,
            diagnostic_label + " limit bytes differs");
        ok &= check(actual.recovery == expected.recovery,
            diagnostic_label + " recovery differs");
    }

    return ok;
}

bool check_expected_replies(
    const Test_case&   test_case,
    const Run_result&  result,
    const std::string& label)
{
    if (test_case.expected_replies.empty()) {
        return true;
    }

    bool ok = true;
    ok &= check(result.replies.size() == test_case.expected_replies.size(),
        label + ": terminal reply identity count differs from expectation");

    const std::size_t reply_count =
        std::min(result.replies.size(), test_case.expected_replies.size());
    for (std::size_t i = 0U; i < reply_count; ++i) {
        const Expected_reply&       expected    = test_case.expected_replies[i];
        const term::Terminal_reply& actual      = result.replies[i];
        const std::string           reply_label = label + ": terminal reply " + std::to_string(i);

        ok &= check(actual.kind == expected.kind, reply_label + " kind differs");
        ok &= check(actual.source_family == expected.source_family,
            reply_label + " source family differs");
        ok &= check(actual.wire_bytes == expected.wire_bytes,
            reply_label + " wire bytes differ");
        ok &= check(actual.source_sequence == expected.source_sequence,
            reply_label + " source sequence differs");
    }

    return ok;
}

bool check_expected_final_state(
    const Test_case&   test_case,
    const Run_result&  result,
    const std::string& label)
{
    bool ok = true;

    if (test_case.has_expected_visible_text_utf8) {
        ok &= check(result.visible_text.toUtf8() == test_case.expected_visible_text_utf8,
            label + ": visible text differs from seed expectation");
    }

    if (test_case.has_expected_title_utf8) {
        ok &= check(result.title.toUtf8() == test_case.expected_title_utf8,
            label + ": title differs from seed expectation");
    }

    if (test_case.has_expected_cursor) {
        ok &= check(result.cursor == test_case.expected_cursor,
            label + ": cursor position differs from seed expectation");
    }

    if (test_case.has_expected_wide_cell) {
        const term::Terminal_render_cell* base_cell = find_cell(
            result.snapshot,
            test_case.expected_wide_cell_row,
            test_case.expected_wide_cell_column);
        ok &= check(base_cell != nullptr, label + ": expected wide base cell is missing");
        if (base_cell != nullptr) {
            ok &= check(!base_cell->wide_continuation,
                label + ": expected wide base cell is a continuation");
            ok &= check(base_cell->display_width == test_case.expected_wide_cell_width,
                label + ": expected wide base cell width differs");
            ok &= check(base_cell->text.toUtf8() == test_case.expected_wide_cell_text_utf8,
                label + ": expected wide base cell text differs");
        }

        const term::Terminal_render_cell* continuation_cell = find_cell(
            result.snapshot,
            test_case.expected_wide_cell_row,
            test_case.expected_wide_continuation_column);
        ok &= check(
            continuation_cell != nullptr,
            label + ": expected wide continuation cell is missing");
        if (continuation_cell != nullptr) {
            ok &= check(continuation_cell->wide_continuation,
                label + ": expected wide continuation cell is not a continuation");
            ok &= check(continuation_cell->display_width == 0,
                label + ": expected wide continuation cell has nonzero width");
        }
    }

    if (test_case.expected_diagnostic_count >= 0) {
        ok &= check(action_kind_count(result.actions, term::Parser_action_kind::DIAGNOSTIC) ==
            test_case.expected_diagnostic_count,
            label + ": diagnostic count differs from expectation");
    }

    if (test_case.expected_reply_count >= 0) {
        ok &= check(action_kind_count(result.actions, term::Parser_action_kind::TERMINAL_REPLY) ==
            test_case.expected_reply_count,
            label + ": terminal reply count differs from expectation");
    }

    ok &= check_expected_diagnostics(test_case, result, label);
    ok &= check_expected_replies(test_case, result, label);

    return ok;
}

bool run_case(const Test_case& test_case)
{
    bool ok = true;
    const std::vector<Chunk_plan> plans = chunk_plans_for_case(test_case, ok);

    Run_result baseline;
    ok &= run_with_chunks(test_case, plans.front(), baseline);
    ok &= check_expected_final_state(test_case, baseline, test_case.name + "/one-shot");

    for (std::size_t i = 1U; i < plans.size(); ++i) {
        Run_result chunked;
        ok &= run_with_chunks(test_case, plans[i], chunked);
        ok &= check_expected_final_state(test_case, chunked, test_case.name + "/" + plans[i].name);

        ok &= check(chunked.visible_text == baseline.visible_text,
            test_case.name + "/" + plans[i].name + ": visible text differs from one-shot");
        ok &= check(chunked.title == baseline.title,
            test_case.name + "/" + plans[i].name + ": title differs from one-shot");
        ok &= check(chunked.icon_name == baseline.icon_name,
            test_case.name + "/" + plans[i].name + ": icon name differs from one-shot");
        ok &= check(chunked.cursor == baseline.cursor,
            test_case.name + "/" + plans[i].name + ": cursor differs from one-shot");
        ok &= check(chunked.active_buffer == baseline.active_buffer,
            test_case.name + "/" + plans[i].name + ": active buffer differs from one-shot");
        ok &= check(chunked.scrollback_size == baseline.scrollback_size,
            test_case.name + "/" + plans[i].name + ": scrollback differs from one-shot");
        ok &= snapshots_equivalent(
            chunked.snapshot,
            baseline.snapshot,
            test_case.name + "/" + plans[i].name);
        ok &= action_summaries_equal(
            chunked.side_effects,
            baseline.side_effects,
            test_case.name + "/" + plans[i].name);

        if (test_case.compare_actions) {
            ok &= check(chunked.actions == baseline.actions,
                test_case.name + "/" + plans[i].name + ": parser actions differ from one-shot");
        }
    }

    return ok;
}

term::Parser_diagnostic_code diagnostic_code_value(
    const std::map<std::string, std::string>&  values,
    const std::string&                         key,
    const fs::path&                            path,
    bool&                                      ok)
{
    int value = integer_value(values, key, 0, path, ok);
    term::Parser_diagnostic_code code = term::Parser_diagnostic_code::MALFORMED_INPUT;
    ok &= check(parser_diagnostic_code_from_int(value, code),
        "seed field '" + key + "' has unsupported diagnostic code: " + path.string());
    return code;
}

term::Parser_sequence_family sequence_family_value(
    const std::map<std::string, std::string>&  values,
    const std::string&                         key,
    const fs::path&                            path,
    bool&                                      ok)
{
    int value = integer_value(values, key, 0, path, ok);
    term::Parser_sequence_family family = term::Parser_sequence_family::NONE;
    ok &= check(parser_sequence_family_from_int(value, family),
        "seed field '" + key + "' has unsupported sequence family: " + path.string());
    return family;
}

term::Parser_recovery_strategy recovery_strategy_value(
    const std::map<std::string, std::string>&  values,
    const std::string&                         key,
    const fs::path&                            path,
    bool&                                      ok)
{
    int value = integer_value(values, key, 0, path, ok);
    term::Parser_recovery_strategy recovery = term::Parser_recovery_strategy::NONE;
    ok &= check(parser_recovery_strategy_from_int(value, recovery),
        "seed field '" + key + "' has unsupported recovery strategy: " + path.string());
    return recovery;
}

term::Terminal_reply_kind terminal_reply_kind_value(
    const std::map<std::string, std::string>&  values,
    const std::string&                         key,
    const fs::path&                            path,
    bool&                                      ok)
{
    int value = integer_value(values, key, 0, path, ok);
    term::Terminal_reply_kind kind = term::Terminal_reply_kind::RAW;
    ok &= check(terminal_reply_kind_from_int(value, kind),
        "seed field '" + key + "' has unsupported terminal reply kind: " + path.string());
    return kind;
}

std::size_t size_value(
    const std::map<std::string, std::string>&  values,
    const std::string&                         key,
    const fs::path&                            path,
    bool&                                      ok)
{
    const std::uint64_t value = uint64_value(values, key, 0U, path, ok);
    return static_cast<std::size_t>(value);
}

void load_expected_diagnostics(
    const std::map<std::string, std::string>&  values,
    const fs::path&                            path,
    Test_case&                                 out_case,
    bool&                                      ok)
{
    if (out_case.expected_diagnostic_count <= 0) {
        return;
    }

    out_case.expected_diagnostics.reserve(
        static_cast<std::size_t>(out_case.expected_diagnostic_count));
    for (int i = 0; i < out_case.expected_diagnostic_count; ++i) {
        const std::string prefix = "expected_diagnostic_" + std::to_string(i);
        out_case.expected_diagnostics.push_back({
            diagnostic_code_value(values, prefix + "_code", path, ok),
            qstring_from_utf8_hex(values, prefix + "_source_sequence_utf8_hex", path, ok),
            sequence_family_value(values, prefix + "_family", path, ok),
            size_value(values, prefix + "_raw_payload_size", path, ok),
            size_value(values, prefix + "_limit_bytes", path, ok),
            recovery_strategy_value(values, prefix + "_recovery", path, ok),
        });
    }
}

void load_expected_replies(
    const std::map<std::string, std::string>&  values,
    const fs::path&                            path,
    Test_case&                                 out_case,
    bool&                                      ok)
{
    if (out_case.expected_reply_count <= 0) {
        return;
    }

    out_case.expected_replies.reserve(static_cast<std::size_t>(out_case.expected_reply_count));
    for (int i = 0; i < out_case.expected_reply_count; ++i) {
        const std::string prefix = "expected_reply_" + std::to_string(i);

        QByteArray wire_bytes;
        ok &= bytes_from_hex(
            values.at(prefix + "_wire_bytes_hex"),
            wire_bytes,
            path.string() + "/" + prefix + "_wire_bytes_hex");

        out_case.expected_replies.push_back({
            terminal_reply_kind_value(values, prefix + "_kind", path, ok),
            sequence_family_value(values, prefix + "_source_family", path, ok),
            std::move(wire_bytes),
            qstring_from_utf8_hex(values, prefix + "_source_sequence_utf8_hex", path, ok),
        });
    }
}

bool load_seed_file(const fs::path& path, Test_case& out_case)
{
    const std::string                        text   = read_text_file(path);
    const std::map<std::string, std::string> values = parse_key_values(text);
    bool                                     ok     = validate_seed_provenance_header(text, path);

    ok &= validate_seed_keys(values, path);
    ok &= validate_all_or_none_keys(
        values,
        {"expected_cursor_row", "expected_cursor_column"},
        path);
    ok &= validate_all_or_none_keys(
        values,
        {
            "expected_wide_cell_row",
            "expected_wide_cell_column",
            "expected_wide_cell_display_width",
            "expected_wide_continuation_column",
            "expected_wide_cell_text_utf8_hex",
        },
        path);
    ok &= validate_seed_has_meaningful_expectation(values, path);
    ok &= check(contains_key(values, "compare_actions"),
        "seed must explicitly set compare_actions: " + path.string());

    const auto stream_it = values.find("byte_stream_hex");
    if (stream_it == values.end()) {
        std::cerr << "FAIL: seed missing byte_stream_hex: " << path << '\n';
        return false;
    }

    out_case.name = path.stem().string();
    const auto name_it = values.find("name");
    if (name_it != values.end() && !name_it->second.empty()) {
        out_case.name = name_it->second;
    }

    out_case.chunk_seed = uint64_value(values, "chunk_seed", 0x6c8e9cf570932bd5ULL, path, ok);
    out_case.expected_diagnostic_count =
        integer_value(values, "expected_diagnostic_count", -1, path, ok);
    out_case.expected_reply_count = integer_value(values, "expected_reply_count", -1, path, ok);
    out_case.compare_actions      = integer_value(values, "compare_actions", 0, path, ok) != 0;
    out_case.exhaustive_byte_chunks =
        integer_value(values, "exhaustive_byte_chunks", 1, path, ok) != 0;

    static const std::vector<std::string> k_diagnostic_suffixes = {
        "_code",
        "_source_sequence_utf8_hex",
        "_family",
        "_raw_payload_size",
        "_limit_bytes",
        "_recovery",
    };
    static const std::vector<std::string> k_reply_suffixes = {
        "_kind",
        "_source_family",
        "_wire_bytes_hex",
        "_source_sequence_utf8_hex",
    };
    ok &= validate_indexed_expectation_keys(
        values,
        path,
        "expected_diagnostic_count",
        "expected_diagnostic_",
        k_diagnostic_suffixes,
        out_case.expected_diagnostic_count);
    ok &= validate_indexed_expectation_keys(
        values,
        path,
        "expected_reply_count",
        "expected_reply_",
        k_reply_suffixes,
        out_case.expected_reply_count);

    if (!ok) {
        return false;
    }

    out_case.config = {
        term::terminal_grid_size_t{
            integer_value(values, "grid_rows", 3, path, ok),
            integer_value(values, "grid_columns", 12, path, ok),
        },
        integer_value(values, "scrollback_limit", 8, path, ok),
        integer_value(values, "tab_width", 4, path, ok),
    };

    ok &= bytes_from_hex(stream_it->second, out_case.bytes, out_case.name + "/byte_stream_hex");

    const auto visible_it = values.find("expected_visible_text_utf8_hex");
    if (visible_it != values.end()) {
        out_case.has_expected_visible_text_utf8 = true;
        ok &= bytes_from_hex(
            visible_it->second,
            out_case.expected_visible_text_utf8,
            out_case.name + "/expected_visible_text_utf8_hex");
    }

    const auto title_it = values.find("expected_title_utf8_hex");
    if (title_it != values.end()) {
        out_case.has_expected_title_utf8 = true;
        ok &= bytes_from_hex(
            title_it->second,
            out_case.expected_title_utf8,
            out_case.name + "/expected_title_utf8_hex");
    }

    if (contains_key(values, "expected_cursor_row") &&
        contains_key(values, "expected_cursor_column"))
    {
        out_case.has_expected_cursor = true;
        out_case.expected_cursor     = {
            integer_value(values, "expected_cursor_row", 0, path, ok),
            integer_value(values, "expected_cursor_column", 0, path, ok),
        };
    }

    if (contains_key(values, "expected_wide_cell_row") &&
        contains_key(values, "expected_wide_cell_column") &&
        contains_key(values, "expected_wide_cell_display_width") &&
        contains_key(values, "expected_wide_continuation_column") &&
        contains_key(values, "expected_wide_cell_text_utf8_hex"))
    {
        out_case.has_expected_wide_cell = true;
        out_case.expected_wide_cell_row =
            integer_value(values, "expected_wide_cell_row", 0, path, ok);
        out_case.expected_wide_cell_column =
            integer_value(values, "expected_wide_cell_column", 0, path, ok);
        out_case.expected_wide_cell_width =
            integer_value(values, "expected_wide_cell_display_width", 0, path, ok);
        out_case.expected_wide_continuation_column  = integer_value(
            values,
            "expected_wide_continuation_column",
            0,
            path,
            ok);
        ok                                         &= bytes_from_hex(
            values.at("expected_wide_cell_text_utf8_hex"),
            out_case.expected_wide_cell_text_utf8,
            out_case.name + "/expected_wide_cell_text_utf8_hex");
        ok                                         &= check(out_case.expected_wide_cell_width > 1,
            out_case.name + ": expected wide cell width must be greater than one");
    }

    load_expected_diagnostics(values, path, out_case, ok);
    load_expected_replies(values, path, out_case, ok);

    return ok;
}

std::vector<Test_case> load_seed_cases(const fs::path& seed_dir, bool& ok)
{
    ok = true;
    std::vector<fs::path> seed_paths;
    for (const fs::directory_entry& entry : fs::directory_iterator(seed_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".vnm_seed") {
            seed_paths.push_back(entry.path());
        }
    }

    std::sort(seed_paths.begin(), seed_paths.end());

    static const std::vector<std::string> k_required_seed_names = {
        "ascii_wrap_scroll",
        "control_sequence_title",
        "string_recovery_query",
        "utf8_wide_boundary",
    };
    std::vector<std::string> seed_names;
    seed_names.reserve(seed_paths.size());
    for (const fs::path& path : seed_paths) {
        seed_names.push_back(path.stem().string());
    }

    ok &= check(seed_names == k_required_seed_names,
        "seed directory must contain exactly the required parser randomized seed corpus");

    std::vector<Test_case> cases;
    for (const fs::path& path : seed_paths) {
        Test_case test_case;
        if (!load_seed_file(path, test_case)) {
            ok = false;
            continue;
        }
        cases.push_back(std::move(test_case));
    }

    ok &= check(!cases.empty(), "seed directory must contain .vnm_seed files");
    return cases;
}

QByteArray generated_control_mix(std::uint64_t seed)
{
    static const std::vector<QByteArray> k_tokens = {
        QByteArrayLiteral("a"),
        QByteArrayLiteral("Z"),
        QByteArrayLiteral("09"),
        QByteArrayLiteral("\r"),
        QByteArrayLiteral("\n"),
        QByteArrayLiteral("\b"),
        QByteArrayLiteral("\t"),
        QByteArrayLiteral("\x1b[0m"),
        QByteArrayLiteral("\x1b[1m"),
        QByteArrayLiteral("\x1b[31m"),
        QByteArrayLiteral("\x1b[38;5;196m"),
        QByteArrayLiteral("\x1b[2;1H"),
        QByteArrayLiteral("\x1b[3;4H"),
        QByteArrayLiteral("\x1b[2J"),
        QByteArrayLiteral("\x1b[K"),
        QByteArrayLiteral("\x1b[?25l"),
        QByteArrayLiteral("\x1b[?25h"),
        QByteArrayLiteral("\x1b[?1000h"),
        QByteArrayLiteral("\x1b[?1000l"),
        QByteArrayLiteral("\x1b]0;generated-title\a"),
        QByteArrayLiteral("\x1b]10;?\x1b\\"),
        QByteArrayLiteral("\x1bPignored\x1b\\"),
        QByteArrayLiteral("\xe2\x82\xac"),
        QByteArray::fromHex("e4b880"),
    };

    Deterministic_rng rng(seed);
    QByteArray bytes;
    for (int i = 0; i < 180; ++i) {
        bytes.append(k_tokens[static_cast<std::size_t>(rng.bounded(
            static_cast<int>(k_tokens.size())))]);
    }
    return bytes;
}

QByteArray malformed_utf8_case()
{
    return QByteArray::fromHex("41 C0 42 E2 28 43");
}

QByteArray unterminated_utf8_case()
{
    return QByteArray::fromHex("41 E2 82");
}

QByteArray unterminated_string_case(const QByteArray& introducer)
{
    QByteArray bytes = QByteArrayLiteral("A");
    bytes.append(introducer);
    bytes.append(QByteArrayLiteral("unterminated"));
    return bytes;
}

QByteArray over_limit_string_case(const QByteArray& introducer, std::size_t payload_limit)
{
    QByteArray bytes = introducer;
    bytes.append(QByteArray(static_cast<int>(payload_limit + 1U), 'x'));
    bytes.append("\x1b\\OK", 4);
    return bytes;
}

QByteArray terminated_unsupported_string_case(const QByteArray& introducer)
{
    QByteArray bytes = introducer;
    bytes.append(QByteArrayLiteral("ignored"));
    bytes.append("\x1b\\R", 3);
    return bytes;
}

QByteArray string_recovery_case(const QByteArray& introducer)
{
    QByteArray bytes = QByteArrayLiteral("A");
    bytes.append(introducer);
    bytes.append(QByteArrayLiteral("recover"));
    bytes.append("\x1b[0mB", 5);
    return bytes;
}

QString source_sequence_for_family(term::Parser_sequence_family family)
{
    switch (family) {
        case term::Parser_sequence_family::OSC:       return QStringLiteral("OSC");
        case term::Parser_sequence_family::DCS:       return QStringLiteral("DCS");
        case term::Parser_sequence_family::APC:       return QStringLiteral("APC");
        case term::Parser_sequence_family::PM:        return QStringLiteral("PM");
        case term::Parser_sequence_family::SOS:       return QStringLiteral("SOS");
        case term::Parser_sequence_family::CSI:       return QStringLiteral("CSI");
        case term::Parser_sequence_family::ESC:       return QStringLiteral("ESC");
        case term::Parser_sequence_family::NONE:
        case term::Parser_sequence_family::PRINTABLE: return QStringLiteral("unknown");
    }

    return QStringLiteral("unknown");
}

Expected_diagnostic expected_diagnostic(
    term::Parser_diagnostic_code       code,
    QString                            source_sequence,
    term::Parser_sequence_family       family,
    std::size_t                        raw_payload_size,
    std::size_t                        limit_bytes,
    term::Parser_recovery_strategy     recovery)
{
    return {code, std::move(source_sequence), family, raw_payload_size, limit_bytes, recovery};
}

void set_expected_diagnostics(
    Test_case&                         test_case,
    std::vector<Expected_diagnostic>   diagnostics)
{
    test_case.expected_diagnostic_count = static_cast<int>(diagnostics.size());
    test_case.expected_diagnostics      = std::move(diagnostics);
}

Test_case make_unterminated_string_test_case(
    const std::string&             name,
    const QByteArray&              introducer,
    std::uint64_t                  chunk_seed)
{
    Test_case test_case;
    test_case.name                           = name;
    test_case.config                         = {term::terminal_grid_size_t{2, 12}, 4, 4};
    test_case.bytes                          = unterminated_string_case(introducer);
    test_case.expected_visible_text_utf8     = QByteArrayLiteral("A\n");
    test_case.has_expected_visible_text_utf8 = true;
    test_case.expected_title_utf8            = QByteArrayLiteral("");
    test_case.has_expected_title_utf8        = true;
    test_case.chunk_seed                     = chunk_seed;
    test_case.expected_diagnostic_count      = 0;
    test_case.expected_reply_count           = 0;
    return test_case;
}

Test_case make_over_limit_string_test_case(
    const std::string&             name,
    const QByteArray&              introducer,
    term::Parser_sequence_family   family,
    std::size_t                    payload_limit,
    std::uint64_t                  chunk_seed)
{
    Test_case test_case;
    test_case.name                           = name;
    test_case.config                         = {term::terminal_grid_size_t{2, 8}, 4, 4};
    test_case.bytes                          = over_limit_string_case(introducer, payload_limit);
    test_case.expected_visible_text_utf8     = QByteArrayLiteral("OK\n");
    test_case.has_expected_visible_text_utf8 = true;
    test_case.chunk_seed                     = chunk_seed;
    set_expected_diagnostics(
        test_case,
        {
            expected_diagnostic(
                term::Parser_diagnostic_code::PAYLOAD_LIMIT_EXCEEDED,
                source_sequence_for_family(family),
                family,
                payload_limit + 1U,
                payload_limit,
                term::Parser_recovery_strategy::DISCARD_STRING),
        });
    test_case.expected_reply_count               = 0;
    test_case.exhaustive_byte_chunks             = false;
    test_case.has_large_payload_limit_boundaries = true;
    test_case.large_payload_introducer_size =
        static_cast<std::size_t>(introducer.size());
    test_case.large_payload_limit_bytes = payload_limit;
    return test_case;
}

Test_case make_terminated_unsupported_string_test_case(
    const std::string&             name,
    const QByteArray&              introducer,
    term::Parser_sequence_family   family,
    std::size_t                    payload_limit,
    std::uint64_t                  chunk_seed)
{
    Test_case test_case;
    test_case.name                           = name;
    test_case.config                         = {term::terminal_grid_size_t{2, 12}, 4, 4};
    test_case.bytes                          = terminated_unsupported_string_case(introducer);
    test_case.expected_visible_text_utf8     = QByteArrayLiteral("R\n");
    test_case.has_expected_visible_text_utf8 = true;
    test_case.chunk_seed                     = chunk_seed;
    set_expected_diagnostics(
        test_case,
        {
            expected_diagnostic(
                term::Parser_diagnostic_code::UNSUPPORTED_SEQUENCE,
                source_sequence_for_family(family),
                family,
                7U,
                payload_limit,
                term::Parser_recovery_strategy::DISCARD_STRING),
        });
    test_case.expected_reply_count = 0;
    return test_case;
}

Test_case make_string_recovery_test_case(
    const std::string&             name,
    const QByteArray&              introducer,
    term::Parser_sequence_family   family,
    std::uint64_t                  chunk_seed)
{
    Test_case test_case;
    test_case.name                           = name;
    test_case.config                         = {term::terminal_grid_size_t{2, 12}, 4, 4};
    test_case.bytes                          = string_recovery_case(introducer);
    test_case.expected_visible_text_utf8     = QByteArrayLiteral("AB\n");
    test_case.has_expected_visible_text_utf8 = true;
    test_case.chunk_seed                     = chunk_seed;
    set_expected_diagnostics(
        test_case,
        {
            expected_diagnostic(
                term::Parser_diagnostic_code::MALFORMED_INPUT,
                source_sequence_for_family(family) + QStringLiteral(" recovery"),
                family,
                0U,
                0U,
                term::Parser_recovery_strategy::RESET_TO_GROUND),
        });
    test_case.expected_reply_count = 0;
    return test_case;
}

std::vector<Test_case> generated_cases()
{
    std::vector<Test_case> cases;

    Test_case primary;
    primary.name       = "generated_control_mix_primary";
    primary.config     = {term::terminal_grid_size_t{4, 16}, 12, 4};
    primary.bytes      = generated_control_mix(0xa0f4b6c2d1897351ULL);
    primary.chunk_seed = 0x54f72d3c18e695b0ULL;
    cases.push_back(std::move(primary));

    Test_case scrollback;
    scrollback.name       = "generated_control_mix_scrollback";
    scrollback.config     = {term::terminal_grid_size_t{3, 10}, 5, 4};
    scrollback.bytes      = generated_control_mix(0x7f31d0e45b8a9c26ULL);
    scrollback.chunk_seed = 0x0bd78f6a3e4c2195ULL;
    cases.push_back(std::move(scrollback));

    Test_case malformed_utf8;
    malformed_utf8.name   = "generated_malformed_utf8";
    malformed_utf8.config = {term::terminal_grid_size_t{2, 12}, 4, 4};
    malformed_utf8.bytes  = malformed_utf8_case();
    malformed_utf8.expected_visible_text_utf8 =
        QByteArray::fromHex("41 EF BF BD 42 EF BF BD 28 43 0A");
    malformed_utf8.has_expected_visible_text_utf8 = true;
    malformed_utf8.chunk_seed                     = 0xb9e2d6410cf8357aULL;
    set_expected_diagnostics(
        malformed_utf8,
        {
            expected_diagnostic(
                term::Parser_diagnostic_code::MALFORMED_INPUT,
                QStringLiteral("invalid UTF-8"),
                term::Parser_sequence_family::PRINTABLE,
                0U,
                0U,
                term::Parser_recovery_strategy::IGNORE_BYTE),
            expected_diagnostic(
                term::Parser_diagnostic_code::MALFORMED_INPUT,
                QStringLiteral("invalid UTF-8"),
                term::Parser_sequence_family::PRINTABLE,
                0U,
                0U,
                term::Parser_recovery_strategy::IGNORE_BYTE),
        });
    malformed_utf8.expected_reply_count = 0;
    cases.push_back(std::move(malformed_utf8));

    Test_case unterminated_utf8;
    unterminated_utf8.name                           = "generated_unterminated_utf8";
    unterminated_utf8.config                         = {term::terminal_grid_size_t{2, 12}, 4, 4};
    unterminated_utf8.bytes                          = unterminated_utf8_case();
    unterminated_utf8.expected_visible_text_utf8     = QByteArrayLiteral("A\n");
    unterminated_utf8.has_expected_visible_text_utf8 = true;
    unterminated_utf8.chunk_seed                     = 0x5a8471e39c206dfbULL;
    unterminated_utf8.expected_diagnostic_count      = 0;
    unterminated_utf8.expected_reply_count           = 0;
    cases.push_back(std::move(unterminated_utf8));

    cases.push_back(make_unterminated_string_test_case(
        "generated_unterminated_osc", QByteArray("\x1b]", 2), 0x698f2b3dc04175eaULL));
    cases.push_back(make_unterminated_string_test_case(
        "generated_unterminated_dcs", QByteArray("\x1bP", 2), 0x8f130642c9d7a5beULL));
    cases.push_back(make_unterminated_string_test_case(
        "generated_unterminated_apc", QByteArray("\x1b_", 2), 0xd72491e6b05c83afULL));
    cases.push_back(make_unterminated_string_test_case(
        "generated_unterminated_pm", QByteArray("\x1b^", 2), 0x31bc508a6e9d247fULL));
    cases.push_back(make_unterminated_string_test_case(
        "generated_unterminated_sos", QByteArray("\x1bX", 2), 0xa8e56239f10c4d7bULL));

    cases.push_back(make_string_recovery_test_case(
        "generated_string_recovery_osc",
        QByteArray("\x1b]", 2),
        term::Parser_sequence_family::OSC,
        0x1264f9b8e35d70caULL));
    cases.push_back(make_string_recovery_test_case(
        "generated_string_recovery_dcs",
        QByteArray("\x1bP", 2),
        term::Parser_sequence_family::DCS,
        0x5c0a4e8d917b326fULL));
    cases.push_back(make_string_recovery_test_case(
        "generated_string_recovery_apc",
        QByteArray("\x1b_", 2),
        term::Parser_sequence_family::APC,
        0xe6b8153709fc2d4aULL));
    cases.push_back(make_string_recovery_test_case(
        "generated_string_recovery_pm",
        QByteArray("\x1b^", 2),
        term::Parser_sequence_family::PM,
        0x4390ce26a7b518fdULL));
    cases.push_back(make_string_recovery_test_case(
        "generated_string_recovery_sos",
        QByteArray("\x1bX", 2),
        term::Parser_sequence_family::SOS,
        0x7df218a630c4be95ULL));

    cases.push_back(make_terminated_unsupported_string_test_case(
        "generated_terminated_unsupported_osc",
        QByteArray("\x1b]", 2),
        term::Parser_sequence_family::OSC,
        term::k_osc_payload_limit_bytes,
        0x730e2df59a8c641bULL));
    cases.push_back(make_terminated_unsupported_string_test_case(
        "generated_terminated_unsupported_dcs",
        QByteArray("\x1bP", 2),
        term::Parser_sequence_family::DCS,
        term::k_dcs_payload_limit_bytes,
        0x83df015b6ac729e4ULL));
    cases.push_back(make_terminated_unsupported_string_test_case(
        "generated_terminated_unsupported_apc",
        QByteArray("\x1b_", 2),
        term::Parser_sequence_family::APC,
        term::k_apc_payload_limit_bytes,
        0x1b7590c4fe3268adULL));
    cases.push_back(make_terminated_unsupported_string_test_case(
        "generated_terminated_unsupported_pm",
        QByteArray("\x1b^", 2),
        term::Parser_sequence_family::PM,
        term::k_pm_payload_limit_bytes,
        0xae61d43c08975b2fULL));
    cases.push_back(make_terminated_unsupported_string_test_case(
        "generated_terminated_unsupported_sos",
        QByteArray("\x1bX", 2),
        term::Parser_sequence_family::SOS,
        term::k_sos_payload_limit_bytes,
        0x59c8a72e3140f6dbULL));

    cases.push_back(make_over_limit_string_test_case(
        "generated_over_limit_osc",
        QByteArray("\x1b]", 2),
        term::Parser_sequence_family::OSC,
        term::k_osc_payload_limit_bytes,
        0x6743af8c9d2e510bULL));
    cases.push_back(make_over_limit_string_test_case(
        "generated_over_limit_dcs",
        QByteArray("\x1bP", 2),
        term::Parser_sequence_family::DCS,
        term::k_dcs_payload_limit_bytes,
        0x2e7186a4c0b95d3fULL));
    cases.push_back(make_over_limit_string_test_case(
        "generated_over_limit_apc",
        QByteArray("\x1b_", 2),
        term::Parser_sequence_family::APC,
        term::k_apc_payload_limit_bytes,
        0xc4705d1b8e9326faULL));
    cases.push_back(make_over_limit_string_test_case(
        "generated_over_limit_pm",
        QByteArray("\x1b^", 2),
        term::Parser_sequence_family::PM,
        term::k_pm_payload_limit_bytes,
        0x9d4c38b72f0615aeULL));
    cases.push_back(make_over_limit_string_test_case(
        "generated_over_limit_sos",
        QByteArray("\x1bX", 2),
        term::Parser_sequence_family::SOS,
        term::k_sos_payload_limit_bytes,
        0x42a6f0d9718c3b5eULL));

    for (Test_case& test_case : cases) {
        const bool actions_depend_on_chunk_boundaries =
            test_case.name == "generated_control_mix_primary"    ||
            test_case.name == "generated_control_mix_scrollback" ||
            test_case.name == "generated_malformed_utf8"         ||
            test_case.name.starts_with("generated_over_limit_");
        test_case.compare_actions = !actions_depend_on_chunk_boundaries;
    }

    return cases;
}

}

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: parser_randomized_tests <seed-dir>\n";
        return 2;
    }

    bool                   ok           = true;
    bool                   seed_load_ok = true;
    std::vector<Test_case> cases        = load_seed_cases(fs::path(argv[1]), seed_load_ok);
    ok &= seed_load_ok;

    std::vector<Test_case> generated = generated_cases();
    cases.insert(
        cases.end(),
        std::make_move_iterator(generated.begin()),
        std::make_move_iterator(generated.end()));

    for (const Test_case& test_case : cases) {
        ok &= run_case(test_case);
    }

    return ok ? 0 : 1;
}
