#include "vnm_terminal/internal/cell_stable_shaping.h"
#include "vnm_terminal/internal/unicode_width.h"
#include "helpers/test_check.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QString>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace term = vnm_terminal::internal;

namespace {

constexpr const char* k_manifest_file_name     = "vnm_terminal_unicode_data_manifest.json";
constexpr const char* k_manifest_artifact_kind = "vnm_terminal_unicode_conformance_data";
constexpr const char* k_unicode_version        = "16.0.0";
constexpr const char* k_emoji_version          = "16.0";

struct Codepoint_range
{
    char32_t               first = 0U;
    char32_t               last  = 0U;
};

struct source_file_t
{
    const char*            relative_path = "";
    const char*            url           = "";
};

struct grapheme_break_case_t
{
    std::vector<char32_t>  codepoints;
    std::vector<bool>      starts_cluster;
    int                    expected_cluster_count = 0;
};

struct conformance_options_t
{
    bool                   include_emoji_zwj     = false;
    bool                   include_full_grapheme = false;
};

const std::vector<source_file_t>& required_source_files()
{
    static const std::vector<source_file_t> source_files = {
        {
            "EastAsianWidth.txt",
            "https://www.unicode.org/Public/16.0.0/ucd/EastAsianWidth.txt",
        },
        {
            "UnicodeData.txt",
            "https://www.unicode.org/Public/16.0.0/ucd/UnicodeData.txt",
        },
        {
            "PropList.txt",
            "https://www.unicode.org/Public/16.0.0/ucd/PropList.txt",
        },
        {
            "auxiliary/GraphemeBreakTest.txt",
            "https://www.unicode.org/Public/16.0.0/ucd/auxiliary/GraphemeBreakTest.txt",
        },
        {
            "emoji/emoji-data.txt",
            "https://www.unicode.org/Public/16.0.0/ucd/emoji/emoji-data.txt",
        },
        {
            "emoji/emoji-variation-sequences.txt",
            "https://www.unicode.org/Public/16.0.0/ucd/emoji/emoji-variation-sequences.txt",
        },
        {
            "emoji/emoji-zwj-sequences.txt",
            "https://www.unicode.org/Public/emoji/16.0/emoji-zwj-sequences.txt",
        },
    };

    return source_files;
}

using vnm_terminal::test_helpers::check;

QString qstring_from_path(const fs::path& path)
{
#ifdef _WIN32
    return QString::fromStdWString(path.wstring());
#else
    return QString::fromStdString(path.string());
#endif
}

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

std::string strip_comment(const std::string& line)
{
    return trim_copy(line.substr(0U, line.find('#')));
}

std::vector<std::string> split(const std::string& text, char separator)
{
    std::vector<std::string> fields;
    std::istringstream in(text);
    std::string field;

    while (std::getline(in, field, separator)) {
        fields.push_back(trim_copy(field));
    }

    return fields;
}

std::vector<std::string> split_whitespace(const std::string& text)
{
    std::vector<std::string> fields;
    std::istringstream in(text);
    std::string field;

    while (in >> field) {
        fields.push_back(field);
    }

    return fields;
}

bool read_text_lines(const fs::path& path, std::vector<std::string>& lines)
{
    std::ifstream in(path);
    if (!in) {
        std::cerr << "FAIL: could not open " << path << '\n';
        return false;
    }

    std::string line;
    while (std::getline(in, line)) {
        lines.push_back(line);
    }

    return true;
}

bool read_file_bytes(const fs::path& path, QByteArray& bytes)
{
    QFile file(qstring_from_path(path));
    if (!file.open(QIODevice::ReadOnly)) {
        std::cerr << "FAIL: could not open " << path << '\n';
        return false;
    }

    bytes = file.readAll();
    return true;
}

fs::path grapheme_break_test_path(const fs::path& data_dir)
{
    const fs::path auxiliary_path = data_dir / "auxiliary" / "GraphemeBreakTest.txt";
    if (fs::is_regular_file(auxiliary_path)) {
        return auxiliary_path;
    }

    return data_dir / "GraphemeBreakTest.txt";
}

fs::path relative_manifest_path(const char* relative_path)
{
    fs::path path;
    for (const std::string& part : split(relative_path, '/')) {
        path /= part;
    }

    return path;
}

char32_t parse_codepoint(const std::string& text)
{
    return static_cast<char32_t>(std::stoul(text, nullptr, 16));
}

Codepoint_range parse_range(const std::string& text)
{
    const std::size_t delimiter = text.find("..");
    if (delimiter == std::string::npos) {
        const char32_t value = parse_codepoint(text);
        return {value, value};
    }

    return {
        parse_codepoint(text.substr(0U, delimiter)),
        parse_codepoint(text.substr(delimiter + 2U)),
    };
}

std::vector<char32_t> sample_range(Codepoint_range range)
{
    std::vector<char32_t> samples = {range.first};
    if (range.last != range.first)      { samples.push_back(range.last);                                    }
    if (range.last >  range.first + 1U) { samples.push_back(range.first + (range.last - range.first) / 2U); }

    std::sort(samples.begin(), samples.end());
    samples.erase(std::unique(samples.begin(), samples.end()), samples.end());
    return samples;
}

std::string codepoint_label(char32_t codepoint)
{
    std::ostringstream out;
    out << "U+" << std::uppercase << std::hex << static_cast<std::uint32_t>(codepoint);
    return out.str();
}

QByteArray utf8_for_codepoint(char32_t codepoint)
{
    QByteArray bytes;

    if (codepoint <= 0x7fU) {
        bytes.append(static_cast<char>(codepoint));
    }
    else
    if (codepoint <= 0x7ffU) {
        bytes.append(static_cast<char>(0xc0U | (codepoint >> 6U)));
        bytes.append(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    }
    else
    if (codepoint <= 0xffffU) {
        bytes.append(static_cast<char>(0xe0U | (codepoint >> 12U)));
        bytes.append(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        bytes.append(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    }
    else {
        bytes.append(static_cast<char>(0xf0U | (codepoint >> 18U)));
        bytes.append(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
        bytes.append(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        bytes.append(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    }

    return bytes;
}

bool is_grapheme_boundary_token(const std::string& token)
{
    return token == "\xc3\xb7";
}

bool is_grapheme_non_boundary_token(const std::string& token)
{
    return token == "\xc3\x97";
}

bool has_terminal_control_codepoint(const std::vector<char32_t>& codepoints)
{
    for (char32_t codepoint : codepoints) {
        if (term::width_for_codepoint(codepoint).width_class ==
            term::Terminal_unicode_width_class::CONTROL)
        {
            return true;
        }
    }

    return false;
}

std::string codepoint_sequence_label(const std::vector<char32_t>& codepoints)
{
    std::string label;
    for (char32_t codepoint : codepoints) {
        if (!label.empty()) {
            label += ' ';
        }
        label += codepoint_label(codepoint);
    }

    return label;
}

bool string_property_equals(
    const QJsonObject& object,
    const char*        property,
    const char*        expected,
    const std::string& label)
{
    const QJsonValue value = object.value(QString::fromLatin1(property));
    return check(value.isString() && value.toString() == QString::fromLatin1(expected),
        label + " must be " + expected);
}

std::string manifest_key(const QJsonObject& object)
{
    const QJsonValue value = object.value(QStringLiteral("relative_path"));
    return value.isString() ? value.toString().toStdString() : std::string();
}

bool validate_manifest_file_entry(
    const fs::path&        data_dir,
    const source_file_t&   source_file,
    const QJsonObject&     file_object)
{
    bool ok = true;

    ok &= string_property_equals(
        file_object,
        "relative_path",
        source_file.relative_path,
        std::string("manifest path for ") + source_file.relative_path);
    ok &= string_property_equals(
        file_object,
        "url",
        source_file.url,
        std::string("manifest URL for ") + source_file.relative_path);

    const QJsonValue sha256_value = file_object.value(QStringLiteral("sha256"));
    ok &= check(sha256_value.isString(),
        std::string("manifest sha256 for ") + source_file.relative_path);
    if (!ok) {
        return false;
    }

    QByteArray bytes;
    const fs::path path = data_dir / relative_manifest_path(source_file.relative_path);
    if (!read_file_bytes(path, bytes)) {
        return false;
    }

    const QString expected_hash = QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    ok &= check(sha256_value.toString() == expected_hash,
        std::string("manifest sha256 matches ") + source_file.relative_path);
    return ok;
}

bool validate_unicode_data_manifest(const fs::path& data_dir)
{
    const fs::path manifest_path = data_dir / k_manifest_file_name;
    QByteArray manifest_bytes;
    if (!read_file_bytes(manifest_path, manifest_bytes)) {
        return false;
    }

    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(manifest_bytes, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        std::cerr << "FAIL: invalid Unicode data manifest "
            << manifest_path << ": "
            << parse_error.errorString().toStdString() << '\n';
        return false;
    }

    const QJsonObject manifest = document.object();
    bool ok = true;

    ok &= string_property_equals(
        manifest, "artifact_kind", k_manifest_artifact_kind, "manifest artifact kind");
    ok &= string_property_equals(
        manifest, "unicode_version", k_unicode_version, "manifest Unicode version");
    ok &= string_property_equals(
        manifest, "emoji_version", k_emoji_version, "manifest emoji version");

    const QJsonValue files_value = manifest.value(QStringLiteral("files"));
    ok &= check(files_value.isArray(), "manifest files array");
    if (!ok) {
        return false;
    }

    const QJsonArray files = files_value.toArray();
    ok &= check(files.size() == static_cast<int>(required_source_files().size()),
        "manifest records the expected Unicode data files");

    std::map<std::string, QJsonObject> file_objects;
    for (const QJsonValue& file_value : files) {
        if (!file_value.isObject()) {
            ok &= check(false, "manifest file entry is an object");
            continue;
        }

        const QJsonObject file_object = file_value.toObject();
        const std::string key         = manifest_key(file_object);
        if (key.empty()) {
            ok &= check(false, "manifest file entry has a relative_path");
            continue;
        }

        file_objects[key] = file_object;
    }

    for (const source_file_t& source_file : required_source_files()) {
        const auto it = file_objects.find(source_file.relative_path);
        if (it == file_objects.end()) {
            ok &= check(false,
                std::string("manifest includes ") + source_file.relative_path);
            continue;
        }

        ok &= validate_manifest_file_entry(data_dir, source_file, it->second);
    }

    return ok;
}

bool parse_grapheme_break_test_line(const std::string& line, grapheme_break_case_t& out_case)
{
    out_case = {};

    bool next_codepoint_starts_cluster = false;
    for (const std::string& token : split_whitespace(strip_comment(line))) {
        if (is_grapheme_boundary_token(token)) {
            next_codepoint_starts_cluster = true;
            continue;
        }

        if (is_grapheme_non_boundary_token(token)) {
            next_codepoint_starts_cluster = false;
            continue;
        }

        out_case.codepoints.push_back(parse_codepoint(token));
        out_case.starts_cluster.push_back(next_codepoint_starts_cluster);
        if (next_codepoint_starts_cluster) {
            ++out_case.expected_cluster_count;
        }
        next_codepoint_starts_cluster = false;
    }

    return !out_case.codepoints.empty();
}

QString string_from_codepoints(const std::vector<char32_t>& codepoints)
{
    QByteArray bytes;
    for (char32_t codepoint : codepoints) {
        bytes += utf8_for_codepoint(codepoint);
    }

    return QString::fromUtf8(bytes);
}

bool require_file(const fs::path& path)
{
    return check(fs::is_regular_file(path), "required Unicode data file is missing: " +
        path.string());
}

bool is_zwj(char32_t codepoint)
{
    return codepoint == 0x200dU;
}

bool is_terminal_adopted_cluster_continuation(char32_t codepoint)
{
    return !is_zwj(codepoint) &&
        (term::is_terminal_combining_codepoint(codepoint) ||
            term::is_terminal_variation_selector(codepoint));
}

bool grapheme_case_matches_terminal_adopted_policy(const grapheme_break_case_t& test_case)
{
    bool cluster_active = false;
    for (std::size_t i = 0U; i < test_case.codepoints.size(); ++i) {
        const bool starts_cluster = !cluster_active ||
            !is_terminal_adopted_cluster_continuation(test_case.codepoints[i]);
        if (starts_cluster != test_case.starts_cluster[i]) {
            return false;
        }

        cluster_active = true;
    }

    return true;
}

bool test_east_asian_width(const fs::path& data_dir)
{
    std::vector<std::string> lines;
    if (!read_text_lines(data_dir / "EastAsianWidth.txt", lines)) {
        return false;
    }

    bool ok               = true;
    int  wide_ranges      = 0;
    int  ambiguous_ranges = 0;

    for (const std::string& line : lines) {
        const std::string body = strip_comment(line);
        if (body.empty()) {
            continue;
        }

        const std::vector<std::string> fields = split(body, ';');
        if (fields.size() < 2U) {
            continue;
        }

        const Codepoint_range range    = parse_range(fields[0]);
        const std::string&    property = fields[1];
        if (property != "W" && property != "F" && property != "A") {
            continue;
        }

        for (char32_t codepoint : sample_range(range)) {
            const term::Terminal_codepoint_width width =
                term::width_for_codepoint(codepoint);
            if (width.width_class == term::Terminal_unicode_width_class::ZERO ||
                width.width_class == term::Terminal_unicode_width_class::CONTROL)
            {
                continue;
            }

            if (property == "W" || property == "F") {
                ++wide_ranges;
                ok &= check(width.cells == 2,
                    "EastAsianWidth " + property + " is wide: " +
                        codepoint_label(codepoint));
            }
            else {
                ++ambiguous_ranges;
                ok &= check(width.cells == 1 &&
                    width.width_class ==
                        term::Terminal_unicode_width_class::AMBIGUOUS_NARROW,
                    "EastAsianWidth A is ambiguous-narrow: " +
                    codepoint_label(codepoint));
            }
        }
    }

    ok &= check(wide_ranges > 0, "EastAsianWidth conformance saw wide ranges");
    ok &= check(ambiguous_ranges > 0,
        "EastAsianWidth conformance saw ambiguous ranges");
    return ok;
}

bool test_unicode_data_zero_width(const fs::path& data_dir)
{
    std::vector<std::string> lines;
    if (!read_text_lines(data_dir / "UnicodeData.txt", lines)) {
        return false;
    }

    bool ok      = true;
    int  sampled = 0;

    for (const std::string& line : lines) {
        const std::vector<std::string> fields = split(line, ';');
        if (fields.size() < 3U) {
            continue;
        }

        const std::string& category = fields[2];
        if (category.empty() || (category[0] != 'M' && category != "Cf")) {
            continue;
        }

        const char32_t codepoint = parse_codepoint(fields[0]);
        const term::Terminal_codepoint_width width = term::width_for_codepoint(codepoint);
        ++sampled;
        ok &= check(width.cells == 0,
            "UnicodeData mark/format is zero-width: " + codepoint_label(codepoint));
    }

    ok &= check(sampled > 0, "UnicodeData conformance saw mark/format entries");
    return ok;
}

bool test_property_list_variation_selectors(const fs::path& data_dir)
{
    std::vector<std::string> lines;
    if (!read_text_lines(data_dir / "PropList.txt", lines)) {
        return false;
    }

    bool ok      = true;
    int  sampled = 0;

    for (const std::string& line : lines) {
        const std::string body = strip_comment(line);
        if (body.empty()) {
            continue;
        }

        const std::vector<std::string> fields = split(body, ';');
        if (fields.size() < 2U || fields[1] != "Variation_Selector") {
            continue;
        }

        for (char32_t codepoint : sample_range(parse_range(fields[0]))) {
            const term::Terminal_codepoint_width width =
                term::width_for_codepoint(codepoint);
            ++sampled;
            ok &= check(width.cells == 0,
                "PropList variation selector is zero-width: " +
                    codepoint_label(codepoint));
        }
    }

    ok &= check(sampled > 0, "PropList conformance saw variation selectors");
    return ok;
}

bool test_emoji_presentation(const fs::path& data_dir)
{
    std::vector<std::string> lines;
    if (!read_text_lines(data_dir / "emoji" / "emoji-data.txt", lines)) {
        return false;
    }

    bool ok      = true;
    int  sampled = 0;

    for (const std::string& line : lines) {
        const std::string body = strip_comment(line);
        if (body.empty()) {
            continue;
        }

        const std::vector<std::string> fields = split(body, ';');
        if (fields.size() < 2U || fields[1] != "Emoji_Presentation") {
            continue;
        }

        for (char32_t codepoint : sample_range(parse_range(fields[0]))) {
            const term::Terminal_codepoint_width width =
                term::width_for_codepoint(codepoint);
            ++sampled;
            ok &= check(width.cells == 2,
                "Emoji_Presentation scalar is two cells: " +
                    codepoint_label(codepoint));
        }
    }

    ok &= check(sampled > 0, "emoji-data conformance saw Emoji_Presentation ranges");
    return ok;
}

bool test_emoji_variation_sequences(const fs::path& data_dir)
{
    std::vector<std::string> lines;
    if (!read_text_lines(data_dir / "emoji" / "emoji-variation-sequences.txt", lines)) {
        return false;
    }

    bool ok              = true;
    int  text_sequences  = 0;
    int  emoji_sequences = 0;

    for (const std::string& line : lines) {
        const std::string body = strip_comment(line);
        if (body.empty()) {
            continue;
        }

        const std::vector<std::string> fields = split(body, ';');
        if (fields.empty()) {
            continue;
        }

        const std::vector<std::string> sequence = split(fields[0], ' ');
        if (sequence.size() < 2U) {
            continue;
        }

        const char32_t base     = parse_codepoint(sequence[0]);
        const char32_t selector = parse_codepoint(sequence[1]);
        if (selector != 0xfe0eU && selector != 0xfe0fU) {
            continue;
        }
        if (base < 0x80U) {
            continue;
        }

        const QByteArray bytes = utf8_for_codepoint(base) + utf8_for_codepoint(selector);
        const term::Terminal_utf8_width_result width = term::measure_utf8_width(bytes);
        if (selector == 0xfe0eU) {
            ++text_sequences;
            ok &= check(width.cells == 1,
                "emoji variation text sequence is one cell: " +
                    codepoint_label(base));
        }
        else {
            ++emoji_sequences;
            ok &= check(width.cells == 2,
                "emoji variation emoji sequence is two cells: " +
                    codepoint_label(base));
        }
    }

    ok &= check(text_sequences > 0,
        "emoji variation conformance saw text variation sequences");
    ok &= check(emoji_sequences > 0,
        "emoji variation conformance saw emoji variation sequences");
    return ok;
}

bool test_emoji_zwj_sequences_if_present(const fs::path& data_dir)
{
    const fs::path path = data_dir / "emoji" / "emoji-zwj-sequences.txt";
    if (!fs::is_regular_file(path)) {
        std::cout << "SKIP: optional emoji-zwj-sequences.txt not present\n";
        return true;
    }

    std::vector<std::string> lines;
    if (!read_text_lines(path, lines)) {
        return false;
    }

    bool ok                  = true;
    int  sampled             = 0;
    int  mismatches          = 0;
    int  reported_mismatches = 0;

    for (const std::string& line : lines) {
        const std::string body = strip_comment(line);
        if (body.empty()) {
            continue;
        }

        const std::vector<std::string> fields = split(body, ';');
        if (fields.empty()) {
            continue;
        }

        QByteArray bytes;
        for (const std::string& item : split(fields[0], ' ')) {
            if (!item.empty()) {
                bytes += utf8_for_codepoint(parse_codepoint(item));
            }
        }

        if (bytes.isEmpty()) {
            continue;
        }

        const term::Terminal_utf8_width_result width = term::measure_utf8_width(bytes);
        ++sampled;
        if (width.cells == 2) {
            continue;
        }

        ++mismatches;
        if (reported_mismatches < 32) {
            ++reported_mismatches;
            std::cerr << "FAIL: emoji ZWJ sequence should occupy two cells: "
                << fields[0] << ", got " << width.cells << '\n';
        }
        if (sampled >= 128) {
            break;
        }
    }

    ok &= check(sampled > 0, "emoji ZWJ conformance saw sequences");
    ok &= check(mismatches == 0,
        "emoji ZWJ width mismatches: " + std::to_string(mismatches));
    return ok;
}

bool test_grapheme_break_clusters(
    const fs::path&    data_dir,
    bool               include_full_grapheme)
{
    std::vector<std::string> lines;
    const fs::path path = grapheme_break_test_path(data_dir);
    if (!read_text_lines(path, lines)) {
        return false;
    }

    bool ok                   = true;
    int  checked              = 0;
    int  skipped_controls     = 0;
    int  skipped_aspirational = 0;
    int  mismatches           = 0;
    int  reported_mismatches  = 0;

    for (const std::string& line : lines) {
        grapheme_break_case_t test_case;
        if (!parse_grapheme_break_test_line(line, test_case)) {
            continue;
        }

        if (has_terminal_control_codepoint(test_case.codepoints)) {
            ++skipped_controls;
            continue;
        }

        if (!include_full_grapheme &&
            !grapheme_case_matches_terminal_adopted_policy(test_case))
        {
            ++skipped_aspirational;
            continue;
        }

        const QString text                 = string_from_codepoints(test_case.codepoints);
        const int     actual_cluster_count = term::terminal_text_cluster_count(text);
        ++checked;

        if (actual_cluster_count == test_case.expected_cluster_count) {
            continue;
        }

        ++mismatches;
        if (reported_mismatches < 32) {
            ++reported_mismatches;
            std::cerr << "FAIL: GraphemeBreakTest cluster count mismatch for "
                << codepoint_sequence_label(test_case.codepoints) << ": expected "
                << test_case.expected_cluster_count << ", got "
                << actual_cluster_count << '\n';
        }
    }

    ok &= check(checked > 0, "GraphemeBreakTest conformance saw printable cases");
    ok &= check(skipped_controls > 0,
        "GraphemeBreakTest conformance skipped terminal control cases");
    if (!include_full_grapheme) {
        ok &= check(skipped_aspirational > 0,
            "GraphemeBreakTest conformance identified aspirational cases");
        std::cout << "SKIP: GraphemeBreakTest aspirational cases: "
            << skipped_aspirational << '\n';
    }
    ok &= check(mismatches == 0,
        "GraphemeBreakTest terminal text cluster mismatches: " +
            std::to_string(mismatches));
    return ok;
}

bool run_conformance(const fs::path& data_dir, conformance_options_t options)
{
    bool ok = true;

    ok &= validate_unicode_data_manifest(data_dir);
    for (const source_file_t& source_file : required_source_files()) {
        ok &= require_file(data_dir / relative_manifest_path(source_file.relative_path));
    }
    if (!ok) {
        return false;
    }

    ok &= test_east_asian_width(data_dir);
    ok &= test_unicode_data_zero_width(data_dir);
    ok &= test_property_list_variation_selectors(data_dir);
    ok &= test_emoji_presentation(data_dir);
    ok &= test_emoji_variation_sequences(data_dir);
    if (options.include_emoji_zwj) {
        ok &= test_emoji_zwj_sequences_if_present(data_dir);
    }
    else {
        std::cout << "SKIP: emoji ZWJ sequence shaping is aspirational\n";
    }
    ok &= test_grapheme_break_clusters(data_dir, options.include_full_grapheme);
    return ok;
}

}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "usage: unicode_data_conformance_tests <unicode-data-dir> "
                     "[--include-emoji-zwj] [--include-full-grapheme] "
                     "[--include-aspirational]\n";
        return 1;
    }

    conformance_options_t options;
    for (int i = 2; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--include-emoji-zwj") {
            options.include_emoji_zwj = true;
            continue;
        }

        if (argument == "--include-full-grapheme") {
            options.include_full_grapheme = true;
            continue;
        }

        if (argument == "--include-aspirational") {
            options.include_emoji_zwj     = true;
            options.include_full_grapheme = true;
            continue;
        }

        std::cerr << "FAIL: unknown option: " << argument << '\n';
        return 1;
    }

    return run_conformance(fs::path(argv[1]), options) ? 0 : 1;
}
