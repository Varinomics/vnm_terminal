#pragma once

#include <QByteArray>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace vnm_terminal::tests::conformance {

namespace fs = std::filesystem;

inline std::string trim_copy(const std::string& text)
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

inline std::string strip_inline_comment(const std::string& line)
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

inline std::string unquote_value(std::string value)
{
    value = trim_copy(std::move(value));
    if (value.size() >= 2U && value.front() == '"' && value.back() == '"') {
        return value.substr(1U, value.size() - 2U);
    }

    return value;
}

inline std::string read_text_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

inline QByteArray read_binary_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::string bytes{
        std::istreambuf_iterator<char>(in),
        std::istreambuf_iterator<char>()};
    return QByteArray(bytes.data(), static_cast<int>(bytes.size()));
}

inline std::map<std::string, std::string> parse_key_values(const std::string& text)
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

inline std::map<std::string, std::string> parse_key_values_file(const fs::path& path)
{
    return parse_key_values(read_text_file(path));
}

inline std::vector<std::pair<std::string, std::string>> parse_comment_header_entries(
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

inline bool check_fixture(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }

    return true;
}

inline bool validate_fixture_provenance_header(
    const std::string& text,
    const fs::path&    path)
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

        ok &= check_fixture(!values.empty(),
            "fixture missing provenance header key '" + required.first + "': " +
                path.string());
        ok &= check_fixture(values.size() <= 1U,
            "fixture duplicate provenance header key '" + required.first + "': " +
                path.string());
        for (const std::string& value : values) {
            ok &= check_fixture(value == required.second,
                "fixture provenance header key '" + required.first +
                "' has unsupported value '" + value + "': " + path.string());
        }
    }

    for (const std::string& key : k_required_non_empty_values) {
        std::vector<std::string> values;
        for (const auto& entry : entries) {
            if (entry.first == key) {
                values.push_back(entry.second);
            }
        }

        ok &= check_fixture(!values.empty(),
            "fixture missing provenance header key '" + key + "': " + path.string());
        ok &= check_fixture(values.size() <= 1U,
            "fixture duplicate provenance header key '" + key + "': " + path.string());
        for (const std::string& value : values) {
            ok &= check_fixture(!value.empty(),
                "fixture empty provenance header key '" + key + "': " + path.string());
        }
    }

    return ok;
}

inline bool hex_value(char ch, unsigned char& value)
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

inline bool bytes_from_hex(
    const std::string& hex,
    QByteArray&        out_bytes,
    const std::string& label)
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

inline bool is_authored_byte_stream_fixture(const fs::path& path)
{
    const std::string extension = path.extension().string();
    return extension == ".vnm_capture" || extension == ".vnm_seed";
}

inline bool load_authored_byte_stream_fixture(
    const fs::path&                        path,
    QByteArray&                            out_bytes,
    std::map<std::string, std::string>*    out_values = nullptr)
{
    const std::string text = read_text_file(path);
    std::map<std::string, std::string> values = parse_key_values(text);

    bool       ok        = validate_fixture_provenance_header(text, path);
    const auto stream_it = values.find("byte_stream_hex");
    ok &= check_fixture(stream_it != values.end(),
        "fixture missing byte_stream_hex: " + path.string());
    if (stream_it != values.end()) {
        ok &= bytes_from_hex(
            stream_it->second,
            out_bytes,
            path.string() + "/byte_stream_hex");
    }

    if (out_values != nullptr) {
        *out_values = std::move(values);
    }

    return ok;
}

inline QByteArray read_corpus_bytes(const fs::path& path, bool& ok)
{
    if (!is_authored_byte_stream_fixture(path)) {
        return read_binary_file(path);
    }

    QByteArray bytes;
    ok &= load_authored_byte_stream_fixture(path, bytes);
    return bytes;
}

inline std::string byte_array_to_string(const QByteArray& bytes)
{
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

inline bool hex_string_value(
    const std::map<std::string, std::string>&  values,
    const std::string&                         key,
    std::string&                               out_value,
    const fs::path&                            path,
    bool&                                      ok)
{
    const auto it = values.find(key);
    if (it == values.end()) {
        return false;
    }

    QByteArray bytes;
    ok &= bytes_from_hex(it->second, bytes, path.string() + "/" + key);
    out_value = byte_array_to_string(bytes);
    return true;
}

}
