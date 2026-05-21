#include "vnm_terminal/internal/parser_action.h"
#include "vnm_terminal/internal/terminal_screen_model.h"
#include "conformance_fixture_io.h"
#include "helpers/test_check.h"

#include <QByteArray>
#include <QString>
#include <cstdlib>
#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace term = vnm_terminal::internal;
namespace fixture = vnm_terminal::tests::conformance;

namespace {

struct Replay_expectations
{
    int                    max_diagnostics           = 0;
    int                    min_scrollback_rows       = -1;
    bool                   check_dirty_text_coverage = false;
    bool                   check_dirty_cell_coverage = false;
    std::string            visible_contains;
    std::string            visible_excludes;
    std::string            title_contains;
};

struct Dirty_coverage_config
{
    bool                   enabled       = false;
    bool                   compare_cells = false;
    int                    byte_start    = 0;
    int                    byte_limit    = 0;
};

struct Replay_result
{
    QString                visible_text;
    QString                title;
    QString                icon_name;
    term::terminal_grid_position_t
                           cursor;
    int                    scrollback_size           = 0;
    int                    diagnostic_count          = 0;
    int                    dirty_coverage_violations = 0;
    term::Terminal_render_snapshot
                           snapshot;
};

struct Capture_input
{
    QByteArray             bytes;
    Replay_expectations    expectations;
};

using vnm_terminal::test_helpers::check;

int int_value(
    const std::map<std::string, std::string>&  values,
    const char*                                key,
    int                                        fallback,
    const fs::path&                            path,
    bool&                                      ok)
{
    const auto it = values.find(key);
    if (it == values.end()) {
        return fallback;
    }

    try {
        std::size_t consumed = 0U;
        const int   parsed   = std::stoi(it->second, &consumed, 10);
        if (consumed != it->second.size()) {
            ok &= check(false, path.string() + ": integer field is malformed: " + key);
        }

        return parsed;
    }
    catch (const std::exception&) {
        ok &= check(false, path.string() + ": integer field is malformed: " + key);
        return fallback;
    }
}

bool bool_value(
    const std::map<std::string, std::string>&  values,
    const char*                                key,
    bool                                       fallback,
    const fs::path&                            path,
    bool&                                      ok)
{
    const auto it = values.find(key);
    if (it == values.end()) {
        return fallback;
    }

    const std::string& value = it->second;
    if (value == "1" || value == "true"  || value == "on")  { return true;  }
    if (value == "0" || value == "false" || value == "off") { return false; }

    ok &= check(false, path.string() + ": boolean field is malformed: " + key);
    return fallback;
}

Replay_expectations expectations_from_values(
    const fs::path&                            path,
    const std::map<std::string, std::string>&  values,
    bool&                                      ok)
{
    Replay_expectations expectations;

    auto read_string = [&](const char* key, std::string& out_value) {
        const auto it = values.find(key);
        if (it != values.end()) {
            out_value = it->second;
        }
    };

    expectations.max_diagnostics =
        int_value(values, "max_diagnostics", expectations.max_diagnostics, path, ok);
    expectations.min_scrollback_rows = int_value(
        values,
        "min_scrollback_rows",
        expectations.min_scrollback_rows,
        path,
        ok);
    expectations.check_dirty_text_coverage =
        bool_value(values, "check_dirty_text_coverage", false, path, ok);
    expectations.check_dirty_cell_coverage =
        bool_value(values, "check_dirty_cell_coverage", false, path, ok);
    read_string("visible_contains", expectations.visible_contains);
    read_string("visible_excludes", expectations.visible_excludes);
    read_string("title_contains",   expectations.title_contains);
    fixture::hex_string_value(
        values,
        "visible_contains_utf8_hex",
        expectations.visible_contains,
        path,
        ok);
    fixture::hex_string_value(
        values,
        "visible_excludes_utf8_hex",
        expectations.visible_excludes,
        path,
        ok);
    fixture::hex_string_value(
        values,
        "title_contains_utf8_hex",
        expectations.title_contains,
        path,
        ok);
    return expectations;
}

Replay_expectations load_sidecar_expectations(const fs::path& capture_path, bool& ok)
{
    const fs::path expectation_path = capture_path.string() + ".expect";
    if (!fs::is_regular_file(expectation_path)) {
        return {};
    }

    return expectations_from_values(
        expectation_path,
        fixture::parse_key_values_file(expectation_path),
        ok);
}

bool load_capture_input(const fs::path& path, Capture_input& out_input)
{
    bool ok = true;

    if (fixture::is_authored_byte_stream_fixture(path)) {
        std::map<std::string, std::string> values;
        ok &= fixture::load_authored_byte_stream_fixture(path, out_input.bytes, &values);
        out_input.expectations = expectations_from_values(path, values, ok);
        return ok;
    }

    out_input.bytes        = fixture::read_binary_file(path);
    out_input.expectations = load_sidecar_expectations(path, ok);
    return ok;
}

int diagnostic_count(const term::Terminal_screen_model_result& result)
{
    int count = 0;
    for (const term::Parser_action& action : result.actions) {
        if (term::parser_action_kind(action) == term::Parser_action_kind::DIAGNOSTIC) {
            ++count;
        }
    }
    return count;
}

term::Terminal_screen_model make_model()
{
    return term::Terminal_screen_model({
        term::terminal_grid_size_t{24, 80},
        10000,
        8,
        true,
    });
}

std::vector<int> deterministic_chunks(int byte_count)
{
    std::vector<int> chunks;
    int offset = 0;
    int step   = 1;

    while (offset < byte_count) {
        const int remaining = byte_count - offset;
        const int count     = std::min(remaining, step);
        chunks.push_back(count);
        offset += count;
        step = step == 1 ? 7 : step == 7 ? 64 : step == 64 ? 251 : 4096;
    }

    return chunks;
}

bool dirty_ranges_contain_row(
    const std::vector<term::Terminal_render_dirty_row_range>&
                           ranges,
    int                    row)
{
    for (const term::Terminal_render_dirty_row_range& range : ranges) {
        if (row >= range.first_row && row < range.first_row + range.row_count) {
            return true;
        }
    }

    return false;
}

bool dirty_rows_contain_row(const std::vector<int>& rows, int row)
{
    return std::find(rows.begin(), rows.end(), row) != rows.end();
}

term::Terminal_text_style style_for_cell(
    const term::Terminal_render_snapshot&  snapshot,
    term::Terminal_style_id                style_id)
{
    if (static_cast<std::size_t>(style_id) < snapshot.styles.size()) {
        return snapshot.styles[static_cast<std::size_t>(style_id)];
    }

    return term::make_default_terminal_text_style();
}

struct Cell_visual_attributes
{
    quint32        foreground_rgba  = term::k_terminal_default_foreground_rgba;
    quint32        background_rgba  = term::k_terminal_default_background_rgba;
    std::uint16_t  style_attributes = 0U;
};

Cell_visual_attributes visual_attributes_for_style(
    const term::Terminal_render_snapshot&  snapshot,
    term::Terminal_style_id                style_id)
{
    const term::Terminal_text_style style = style_for_cell(snapshot, style_id);

    Cell_visual_attributes attributes;
    attributes.foreground_rgba  = term::resolve_terminal_color_ref(
        style.foreground,
        snapshot.color_state,
        true);
    attributes.background_rgba  = term::resolve_terminal_color_ref(
        style.background,
        snapshot.color_state,
        false);
    attributes.style_attributes = style.attributes;

    const bool inverse =
        term::terminal_style_has_attribute(style, term::Terminal_style_attribute::INVERSE) !=
        snapshot.modes.reverse_video;
    if (inverse) {
        std::swap(attributes.foreground_rgba, attributes.background_rgba);
    }
    if (term::terminal_style_has_attribute(style, term::Terminal_style_attribute::INVISIBLE)) {
        attributes.foreground_rgba = attributes.background_rgba;
    }
    return attributes;
}

quint32 default_background_rgba(const term::Terminal_render_snapshot& snapshot)
{
    const Cell_visual_attributes attributes =
        visual_attributes_for_style(snapshot, term::k_default_terminal_style_id);
    return attributes.background_rgba;
}

std::vector<QString> row_cells_keys(const term::Terminal_render_snapshot& snapshot)
{
    std::vector<QString> keys(static_cast<std::size_t>(snapshot.grid_size.rows));
    const QString row_background_key = QStringLiteral("default-bg:%1|").arg(
        static_cast<qulonglong>(default_background_rgba(snapshot)),
        0,
        16);
    for (QString& key : keys) {
        key = row_background_key;
    }

    for (const term::Terminal_render_cell& cell : snapshot.cells) {
        if (cell.position.row < 0 || cell.position.row >= snapshot.grid_size.rows) {
            continue;
        }

        const Cell_visual_attributes attributes =
            visual_attributes_for_style(snapshot, cell.style_id);
        keys[static_cast<std::size_t>(cell.position.row)] +=
            QStringLiteral("%1:%2:%3:%4:%5:%6:%7:%8:%9|")
            .arg(cell.position.column)
            .arg(cell.display_width)
            .arg(cell.wide_continuation ? 1 : 0)
            .arg(cell.style_id)
            .arg(cell.hyperlink_id)
            .arg(static_cast<qulonglong>(attributes.foreground_rgba), 0, 16)
            .arg(static_cast<qulonglong>(attributes.background_rgba), 0, 16)
            .arg(static_cast<qulonglong>(attributes.style_attributes), 0, 16)
            .arg(cell.text);
    }
    return keys;
}

bool dirty_coverage_check_enabled()
{
    const char* const value = std::getenv("VNM_TERMINAL_CHECK_DIRTY_COVERAGE");
    return value != nullptr && std::string(value) == "1";
}

bool dirty_cell_coverage_check_enabled()
{
    const char* const value = std::getenv("VNM_TERMINAL_CHECK_DIRTY_CELL_COVERAGE");
    return value != nullptr && std::string(value) == "1";
}

int dirty_coverage_byte_limit()
{
    const char* const value = std::getenv("VNM_TERMINAL_CHECK_DIRTY_COVERAGE_LIMIT");
    if (value == nullptr) {
        return 0;
    }

    try {
        return std::max(0, std::stoi(value));
    }
    catch (const std::exception&) {
        return 0;
    }
}

int dirty_coverage_byte_start()
{
    const char* const value = std::getenv("VNM_TERMINAL_CHECK_DIRTY_COVERAGE_START");
    if (value == nullptr) {
        return 0;
    }

    try {
        return std::max(0, std::stoi(value));
    }
    catch (const std::exception&) {
        return 0;
    }
}

Dirty_coverage_config dirty_coverage_config(const Replay_expectations& expectations)
{
    const bool env_enabled       = dirty_coverage_check_enabled();
    const bool env_cell_coverage = dirty_cell_coverage_check_enabled();

    Dirty_coverage_config config;
    config.enabled =
        env_enabled                            ||
        expectations.check_dirty_text_coverage ||
        expectations.check_dirty_cell_coverage;
    config.compare_cells =
        env_cell_coverage ||
        expectations.check_dirty_cell_coverage;
    config.byte_start = dirty_coverage_byte_start();
    config.byte_limit = dirty_coverage_byte_limit();
    return config;
}

int count_dirty_coverage_violations(
    const term::Terminal_render_snapshot&  before,
    const term::Terminal_render_snapshot&  after,
    int                                    byte_offset,
    int                                    byte_count)
{
    if (before.grid_size.rows            != after.grid_size.rows           ||
        before.grid_size.columns         != after.grid_size.columns        ||
        before.viewport.active_buffer    != after.viewport.active_buffer   ||
        before.viewport.scrollback_rows  != after.viewport.scrollback_rows ||
        before.viewport.offset_from_tail != after.viewport.offset_from_tail)
    {
        return 0;
    }

    const std::vector<QString> before_keys = row_cells_keys(before);
    const std::vector<QString> after_keys  = row_cells_keys(after);

    int violations = 0;
    for (int row = 0; row < after.grid_size.rows; ++row) {
        if (before_keys[static_cast<std::size_t>(row)] ==
            after_keys[static_cast<std::size_t>(row)])
        {
            continue;
        }

        if (dirty_ranges_contain_row(after.dirty_row_ranges, row)) {
            continue;
        }

        ++violations;
        if (violations <= 8) {
            std::cerr << "dirty coverage violation at byte offset "
                << byte_offset << " count " << byte_count
                << " row " << row << '\n';
        }
    }
    return violations;
}

std::vector<QString> model_row_text_keys(const term::Terminal_screen_model& model)
{
    const term::terminal_grid_size_t grid_size = model.grid_size();
    std::vector<QString> keys(static_cast<std::size_t>(grid_size.rows));
    for (int row = 0; row < grid_size.rows; ++row) {
        keys[static_cast<std::size_t>(row)] = model.row_text(row);
    }
    return keys;
}

int count_dirty_text_coverage_violations(
    const std::vector<QString>&                before_rows,
    const term::terminal_grid_size_t           before_grid_size,
    const term::Terminal_buffer_id             before_buffer,
    const std::vector<QString>&                after_rows,
    const term::terminal_grid_size_t           after_grid_size,
    const term::Terminal_buffer_id             after_buffer,
    const term::Terminal_screen_model_result&  result,
    int                                        byte_offset,
    int                                        byte_count)
{
    if (before_grid_size.rows    != after_grid_size.rows    ||
        before_grid_size.columns != after_grid_size.columns ||
        before_buffer            != after_buffer            ||
        result.viewport_changed)
    {
        return 0;
    }

    int violations = 0;
    for (int row = 0; row < after_grid_size.rows; ++row) {
        if (before_rows[static_cast<std::size_t>(row)] ==
            after_rows[static_cast<std::size_t>(row)])
        {
            continue;
        }

        if (dirty_rows_contain_row(result.dirty_rows, row)) {
            continue;
        }

        ++violations;
        if (violations <= 8) {
            std::cerr << "dirty text coverage violation at byte offset "
                << byte_offset << " count " << byte_count
                << " row " << row << '\n';
        }
    }
    return violations;
}

Replay_result replay_bytes(
    const QByteArray&              bytes,
    bool                           chunked,
    const Dirty_coverage_config&   dirty_coverage)
{
    term::Terminal_screen_model model = make_model();
    Replay_result out;
    std::uint64_t snapshot_sequence = 1U;

    if (chunked) {
        int offset = 0;
        for (int count : deterministic_chunks(bytes.size())) {
            const bool check_this_chunk =
                dirty_coverage.enabled                                                &&
                offset >= dirty_coverage.byte_start                                   &&
                (dirty_coverage.byte_limit == 0 || offset < dirty_coverage.byte_limit);
            const term::terminal_grid_size_t before_grid_size = model.grid_size();
            const term::Terminal_buffer_id   before_buffer    = model.active_buffer_id();
            const std::vector<QString>       before_rows      =
                (check_this_chunk && !dirty_coverage.compare_cells)
                    ? model_row_text_keys(model)
                    : std::vector<QString>{};
            const term::Terminal_render_snapshot before_snapshot =
                (check_this_chunk && dirty_coverage.compare_cells)
                    ? model.render_snapshot(snapshot_sequence++)
                    : term::Terminal_render_snapshot{};
            const term::Terminal_screen_model_result result =
                model.ingest(QByteArrayView(bytes).sliced(offset, count));
            out.diagnostic_count += diagnostic_count(result);
            if (check_this_chunk && dirty_coverage.compare_cells) {
                const term::Terminal_render_snapshot after_snapshot =
                    model.render_snapshot(snapshot_sequence++);
                out.dirty_coverage_violations += count_dirty_coverage_violations(
                    before_snapshot,
                    after_snapshot,
                    offset,
                    count);
            }
            else
            if (check_this_chunk) {
                out.dirty_coverage_violations += count_dirty_text_coverage_violations(
                    before_rows,
                    before_grid_size,
                    before_buffer,
                    model_row_text_keys(model),
                    model.grid_size(),
                    model.active_buffer_id(),
                    result,
                    offset,
                    count);
            }
            offset += count;
        }
    }
    else {
        const term::Terminal_screen_model_result result = model.ingest(bytes);
        out.diagnostic_count += diagnostic_count(result);
    }

    out.visible_text    = model.visible_text();
    out.title           = model.title();
    out.icon_name       = model.icon_name();
    out.cursor          = model.cursor_position();
    out.scrollback_size = model.scrollback_size();
    out.snapshot        = model.render_snapshot(snapshot_sequence);
    return out;
}

bool check_result_against_expectations(
    const fs::path&            path,
    const Replay_result&       result,
    const Replay_expectations& expectations)
{
    bool ok = true;

    ok &= check(result.diagnostic_count <= expectations.max_diagnostics,
        path.string() + ": diagnostic count is within expectation");
    ok &= check(result.dirty_coverage_violations == 0,
        path.string() + ": dirty row coverage is complete");
    ok &= check(term::validate_render_snapshot(result.snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        path.string() + ": final render snapshot is valid");

    if (expectations.min_scrollback_rows >= 0) {
        ok &= check(result.scrollback_size >= expectations.min_scrollback_rows,
            path.string() + ": scrollback row count is within expectation");
    }

    const std::string visible = result.visible_text.toStdString();
    if (!expectations.visible_contains.empty()) {
        ok &= check(visible.find(expectations.visible_contains) != std::string::npos,
            path.string() + ": visible text contains expected text");
    }
    if (!expectations.visible_excludes.empty()) {
        ok &= check(visible.find(expectations.visible_excludes) == std::string::npos,
            path.string() + ": visible text excludes forbidden text");
    }
    if (!expectations.title_contains.empty()) {
        const std::string title = result.title.toStdString();
        ok &= check(title.find(expectations.title_contains) != std::string::npos,
            path.string() + ": title contains expected text");
    }

    return ok;
}

bool check_chunking_invariance(
    const fs::path&        path,
    const Replay_result&   whole,
    const Replay_result&   chunked)
{
    bool ok = true;

    ok &= check(whole.visible_text == chunked.visible_text,
        path.string() + ": chunked replay visible text matches whole replay");
    ok &= check(whole.title == chunked.title,
        path.string() + ": chunked replay title matches whole replay");
    ok &= check(whole.icon_name == chunked.icon_name,
        path.string() + ": chunked replay icon name matches whole replay");
    ok &= check(whole.cursor.row == chunked.cursor.row &&
        whole.cursor.column == chunked.cursor.column,
        path.string() + ": chunked replay cursor matches whole replay");
    ok &= check(whole.scrollback_size == chunked.scrollback_size,
        path.string() + ": chunked replay scrollback matches whole replay");
    return ok;
}

bool is_capture_file(const fs::path& path)
{
    const std::string extension = path.extension().string();
    return extension == ".raw" || extension == ".vnm_capture";
}

bool run_capture(const fs::path& path)
{
    Capture_input input;
    bool ok = true;

    ok &= load_capture_input(path, input);
    ok &= check(!input.bytes.isEmpty(), path.string() + ": capture is not empty");
    if (!ok || input.bytes.isEmpty()) {
        return false;
    }

    const Dirty_coverage_config coverage = dirty_coverage_config(input.expectations);
    const Replay_result         whole    = replay_bytes(input.bytes, false, {});
    const Replay_result         chunked  = replay_bytes(input.bytes, true,  coverage);

    ok &= check_result_against_expectations(path, whole, input.expectations);
    ok &= check_result_against_expectations(path, chunked, input.expectations);
    ok &= check_chunking_invariance(path, whole, chunked);
    return ok;
}

bool run_capture_directory(const fs::path& directory)
{
    bool ok            = true;
    int  capture_count = 0;

    ok &= check(fs::is_directory(directory),
        "capture replay directory exists: " + directory.string());
    if (!ok) {
        return false;
    }

    std::vector<fs::path> captures;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(directory)) {
        if (entry.is_regular_file() && is_capture_file(entry.path())) {
            captures.push_back(entry.path());
        }
    }

    std::sort(captures.begin(), captures.end());
    for (const fs::path& path : captures) {
        ++capture_count;
        ok &= run_capture(path);
    }

    ok &= check(capture_count > 0,
        "capture replay directory contains .raw or .vnm_capture files");
    return ok;
}

}

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: capture_replay_conformance_tests <capture-dir>\n";
        return 1;
    }

    return run_capture_directory(fs::path(argv[1])) ? 0 : 1;
}
