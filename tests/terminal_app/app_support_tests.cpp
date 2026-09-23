#include "vnm_terminal/app_support/app_settings.h"
#include "vnm_terminal/app_support/terminal_display_settings.h"
#include "vnm_terminal/app_support/terminal_settings_controller.h"

#include "terminal_file_drop.h"
#include "vnm_terminal/vnm_terminal_surface.h"

#include <QCoreApplication>
#include <QDragEnterEvent>
#include <QGuiApplication>
#include <QJsonObject>
#include <QJsonValue>
#include <QMimeData>
#include <QPoint>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

#include <limits>

namespace terminal_app = vnm_terminal::terminal_app;

class App_support_tests final : public QObject
{
    Q_OBJECT

private slots:
    void terminal_drop_paths_are_quoted_for_known_shells()
    {
        const QStringList posix_command{QStringLiteral("/bin/bash")};
        const std::optional<QString> posix_text = terminal_app::quote_terminal_paths(
            {
                QStringLiteral("/tmp/space name"),
                QStringLiteral("/tmp/it's quoted"),
            },
            posix_command);
        QVERIFY(posix_text.has_value());
        QCOMPARE(
            *posix_text,
            QStringLiteral("'/tmp/space name' '/tmp/it'\\''s quoted'"));

        const std::optional<QString> powershell_text = terminal_app::quote_terminal_paths(
            {QStringLiteral("C:/Users/Ada's files/read me.txt")},
            {QStringLiteral("C:/Program Files/PowerShell/7/pwsh.exe")});
        QVERIFY(powershell_text.has_value());
        QCOMPARE(
            *powershell_text,
            QStringLiteral("'C:/Users/Ada''s files/read me.txt'"));

        const QString right_smart_quote_path =
            QStringLiteral("C:/Users/Ada") + QChar(0x2019) + QStringLiteral("s file.txt");
        const std::optional<QString> smart_quote_text = terminal_app::quote_terminal_paths(
            {right_smart_quote_path},
            {QStringLiteral("pwsh")});
        QVERIFY(smart_quote_text.has_value());
        QCOMPARE(
            *smart_quote_text,
            QStringLiteral("'C:/Users/Ada") + QChar(0x2019) + QChar(0x2019) +
                QStringLiteral("s file.txt'"));

        const std::optional<QString> cmd_text = terminal_app::quote_terminal_paths(
            {QStringLiteral("C:/Program Files/notes.txt")},
            {QStringLiteral("C:/Windows/System32/cmd.exe")});
        QVERIFY(cmd_text.has_value());
        QCOMPARE(*cmd_text, QStringLiteral("\"C:/Program Files/notes.txt\""));
    }

    void terminal_drop_paths_reject_unsafe_inputs()
    {
        const QStringList posix_command{QStringLiteral("/bin/sh")};
        QVERIFY(!terminal_app::quote_terminal_paths({}, posix_command).has_value());
        QVERIFY(!terminal_app::quote_terminal_paths(
            {QStringLiteral("/tmp/line\nbreak")},
            posix_command).has_value());
        QVERIFY(!terminal_app::quote_terminal_paths(
            {QStringLiteral("/tmp/paragraph") + QChar(0x2029)},
            posix_command).has_value());
        QVERIFY(!terminal_app::quote_terminal_paths(
            {QStringLiteral("/tmp/file")},
            {QStringLiteral("/usr/bin/custom-shell")}).has_value());
        QVERIFY(!terminal_app::quote_terminal_paths(
            {QStringLiteral("C:/Users/Ada/100% done.txt")},
            {QStringLiteral("cmd.exe")}).has_value());
        QVERIFY(!terminal_app::quote_terminal_paths(
            {QStringLiteral("C:/Users/Ada/important! file.txt")},
            {QStringLiteral("cmd.exe")}).has_value());
        QVERIFY(!terminal_app::quote_terminal_paths(
            {QStringLiteral("C:/Users/Ada/file.txt")},
            {QStringLiteral("cmd.exe"), QStringLiteral("/v:on")}).has_value());
    }

    void terminal_drop_accepts_only_local_urls()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString local_path = directory.filePath(QStringLiteral("a file.txt"));
        const QList<QUrl> local_urls{
            QUrl::fromLocalFile(local_path),
        };
        const std::optional<QString> text =
            terminal_app::terminal_drop_text_for_local_urls(
                local_urls,
                {QStringLiteral("/bin/sh")});
        QVERIFY(text.has_value());
        QVERIFY(text->startsWith(QLatin1Char('\'')));
        QVERIFY(!text->contains(QLatin1Char('\n')));
        QVERIFY(!text->contains(QLatin1Char('\r')));

        const QList<QUrl> mixed_urls{
            QUrl::fromLocalFile(local_path),
            QUrl(QStringLiteral("https://example.com/file.txt")),
        };
        QVERIFY(!terminal_app::terminal_drop_text_for_local_urls(
            mixed_urls,
            {QStringLiteral("/bin/sh")}).has_value());

        QVERIFY(!terminal_app::terminal_drop_text_for_local_urls(
            {QUrl(QStringLiteral("file://server/share/file.txt"))},
            {QStringLiteral("/bin/sh")}).has_value());
        QVERIFY(!terminal_app::terminal_drop_text_for_local_urls(
            {QUrl(QStringLiteral("file:relative.txt"))},
            {QStringLiteral("/bin/sh")}).has_value());
    }

    void terminal_drop_drag_enter_is_routed_to_surface_filter()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        VNM_TerminalSurface surface;
        terminal_app::install_terminal_file_drop(surface, {QStringLiteral("/bin/sh")});

        QMimeData mime_data;
        mime_data.setUrls({
            QUrl::fromLocalFile(directory.filePath(QStringLiteral("file.txt"))),
        });
        QDragEnterEvent event(
            QPoint(0, 0),
            Qt::CopyAction,
            &mime_data,
            Qt::LeftButton,
            Qt::NoModifier);
        event.ignore();

        QVERIFY(QCoreApplication::sendEvent(&surface, &event));
        QVERIFY(event.isAccepted());
        QCOMPARE(event.dropAction(), Qt::CopyAction);
    }

    void shared_display_settings_preserve_font_across_modes_and_restart()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QSettings store(directory.filePath(QStringLiteral("shared-display.ini")), QSettings::IniFormat);
        terminal_app::Terminal_display_settings display(true, &store);
        const QVariantMap changes{
            {QStringLiteral("font_family"), QStringLiteral("Cascadia Mono")},
            {QStringLiteral("font_size"), 27},
            {QStringLiteral("font_advance_policy"),
                static_cast<int>(vnm_terminal::Font_advance_policy::SNAP_ADVANCE_UP)},
            {QStringLiteral("text_renderer_mode"),
                static_cast<int>(VNM_TerminalSurface::Text_renderer_mode::GLYPH)},
            {QStringLiteral("lcd_subpixel_order"),
                static_cast<int>(VNM_TerminalSurface::Lcd_subpixel_order::BGR)},
            {QStringLiteral("invert_brightness"), true},
            {QStringLiteral("row_timestamp_tooltip_enabled"), false},
            {QStringLiteral("scrollback_buffer_size_mib"), 32},
        };
        QVERIFY(display.apply_changes(changes, true));
        QVERIFY(display.apply_changes({{QStringLiteral("color_scheme"), QStringLiteral("Campbell")}}, true));
        const QVariantMap dark_values = display.values();
        QVERIFY(display.set_dark_mode(false));
        for (auto it = changes.cbegin(); it != changes.cend(); ++it) {
            // Inversion is stored independently for each lighting mode.
            if (it.key() == QStringLiteral("invert_brightness")) {
                QCOMPARE(display.values().value(it.key()), QVariant(false));
                continue;
            }
            QCOMPARE(display.values().value(it.key()), it.value());
        }
        QVERIFY(display.values().value(QStringLiteral("color_scheme")) !=
            dark_values.value(QStringLiteral("color_scheme")));
        QVERIFY(display.apply_changes(
            {{QStringLiteral("color_scheme"), QStringLiteral("One Half Light")}}, false));
        terminal_app::Terminal_display_settings restarted(false, &store);
        QCOMPARE(restarted.values(), display.values());
        QVERIFY(restarted.set_dark_mode(true));
        QCOMPARE(restarted.values(), dark_values);
    }

    void deltas_preserve_other_terminal_edits_and_palette_mode()
    {
        terminal_app::Terminal_display_settings display(true);
        QVERIFY(display.apply_changes({{QStringLiteral("font_size"), 25}}, true));
        QVERIFY(display.apply_changes(
            {{QStringLiteral("font_family"), QStringLiteral("Cascadia Mono")}}, true));
        QCOMPARE(display.values().value(QStringLiteral("font_size")).toInt(), 25);
        QVERIFY(display.set_dark_mode(false));
        const QVariant light_scheme = display.values().value(QStringLiteral("color_scheme"));
        // A palette edit in flight from the dark worker still belongs to dark mode.
        QVERIFY(display.apply_changes({{QStringLiteral("color_scheme"), QStringLiteral("Campbell")}}, true));
        QCOMPARE(display.values().value(QStringLiteral("color_scheme")), light_scheme);
        QVERIFY(display.set_dark_mode(true));
        QCOMPARE(display.values().value(QStringLiteral("color_scheme")).toString(), QStringLiteral("Campbell"));
        QCOMPARE(display.values().value(QStringLiteral("font_size")).toInt(), 25);
    }

    void invalid_shared_delta_does_not_replace_valid_display_settings()
    {
        terminal_app::Terminal_display_settings display(true);
        const QVariantMap previous = display.values();
        QVERIFY(!display.apply_changes({
            {QStringLiteral("font_family"), QStringLiteral("Cascadia Mono")},
            {QStringLiteral("font_size"), -8}}, true));
        QCOMPARE(display.values(), previous);
    }

    void wire_validation_data()
    {
        QTest::addColumn<QString>("key");
        QTest::addColumn<QVariant>("value");
        QTest::addColumn<bool>("delta_valid");
        QTest::addColumn<bool>("snapshot_valid");
        const QString font      = QStringLiteral("font_family");
        const QString palette   = QStringLiteral("color_scheme");
        const QString tooltip   = QStringLiteral("row_timestamp_tooltip_enabled");
        const QString size      = QStringLiteral("font_size");
        const QString advance   = QStringLiteral("font_advance_policy");
        const QString renderer  = QStringLiteral("text_renderer_mode");
        const QString lcd       = QStringLiteral("lcd_subpixel_order");
        const QString invert    = QStringLiteral("invert_brightness");
        const QString scrollback = QStringLiteral("scrollback_buffer_size_mib");

        QTest::newRow("font")          << font << QVariant("Cascadia Mono") << true << true;
        QTest::newRow("empty-font")    << font << QVariant("")              << false << true;
        QTest::newRow("blank-font")    << font << QVariant("  ")            << false << true;
        QTest::newRow("font-type")     << font << QVariant(17)              << false << false;
        QTest::newRow("font-nul")      << font
            << QVariant(QStringLiteral("Cascadia") + QChar(u'\0') + QStringLiteral("Mono")) << false << false;
        QTest::newRow("palette")       << palette << QVariant("Campbell") << true << true;
        QTest::newRow("palette-name")  << palette << QVariant("Missing")  << false << true;
        QTest::newRow("palette-empty") << palette << QVariant("")         << false << false;
        QTest::newRow("palette-nul")   << palette
            << QVariant(QStringLiteral("Classic") + QChar(u'\0')) << false << false;
        QTest::newRow("bool")          << tooltip << QVariant(false) << true << true;
        QTest::newRow("bool-number")   << tooltip << QVariant(1)     << false << false;
        QTest::newRow("invert-bool")   << invert << QVariant(true)  << true << true;
        QTest::newRow("invert-number") << invert << QVariant(1)     << false << false;
        QTest::newRow("unknown-key")   << QStringLiteral("unknown") << QVariant(1) << false << false;
        QTest::newRow("null")          << size << QVariant() << false << false;
        QTest::newRow("size-min")      << size << QVariant(6.0)  << true << true;
        QTest::newRow("size-max")      << size << QVariant(72.0) << true << true;
        QTest::newRow("size-fraction") << size << QVariant(16.5) << true << true;
        QTest::newRow("size-low")      << size << QVariant(5.9)  << false << false;
        QTest::newRow("size-high")     << size << QVariant(72.1) << false << false;
        QTest::newRow("size-text")     << size << QVariant("16") << false << false;
        QTest::newRow("size-bool")     << size << QVariant(true) << false << false;
        QTest::newRow("size-infinite") << size
            << QVariant(std::numeric_limits<double>::infinity()) << false << false;
        QTest::newRow("advance-min")   << advance << QVariant(0) << true << true;
        QTest::newRow("advance-max")   << advance << QVariant(2) << true << true;
        QTest::newRow("advance-low")   << advance << QVariant(-1) << false << false;
        QTest::newRow("advance-high")  << advance << QVariant(3) << false << false;
        QTest::newRow("advance-frac")  << advance << QVariant(1.5) << false << false;
        QTest::newRow("renderer-min")  << renderer << QVariant(0) << true << true;
        QTest::newRow("renderer-max")  << renderer << QVariant(2) << true << true;
        QTest::newRow("renderer-high") << renderer << QVariant(3) << false << false;
        QTest::newRow("renderer-frac") << renderer << QVariant(1.5) << false << false;
        QTest::newRow("lcd-min")       << lcd << QVariant(0) << true << true;
        QTest::newRow("lcd-max")       << lcd << QVariant(5) << true << true;
        QTest::newRow("lcd-low")       << lcd << QVariant(-1) << false << false;
        QTest::newRow("lcd-high")      << lcd << QVariant(6) << false << false;
        QTest::newRow("scroll-min")    << scrollback << QVariant(1) << true << true;
        QTest::newRow("scroll-max")    << scrollback << QVariant(64) << true << true;
        QTest::newRow("scroll-high")   << scrollback << QVariant(65) << false << false;
        QTest::newRow("scroll-frac")   << scrollback << QVariant(1.5) << false << false;
    }

    void wire_validation()
    {
        QFETCH(QString, key);
        QFETCH(QVariant, value);
        QFETCH(bool, delta_valid);
        QFETCH(bool, snapshot_valid);
        const QVariantMap changes{{key, value}};
        QCOMPARE(terminal_app::terminal_settings_changes_valid(changes), delta_valid);
        const auto snapshot = terminal_app::terminal_settings_from_json(
            QJsonObject::fromVariantMap(changes), {});
        QCOMPARE(snapshot.has_value(), snapshot_valid);
    }

    void snapshots_preserve_omitted_values_and_reject_invalid_envelopes()
    {
        terminal_app::Terminal_settings_snapshot base;
        base.font_size = 23.5;
        const auto defaults = terminal_app::terminal_settings_from_json(QJsonValue::Undefined, base);
        QVERIFY(defaults);
        QCOMPARE(defaults->font_size, base.font_size);
        QVERIFY(!defaults->scrollback_buffer_size_mib);
        const auto empty = terminal_app::terminal_settings_from_json(QJsonObject{}, base);
        QVERIFY(empty);
        QCOMPARE(empty->font_family, base.font_family);
        QVERIFY(!empty->scrollback_buffer_size_mib);
        base.scrollback_buffer_size_mib = 32;
        const auto partial = terminal_app::terminal_settings_from_json(
            QJsonObject{{QStringLiteral("font_size"), 28}}, base);
        QVERIFY(partial);
        QCOMPARE(partial->font_size, 28.0);
        QCOMPARE(partial->scrollback_buffer_size_mib, base.scrollback_buffer_size_mib);
        for (const QJsonValue value : {QJsonValue(), QJsonValue(false), QJsonValue(17), QJsonValue("font")}) {
            QVERIFY(!terminal_app::terminal_settings_from_json(value, base));
        }
        QVERIFY(terminal_app::terminal_settings_changes_valid({}));
    }

    void payload_round_trip_preserves_all_appearance_values()
    {
        terminal_app::Terminal_settings_snapshot expected;
        expected.font_family = QStringLiteral("Cascadia Mono");
        expected.color_scheme = QStringLiteral("Campbell");
        expected.font_size = 23.5;
        expected.font_advance_policy =
            static_cast<int>(vnm_terminal::Font_advance_policy::SNAP_ADVANCE_NEAREST);
        expected.text_renderer_mode = 2;
        expected.lcd_subpixel_order = 5;
        expected.invert_brightness = true;
        expected.row_timestamp_tooltip_enabled = false;
        const auto without_scrollback = terminal_app::terminal_settings_payload(expected);
        QVERIFY(!without_scrollback.contains(QStringLiteral("scrollback_buffer_size_mib")));
        expected.scrollback_buffer_size_mib = 32;
        const auto payload = terminal_app::terminal_settings_payload(expected);
        const auto actual = terminal_app::terminal_settings_from_json(QJsonObject::fromVariantMap(payload), {});
        QVERIFY(actual);
        QCOMPARE(terminal_app::terminal_settings_payload(*actual), payload);
    }

    void rejected_nul_edit_preserves_shared_state_and_store()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QSettings store(directory.filePath(QStringLiteral("appearance.ini")), QSettings::IniFormat);
        terminal_app::Terminal_display_settings display(true, &store);
        QVERIFY(display.apply_changes({{QStringLiteral("font_family"), QStringLiteral("Cascadia Mono")}}, true));
        const QVariantMap previous = display.values();
        const QString nul_font = QStringLiteral("Cascadia") + QChar(u'\0') + QStringLiteral("Mono");
        QVERIFY(!display.apply_changes({{QStringLiteral("font_family"), nul_font}}, true));
        QCOMPARE(display.values(), previous);
        QCOMPARE(store.value(QStringLiteral("appearance/font_family")),
            previous.value(QStringLiteral("font_family")));
        QVERIFY(terminal_app::terminal_settings_from_json(QJsonObject::fromVariantMap(display.values()), {}));
    }

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
        expected.font_advance_policy =
            static_cast<int>(vnm_terminal::Font_advance_policy::SNAP_ADVANCE_UP);
        expected.text_renderer_mode =
            static_cast<int>(VNM_TerminalSurface::Text_renderer_mode::GLYPH);
        expected.lcd_subpixel_order =
            static_cast<int>(VNM_TerminalSurface::Lcd_subpixel_order::BGR);
        expected.invert_brightness = true;
        expected.row_timestamp_tooltip_enabled = false;
        expected.scrollback_buffer_size_mib = 32;

        terminal_app::save_terminal_settings_snapshot(settings, expected);
        const terminal_app::Terminal_settings_snapshot actual =
            terminal_app::load_terminal_settings_snapshot(settings);

        QCOMPARE(actual.color_scheme, expected.color_scheme);
        QCOMPARE(actual.font_family, expected.font_family);
        QCOMPARE(actual.font_size, expected.font_size);
        QCOMPARE(actual.font_advance_policy, expected.font_advance_policy);
        QCOMPARE(actual.text_renderer_mode, expected.text_renderer_mode);
        QCOMPARE(actual.lcd_subpixel_order, expected.lcd_subpixel_order);
        QCOMPARE(actual.invert_brightness, expected.invert_brightness);
        QCOMPARE(
            actual.row_timestamp_tooltip_enabled,
            expected.row_timestamp_tooltip_enabled);
        QCOMPARE(actual.scrollback_buffer_size_mib, expected.scrollback_buffer_size_mib);
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
        settings.setValue(QStringLiteral("appearance/font_advance_policy"), 999);
        settings.setValue(QStringLiteral("appearance/lcd_subpixel_order"), -1);
        settings.setValue(QStringLiteral("appearance/scrollback_buffer_size_mib"), 0);

        const terminal_app::Terminal_settings_snapshot defaults;
        const terminal_app::Terminal_settings_snapshot actual =
            terminal_app::load_terminal_settings_snapshot(settings);
        QCOMPARE(actual.color_scheme, defaults.color_scheme);
        QCOMPARE(actual.font_family, defaults.font_family);
        QCOMPARE(actual.font_size, defaults.font_size);
        QCOMPARE(actual.font_advance_policy, defaults.font_advance_policy);
        QCOMPARE(actual.text_renderer_mode, defaults.text_renderer_mode);
        QCOMPARE(actual.lcd_subpixel_order, defaults.lcd_subpixel_order);
        QCOMPARE(actual.scrollback_buffer_size_mib, defaults.scrollback_buffer_size_mib);
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
        snapshot.font_advance_policy =
            static_cast<int>(vnm_terminal::Font_advance_policy::SNAP_ADVANCE_NEAREST);
        snapshot.invert_brightness = true;
        snapshot.scrollback_buffer_size_mib = 16;

        VNM_TerminalSurface surface;
        terminal_app::apply_terminal_settings_snapshot(snapshot, surface);
        QCOMPARE(surface.color_scheme(), snapshot.color_scheme);
        QCOMPARE(surface.font_size(), snapshot.font_size);
        QCOMPARE(surface.font_advance_policy_value(), snapshot.font_advance_policy);
        QCOMPARE(surface.invert_brightness(), snapshot.invert_brightness);
        QCOMPARE(surface.scrollback_buffer_size_mib(), *snapshot.scrollback_buffer_size_mib);
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
