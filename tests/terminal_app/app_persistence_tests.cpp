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

    Command_line_setting_overrides overrides;
    QSettings writer(dir.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    save_persisted_terminal_window_state(writer, expected, overrides);

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
    explicit_options.scrollback_limit_explicit      = true;
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

    Command_line_setting_overrides overrides;
    QSettings writer(dir.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    save_persisted_appearance_settings(writer, surface, overrides);

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
    save_persisted_appearance_settings(writer, surface, overrides);

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

bool test_platform_adjusted_command_line_geometry_does_not_replace_stored_geometry()
{
    QTemporaryDir dir;
    bool ok = check(dir.isValid(), "temporary adjusted-geometry settings directory is valid");
    if (!ok) {
        return false;
    }

    const QString path = dir.filePath(QStringLiteral("settings.ini"));

    // The window geometry the user arrived at on an earlier run.
    QSettings stored(path, QSettings::IniFormat);
    stored.beginGroup(QLatin1String(k_window_settings_group));
    stored.setValue(QLatin1String(k_window_settings_width),  1024);
    stored.setValue(QLatin1String(k_window_settings_height), 720);
    stored.setValue(QLatin1String(k_window_settings_x),      120);
    stored.setValue(QLatin1String(k_window_settings_y),      140);
    stored.setValue(QLatin1String(k_window_settings_maximized), true);
    stored.endGroup();
    stored.sync();

    App_options options;
    options.window_size          = QSize(640, 480);
    options.window_size_explicit = true;

    VNM_TerminalSurface surface;
    Command_line_setting_overrides overrides =
        command_line_setting_overrides(options, surface);

    // What the window system actually granted: not the requested size, and at a
    // position it picked. None of this is a user decision.
    Persisted_terminal_window_state granted;
    granted.position  = QPoint(31, 47);
    granted.size      = QSize(638, 461);
    granted.maximized = false;

    QSettings writer(path, QSettings::IniFormat);
    save_persisted_terminal_window_state(writer, granted, overrides);

    QSettings first_reader(path, QSettings::IniFormat);
    const Persisted_terminal_window_state after_grant =
        load_persisted_terminal_window_state(first_reader);
    ok &= check_optional_size(after_grant.size, QSize(1024, 720),
        "platform-adjusted startup geometry leaves the stored size alone");
    ok &= check_optional_position(after_grant.position, QPoint(120, 140),
        "platform-adjusted startup geometry leaves the stored position alone");
    ok &= check(after_grant.maximized,
        "an explicit window size leaves the stored maximized state alone");

    // A second save of the same granted geometry is still not a user decision.
    save_persisted_terminal_window_state(writer, granted, overrides);
    QSettings second_reader(path, QSettings::IniFormat);
    const Persisted_terminal_window_state after_settle =
        load_persisted_terminal_window_state(second_reader);
    ok &= check_optional_size(after_settle.size, QSize(1024, 720),
        "a repeated save of the granted geometry stays ephemeral");
    ok &= check_optional_position(after_settle.position, QPoint(120, 140),
        "a repeated save of the granted position stays ephemeral");

    // Moving and resizing the window during the run is, so it reaches the
    // stored preferences exactly as it would in an unforced run.
    Persisted_terminal_window_state moved;
    moved.position  = QPoint(300, 320);
    moved.size      = QSize(1280, 800);
    moved.maximized = false;
    save_persisted_terminal_window_state(writer, moved, overrides);

    QSettings third_reader(path, QSettings::IniFormat);
    const Persisted_terminal_window_state after_move =
        load_persisted_terminal_window_state(third_reader);
    ok &= check_optional_size(after_move.size, QSize(1280, 800),
        "a window size chosen during the run replaces the stored size");
    ok &= check_optional_position(after_move.position, QPoint(300, 320),
        "a window position chosen during the run replaces the stored position");
    ok &= check(after_move.maximized,
        "a maximized state the run never entered stays ephemeral");

    // Maximizing releases the maximized latch, so unmaximizing afterwards is an
    // ordinary choice and reaches the stored preference.
    Persisted_terminal_window_state maximized = moved;
    maximized.maximized = true;
    save_persisted_terminal_window_state(writer, maximized, overrides);
    Persisted_terminal_window_state unmaximized = moved;
    unmaximized.maximized = false;
    save_persisted_terminal_window_state(writer, unmaximized, overrides);

    QSettings fourth_reader(path, QSettings::IniFormat);
    const Persisted_terminal_window_state after_unmaximize =
        load_persisted_terminal_window_state(fourth_reader);
    ok &= check(!after_unmaximize.maximized,
        "unmaximizing after maximizing during the run replaces the stored state");

    return ok;
}

bool test_command_line_overrides_do_not_replace_stored_settings()
{
    QTemporaryDir dir;
    bool ok = check(dir.isValid(), "temporary command-line-override settings directory is valid");
    if (!ok) {
        return false;
    }

    const QString path = dir.filePath(QStringLiteral("settings.ini"));

    // What the user chose in the settings panel on an earlier run.
    QSettings stored(path, QSettings::IniFormat);
    stored.beginGroup(QLatin1String(k_appearance_settings_group));
    stored.setValue(QLatin1String(k_appearance_color_scheme), QStringLiteral("Solarized Dark"));
    stored.setValue(QLatin1String(k_appearance_font_family),  QStringLiteral("Cascadia Mono"));
    stored.setValue(
        QLatin1String(k_appearance_text_renderer_mode),
        static_cast<int>(VNM_TerminalSurface::Text_renderer_mode::AUTO));
    stored.setValue(
        QLatin1String(k_appearance_lcd_subpixel_order),
        static_cast<int>(VNM_TerminalSurface::Lcd_subpixel_order::NONE));
    stored.setValue(QLatin1String(k_appearance_row_timestamp_tooltip), false);
    stored.setValue(QLatin1String(k_appearance_scrollback_limit), 25000);
    stored.endGroup();
    stored.beginGroup(QLatin1String(k_window_settings_group));
    stored.setValue(QLatin1String(k_window_settings_font_size), 14.0);
    stored.setValue(QLatin1String(k_window_settings_width),  1024);
    stored.setValue(QLatin1String(k_window_settings_height), 720);
    stored.setValue(QLatin1String(k_window_settings_maximized), true);
    stored.endGroup();
    stored.sync();

    // A run started with every appearance setting forced on the command line.
    App_options options;
    options.color_scheme                   = QStringLiteral("Campbell");
    options.color_scheme_explicit          = true;
    options.font_family                    = QStringLiteral("Consolas");
    options.font_family_explicit           = true;
    options.font_size                      = 30.0;
    options.font_size_explicit             = true;
    options.text_renderer_mode             = VNM_TerminalSurface::Text_renderer_mode::GLYPH;
    options.text_renderer_mode_explicit    = true;
    options.lcd_subpixel_order             = VNM_TerminalSurface::Lcd_subpixel_order::RGB;
    options.lcd_subpixel_order_explicit    = true;
    options.row_timestamp_tooltip_enabled  = true;
    options.row_timestamp_tooltip_explicit = true;
    options.scrollback_limit               = 4000;
    options.scrollback_limit_explicit      = true;
    options.window_size                    = QSize(640, 480);
    options.window_size_explicit           = true;

    VNM_TerminalSurface surface;
    surface.set_color_scheme(options.color_scheme);
    surface.set_font_family(options.font_family);
    surface.set_font_size(options.font_size);
    surface.set_text_renderer_mode(options.text_renderer_mode);
    surface.set_lcd_subpixel_order(options.lcd_subpixel_order);
    surface.set_row_timestamp_tooltip_enabled(options.row_timestamp_tooltip_enabled);
    surface.set_scrollback_limit(*options.scrollback_limit);

    Command_line_setting_overrides overrides =
        command_line_setting_overrides(options, surface);

    Persisted_terminal_window_state window_state;
    window_state.font_size = surface.font_size();
    window_state.size      = options.window_size;

    QSettings writer(path, QSettings::IniFormat);
    save_persisted_appearance_settings(writer, surface, overrides);
    save_persisted_terminal_window_state(writer, window_state, overrides);

    QSettings reader(path, QSettings::IniFormat);
    const Persisted_appearance_settings appearance =
        load_persisted_appearance_settings(reader);
    const Persisted_terminal_window_state window =
        load_persisted_terminal_window_state(reader);

    ok &= check(appearance.color_scheme.value_or(QString()) == QStringLiteral("Solarized Dark"),
        "an explicit color scheme leaves the stored scheme alone");
    ok &= check(appearance.font_family.value_or(QString()) == QStringLiteral("Cascadia Mono"),
        "an explicit font family leaves the stored family alone");
    ok &= check(
        appearance.text_renderer_mode.value_or(-1) ==
            static_cast<int>(VNM_TerminalSurface::Text_renderer_mode::AUTO),
        "an explicit renderer mode leaves the stored mode alone");
    ok &= check(
        appearance.lcd_subpixel_order.value_or(-1) ==
            static_cast<int>(VNM_TerminalSurface::Lcd_subpixel_order::NONE),
        "an explicit lcd subpixel order leaves the stored order alone");
    ok &= check(appearance.row_timestamp_tooltip.has_value() && !*appearance.row_timestamp_tooltip,
        "an explicit row timestamp flag leaves the stored toggle alone");
    ok &= check(appearance.scrollback_limit.value_or(-1) == 25000,
        "an explicit scrollback limit leaves the stored limit alone");
    ok &= check_optional_font_size(window.font_size, 14.0,
        "stored font size under an explicit font size");
    ok &= check_optional_size(window.size, QSize(1024, 720),
        "stored window size under an explicit window size");
    // An explicit window size also forces the run out of the maximized state,
    // so the run's own unmaximized window must not write that back either.
    ok &= check(window.maximized,
        "an explicit window size leaves the stored maximized state alone");

    // Moving a forced setting during the session is the user's own choice and
    // must reach their stored preferences.
    surface.set_color_scheme(QStringLiteral("Solarized Light"));
    surface.set_font_size(22.0);
    surface.set_row_timestamp_tooltip_enabled(false);
    surface.set_text_renderer_mode(VNM_TerminalSurface::Text_renderer_mode::AUTO);
    surface.set_lcd_subpixel_order(VNM_TerminalSurface::Lcd_subpixel_order::BGR);
    surface.set_scrollback_limit(9000);

    Persisted_terminal_window_state changed_window_state;
    changed_window_state.font_size = surface.font_size();
    changed_window_state.size      = QSize(1280, 800);
    changed_window_state.maximized = true;
    save_persisted_appearance_settings(writer, surface, overrides);
    save_persisted_terminal_window_state(writer, changed_window_state, overrides);

    QSettings changed_reader(path, QSettings::IniFormat);
    const Persisted_appearance_settings changed_appearance =
        load_persisted_appearance_settings(changed_reader);
    const Persisted_terminal_window_state changed_window =
        load_persisted_terminal_window_state(changed_reader);

    ok &= check(
        changed_appearance.color_scheme.value_or(QString()) == QStringLiteral("Solarized Light"),
        "a scheme chosen during the session replaces the stored scheme");
    ok &= check_optional_font_size(changed_window.font_size, 22.0,
        "a font size chosen during the session replaces the stored size");
    ok &= check_optional_size(changed_window.size, QSize(1280, 800),
        "a window size chosen during the session replaces the stored size");
    ok &= check(
        changed_appearance.text_renderer_mode.value_or(-1) ==
            static_cast<int>(VNM_TerminalSurface::Text_renderer_mode::AUTO),
        "a renderer mode chosen during the session replaces the stored mode");
    ok &= check(
        changed_appearance.lcd_subpixel_order.value_or(-1) ==
            static_cast<int>(VNM_TerminalSurface::Lcd_subpixel_order::BGR),
        "a subpixel order chosen during the session replaces the stored order");
    ok &= check(changed_appearance.scrollback_limit.value_or(-1) == 9000,
        "a scrollback limit chosen during the session replaces the stored limit");
    ok &= check(
        changed_appearance.font_family.value_or(QString()) == QStringLiteral("Cascadia Mono"),
        "changing one setting does not release the other forced values");

    // Returning a setting to the value the command line forced is still the
    // user's choice. Suppression that never lifts would make every two-valued
    // setting unable to store half its states for the rest of the session.
    surface.set_color_scheme(QStringLiteral("Campbell"));
    surface.set_font_size(30.0);
    surface.set_row_timestamp_tooltip_enabled(true);
    surface.set_text_renderer_mode(VNM_TerminalSurface::Text_renderer_mode::GLYPH);
    surface.set_lcd_subpixel_order(VNM_TerminalSurface::Lcd_subpixel_order::RGB);
    surface.set_scrollback_limit(4000);

    Persisted_terminal_window_state restored_window_state;
    restored_window_state.font_size = surface.font_size();
    restored_window_state.size      = QSize(640, 480);
    restored_window_state.maximized = false;
    save_persisted_appearance_settings(writer, surface, overrides);
    save_persisted_terminal_window_state(writer, restored_window_state, overrides);

    QSettings restored_reader(path, QSettings::IniFormat);
    const Persisted_appearance_settings restored_appearance =
        load_persisted_appearance_settings(restored_reader);
    const Persisted_terminal_window_state restored_window =
        load_persisted_terminal_window_state(restored_reader);

    ok &= check(
        restored_appearance.color_scheme.value_or(QString()) == QStringLiteral("Campbell"),
        "reselecting the forced scheme stores it");
    ok &= check_optional_font_size(restored_window.font_size, 30.0,
        "returning to the forced font size stores it");
    ok &= check_optional_size(restored_window.size, QSize(640, 480),
        "returning to the forced window size stores it");
    ok &= check(
        restored_appearance.row_timestamp_tooltip.value_or(false),
        "toggling a forced flag off and back on stores the final choice");
    ok &= check(
        restored_appearance.text_renderer_mode.value_or(-1) ==
            static_cast<int>(VNM_TerminalSurface::Text_renderer_mode::GLYPH),
        "returning to the forced renderer mode stores it");
    ok &= check(
        restored_appearance.lcd_subpixel_order.value_or(-1) ==
            static_cast<int>(VNM_TerminalSurface::Lcd_subpixel_order::RGB),
        "returning to the forced subpixel order stores it");
    ok &= check(restored_appearance.scrollback_limit.value_or(-1) == 4000,
        "returning to the forced scrollback limit stores it");
    // Maximizing during the run released the forced state, so unmaximizing
    // afterwards is an ordinary choice and reaches the stored preference.
    ok &= check(!restored_window.maximized,
        "unmaximizing after maximizing during the session stores it");
    ok &= check(
        restored_appearance.font_family.value_or(QString()) == QStringLiteral("Cascadia Mono"),
        "a forced setting the user never moved is still not written back");
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
    ok &= test_platform_adjusted_command_line_geometry_does_not_replace_stored_geometry();
    ok &= test_command_line_overrides_do_not_replace_stored_settings();
    ok &= test_chrome_palette_settings();
    ok &= test_interaction_settings_round_trip();
    return ok ? 0 : 1;
}
