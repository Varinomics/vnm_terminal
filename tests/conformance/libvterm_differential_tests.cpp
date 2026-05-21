#include "vnm_terminal/internal/terminal_screen_model.h"
#include "conformance_fixture_io.h"

extern "C" {
#include "helpers/test_check.h"
#include <vterm.h>
}

#include <QByteArray>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace term = vnm_terminal::internal;
namespace fixture = vnm_terminal::tests::conformance;

namespace {

constexpr int k_default_rows    = 24;
constexpr int k_default_columns = 80;

struct Grid_size
{
    int            rows    = k_default_rows;
    int            columns = k_default_columns;
};

struct Cell_attributes
{
    std::uint32_t  foreground = 0U;
    std::uint32_t  background = 0U;
    bool           bold       = false;
    bool           underline  = false;
    bool           italic     = false;
    bool           blink      = false;
    bool           inverse    = false;
    bool           conceal    = false;
    bool           strike     = false;
};

struct Vterm_deleter
{
    void operator()(VTerm* terminal) const
    {
        if (terminal != nullptr) {
            vterm_free(terminal);
        }
    }
};

using Vterm_ptr = std::unique_ptr<VTerm, Vterm_deleter>;

using vnm_terminal::test_helpers::check;

bool is_corpus_file(const fs::path& path)
{
    const std::string extension = path.extension().string();
    return extension == ".raw" || extension == ".vnm_capture";
}

std::string trim_trailing_spaces(std::string text)
{
    while (!text.empty() && text.back() == ' ') {
        text.pop_back();
    }

    return text;
}

char hex_digit(unsigned int value)
{
    return value < 10U
        ? static_cast<char>('0' + value)
        : static_cast<char>('A' + value - 10U);
}

std::string byte_hex(unsigned char byte)
{
    std::string out = "0x00";
    out[2] = hex_digit(static_cast<unsigned int>(byte >> 4U));
    out[3] = hex_digit(static_cast<unsigned int>(byte & 0x0FU));
    return out;
}

std::string escaped_text(const std::string& text)
{
    std::string out;
    out.reserve(text.size() + 2U);
    out.push_back('"');

    for (const unsigned char byte : text) {
        switch (byte) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (byte >= 0x20U && byte <= 0x7EU) {
                    out.push_back(static_cast<char>(byte));
                }
                else {
                    out += "\\x";
                    out.push_back(hex_digit(static_cast<unsigned int>(byte >> 4U)));
                    out.push_back(hex_digit(static_cast<unsigned int>(byte & 0x0FU)));
                }
                break;
        }
    }

    out.push_back('"');
    return out;
}

std::string byte_at_or_eof(const std::string& text, std::size_t offset)
{
    if (offset >= text.size()) {
        return "EOF";
    }

    return byte_hex(static_cast<unsigned char>(text[offset]));
}

std::size_t first_differing_offset(const std::string& left, const std::string& right)
{
    const std::size_t shared_size = std::min(left.size(), right.size());
    for (std::size_t offset = 0U; offset < shared_size; ++offset) {
        if (left[offset] != right[offset]) {
            return offset;
        }
    }

    return shared_size;
}

int environment_int(const char* name, int fallback)
{
    const char* const value = std::getenv(name);
    if (value == nullptr) {
        return fallback;
    }

    try {
        std::size_t consumed = 0U;
        const int   parsed   = std::stoi(value, &consumed, 10);
        if (consumed == std::string(value).size() && parsed > 0) {
            return parsed;
        }
    }
    catch (const std::exception&) {
    }

    return fallback;
}

Grid_size differential_grid_size()
{
    return {
        environment_int("VNM_TERMINAL_LIBVTERM_ROWS", k_default_rows),
        environment_int("VNM_TERMINAL_LIBVTERM_COLUMNS", k_default_columns),
    };
}

bool compare_attributes_enabled()
{
    const char* const value = std::getenv("VNM_TERMINAL_LIBVTERM_COMPARE_ATTRIBUTES");
    return value != nullptr && std::string(value) == "1";
}

int differential_byte_limit()
{
    return environment_int("VNM_TERMINAL_LIBVTERM_BYTE_LIMIT", 0);
}

std::uint32_t color_without_alpha(quint32 rgba)
{
    return static_cast<std::uint32_t>(rgba & 0x00ffffffU);
}

std::uint32_t vterm_color_rgb_key(const VTermColor& source_color, const VTermScreen* screen)
{
    VTermColor color = source_color;
    vterm_screen_convert_color_to_rgb(screen, &color);
    return
        (static_cast<std::uint32_t>(color.rgb.red) << 16U)  |
        (static_cast<std::uint32_t>(color.rgb.green) << 8U) |
         static_cast<std::uint32_t>(color.rgb.blue);
}

VTermColor vterm_rgb_color(quint32 rgba)
{
    VTermColor color;
    vterm_color_rgb(
        &color,
        static_cast<std::uint8_t>((rgba >> 16U) & 0xffU),
        static_cast<std::uint8_t>((rgba >> 8U) & 0xffU),
        static_cast<std::uint8_t>(rgba & 0xffU));
    return color;
}

term::Terminal_color_state default_vnm_color_state(Grid_size grid_size)
{
    term::Terminal_screen_model_config config;
    config.grid_size                                = term::terminal_grid_size_t{grid_size.rows, grid_size.columns};
    config.scrollback_limit                         = 10000;
    config.tab_width                                = 8;
    config.recover_scrollback_from_primary_repaints = true;
    return term::Terminal_screen_model(config).render_snapshot(0U).color_state;
}

void configure_vterm_colors(VTerm* terminal, Grid_size grid_size)
{
    const term::Terminal_color_state color_state = default_vnm_color_state(grid_size);
    VTermState*                      state       = vterm_obtain_state(terminal);
    VTermColor                       default_fg  = vterm_rgb_color(color_state.default_foreground_rgba);
    VTermColor                       default_bg  = vterm_rgb_color(color_state.default_background_rgba);
    vterm_state_set_default_colors(state, &default_fg, &default_bg);
    for (int index = 0; index < static_cast<int>(color_state.palette_rgba.size()); ++index) {
        VTermColor color = vterm_rgb_color(
            color_state.palette_rgba[static_cast<std::size_t>(index)]);
        vterm_state_set_palette_color(state, index, &color);
    }
}

Cell_attributes default_vnm_attributes(const term::Terminal_render_snapshot& snapshot)
{
    const term::Terminal_text_style default_style = snapshot.styles.empty()
        ? term::make_default_terminal_text_style()
        : snapshot.styles.front();
    Cell_attributes attributes;
    attributes.foreground = color_without_alpha(
        term::resolve_terminal_color_ref(default_style.foreground, snapshot.color_state, true));
    attributes.background = color_without_alpha(
        term::resolve_terminal_color_ref(default_style.background, snapshot.color_state, false));
    return attributes;
}

Cell_attributes vnm_attributes_for_style(
    const term::Terminal_render_snapshot&  snapshot,
    term::Terminal_style_id                style_id)
{
    if (static_cast<std::size_t>(style_id) >= snapshot.styles.size()) {
        return default_vnm_attributes(snapshot);
    }

    const term::Terminal_text_style style =
        snapshot.styles[static_cast<std::size_t>(style_id)];
    Cell_attributes attributes;
    attributes.foreground = color_without_alpha(
        term::resolve_terminal_color_ref(style.foreground, snapshot.color_state, true));
    attributes.background = color_without_alpha(
        term::resolve_terminal_color_ref(style.background, snapshot.color_state, false));
    attributes.bold       = term::terminal_style_has_attribute(
        style,
        term::Terminal_style_attribute::BOLD);
    attributes.underline  = term::terminal_style_has_attribute(
        style,
        term::Terminal_style_attribute::UNDERLINE);
    attributes.italic     = term::terminal_style_has_attribute(
        style,
        term::Terminal_style_attribute::ITALIC);
    attributes.blink      = term::terminal_style_has_attribute(
        style,
        term::Terminal_style_attribute::BLINK);
    attributes.inverse    = term::terminal_style_has_attribute(
        style,
        term::Terminal_style_attribute::INVERSE);
    attributes.conceal    = term::terminal_style_has_attribute(
        style,
        term::Terminal_style_attribute::INVISIBLE);
    attributes.strike     = term::terminal_style_has_attribute(
        style,
        term::Terminal_style_attribute::STRIKE);
    if (attributes.inverse != snapshot.modes.reverse_video) { std::swap(attributes.foreground, attributes.background); }
    if (attributes.conceal)                                 { attributes.foreground = attributes.background;           }
    return attributes;
}

bool same_attributes(const Cell_attributes& left, const Cell_attributes& right)
{
    return
        left.foreground == right.foreground &&
        left.background == right.background &&
        left.bold       == right.bold       &&
        left.underline  == right.underline  &&
        left.italic     == right.italic     &&
        left.blink      == right.blink      &&
        left.inverse    == right.inverse    &&
        left.conceal    == right.conceal    &&
        left.strike     == right.strike;
}

void print_row_mismatch(
    const fs::path&    path,
    int                row,
    const std::string& vnm_row,
    const std::string& libvterm_row)
{
    const std::size_t offset = first_differing_offset(vnm_row, libvterm_row);
    std::cerr
        << "FAIL: " << path.string() << ": row " << row << " differs from libvterm\n"
        << "  vnm:      " << escaped_text(vnm_row) << '\n'
        << "  libvterm: " << escaped_text(libvterm_row) << '\n'
        << "  first differing byte: offset " << offset
        << ", vnm=" << byte_at_or_eof(vnm_row, offset)
        << ", libvterm=" << byte_at_or_eof(libvterm_row, offset) << '\n';
}

std::vector<std::string> vnm_rows_for_bytes(const QByteArray& bytes, Grid_size grid_size)
{
    term::Terminal_screen_model_config config;
    config.grid_size                                = term::terminal_grid_size_t{grid_size.rows, grid_size.columns};
    config.scrollback_limit                         = 10000;
    config.tab_width                                = 8;
    config.recover_scrollback_from_primary_repaints = true;

    term::Terminal_screen_model model(config);

    model.ingest(bytes);

    std::vector<std::string> rows;
    rows.reserve(static_cast<std::size_t>(grid_size.rows));
    for (int row = 0; row < grid_size.rows; ++row) {
        rows.push_back(trim_trailing_spaces(model.row_text(row).toStdString()));
    }
    return rows;
}

std::vector<std::vector<Cell_attributes>> vnm_attributes_for_bytes(
    const QByteArray&  bytes,
    Grid_size          grid_size)
{
    term::Terminal_screen_model_config config;
    config.grid_size                                = term::terminal_grid_size_t{grid_size.rows, grid_size.columns};
    config.scrollback_limit                         = 10000;
    config.tab_width                                = 8;
    config.recover_scrollback_from_primary_repaints = true;

    term::Terminal_screen_model model(config);
    model.ingest(bytes);
    const term::Terminal_render_snapshot snapshot = model.render_snapshot(1U);
    std::vector<std::vector<Cell_attributes>> rows(
        static_cast<std::size_t>(grid_size.rows),
        std::vector<Cell_attributes>(
            static_cast<std::size_t>(grid_size.columns),
            default_vnm_attributes(snapshot)));

    for (const term::Terminal_render_cell& cell : snapshot.cells) {
        if (cell.position.row    < 0  || cell.position.row    >= grid_size.rows ||
            cell.position.column < 0  || cell.position.column >= grid_size.columns)
        {
            continue;
        }

        const Cell_attributes attributes = vnm_attributes_for_style(snapshot, cell.style_id);
        const int width = std::clamp(
            cell.display_width,
            1,
            grid_size.columns - cell.position.column);
        for (int column_offset = 0; column_offset < width; ++column_offset) {
            rows[static_cast<std::size_t>(cell.position.row)]
                [static_cast<std::size_t>(cell.position.column + column_offset)] =
                    attributes;
        }
    }

    return rows;
}

bool libvterm_rows_for_bytes(
    const fs::path&            path,
    const QByteArray&          bytes,
    Grid_size                  grid_size,
    std::vector<std::string>&  rows)
{
    Vterm_ptr terminal(vterm_new(grid_size.rows, grid_size.columns));
    configure_vterm_colors(terminal.get(), grid_size);
    vterm_set_utf8(terminal.get(), 1);

    VTermScreen* screen = vterm_obtain_screen(terminal.get());
    // The corpus includes alternate-screen switches; opt libvterm into tracking that buffer.
    vterm_screen_enable_altscreen(screen, 1);
    vterm_screen_reset(screen, 1);

    vterm_input_write(
        terminal.get(),
        bytes.constData(),
        static_cast<std::size_t>(bytes.size()));
    vterm_screen_flush_damage(screen);

    rows.clear();
    rows.reserve(static_cast<std::size_t>(grid_size.rows));
    std::vector<char> buffer;
    for (int row = 0; row < grid_size.rows; ++row) {
        const VTermRect rect{row, row + 1, 0, grid_size.columns};
        const std::size_t expected_count = vterm_screen_get_text(
            screen,
            nullptr,
            0,
            rect);
        buffer.resize(expected_count);

        const std::size_t count = vterm_screen_get_text(
            screen,
            buffer.empty() ? nullptr : buffer.data(),
            buffer.size(),
            rect);
        if (count > expected_count) {
            return check(false,
                path.string() + ": libvterm row " + std::to_string(row) +
                " text exceeded sized extraction buffer (count " +
                    std::to_string(count) + ", capacity " +
                    std::to_string(expected_count) + ")");
        }
        rows.push_back(trim_trailing_spaces(
            count == 0U
                ? std::string()
                : std::string(buffer.data(), count)));
    }

    return true;
}

bool libvterm_attributes_for_bytes(
    const QByteArray&                          bytes,
    Grid_size                                  grid_size,
    std::vector<std::vector<Cell_attributes>>& rows)
{
    Vterm_ptr terminal(vterm_new(grid_size.rows, grid_size.columns));
    configure_vterm_colors(terminal.get(), grid_size);
    vterm_set_utf8(terminal.get(), 1);

    VTermScreen* screen = vterm_obtain_screen(terminal.get());
    vterm_screen_enable_altscreen(screen, 1);
    vterm_screen_reset(screen, 1);
    vterm_input_write(
        terminal.get(),
        bytes.constData(),
        static_cast<std::size_t>(bytes.size()));
    vterm_screen_flush_damage(screen);

    rows.assign(
        static_cast<std::size_t>(grid_size.rows),
        std::vector<Cell_attributes>(static_cast<std::size_t>(grid_size.columns)));
    for (int row = 0; row < grid_size.rows; ++row) {
        for (int column = 0; column < grid_size.columns; ++column) {
            VTermScreenCell cell;
            if (!vterm_screen_get_cell(screen, {row, column}, &cell)) {
                return false;
            }

            Cell_attributes attributes;
            attributes.foreground = vterm_color_rgb_key(cell.fg, screen);
            attributes.background = vterm_color_rgb_key(cell.bg, screen);
            attributes.bold       = cell.attrs.bold != 0U;
            attributes.underline  = cell.attrs.underline != 0U;
            attributes.italic     = cell.attrs.italic != 0U;
            attributes.blink      = cell.attrs.blink != 0U;
            attributes.inverse    = cell.attrs.reverse != 0U;
            attributes.conceal    = cell.attrs.conceal != 0U;
            attributes.strike     = cell.attrs.strike != 0U;
            rows[static_cast<std::size_t>(row)][static_cast<std::size_t>(column)] =
                attributes;
        }
    }

    return true;
}

bool compare_rows(
    const fs::path&                    path,
    Grid_size                          grid_size,
    const std::vector<std::string>&    vnm_rows,
    const std::vector<std::string>&    libvterm_rows)
{
    bool ok = true;

    for (int row = 0; row < grid_size.rows; ++row) {
        const std::string& vnm_row      = vnm_rows[static_cast<std::size_t>(row)];
        const std::string& libvterm_row = libvterm_rows[static_cast<std::size_t>(row)];
        if (vnm_row != libvterm_row) {
            print_row_mismatch(path, row, vnm_row, libvterm_row);
            ok = false;
        }
    }

    return ok;
}

bool compare_attributes(
    const fs::path&                                    path,
    Grid_size                                          grid_size,
    const std::vector<std::vector<Cell_attributes>>&   vnm_rows,
    const std::vector<std::vector<Cell_attributes>>&   libvterm_rows)
{
    bool ok       = true;
    int  reported = 0;

    for (int row = 0; row < grid_size.rows; ++row) {
        for (int column = 0; column < grid_size.columns; ++column) {
            const Cell_attributes& vnm =
                vnm_rows[static_cast<std::size_t>(row)][static_cast<std::size_t>(column)];
            const Cell_attributes& libvterm =
                libvterm_rows[static_cast<std::size_t>(row)]
                    [static_cast<std::size_t>(column)];
            if (same_attributes(vnm, libvterm)) {
                continue;
            }

            if (reported < 24) {
                std::cerr
                    << "FAIL: " << path.string() << ": cell " << row << ',' << column
                    << " attributes differ from libvterm\n"
                    << "  vnm:      fg=0x" << std::hex << vnm.foreground
                    << " bg=0x" << vnm.background << std::dec
                    << " bold="      << vnm.bold
                    << " underline=" << vnm.underline
                    << " inverse="   << vnm.inverse
                    << " conceal="   << vnm.conceal
                    << '\n'
                    << "  libvterm: fg=0x" << std::hex << libvterm.foreground
                    << " bg=0x" << libvterm.background << std::dec
                    << " bold="      << libvterm.bold
                    << " underline=" << libvterm.underline
                    << " inverse="   << libvterm.inverse
                    << " conceal="   << libvterm.conceal
                    << '\n';
            }
            ++reported;
            ok = false;
        }
    }

    if (reported > 24) {
        std::cerr << "FAIL: " << path.string() << ": "
            << (reported - 24) << " additional attribute mismatches omitted\n";
    }
    return ok;
}

bool run_file(const fs::path& path)
{
    bool            ok         = true;
    const Grid_size grid_size  = differential_grid_size();
    QByteArray      bytes      = fixture::read_corpus_bytes(path, ok);
    const int       byte_limit = differential_byte_limit();
    if (byte_limit > 0 && byte_limit < bytes.size()) {
        bytes.truncate(byte_limit);
    }

    ok &= check(!bytes.isEmpty(), path.string() + ": corpus file is not empty");
    if (bytes.isEmpty()) {
        return false;
    }

    const std::vector<std::string> vnm_rows = vnm_rows_for_bytes(bytes, grid_size);
    std::vector<std::string> libvterm_rows;
    if (!libvterm_rows_for_bytes(path, bytes, grid_size, libvterm_rows)) {
        return false;
    }

    ok &= compare_rows(path, grid_size, vnm_rows, libvterm_rows);
    if (compare_attributes_enabled()) {
        const std::vector<std::vector<Cell_attributes>> vnm_attributes =
            vnm_attributes_for_bytes(bytes, grid_size);
        std::vector<std::vector<Cell_attributes>> libvterm_attributes;
        ok &= check(
            libvterm_attributes_for_bytes(bytes, grid_size, libvterm_attributes),
            path.string() + ": libvterm attributes are readable");
        if (!libvterm_attributes.empty()) {
            ok &= compare_attributes(path, grid_size, vnm_attributes, libvterm_attributes);
        }
    }
    return ok;
}

bool run_corpus(const fs::path& directory)
{
    bool ok = true;
    ok &= check(fs::is_directory(directory),
        "libvterm differential corpus directory exists: " + directory.string());
    if (!ok) {
        return false;
    }

    std::vector<fs::path> corpus_files;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(directory)) {
        if (entry.is_regular_file() && is_corpus_file(entry.path())) {
            corpus_files.push_back(entry.path());
        }
    }

    std::sort(corpus_files.begin(), corpus_files.end());
    for (const fs::path& path : corpus_files) {
        ok &= run_file(path);
    }

    ok &= check(!corpus_files.empty(),
        "libvterm differential corpus has .raw or .vnm_capture files");
    return ok;
}

}

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: libvterm_differential_tests <corpus-dir>\n";
        return 1;
    }

    return run_corpus(fs::path(argv[1])) ? 0 : 1;
}
