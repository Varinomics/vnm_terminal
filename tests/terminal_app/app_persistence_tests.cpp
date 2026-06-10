#define VNM_TERMINAL_APP_NO_MAIN
#include "../../src/main.cpp"
#undef VNM_TERMINAL_APP_NO_MAIN

#include "helpers/test_check.h"

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

}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);

    bool ok = true;
    ok &= test_save_and_load_window_state();
    ok &= test_apply_persisted_window_state();
    ok &= test_invalid_persisted_values_are_ignored();
    ok &= test_appearance_settings_round_trip();
    return ok ? 0 : 1;
}
