#include "vnm_terminal/app_support/app_settings.h"
#include "vnm_terminal/app_support/terminal_settings_controller.h"

#include "vnm_terminal/vnm_terminal_surface.h"

#include <QGuiApplication>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

namespace terminal_app = vnm_terminal::terminal_app;

class App_support_tests final : public QObject
{
    Q_OBJECT

private slots:
    void snapshot_round_trips()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QSettings settings(
            directory.filePath(QStringLiteral("terminal.ini")),
            QSettings::IniFormat);

        terminal_app::Terminal_settings_snapshot expected;
        expected.color_scheme = QStringLiteral("Solarized Light");
        expected.font_family  = QStringLiteral("Cascadia Mono");
        expected.font_size    = 18.0;
        expected.text_renderer_mode =
            static_cast<int>(VNM_TerminalSurface::Text_renderer_mode::GLYPH);
        expected.lcd_subpixel_order =
            static_cast<int>(VNM_TerminalSurface::Lcd_subpixel_order::BGR);
        expected.row_timestamp_tooltip_enabled = false;
        expected.scrollback_limit = 12'345;

        terminal_app::save_terminal_settings_snapshot(settings, expected);
        const terminal_app::Terminal_settings_snapshot actual =
            terminal_app::load_terminal_settings_snapshot(settings);

        QCOMPARE(actual.color_scheme, expected.color_scheme);
        QCOMPARE(actual.font_family, expected.font_family);
        QCOMPARE(actual.font_size, expected.font_size);
        QCOMPARE(actual.text_renderer_mode, expected.text_renderer_mode);
        QCOMPARE(actual.lcd_subpixel_order, expected.lcd_subpixel_order);
        QCOMPARE(
            actual.row_timestamp_tooltip_enabled,
            expected.row_timestamp_tooltip_enabled);
        QCOMPARE(actual.scrollback_limit, expected.scrollback_limit);
    }

    void invalid_settings_keep_neutral_defaults()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QSettings settings(
            directory.filePath(QStringLiteral("terminal.ini")),
            QSettings::IniFormat);

        settings.setValue(QStringLiteral("window/font_size"), QStringLiteral("nope"));
        settings.setValue(QStringLiteral("appearance/color_scheme"), QStringLiteral("missing"));
        settings.setValue(QStringLiteral("appearance/font_family"), QStringLiteral("  "));
        settings.setValue(QStringLiteral("appearance/text_renderer_mode"), 999);
        settings.setValue(QStringLiteral("appearance/lcd_subpixel_order"), -1);
        settings.setValue(QStringLiteral("appearance/scrollback_limit"), -2);

        const terminal_app::Terminal_settings_snapshot defaults;
        const terminal_app::Terminal_settings_snapshot actual =
            terminal_app::load_terminal_settings_snapshot(settings);
        QCOMPARE(actual.color_scheme, defaults.color_scheme);
        QCOMPARE(actual.font_family, defaults.font_family);
        QCOMPARE(actual.font_size, defaults.font_size);
        QCOMPARE(actual.text_renderer_mode, defaults.text_renderer_mode);
        QCOMPARE(actual.lcd_subpixel_order, defaults.lcd_subpixel_order);
        QCOMPARE(actual.scrollback_limit, defaults.scrollback_limit);
    }

    void transient_msdf_does_not_replace_the_durable_renderer_on_save()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QSettings settings(
            directory.filePath(QStringLiteral("terminal.ini")),
            QSettings::IniFormat);

        terminal_app::Terminal_settings_snapshot durable;
        durable.text_renderer_mode =
            static_cast<int>(VNM_TerminalSurface::Text_renderer_mode::GLYPH);
        terminal_app::save_terminal_settings_snapshot(settings, durable);

        VNM_TerminalSurface surface;
        surface.set_text_renderer_mode(
            VNM_TerminalSurface::Text_renderer_mode::MSDF);
        surface.set_color_scheme(QStringLiteral("Solarized Light"));
        terminal_app::save_terminal_settings_snapshot(
            settings,
            terminal_app::terminal_settings_snapshot(surface));

        QCOMPARE(
            settings.value(QStringLiteral("appearance/text_renderer_mode")).toInt(),
            static_cast<int>(VNM_TerminalSurface::Text_renderer_mode::GLYPH));
        QCOMPARE(
            settings.value(QStringLiteral("appearance/color_scheme")).toString(),
            QStringLiteral("Solarized Light"));
        QCOMPARE(
            terminal_app::load_terminal_settings_snapshot(settings)
                .text_renderer_mode,
            static_cast<int>(VNM_TerminalSurface::Text_renderer_mode::GLYPH));
    }

    void snapshot_applies_to_surface()
    {
        terminal_app::Terminal_settings_snapshot snapshot;
        snapshot.color_scheme = QStringLiteral("Solarized Light");
        snapshot.font_size    = 20.0;
        snapshot.scrollback_limit = 512;

        VNM_TerminalSurface surface;
        terminal_app::apply_terminal_settings_snapshot(snapshot, surface);
        QCOMPARE(surface.color_scheme(), snapshot.color_scheme);
        QCOMPARE(surface.font_size(), snapshot.font_size);
        QCOMPARE(surface.scrollback_limit(), *snapshot.scrollback_limit);
    }

    void settings_controller_is_reusable()
    {
        terminal_app::Terminal_settings_controller controller;
        QVERIFY(!controller.available_font_families().isEmpty());
    }
};

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    App_support_tests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "app_support_tests.moc"
