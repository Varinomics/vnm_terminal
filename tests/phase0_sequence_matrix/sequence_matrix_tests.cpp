#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

using record_t = std::map<std::string, std::string>;

constexpr const char* k_heading_key = "__heading";

std::string trim_copy(const std::string& text)
{
    const char*       whitespace = " \t\r\n";
    const std::size_t begin      = text.find_first_not_of(whitespace);
    if (begin == std::string::npos) {
        return {};
    }

    const std::size_t end = text.find_last_not_of(whitespace);
    return text.substr(begin, end - begin + 1U);
}

std::string lower_copy(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

std::string read_text_file(const fs::path& path, std::vector<std::string>& errors)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        errors.push_back("file is not readable: " + path.string());
        return {};
    }

    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

bool has_public_fields(const record_t& record)
{
    for (const auto& [key, value] : record) {
        if (key != k_heading_key) {
            return true;
        }
    }
    return false;
}

std::vector<record_t> parse_records(const std::string& text)
{
    std::vector<record_t> records;
    record_t current;
    std::istringstream in(text);
    std::string line;

    while (std::getline(in, line)) {
        if (line.rfind("## ", 0) == 0) {
            if (has_public_fields(current)) {
                records.push_back(current);
            }

            current.clear();
            current[k_heading_key] = trim_copy(line.substr(3U));
            continue;
        }

        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }

        if (current.find(k_heading_key) == current.end()) {
            continue;
        }

        const std::string key   = trim_copy(line.substr(0, colon));
        const std::string value = trim_copy(line.substr(colon + 1U));
        if (!key.empty()) {
            current[key] = value;
        }
    }

    if (has_public_fields(current)) {
        records.push_back(current);
    }

    return records;
}

std::vector<std::string> parse_declared_fields(
    const std::string& text,
    const std::string& marker)
{
    std::vector<std::string> fields;
    std::istringstream in(text);
    std::string line;
    bool in_list = false;

    while (std::getline(in, line)) {
        if (line.find(marker) != std::string::npos) {
            in_list = true;
            continue;
        }

        if (!in_list) {
            continue;
        }

        const std::string trimmed = trim_copy(line);
        if (trimmed.rfind("- `", 0) != 0) {
            if (!fields.empty()) {
                break;
            }
            continue;
        }

        const std::size_t closing = trimmed.find('`', 3U);
        if (closing != std::string::npos) {
            fields.push_back(trimmed.substr(3U, closing - 3U));
        }
    }

    return fields;
}

std::string value_of(const record_t& record, const std::string& key)
{
    const auto it = record.find(key);
    return it == record.end() ? std::string() : it->second;
}

bool contains_text(const std::string& text, const std::string& needle)
{
    return lower_copy(text).find(lower_copy(needle)) != std::string::npos;
}

void require(bool condition, std::vector<std::string>& errors, const std::string& message)
{
    if (!condition) {
        errors.push_back(message);
    }
}

std::set<std::string> collect_ids(
    const std::vector<record_t>&   records,
    const std::string&             key,
    std::vector<std::string>&      errors)
{
    std::set<std::string> ids;
    for (const record_t& record : records) {
        const std::string id = value_of(record, key);
        require(!id.empty(),           errors, "record is missing " + key);
        require(ids.insert(id).second, errors, id + " is duplicated");
    }
    return ids;
}

std::map<std::string, record_t> by_key(
    const std::vector<record_t>&   records,
    const std::string&             key)
{
    std::map<std::string, record_t> out;
    for (const record_t& record : records) {
        const std::string id = value_of(record, key);
        if (!id.empty()) {
            out[id] = record;
        }
    }
    return out;
}

const std::vector<std::string>& oracle_fields()
{
    static const std::vector<std::string> fields = {
        "oracle_id",
        "oracle_type",
        "status",
        "license_posture",
        "pin_or_version",
        "checked_in_output_allowed",
        "allowed_use",
        "forbidden_use",
        "inventory_ref",
        "notes",
    };
    return fields;
}

const std::vector<std::string>& matrix_fields()
{
    static const std::vector<std::string> fields = {
        "id",
        "family",
        "sequence",
        "feature",
        "status",
        "action_category",
        "behavior",
        "host_policy",
        "payload_limit",
        "recovery",
        "reply",
        "diagnostic",
        "oracle",
    };
    return fields;
}

std::set<std::string> field_set(const std::vector<std::string>& fields)
{
    return {fields.begin(), fields.end()};
}

const std::set<std::string>& required_sequence_ids()
{
    static const std::set<std::string> ids = {
        "osc-payload-limit",
        "dcs-payload-limit",
        "apc-payload-limit",
        "pm-payload-limit",
        "sos-payload-limit",
        "dcs-unsupported-discard",
        "apc-unsupported-discard",
        "pm-unsupported-discard",
        "sos-unsupported-discard",
        "osc-0-title",
        "osc-1-icon-name",
        "osc-2-title",
        "osc-title-overflow",
        "osc-8-open",
        "osc-8-close",
        "osc-52-write-default-deny",
        "osc-52-write-host-request",
        "osc-52-read-deny",
        "dec-private-1",
        "dec-private-3",
        "dec-private-5",
        "dec-private-6",
        "dec-private-7",
        "dec-private-25",
        "dec-private-47",
        "dec-private-1000",
        "dec-private-1002",
        "dec-private-1003",
        "dec-private-1004",
        "dec-private-1005",
        "dec-private-1006",
        "dec-private-1007",
        "dec-private-1015",
        "dec-private-1047",
        "dec-private-1048",
        "dec-private-1049",
        "dec-private-2004",
        "dec-private-2026",
        "dec-private-2027",
        "c0-bel",
        "esc-index",
        "esc-next-line",
        "esc-reverse-index",
        "csi-scroll-up",
        "csi-scroll-down",
        "csi-decsca",
        "bracketed-paste-generated-input",
        "mouse-sgr-1006-generated-input",
        "focus-generated-input",
        "reply-da1",
        "reply-da2",
        "reply-dsr-cursor-position",
        "reply-decrqm-private-mode",
        "reply-osc-color-query",
        "reply-unsupported-query-no-reply",
    };
    return ids;
}

const std::map<std::string, std::string>& dec_modes()
{
    static const std::map<std::string, std::string> modes = {
        { "dec-private-1",    "?1"    },
        { "dec-private-3",    "?3"    },
        { "dec-private-5",    "?5"    },
        { "dec-private-6",    "?6"    },
        { "dec-private-7",    "?7"    },
        { "dec-private-25",   "?25"   },
        { "dec-private-47",   "?47"   },
        { "dec-private-1000", "?1000" },
        { "dec-private-1002", "?1002" },
        { "dec-private-1003", "?1003" },
        { "dec-private-1004", "?1004" },
        { "dec-private-1005", "?1005" },
        { "dec-private-1006", "?1006" },
        { "dec-private-1007", "?1007" },
        { "dec-private-1015", "?1015" },
        { "dec-private-1047", "?1047" },
        { "dec-private-1048", "?1048" },
        { "dec-private-1049", "?1049" },
        { "dec-private-2004", "?2004" },
        { "dec-private-2026", "?2026" },
        { "dec-private-2027", "?2027" },
    };
    return modes;
}

const std::set<std::string>& required_oracle_ids()
{
    static const std::set<std::string> ids = {
        "product-decision-vnm-terminal",
        "independent-vnm-fixture",
        "xterm-409-reference",
        "vttest-reference",
        "contour-candidate",
        "libvterm-candidate",
        "wezterm-candidate",
        "microsoft-terminal-candidate",
        "iterm2-esctest-reference-only",
        "strong-copyleft-terminal-rejected",
    };
    return ids;
}

void validate_declared_fields(
    const std::string&                 document_name,
    const std::vector<std::string>&    declared,
    const std::vector<std::string>&    expected,
    std::vector<std::string>&          errors)
{
    require(declared == expected, errors, document_name + " declares the wrong field list");
}

void validate_oracle_records(
    const std::string&                 oracle_text,
    const std::vector<record_t>&       records,
    std::vector<std::string>&          errors)
{
    validate_declared_fields(
        "oracle document",
        parse_declared_fields(oracle_text, "Each oracle record uses:"),
        oracle_fields(),
        errors);

    const std::set<std::string> expected_fields = field_set(oracle_fields());
    const std::set<std::string> oracle_ids      = collect_ids(records, "oracle_id", errors);

    for (const std::string& id : required_oracle_ids()) {
        require(oracle_ids.find(id) != oracle_ids.end(), errors, id + " oracle is missing");
    }

    for (const record_t& record : records) {
        const std::string id      = value_of(record, "oracle_id");
        const std::string heading = value_of(record, k_heading_key);
        require(heading == id, errors, id + " heading must match oracle_id");

        for (const std::string& field : oracle_fields()) {
            require(!value_of(record, field).empty(), errors, id + " is missing " + field);
        }

        for (const auto& [key, value] : record) {
            if (key != k_heading_key) {
                require(expected_fields.find(key) != expected_fields.end(), errors,
                    id + " has unexpected oracle field " + key);
                require(!contains_text(value, "TBD"), errors, id + " contains TBD");
            }
        }

        if (contains_text(id, "reference") || contains_text(id, "rejected") || contains_text(id, "candidate"))
        {
            require(value_of(record, "checked_in_output_allowed") == "no", errors,
                id + " must not allow checked-in output");
        }
    }
}

void validate_matrix_records(
    const std::string&             matrix_text,
    const std::vector<record_t>&   records,
    const std::set<std::string>&   oracle_ids,
    std::vector<std::string>&      errors)
{
    validate_declared_fields(
        "sequence matrix",
        parse_declared_fields(matrix_text, "Each sequence record uses these exact fields:"),
        matrix_fields(),
        errors);

    const std::set<std::string> valid_families = {
        "C0", "C1", "ESC", "CSI", "OSC", "DCS", "APC", "PM", "SOS", "generated-input",
    };
    const std::set<std::string> valid_statuses = {
        "supported", "ignored", "rejected", "unsupported-discard",
    };
    const std::set<std::string> valid_actions = {
        "screen-mutation",
        "mode-mutation",
        "input-mode-mutation",
        "notification",
        "terminal-reply",
        "host-policy-request",
        "ignored-with-diagnostic",
        "rejected-with-recovery",
        "unsupported-discard",
        "payload-limit",
    };

    const std::set<std::string>           expected_fields = field_set(matrix_fields());
    const std::set<std::string>           sequence_ids    = collect_ids(records, "id", errors);
    const std::map<std::string, record_t> matrix          = by_key(records, "id");

    for (const std::string& id : required_sequence_ids()) {
        require(sequence_ids.find(id) != sequence_ids.end(), errors, id + " row is missing");
    }

    for (const record_t& record : records) {
        const std::string id      = value_of(record, "id");
        const std::string heading = value_of(record, k_heading_key);
        require(heading == id, errors, id + " heading must match id");
        require(required_sequence_ids().find(id) != required_sequence_ids().end(), errors,
            id + " is not a required matrix row");

        for (const std::string& field : matrix_fields()) {
            require(!value_of(record, field).empty(), errors, id + " is missing " + field);
        }

        for (const auto& [key, value] : record) {
            if (key != k_heading_key) {
                require(expected_fields.find(key) != expected_fields.end(), errors,
                    id + " has unexpected matrix field " + key);
                require(!contains_text(value, "TBD"), errors, id + " contains TBD");
            }
        }

        const std::string family = value_of(record, "family");
        const std::string status = value_of(record, "status");
        const std::string action = value_of(record, "action_category");

        require(valid_families.find(family) != valid_families.end(), errors,
            id + " has invalid family");
        require(valid_statuses.find(status) != valid_statuses.end(), errors,
            id + " has invalid status");
        require(valid_actions.find(action) != valid_actions.end(), errors,
            id + " has invalid action_category");
        require(oracle_ids.find(value_of(record, "oracle")) != oracle_ids.end(), errors,
            id + " cites unknown oracle");

        if (status == "ignored") {
            require(action == "ignored-with-diagnostic", errors,
                id + " ignored rows must use ignored-with-diagnostic");
        }
        if (status == "rejected") {
            require(action == "rejected-with-recovery", errors,
                id + " rejected rows must use rejected-with-recovery");
        }
        if (status == "unsupported-discard") {
            require(action == "unsupported-discard", errors,
                id + " unsupported rows must use unsupported-discard");
        }

        if (status == "ignored" || status == "rejected" || status == "unsupported-discard") {
            require(!value_of(record, "behavior").empty(), errors,
                id + " must document explicit behavior");
            require(!value_of(record, "recovery").empty(), errors,
                id + " must document recovery");
            require(!value_of(record, "diagnostic").empty(), errors,
                id + " must document diagnostic");
            require(!value_of(record, "reply").empty(), errors,
                id + " must document reply behavior");
            require(value_of(record, "diagnostic") != "no diagnostic", errors,
                id + " must name a diagnostic");
        }

        if (action == "terminal-reply") {
            require(contains_text(value_of(record, "reply"), "same backend write path"), errors,
                id + " terminal replies must use the same backend write path");
        }

        if (family == "OSC" || family == "DCS" ||
            family == "APC" || family == "PM"  ||
            family == "SOS")
        {
            require(value_of(record, "payload_limit") != "none", errors,
                id + " string control row must name payload limit");
        }
    }

    for (const auto& [id, mode] : dec_modes()) {
        const auto it = matrix.find(id);
        if (it != matrix.end()) {
            require(contains_text(value_of(it->second, "sequence"), mode), errors,
                id + " must name mode " + mode);
        }
    }

    for (const std::string& id : {
        "osc-payload-limit",
        "dcs-payload-limit",
        "apc-payload-limit",
        "pm-payload-limit",
        "sos-payload-limit",
        "dcs-unsupported-discard",
        "apc-unsupported-discard",
        "pm-unsupported-discard",
        "sos-unsupported-discard",
        "osc-8-open",
        "osc-8-close",
        "osc-52-write-default-deny",
        "osc-52-write-host-request",
        "osc-52-read-deny",
        "reply-osc-color-query",
    })
    {
        const auto it = matrix.find(id);
        if (it != matrix.end()) {
            require(contains_text(value_of(it->second, "payload_limit"), "1048576"),
                errors, id + " must pin 1048576 byte limit");
        }
    }

    for (const std::string& id : {
        "osc-0-title",
        "osc-1-icon-name",
        "osc-2-title",
        "osc-title-overflow",
    })
    {
        const auto it = matrix.find(id);
        if (it != matrix.end()) {
            require(contains_text(value_of(it->second, "payload_limit"), "4096"),
                errors, id + " must pin 4096 scalar title/icon-name limit");
            require(contains_text(value_of(it->second, "recovery"), "preserve") ||
                contains_text(value_of(it->second, "behavior"), "preserves"),
                errors, id + " must preserve previous title/icon-name state on overflow");
        }
    }

    const auto osc0_title = matrix.find("osc-0-title");
    if (osc0_title != matrix.end()) {
        require(contains_text(value_of(osc0_title->second, "behavior"), "icon name") &&
            contains_text(value_of(osc0_title->second, "behavior"), "title"),
            errors, "OSC 0 must document both icon-name and title updates");
        require(contains_text(value_of(osc0_title->second, "host_policy"), "icon name") &&
            contains_text(value_of(osc0_title->second, "host_policy"), "title"),
            errors, "OSC 0 host policy must mention both icon-name and title signals");
    }

    const auto osc1_icon_name = matrix.find("osc-1-icon-name");
    if (osc1_icon_name != matrix.end()) {
        require(contains_text(value_of(osc1_icon_name->second, "sequence"), "OSC 1"),
            errors, "OSC 1 icon-name row must name OSC 1");
        require(contains_text(value_of(osc1_icon_name->second, "behavior"), "icon name") &&
            contains_text(value_of(osc1_icon_name->second, "behavior"), "without changing terminal title"),
            errors, "OSC 1 must document icon-name-only behavior");
        require(contains_text(value_of(osc1_icon_name->second, "host_policy"), "icon name"),
            errors, "OSC 1 host policy must mention icon-name signal");
    }

    const auto osc52_default = matrix.find("osc-52-write-default-deny");
    if (osc52_default != matrix.end()) {
        require(contains_text(value_of(osc52_default->second, "behavior"), "mutates no clipboard"),
            errors, "OSC 52 default denial must not mutate clipboard");
        require(contains_text(value_of(osc52_default->second, "host_policy"), "missing") &&
            contains_text(value_of(osc52_default->second, "host_policy"), "late") &&
            contains_text(value_of(osc52_default->second, "host_policy"), "duplicate"),
            errors, "OSC 52 default denial must deny missing, late, and duplicate responses");
    }

    const auto osc52_host = matrix.find("osc-52-write-host-request");
    if (osc52_host != matrix.end()) {
        require(contains_text(value_of(osc52_host->second, "host_policy"), "explicit host opt-in"),
            errors, "OSC 52 host request must require explicit host opt-in");
    }

    const auto osc52_read = matrix.find("osc-52-read-deny");
    if (osc52_read != matrix.end()) {
        require(value_of(osc52_read->second, "reply") == "no-reply", errors,
            "OSC 52 read denial must send no wire reply");
        require(contains_text(value_of(osc52_read->second, "diagnostic"), "denied"),
            errors, "OSC 52 read denial must diagnose denial");
    }

    const auto decsca = matrix.find("csi-decsca");
    if (decsca != matrix.end()) {
        require(value_of(decsca->second, "status") == "ignored", errors,
            "DECSCA must be ignored");
        require(contains_text(value_of(decsca->second, "behavior"), "no protected-cell"),
            errors, "DECSCA must not create protected-cell state");
    }

    const auto mouse1005 = matrix.find("dec-private-1005");
    if (mouse1005 != matrix.end()) {
        require(value_of(mouse1005->second, "status") == "ignored", errors,
            "DECSET 1005 must be ignored");
        require(contains_text(value_of(mouse1005->second, "behavior"), "does not enable"),
            errors, "DECSET 1005 must not enable reporting");
    }

    const auto mouse1015 = matrix.find("dec-private-1015");
    if (mouse1015 != matrix.end()) {
        require(value_of(mouse1015->second, "status") == "ignored", errors,
            "DECSET 1015 must be ignored");
        require(contains_text(value_of(mouse1015->second, "behavior"), "does not enable"),
            errors, "DECSET 1015 must not enable reporting");
    }

    const auto sync = matrix.find("dec-private-2026");
    if (sync != matrix.end()) {
        require(contains_text(value_of(sync->second, "behavior"), "coalesces snapshot publication only"),
            errors, "DECSET 2026 must only coalesce snapshot publication");
        require(contains_text(value_of(sync->second, "behavior"), "parser and screen still mutate"),
            errors, "DECSET 2026 must not suppress parser/model mutation");
        require(contains_text(value_of(sync->second, "reply"), "DECRQM"), errors,
            "DECSET 2026 must keep terminal replies observable");
    }

    const auto grapheme = matrix.find("dec-private-2027");
    if (grapheme != matrix.end()) {
        require(value_of(grapheme->second, "status") == "rejected", errors,
            "DECSET 2027 must be rejected");
        require(value_of(grapheme->second, "action_category") == "rejected-with-recovery", errors,
            "DECSET 2027 must be rejected with recovery");
        require(contains_text(value_of(grapheme->second, "behavior"), "no runtime grapheme-cluster mode"),
            errors, "DECSET 2027 must not store runtime grapheme mode");
    }
}

std::string replace_once(std::string text, const std::string& before, const std::string& after)
{
    const std::size_t pos = text.find(before);
    if (pos != std::string::npos) {
        text.replace(pos, before.size(), after);
    }
    return text;
}

std::string replace_in_record(
    std::string        text,
    const std::string& record_id,
    const std::string& before,
    const std::string& after)
{
    const std::string heading = "## " + record_id;
    const std::size_t begin   = text.find(heading);
    if (begin == std::string::npos) {
        return text;
    }

    const std::size_t next = text.find("\n## ", begin + heading.size());
    const std::size_t pos  = text.find(before, begin);
    if (pos != std::string::npos && (next == std::string::npos || pos < next)) {
        text.replace(pos, before.size(), after);
    }

    return text;
}

bool expect_failure(
    const std::string& label,
    const std::string& matrix_text,
    const std::string& oracle_text)
{
    std::vector<std::string> errors;
    const std::vector<record_t> oracle_records = parse_records(oracle_text);
    validate_oracle_records(oracle_text, oracle_records, errors);
    validate_matrix_records(
        matrix_text,
        parse_records(matrix_text),
        collect_ids(oracle_records, "oracle_id", errors),
        errors);

    if (!errors.empty()) {
        return true;
    }

    std::cerr << "FAIL: negative case passed unexpectedly: " << label << '\n';
    return false;
}

bool run_negative_cases(const std::string& matrix_text, const std::string& oracle_text)
{
    bool ok = true;
    ok &= expect_failure("missing row",
        replace_in_record(matrix_text, "dec-private-2027",
            "id: dec-private-2027",
            "id: dec-private-2027-missing"),
        oracle_text);
    ok &= expect_failure("duplicate row",
        replace_in_record(matrix_text, "dec-private-2027",
            "id: dec-private-2027",
            "id: dec-private-2026"),
        oracle_text);
    ok &= expect_failure("OSC limit drift",
        replace_in_record(matrix_text, "osc-payload-limit",
            "payload_limit: 1048576 raw bytes",
            "payload_limit: 1024 raw bytes"),
        oracle_text);
    ok &= expect_failure("title limit drift",
        replace_in_record(matrix_text, "osc-title-overflow",
            "payload_limit: 4096 decoded Unicode scalars",
            "payload_limit: 4095 decoded Unicode scalars"),
        oracle_text);
    ok &= expect_failure("DECSET 2027 status drift",
        replace_in_record(matrix_text, "dec-private-2027",
            "status: rejected",
            "status: ignored"),
        oracle_text);
    ok &= expect_failure("OSC 52 read wire reply drift",
        replace_in_record(matrix_text, "osc-52-read-deny",
            "reply: no-reply",
            "reply: OSC 52 reply through same backend write path"),
        oracle_text);
    ok &= expect_failure("unknown oracle",
        replace_in_record(matrix_text, "osc-payload-limit",
            "oracle: product-decision-vnm-terminal",
            "oracle: missing-oracle"),
        oracle_text);
    ok &= expect_failure("reference output allowed",
        matrix_text,
        replace_in_record(oracle_text, "xterm-409-reference",
            "checked_in_output_allowed: no",
            "checked_in_output_allowed: yes"));
    return ok;
}

void print_errors(const std::vector<std::string>& errors)
{
    for (const std::string& error : errors) {
        std::cerr << "FAIL: " << error << '\n';
    }
}

}

int main(int argc, char** argv)
{
    if (argc != 3) {
        std::cerr << "usage: sequence_matrix_tests <matrix.md> <oracles.md>\n";
        return 2;
    }

    std::vector<std::string> errors;
    const std::string           matrix_text    = read_text_file(argv[1], errors);
    const std::string           oracle_text    = read_text_file(argv[2], errors);
    const std::vector<record_t> oracle_records = parse_records(oracle_text);
    validate_oracle_records(oracle_text, oracle_records, errors);
    validate_matrix_records(
        matrix_text,
        parse_records(matrix_text),
        collect_ids(oracle_records, "oracle_id", errors),
        errors);

    if (errors.empty() && !run_negative_cases(matrix_text, oracle_text)) {
        errors.push_back("negative sequence matrix cases failed");
    }

    if (!errors.empty()) {
        print_errors(errors);
        return 1;
    }

    return 0;
}
