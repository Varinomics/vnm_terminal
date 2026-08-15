#define VNM_TERMINAL_APP_NO_MAIN
#include "../../src/main.cpp"
#undef VNM_TERMINAL_APP_NO_MAIN

#include "helpers/test_check.h"

#include <QColor>
#include <QGuiApplication>
#include <QSettings>
#include <QTemporaryDir>

#include <cmath>
#include <iostream>
#include <string>

namespace {

using vnm_terminal::test_helpers::check;

bool nearly_equal(qreal actual, qreal expected)
{
    return std::abs(actual - expected) <= 0.000001;
}

bool check_optional_size(
    const std::optional<QSize>& actual,
    const QSize&                expected,
    const std::string&          message)
{
    return
        check(actual.has_value(), message + " is present") &&
        check(*actual == expected, message + " matches");
}

bool check_optional_position(
    const std::optional<QPoint>& actual,
    const QPoint&                expected,
    const std::string&           message)
{
    return
        check(actual.has_value(), message + " is present") &&
        check(*actual == expected, message + " matches");
}

bool check_optional_font_size(
    const std::optional<qreal>& actual,
    qreal                       expected,
    const std::string&          message)
{
    return
        check(actual.has_value(), message + " is present") &&
        check(nearly_equal(*actual, expected), message + " matches");
}

bool test_save_and_load_window_state()
{
    QTemporaryDir dir;
    bool ok = check(dir.isValid(), "temporary settings directory is valid");
    if (!ok) {
        return false;
    }

    Persisted_terminal_window_state expected;
    expected.position  = QPoint(64, 96);
    expected.size      = QSize(1024, 720);
    expected.font_size = 18.0;
    expected.maximized = true;

    QSettings writer(dir.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    save_persisted_terminal_window_state(writer, expected);

    QSettings reader(dir.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    const Persisted_terminal_window_state actual =
        load_persisted_terminal_window_state(reader);

    ok &= check_optional_position(actual.position, *expected.position,
        "persisted position");
    ok &= check_optional_size(actual.size, *expected.size,
        "persisted size");
    ok &= check_optional_font_size(actual.font_size, *expected.font_size,
        "persisted font size");
    ok &= check(actual.maximized == expected.maximized,
        "persisted maximized state matches");
    return ok;
}

bool test_rejected_deferred_startup_returns_start_failed()
{
    const auto queued_exit_status =
        deferred_startup_exit_status(vnm::qt::Post_result::QUEUED);
    bool ok = check(
        !queued_exit_status.has_value(),
        "accepted deferred startup does not request an early exit");

    QObject receiver;
    receiver.moveToThread(nullptr);

    const auto post_result = vnm::qt::post(&receiver, [] {});
    ok &= check(
        post_result == vnm::qt::Post_result::NO_THREAD_AFFINITY,
        "deferred startup post is rejected without receiver affinity");

    const auto exit_status = deferred_startup_exit_status(post_result);
    ok &= check(
        exit_status.has_value() && *exit_status == k_exit_start_failed,
        "rejected deferred startup returns the start-failed exit status");
    return ok;
}

bool test_apply_persisted_window_state()
{
    Persisted_terminal_window_state state;
    state.size      = QSize(1200, 820);
    state.font_size = 19.0;
    state.maximized = true;

    App_options options;
    options.window_size = QSize(800, 600);
    options.font_size   = 13.0;
    apply_persisted_terminal_window_state(state, &options);

    bool ok = true;
    ok &= check(options.window_size == *state.size,
        "persisted size is applied without command-line override");
    ok &= check(nearly_equal(options.font_size, *state.font_size),
        "persisted font size is applied without command-line override");
    ok &= check(options.restore_maximized_window_state,
        "persisted maximized state is applied");

    App_options explicit_options;
    explicit_options.window_size          = QSize(900, 640);
    explicit_options.font_size            = 14.0;
    explicit_options.window_size_explicit = true;
    explicit_options.font_size_explicit   = true;
    apply_persisted_terminal_window_state(state, &explicit_options);

    ok &= check(explicit_options.window_size == QSize(900, 640),
        "explicit window size overrides persisted size");
    ok &= check(nearly_equal(explicit_options.font_size, 14.0),
        "explicit font size overrides persisted font size");
    ok &= check(!explicit_options.restore_maximized_window_state,
        "explicit window size overrides persisted maximized state");
    return ok;
}

bool test_invalid_persisted_values_are_ignored()
{
    QTemporaryDir dir;
    bool ok = check(dir.isValid(), "temporary invalid-settings directory is valid");
    if (!ok) {
        return false;
    }

    QSettings settings(dir.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    settings.beginGroup(QLatin1String(k_window_settings_group));
    settings.setValue(QLatin1String(k_window_settings_width),     0);
    settings.setValue(QLatin1String(k_window_settings_height),    480);
    settings.setValue(QLatin1String(k_window_settings_font_size), -1.0);
    settings.setValue(QLatin1String(k_window_settings_x),         10);
    settings.setValue(QLatin1String(k_window_settings_y),         20);
    settings.endGroup();
    settings.sync();

    QSettings reader(dir.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    const Persisted_terminal_window_state state =
        load_persisted_terminal_window_state(reader);

    ok &= check(!state.size.has_value(),
        "invalid persisted size is ignored");
    ok &= check(!state.font_size.has_value(),
        "invalid persisted font size is ignored");
    ok &= check_optional_position(state.position, QPoint(10, 20),
        "valid persisted position survives unrelated invalid values");
    return ok;
}

bool test_appearance_settings_round_trip()
{
    QTemporaryDir dir;
    bool ok = check(dir.isValid(), "temporary appearance-settings directory is valid");
    if (!ok) {
        return false;
    }

    const App_options default_options;
    ok &= check(default_options.color_scheme == QStringLiteral("Classic"),
        "app color scheme defaults to Classic");

    QSettings writer(dir.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    writer.beginGroup(QLatin1String(k_appearance_settings_group));
    writer.setValue(QLatin1String(k_appearance_color_scheme), QStringLiteral("Solarized Dark"));
    writer.setValue(QLatin1String(k_appearance_font_family),  QStringLiteral("Cascadia Mono"));
    writer.setValue(
        QLatin1String(k_appearance_text_renderer_mode),
        static_cast<int>(VNM_TerminalSurface::Text_renderer_mode::GLYPH));
    writer.setValue(
        QLatin1String(k_appearance_lcd_subpixel_order),
        static_cast<int>(VNM_TerminalSurface::Lcd_subpixel_order::NONE));
    writer.setValue(QLatin1String(k_appearance_row_timestamp_tooltip), false);
    writer.setValue(QLatin1String(k_appearance_scrollback_limit), 25000);
    writer.endGroup();
    writer.sync();

    QSettings reader(dir.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    const Persisted_appearance_settings state = load_persisted_appearance_settings(reader);

    ok &= check(state.color_scheme.value_or(QString()) == QStringLiteral("Solarized Dark"),
        "persisted color scheme round-trips");
    ok &= check(state.font_family.value_or(QString()) == QStringLiteral("Cascadia Mono"),
        "persisted font family round-trips");
    ok &= check(
        state.text_renderer_mode.value_or(-1) ==
            static_cast<int>(VNM_TerminalSurface::Text_renderer_mode::GLYPH),
        "persisted renderer mode round-trips");
    ok &= check(
        state.lcd_subpixel_order.value_or(-1) ==
            static_cast<int>(VNM_TerminalSurface::Lcd_subpixel_order::NONE),
        "persisted lcd subpixel order round-trips");
    ok &= check(state.row_timestamp_tooltip.has_value() && !*state.row_timestamp_tooltip,
        "persisted row timestamp toggle round-trips");
    ok &= check(state.scrollback_limit.value_or(-1) == 25000,
        "persisted scrollback limit round-trips");

    App_options options;
    apply_persisted_appearance_settings(state, &options);
    ok &= check(options.color_scheme == QStringLiteral("Solarized Dark"),
        "persisted color scheme is applied without command-line override");
    ok &= check(options.font_family == QStringLiteral("Cascadia Mono"),
        "persisted font family is applied without command-line override");
    ok &= check(
        options.text_renderer_mode == VNM_TerminalSurface::Text_renderer_mode::GLYPH,
        "persisted renderer mode is applied without command-line override");
    ok &= check(
        options.lcd_subpixel_order == VNM_TerminalSurface::Lcd_subpixel_order::NONE,
        "persisted lcd subpixel order is applied without command-line override");
    ok &= check(!options.row_timestamp_tooltip_enabled,
        "persisted row timestamp toggle is applied without command-line override");
    ok &= check(options.scrollback_limit.value_or(0) == 25000,
        "persisted scrollback limit is applied without command-line override");

    App_options explicit_options;
    explicit_options.color_scheme                   = QStringLiteral("Campbell");
    explicit_options.color_scheme_explicit          = true;
    explicit_options.font_family                    = QStringLiteral("Consolas");
    explicit_options.font_family_explicit           = true;
    explicit_options.text_renderer_mode             = VNM_TerminalSurface::Text_renderer_mode::MSDF;
    explicit_options.text_renderer_mode_explicit    = true;
    explicit_options.lcd_subpixel_order             = VNM_TerminalSurface::Lcd_subpixel_order::RGB;
    explicit_options.lcd_subpixel_order_explicit    = true;
    explicit_options.row_timestamp_tooltip_enabled  = true;
    explicit_options.row_timestamp_tooltip_explicit = true;
    explicit_options.scrollback_limit               = 4000;
    apply_persisted_appearance_settings(state, &explicit_options);
    ok &= check(explicit_options.color_scheme == QStringLiteral("Campbell"),
        "explicit color scheme overrides persisted scheme");
    ok &= check(explicit_options.font_family == QStringLiteral("Consolas"),
        "explicit font family overrides persisted family");
    ok &= check(
        explicit_options.text_renderer_mode == VNM_TerminalSurface::Text_renderer_mode::MSDF,
        "explicit renderer mode overrides persisted mode");
    ok &= check(
        explicit_options.lcd_subpixel_order == VNM_TerminalSurface::Lcd_subpixel_order::RGB,
        "explicit lcd subpixel order overrides persisted order");
    ok &= check(explicit_options.row_timestamp_tooltip_enabled,
        "explicit row timestamp flag overrides persisted toggle");
    ok &= check(explicit_options.scrollback_limit.value_or(0) == 4000,
        "explicit scrollback limit overrides persisted limit");

    Persisted_appearance_settings bogus;
    bogus.color_scheme = QStringLiteral("Not A Real Scheme");
    App_options bogus_options;
    const QString default_scheme = bogus_options.color_scheme;
    apply_persisted_appearance_settings(bogus, &bogus_options);
    ok &= check(bogus_options.color_scheme == default_scheme,
        "unknown persisted color scheme is rejected");

    return ok;
}

bool test_stored_forced_msdf_preference_uses_auto()
{
    QTemporaryDir dir;
    bool ok = check(dir.isValid(), "temporary forced-renderer settings directory is valid");
    if (!ok) {
        return false;
    }

    const QString path = dir.filePath(QStringLiteral("settings.ini"));
    QSettings writer(path, QSettings::IniFormat);
    writer.beginGroup(QLatin1String(k_appearance_settings_group));
    writer.setValue(
        QLatin1String(k_appearance_text_renderer_mode),
        static_cast<int>(VNM_TerminalSurface::Text_renderer_mode::MSDF));
    writer.endGroup();
    writer.sync();

    QSettings reader(path, QSettings::IniFormat);
    const Persisted_appearance_settings state = load_persisted_appearance_settings(reader);
    ok &= check(
        state.text_renderer_mode.value_or(-1) ==
            static_cast<int>(VNM_TerminalSurface::Text_renderer_mode::AUTO),
        "stored forced MSDF preference loads as automatic fallback");

    App_options options;
    apply_persisted_appearance_settings(state, &options);
    ok &= check(
        options.text_renderer_mode == VNM_TerminalSurface::Text_renderer_mode::AUTO,
        "stored forced MSDF preference applies automatic fallback");
    return ok;
}

bool test_save_appearance_settings_from_surface()
{
    QTemporaryDir dir;
    bool ok = check(dir.isValid(), "temporary surface-appearance settings directory is valid");
    if (!ok) {
        return false;
    }

    VNM_TerminalSurface surface;
    surface.set_color_scheme(QStringLiteral("Solarized Dark"));
    surface.set_font_family(QStringLiteral("monospace"));
    surface.set_text_renderer_mode(VNM_TerminalSurface::Text_renderer_mode::GLYPH);
    surface.set_lcd_subpixel_order(VNM_TerminalSurface::Lcd_subpixel_order::NONE);
    surface.set_row_timestamp_tooltip_enabled(false);
    surface.set_scrollback_limit(25000);

    QSettings writer(dir.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    save_persisted_appearance_settings(writer, surface);

    QSettings reader(dir.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    const Persisted_appearance_settings state = load_persisted_appearance_settings(reader);

    ok &= check(state.color_scheme.value_or(QString()) == surface.color_scheme(),
        "surface color scheme persists immediately");
    ok &= check(state.font_family.value_or(QString()) == surface.font_family(),
        "surface font family persists immediately");
    ok &= check(
        state.text_renderer_mode.value_or(-1) ==
            static_cast<int>(surface.text_renderer_mode()),
        "surface renderer mode persists immediately");
    ok &= check(
        state.lcd_subpixel_order.value_or(-1) ==
            static_cast<int>(surface.lcd_subpixel_order()),
        "surface lcd subpixel order persists immediately");
    ok &= check(state.row_timestamp_tooltip.has_value() && !*state.row_timestamp_tooltip,
        "surface row timestamp toggle persists immediately");
    ok &= check(state.scrollback_limit.value_or(-1) == surface.scrollback_limit(),
        "surface scrollback limit persists immediately");

    surface.set_text_renderer_mode(VNM_TerminalSurface::Text_renderer_mode::MSDF);
    save_persisted_appearance_settings(writer, surface);

    QSettings forced_reader(dir.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    const Persisted_appearance_settings forced_state =
        load_persisted_appearance_settings(forced_reader);
    ok &= check(
        forced_state.text_renderer_mode.value_or(-1) ==
            static_cast<int>(VNM_TerminalSurface::Text_renderer_mode::GLYPH),
        "forced MSDF runtime preserves the existing renderer preference");
    ok &= check(
        surface.text_renderer_mode() == VNM_TerminalSurface::Text_renderer_mode::MSDF,
        "saving appearance leaves the forced MSDF runtime unchanged");
    return ok;
}

bool test_chrome_palette_settings()
{
    QTemporaryDir dir;
    bool ok = check(dir.isValid(), "temporary chrome-palette settings directory is valid");
    if (!ok) {
        return false;
    }

    const chrome::Terminal_chrome_palette defaults =
        chrome::default_terminal_chrome_palette();
    ok &= check(
        defaults.focused_background.lightness() > defaults.unfocused_background.lightness(),
        "the focused chrome fill is brighter than the unfocused fill");
    ok &= check(
        defaults.focused_frame_edge.lightness() > defaults.unfocused_frame_edge.lightness(),
        "the focused frame edge is brighter than the unfocused edge");
    ok &= check(
        defaults.focused_frame_edge.lightness() > defaults.focused_background.lightness(),
        "the focused outline stays brighter than the focused fill");

    QSettings writer(dir.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    writer.beginGroup(QLatin1String(k_appearance_settings_group));
    writer.setValue(
        QLatin1String(chrome::k_appearance_chrome_focused_background),
        QStringLiteral("#204060"));
    writer.setValue(
        QLatin1String(chrome::k_appearance_chrome_focused_frame_edge),
        QStringLiteral("#3a5a7a"));
    writer.setValue(
        QLatin1String(chrome::k_appearance_chrome_unfocused_frame_edge),
        QStringLiteral("not a color"));
    writer.endGroup();
    writer.sync();

    QSettings reader(dir.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    const Persisted_appearance_settings state = load_persisted_appearance_settings(reader);
    ok &= check(
        state.chrome_focused_background.value_or(QColor()) == QColor(0x20, 0x40, 0x60),
        "persisted focused chrome fill round-trips");
    ok &= check(!state.chrome_unfocused_frame_edge.has_value(),
        "an unreadable chrome color is ignored");

    const chrome::Terminal_chrome_palette palette =
        persisted_terminal_chrome_palette(state);
    ok &= check(palette.focused_background == QColor(0x20, 0x40, 0x60),
        "the persisted focused fill reaches the chrome palette");
    ok &= check(palette.focused_frame_edge == QColor(0x3a, 0x5a, 0x7a),
        "the persisted focused edge reaches the chrome palette");
    ok &= check(palette.unfocused_background == defaults.unfocused_background,
        "an absent chrome color keeps its default");
    ok &= check(palette.unfocused_frame_edge == defaults.unfocused_frame_edge,
        "an unreadable chrome color keeps its default");

    set_terminal_chrome_palette(palette);
    ok &= check(
        chrome::terminal_chrome_background_color(true) == palette.focused_background,
        "the configured palette drives the focused chrome background");
    ok &= check(
        chrome::terminal_chrome_frame_edge_color(false) == palette.unfocused_frame_edge,
        "the configured palette drives the unfocused frame edge");
    set_terminal_chrome_palette(defaults);
    ok &= check(
        chrome::terminal_chrome_background_color(true) == defaults.focused_background,
        "the chrome palette can be restored to its defaults");
    return ok;
}

bool test_interaction_settings_round_trip()
{
    QTemporaryDir dir;
    bool ok = check(dir.isValid(), "temporary interaction-settings directory is valid");
    if (!ok) {
        return false;
    }

    QSettings writer(dir.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    save_persisted_interaction_settings(writer, true);

    QSettings reader(dir.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    const vnm_terminal::terminal_app::Persisted_interaction_settings state =
        load_persisted_interaction_settings(reader);

    ok &= check(state.copy_on_select.has_value() && *state.copy_on_select,
        "persisted copy-on-selection toggle round-trips");

    App_options options;
    ok &= check(!options.copy_on_select,
        "copy on selection defaults off");
    apply_persisted_interaction_settings(state, &options);
    ok &= check(options.copy_on_select,
        "persisted copy-on-selection toggle is applied");
    return ok;
}

}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);

    bool ok = true;
    ok &= test_save_and_load_window_state();
    ok &= test_rejected_deferred_startup_returns_start_failed();
    ok &= test_apply_persisted_window_state();
    ok &= test_invalid_persisted_values_are_ignored();
    ok &= test_appearance_settings_round_trip();
    ok &= test_stored_forced_msdf_preference_uses_auto();
    ok &= test_save_appearance_settings_from_surface();
    ok &= test_chrome_palette_settings();
    ok &= test_interaction_settings_round_trip();
    return ok ? 0 : 1;
}
