#include "vnm_terminal/app_support/app_settings.h"
#include "vnm_terminal/app_support/backend_output_capture_json.h"
#include "vnm_terminal/app_support/terminal_display_settings.h"
#include "vnm_terminal/app_support/terminal_settings_reconciler.h"
#include "vnm_terminal/app_support/terminal_settings_controller.h"
#include "vnm_terminal/app_support/terminal_settings_dialog.h"
#include "vnm_terminal/app_support/terminal_settings_model.h"
#include "vnm_terminal/app_support/terminal_settings_window.h"
#include "vnm_terminal/app_support/terminal_search_bar.h"

#include "terminal_file_drop.h"
#include "vnm_terminal/vnm_terminal_surface.h"

#include <vnm_font_namespace.h>

#include <QCoreApplication>
#include <QDragEnterEvent>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QGuiApplication>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMimeData>
#include <QMetaType>
#include <QPoint>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickWindow>
#include <QQuickItem>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <qqml.h>

#include <cstddef>
#include <limits>
#include <memory>

namespace terminal_app = vnm_terminal::terminal_app;

class App_support_tests final : public QObject
{
    Q_OBJECT

    static QQuickWindow* settings_window()
    {
        for (QWindow* candidate : QGuiApplication::topLevelWindows()) {
            if (candidate->objectName() == QStringLiteral("terminal_settings_window")) {
                return qobject_cast<QQuickWindow*>(candidate);
            }
        }
        return nullptr;
    }

private slots:
    void font_picker_initializes_and_prioritizes_the_shipped_default()
    {
        terminal_app::Terminal_settings_controller controller;
        const QStringList families = controller.available_font_families();
        const QString expected = QStringLiteral("Ubuntu Sans Mono derivative vnm");
        QVERIFY(!families.isEmpty());
        QCOMPARE(families.front(), expected);
        QCOMPARE(families.count(expected), 1);
        QCOMPARE(QFontInfo(QFont(expected)).family(), expected);
        QVERIFY(QFontDatabase::isFixedPitch(expected));
        QVERIFY(QFontDatabase::isSmoothlyScalable(expected));
    }

    void backend_output_capture_json_preserves_integer_precision()
    {
        const vnm_terminal::Backend_output_capture_config config{
            QStringLiteral("C:/diagnostics/session"), 8192U};
        const QJsonObject encoded =
            terminal_app::backend_output_capture_config_to_json(config);
        QCOMPARE(encoded.size(), 2);
        QCOMPARE(encoded.value(QStringLiteral("base_path")).toString(), config.base_path);
        QCOMPARE(encoded.value(QStringLiteral("max_bytes")).toString(),
                 QStringLiteral("8192"));
        QVERIFY(terminal_app::backend_output_capture_config_from_json(encoded) == config);

        if (sizeof(std::size_t) >= sizeof(quint64)) {
            const vnm_terminal::Backend_output_capture_config large{
                QStringLiteral("/var/log/terminal/session"),
                static_cast<std::size_t>(9007199254740993ULL)};
            const QJsonObject large_json =
                terminal_app::backend_output_capture_config_to_json(large);
            QCOMPARE(large_json.value(QStringLiteral("max_bytes")).toString(),
                     QStringLiteral("9007199254740993"));
            QVERIFY(terminal_app::backend_output_capture_config_from_json(large_json) ==
                    large);
        }
    }

    void backend_output_capture_json_rejects_noncanonical_values()
    {
        const QJsonObject canonical =
            terminal_app::backend_output_capture_config_to_json(
                {QStringLiteral("/var/log/terminal/session"), 8192U});
        QList<QJsonObject> invalid;
        invalid.append(QJsonObject{});
        invalid.append(QJsonObject{{QStringLiteral("base_path"), QStringLiteral("x")}});
        invalid.append(QJsonObject{{QStringLiteral("max_bytes"), QStringLiteral("8192")}});

        auto with_value = [&canonical](const QString& key, const QJsonValue& value) {
            QJsonObject object = canonical;
            object.insert(key, value);
            return object;
        };
        invalid.append(with_value(QStringLiteral("base_path"),
                                  QJsonValue(QJsonValue::Null)));
        invalid.append(with_value(QStringLiteral("base_path"), 1));
        invalid.append(with_value(QStringLiteral("base_path"), QString()));
        invalid.append(with_value(QStringLiteral("base_path"),
                                  QStringLiteral("before") + QChar(u'\0') +
                                      QStringLiteral("after")));
        invalid.append(with_value(QStringLiteral("max_bytes"),
                                  QJsonValue(QJsonValue::Null)));
        invalid.append(with_value(QStringLiteral("max_bytes"), 8192));
        for (const QString& text : {
                 QStringLiteral("0"), QStringLiteral("00"),
                 QStringLiteral("08192"), QStringLiteral("+8192"),
                 QStringLiteral("-8192"), QStringLiteral(" 8192"),
                 QStringLiteral("8192 "), QStringLiteral("8.192"),
                 QStringLiteral("18446744073709551616")})
        {
            invalid.append(with_value(QStringLiteral("max_bytes"), text));
        }
        invalid.append(with_value(QStringLiteral("unexpected"), true));
        if (sizeof(std::size_t) < sizeof(quint64)) {
            invalid.append(with_value(QStringLiteral("max_bytes"),
                                      QStringLiteral("4294967296")));
        }

        for (const QJsonObject& object : invalid) {
            QVERIFY(!terminal_app::backend_output_capture_config_from_json(object));
        }
    }

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

    void terminal_drop_paths_escape_every_powershell_quote_delimiter()
    {
        const QString path =
            QStringLiteral("C:/tmp/a") +
            QChar(0x0027) +
            QChar(0x2018) + QChar(0x2019) + QChar(0x201a) + QChar(0x201b) +
            QStringLiteral(";Write-Output DROP_SENTINEL;#");
        const std::optional<QString> text = terminal_app::quote_terminal_paths(
            {path},
            {QStringLiteral("pwsh")});

        QVERIFY(text.has_value());
        const QString expected =
            QStringLiteral("'C:/tmp/a") +
            QChar(0x0027) + QChar(0x0027) +
            QChar(0x2018) + QChar(0x2018) +
            QChar(0x2019) + QChar(0x2019) +
            QChar(0x201a) + QChar(0x201a) +
            QChar(0x201b) + QChar(0x201b) +
            QStringLiteral(";Write-Output DROP_SENTINEL;#'");
        QCOMPARE(*text, expected);
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
            {QStringLiteral("C:/Users/Ada/x\" & echo DROP_SENTINEL & rem \".txt")},
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

        const QString crafted_local_path =
            directory.filePath(QStringLiteral("x\" & echo DROP_SENTINEL & rem \".txt"));
        const QUrl crafted_local_url = QUrl::fromLocalFile(crafted_local_path);
        QVERIFY(crafted_local_url.isLocalFile());
        QVERIFY(crafted_local_url.toString(QUrl::FullyEncoded).contains(QStringLiteral("%22")));
        QCOMPARE(crafted_local_url.toLocalFile(), crafted_local_path);
        QVERIFY(!terminal_app::terminal_drop_text_for_local_urls(
            {crafted_local_url},
            {QStringLiteral("cmd.exe")}).has_value());

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
        const QString font_family = QFontDatabase::families().constFirst();
        const QVariantMap changes{
            {QStringLiteral("font_family"), font_family},
            {QStringLiteral("font_size"), 27},
            {QStringLiteral("font_advance_policy"),
                static_cast<int>(vnm_terminal::Font_advance_policy::SNAP_ADVANCE_UP)},
            {QStringLiteral("text_renderer_mode"),
                static_cast<int>(VNM_TerminalSurface::Text_renderer_mode::GLYPH)},
            {QStringLiteral("lcd_subpixel_order"),
                static_cast<int>(VNM_TerminalSurface::Lcd_subpixel_order::BGR)},
            {QStringLiteral("invert_brightness"), true},
            {QStringLiteral("row_timestamp_tooltip_enabled"), false},
            {QStringLiteral("copy_on_select"), true},
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

    void unavailable_font_preference_survives_unrelated_edits_and_restart_data()
    {
        QTest::addColumn<QVariant>("requested");
        QTest::newRow("missing") << QVariant();
        QTest::newRow("empty") << QVariant(QString());
        QTest::newRow("blank") << QVariant(QStringLiteral("  "));
        QTest::newRow("unavailable") << QVariant(QStringLiteral("VNM Terminal Unavailable Preference Test"));
    }

    void unavailable_font_preference_survives_unrelated_edits_and_restart()
    {
        QFETCH(QVariant, requested);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QSettings store(directory.filePath(QStringLiteral("unavailable-font.ini")), QSettings::IniFormat);
        const QString font_key = QStringLiteral("appearance/font_family");
        QVERIFY(!QFontDatabase::hasFamily(requested.toString()));
        if (requested.isValid()) {
            store.setValue(font_key, requested);
        }
        terminal_app::Terminal_display_settings display(true, &store);
        const QString fallback = vnm_terminal::default_monospace_font_family();
        QCOMPARE(display.values().value(QStringLiteral("font_family")).toString(), fallback);
        QVERIFY(display.apply_changes({{QStringLiteral("font_size"), 22.5}}, true));
        QCOMPARE(store.value(font_key), requested);
        QVERIFY(display.apply_changes(
            {{QStringLiteral("color_scheme"), QStringLiteral("Campbell")}}, true));
        QCOMPARE(store.value(font_key), requested);
        QVERIFY(display.set_dark_mode(false));
        QVERIFY(display.apply_changes({{QStringLiteral("invert_brightness"), true}}, false));
        terminal_app::Terminal_display_settings restarted(false, &store);
        QCOMPARE(restarted.values().value(QStringLiteral("font_family")).toString(), fallback);
        QCOMPARE(restarted.values().value(QStringLiteral("font_size")).toDouble(), 22.5);
        QCOMPARE(store.value(font_key), requested);
        QCOMPARE(store.contains(font_key), requested.isValid());
    }

    void selecting_effective_fallback_replaces_unavailable_preference()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QSettings store(directory.filePath(QStringLiteral("selected-font.ini")), QSettings::IniFormat);
        const QString font_key = QStringLiteral("appearance/font_family");
        store.setValue(font_key, QStringLiteral("VNM Terminal Unavailable Selection Test"));
        terminal_app::Terminal_display_settings display(true, &store);
        const QString fallback = vnm_terminal::default_monospace_font_family();
        QCOMPARE(display.values().value(QStringLiteral("font_family")).toString(), fallback);
        QVERIFY(display.apply_changes({{QStringLiteral("font_family"), fallback}}, true));
        QCOMPARE(store.value(font_key).toString(), fallback);
        QVERIFY(!display.apply_changes({{QStringLiteral("font_family"), fallback}}, true));
        QVERIFY(display.apply_changes({{QStringLiteral("font_family"), QStringLiteral("monospace")}}, true));
        terminal_app::Terminal_display_settings restarted(true, &store);
        QCOMPARE(restarted.values().value(QStringLiteral("font_family")).toString(), QStringLiteral("monospace"));
    }

    void worker_reconciler_keeps_rapid_edits_over_an_old_snapshot()
    {
        VNM_TerminalSurface surface;
        const auto old_snapshot = terminal_app::terminal_settings_snapshot(surface);
        QList<terminal_app::Terminal_setting_delta> deltas;
        terminal_app::Terminal_settings_reconciler reconciler(
            surface, true, [&deltas](const auto& delta) { deltas.push_back(delta); });

        surface.set_font_size(20.0);
        surface.set_font_size(22.0);
        QCOMPARE(deltas.size(), 2);
        QCOMPARE(deltas[0].key, QStringLiteral("font_size"));
        QCOMPARE(deltas[0].value.metaType().id(), QMetaType::Double);
        QCOMPARE(deltas[0].value.toDouble(), 20.0);
        QCOMPARE(deltas[0].sequence, quint64{1});
        QCOMPARE(deltas[1].value.toDouble(), 22.0);
        QCOMPARE(deltas[1].sequence, quint64{2});
        QVERIFY(deltas[0].dark_mode && deltas[1].dark_mode);

        const auto reconciled = reconciler.apply_manager_snapshot(
            old_snapshot, true, 0);
        QCOMPARE(reconciled.font_size, 22.0);
        QCOMPARE(surface.font_size(), 22.0);
        QCOMPARE(deltas.size(), 2);
    }

    void copy_default_survives_restart_and_worker_override_stays_local()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QSettings store(directory.filePath(QStringLiteral("copy.ini")), QSettings::IniFormat);
        terminal_app::Terminal_display_settings preferences(true, &store);
        QVERIFY(!preferences.values().value(QStringLiteral("copy_on_select")).toBool());
        QVERIFY(preferences.apply_changes({{QStringLiteral("copy_on_select"), true}}, true));
        terminal_app::Terminal_display_settings restarted(true, &store);
        QVERIFY(restarted.values().value(QStringLiteral("copy_on_select")).toBool());
        QVERIFY(store.value(QStringLiteral("interaction/copy_on_select")).toBool());

        VNM_TerminalSurface surface;
        terminal_app::apply_terminal_settings_snapshot(
            terminal_app::load_terminal_settings_snapshot(store), surface);
        QVERIFY(surface.copy_on_select());
        QList<terminal_app::Terminal_setting_delta> deltas;
        terminal_app::Terminal_settings_reconciler reconciler(
            surface, true, [&deltas](const auto& delta) { deltas.push_back(delta); });
        auto snapshot = terminal_app::load_terminal_settings_snapshot(store);
        snapshot.copy_on_select = false;
        reconciler.apply_manager_snapshot(snapshot, true, 0);
        QVERIFY(!surface.copy_on_select());
        snapshot.copy_on_select = true;
        reconciler.apply_manager_snapshot(snapshot, false, 0);
        QVERIFY(surface.copy_on_select());
        QVERIFY(deltas.isEmpty());

        surface.set_copy_on_select(false);
        snapshot.font_size = 21.0;
        reconciler.apply_manager_snapshot(snapshot, true, 0);
        QVERIFY(!surface.copy_on_select());
        QCOMPARE(surface.font_size(), 21.0);
        QVERIFY(deltas.isEmpty());
        QVERIFY(terminal_app::load_terminal_settings_snapshot(store).copy_on_select);
    }

    void worker_cell_width_change_survives_updates_and_becomes_shared_default()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QSettings store(directory.filePath(QStringLiteral("cell-width.ini")), QSettings::IniFormat);
        terminal_app::Terminal_display_settings display(true, &store);
        VNM_TerminalSurface surface;
        VNM_TerminalSurface other_surface;
        const auto old_snapshot = terminal_app::terminal_settings_snapshot(surface);
        QList<terminal_app::Terminal_setting_delta> deltas;
        terminal_app::Terminal_settings_reconciler reconciler(
            surface, true, [&deltas](const auto& delta) { deltas.push_back(delta); });
        const int policy = old_snapshot.font_advance_policy ==
            (int)vnm_terminal::Font_advance_policy::SNAP_ADVANCE_UP
            ? (int)vnm_terminal::Font_advance_policy::SNAP_ADVANCE_NEAREST
            : (int)vnm_terminal::Font_advance_policy::SNAP_ADVANCE_UP;

        // The live settings window writes this property; the application owns persistence.
        QVERIFY(surface.setProperty("fontAdvancePolicy", policy));
        QCOMPARE(surface.font_advance_policy_value(), policy);

        auto unrelated_snapshot = old_snapshot;
        unrelated_snapshot.font_size = 23.0;
        reconciler.apply_manager_snapshot(unrelated_snapshot, false, 0);
        QCOMPARE(surface.font_advance_policy_value(), policy);
        QCOMPARE(surface.font_size(), 23.0);
        QCOMPARE(deltas.size(), 1);
        QCOMPARE(deltas.front().key, QStringLiteral("font_advance_policy"));
        QCOMPARE(deltas.front().value.toInt(), policy);
        QCOMPARE(deltas.front().sequence, quint64{1});

        QVERIFY(display.apply_changes({{deltas.front().key, deltas.front().value}}, true));
        const auto shared = terminal_app::terminal_settings_from_json(
            QJsonObject::fromVariantMap(display.values()), old_snapshot);
        QVERIFY(shared.has_value());
        reconciler.apply_manager_snapshot(*shared, false, 1);
        terminal_app::apply_terminal_settings_snapshot(*shared, other_surface);
        QCOMPARE(surface.font_advance_policy_value(), policy);
        QCOMPARE(other_surface.font_advance_policy_value(), policy);
        QCOMPARE(deltas.size(), 1);

        QVERIFY(display.apply_changes({{QStringLiteral("font_size"), 24.0}}, false));
        store.sync();
        QCOMPARE(store.status(), QSettings::NoError);
        QSettings reopened_store(store.fileName(), QSettings::IniFormat);
        terminal_app::Terminal_display_settings restarted(false, &reopened_store);
        const auto persisted = terminal_app::terminal_settings_from_json(
            QJsonObject::fromVariantMap(restarted.values()), old_snapshot);
        QVERIFY(persisted.has_value());
        VNM_TerminalSurface new_surface;
        terminal_app::apply_terminal_settings_snapshot(*persisted, new_surface);
        QCOMPARE(new_surface.font_advance_policy_value(), policy);
        QCOMPARE(new_surface.font_size(), 24.0);

        reconciler.apply_manager_snapshot(old_snapshot, true, 1);
        QCOMPARE(surface.font_advance_policy_value(), old_snapshot.font_advance_policy);
        QCOMPARE(deltas.size(), 1);
    }

    void worker_reconciler_discards_only_acknowledged_edits()
    {
        VNM_TerminalSurface surface;
        QList<terminal_app::Terminal_setting_delta> deltas;
        terminal_app::Terminal_settings_reconciler reconciler(
            surface, true, [&deltas](const auto& delta) { deltas.push_back(delta); });

        surface.set_font_size(20.0);
        surface.set_invert_brightness(true);
        QCOMPARE(deltas.size(), 2);
        QCOMPARE(deltas[1].value.metaType().id(), QMetaType::Bool);
        QCOMPARE(deltas[1].sequence, quint64{2});

        auto manager_snapshot = terminal_app::terminal_settings_snapshot(surface);
        manager_snapshot.invert_brightness = false;
        const auto partially_acknowledged = reconciler.apply_manager_snapshot(
            manager_snapshot, true, 1);
        QCOMPARE(partially_acknowledged.font_size, 20.0);
        QVERIFY(partially_acknowledged.invert_brightness);
        QVERIFY(surface.invert_brightness());

        manager_snapshot.font_size = 18.0;
        const auto fully_acknowledged = reconciler.apply_manager_snapshot(
            manager_snapshot, true, 2);
        QCOMPARE(fully_acknowledged.font_size, 18.0);
        QVERIFY(!fully_acknowledged.invert_brightness);
        QCOMPARE(surface.font_size(), 18.0);
        QVERIFY(!surface.invert_brightness());
        QCOMPARE(deltas.size(), 2);
    }

    void worker_reconciler_retains_mode_bound_edits_for_their_mode()
    {
        VNM_TerminalSurface surface;
        const auto old_snapshot = terminal_app::terminal_settings_snapshot(surface);
        QList<terminal_app::Terminal_setting_delta> deltas;
        terminal_app::Terminal_settings_reconciler reconciler(
            surface, true, [&deltas](const auto& delta) { deltas.push_back(delta); });

        surface.set_color_scheme(QStringLiteral("Campbell"));
        surface.set_invert_brightness(true);
        QCOMPARE(deltas.size(), 2);

        auto light_snapshot = old_snapshot;
        light_snapshot.color_scheme = QStringLiteral("Solarized Light");
        const auto light = reconciler.apply_manager_snapshot(
            light_snapshot, false, 0);
        QCOMPARE(light.color_scheme, QStringLiteral("Solarized Light"));
        QVERIFY(!light.invert_brightness);
        QCOMPARE(surface.color_scheme(), light.color_scheme);
        QVERIFY(!surface.invert_brightness());

        const auto dark = reconciler.apply_manager_snapshot(
            old_snapshot, true, 0);
        QCOMPARE(dark.color_scheme, QStringLiteral("Campbell"));
        QVERIFY(dark.invert_brightness);
        QCOMPARE(surface.color_scheme(), dark.color_scheme);
        QVERIFY(surface.invert_brightness());
        QCOMPARE(deltas.size(), 2);
    }

    void worker_reconciler_does_not_emit_reflected_manager_edits()
    {
        VNM_TerminalSurface surface;
        QList<terminal_app::Terminal_setting_delta> deltas;
        terminal_app::Terminal_settings_reconciler reconciler(
            surface, true, [&deltas](const auto& delta) { deltas.push_back(delta); });

        auto manager_snapshot = terminal_app::terminal_settings_snapshot(surface);
        manager_snapshot.font_size = 24.0;
        reconciler.apply_manager_snapshot(manager_snapshot, true, 0);
        QCOMPARE(surface.font_size(), 24.0);
        QVERIFY(deltas.isEmpty());

        surface.set_font_size(25.0);
        QCOMPARE(deltas.size(), 1);
        QCOMPARE(deltas.front().sequence, quint64{1});
        reconciler.apply_manager_snapshot(manager_snapshot, true, 0);
        QCOMPARE(surface.font_size(), 25.0);
        QCOMPARE(deltas.size(), 1);

        surface.set_font_size(26.0);
        QCOMPARE(deltas.size(), 2);
        QCOMPARE(deltas.back().sequence, quint64{2});
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
        expected.copy_on_select = true;
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
        expected.font_family  = QFontDatabase::families().constFirst();
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

        expected.copy_on_select = true;
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
        QCOMPARE(actual.copy_on_select, expected.copy_on_select);
        expected.scrollback_buffer_size_mib.reset();
        settings.setValue(QStringLiteral("appearance/scrollback_limit"), 200);
        terminal_app::save_terminal_settings_snapshot(
            settings,
            expected);
        QVERIFY(!settings.contains(QStringLiteral("appearance/scrollback_buffer_size_mib")));
        QVERIFY(!settings.contains(QStringLiteral("appearance/scrollback_limit")));
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

    void absent_or_unavailable_font_settings_use_the_shipped_default_data()
    {
        QTest::addColumn<QVariant>("saved_family");
        QTest::newRow("missing")     << QVariant();
        QTest::newRow("empty")       << QVariant(QString());
        QTest::newRow("blank")       << QVariant(QStringLiteral("  "));
        QTest::newRow("unavailable") << QVariant(QStringLiteral("Missing terminal test font"));
    }

    void absent_or_unavailable_font_settings_use_the_shipped_default()
    {
        QFETCH(QVariant, saved_family);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QSettings store(directory.filePath(QStringLiteral("font.ini")), QSettings::IniFormat);
        const QString key = QStringLiteral("appearance/font_family");
        if (saved_family.isValid()) {
            store.setValue(key, saved_family);
        }
        const QStringList original_keys = store.allKeys();
        const auto snapshot = terminal_app::load_terminal_settings_snapshot(store);
        QCOMPARE(snapshot.font_family, QStringLiteral("Ubuntu Sans Mono derivative vnm"));
        QCOMPARE(QFontInfo(QFont(snapshot.font_family)).family(), snapshot.font_family);
        terminal_app::Terminal_display_settings display(true, &store);
        QCOMPARE(display.values().value(QStringLiteral("font_family")).toString(),
            snapshot.font_family);
        QCOMPARE(store.value(key), saved_family);
        QCOMPARE(store.contains(key), original_keys.contains(key));
    }

    void saved_font_selection_is_preserved_after_late_registration()
    {
        vnm_fonts::initialize_resources();
        QFile font_file(QStringLiteral(":/vnm_fonts/UbuntuSansMonoDerivativeVnm-Regular.ttf"));
        QVERIFY(font_file.open(QIODevice::ReadOnly));
        const QString test_family = QStringLiteral("VNM Terminal Saved Font Test");
        const auto marked = vnm_fonts::mark_font_family(font_file.readAll(), {
            {1, test_family}, {4, test_family}, {16, test_family},
        });
        QVERIFY2(marked.is_valid(), qPrintable(marked.error));
        QVERIFY(!QFontDatabase::hasFamily(marked.family));
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QSettings store(directory.filePath(QStringLiteral("font.ini")), QSettings::IniFormat);
        const QString key = QStringLiteral("appearance/font_family");
        store.setValue(key, marked.family);
        QCOMPARE(terminal_app::load_terminal_settings_snapshot(store).font_family,
            QStringLiteral("Ubuntu Sans Mono derivative vnm"));
        QCOMPARE(store.value(key).toString(), marked.family);
        terminal_app::Terminal_display_settings display(true, &store);
        QVERIFY(display.apply_changes({{QStringLiteral("font_size"), 23.0}}, true));
        QCOMPARE(store.value(key).toString(), marked.family);
        const int font_id = QFontDatabase::addApplicationFontFromData(marked.bytes);
        QVERIFY(font_id >= 0);
        QCOMPARE(terminal_app::load_terminal_settings_snapshot(store).font_family, marked.family);
        terminal_app::Terminal_display_settings restarted(true, &store);
        QCOMPARE(restarted.values().value(QStringLiteral("font_family")).toString(), marked.family);
        QCOMPARE(QFontInfo(QFont(marked.family)).family(), marked.family);
        QVERIFY(QFontDatabase::removeApplicationFont(font_id));
    }

    void saved_generic_monospace_selection_is_preserved_data()
    {
        QTest::addColumn<QString>("font_family");
        QTest::newRow("lowercase") << QStringLiteral("monospace");
        QTest::newRow("titlecase") << QStringLiteral("Monospace");
    }

    void saved_generic_monospace_selection_is_preserved()
    {
        QFETCH(QString, font_family);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QSettings store(directory.filePath(QStringLiteral("generic-font.ini")), QSettings::IniFormat);
        terminal_app::Terminal_settings_snapshot snapshot;
        snapshot.font_family = font_family;
        terminal_app::save_terminal_settings_snapshot(store, snapshot);
        QCOMPARE(terminal_app::load_terminal_settings_snapshot(store).font_family, font_family);
        terminal_app::Terminal_display_settings display(true, &store);
        QCOMPARE(display.values().value(QStringLiteral("font_family")).toString(), font_family);
        QCOMPARE(store.value(QStringLiteral("appearance/font_family")).toString(), font_family);
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
        snapshot.copy_on_select = true;
        snapshot.scrollback_buffer_size_mib = 16;

        VNM_TerminalSurface surface;
        terminal_app::apply_terminal_settings_snapshot(snapshot, surface);
        QCOMPARE(surface.color_scheme(), snapshot.color_scheme);
        QCOMPARE(surface.font_size(), snapshot.font_size);
        QCOMPARE(surface.font_advance_policy_value(), snapshot.font_advance_policy);
        QCOMPARE(surface.invert_brightness(), snapshot.invert_brightness);
        QCOMPARE(surface.copy_on_select(), snapshot.copy_on_select);
        QCOMPARE(surface.scrollback_buffer_size_mib(), *snapshot.scrollback_buffer_size_mib);
    }

    void settings_controller_is_reusable()
    {
        terminal_app::Terminal_settings_controller controller;
        QVERIFY(!controller.available_font_families().isEmpty());
    }

    void settings_model_edits_canonical_preferences_without_a_surface()
    {
        QTemporaryDir temporary_directory;
        QVERIFY(temporary_directory.isValid());
        QSettings store(temporary_directory.filePath(QStringLiteral("terminal.ini")), QSettings::IniFormat);
        terminal_app::Terminal_display_settings preferences(true, &store);
        terminal_app::Terminal_settings_model model;
        model.set_values(preferences.values());
        QSignalSpy changes(&model, &terminal_app::Terminal_settings_model::changes_requested);
        QObject::connect(
            &model,
            &terminal_app::Terminal_settings_model::changes_requested,
            &model,
            [
                &preferences,
                &model
            ](
                const QVariantMap& delta)
            {
                preferences.apply_changes(delta, true);
                model.set_values(preferences.values());
            });

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("preferences"), &model);
        const auto edit = [
                &engine
            ](
                const QString& expression)
            {
                QQmlExpression edit_expression(engine.rootContext(), nullptr, expression);
                edit_expression.evaluate();
                return !edit_expression.hasError();
            };
        QVERIFY(edit(QStringLiteral("preferences.fontSize = 24")));
        QCOMPARE(changes.size(), 1);
        QCOMPARE(changes.at(0).at(0).toMap(),
            QVariantMap({{QStringLiteral("font_size"), 24}}));
        QCOMPARE(preferences.values().value(QStringLiteral("font_size")).toDouble(), 24.0);
        QCOMPARE(terminal_app::load_terminal_settings_snapshot(store).font_size, 24.0);

        QVERIFY(edit(QStringLiteral("preferences.fontSize = 24")));
        QVERIFY(edit(QStringLiteral("preferences.fontSize = 3")));
        QCOMPARE(changes.size(), 1);
        QCOMPARE(model.value(QStringLiteral("fontSize")).toDouble(), 24.0);
        QVERIFY(edit(QStringLiteral("preferences.colorScheme = 'Solarized Light'")));
        QCOMPARE(changes.size(), 2);
        QCOMPARE(terminal_app::load_terminal_settings_snapshot(store).color_scheme,
            QStringLiteral("Solarized Light"));

        QVERIFY(preferences.apply_changes({{QStringLiteral("font_size"), 18.0}}, true));
        model.set_values(preferences.values());
        QCOMPARE(changes.size(), 2);
        QCOMPARE(model.value(QStringLiteral("fontSize")).toDouble(), 18.0);
        QVERIFY(edit(QStringLiteral("preferences.invertBrightness = true")));
        QCOMPARE(changes.size(), 3);
        QCOMPARE(preferences.values().value(QStringLiteral("font_size")).toDouble(), 18.0);
        QVERIFY(terminal_app::load_terminal_settings_snapshot(store).invert_brightness);
        QVERIFY(edit(QStringLiteral("preferences.copyOnSelect = true")));
        QCOMPARE(changes.size(), 4);
        QVERIFY(terminal_app::load_terminal_settings_snapshot(store).copy_on_select);
    }

    void settings_window_opens_and_edits_without_a_surface()
    {
        QQmlEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        terminal_app::Terminal_display_settings preferences(true);
        terminal_app::Terminal_settings_model model;
        model.set_values(preferences.values());
        terminal_app::Terminal_settings_controller controller;
        terminal_app::Terminal_settings_window settings(engine, model, controller);
        QVERIFY2(settings.is_valid(), qPrintable(settings.error_string()));

        QQuickWindow* window = settings_window();
        QVERIFY(window != nullptr);
        settings.show_window();
        QCoreApplication::processEvents();
        QVERIFY(window->isVisible());
        QCOMPARE(window->title(), QStringLiteral("Terminal settings"));
        QVERIFY(warnings.isEmpty());
        QObject* font_size = window->findChild<QObject*>(QStringLiteral("font_size_spin"));
        QVERIFY(font_size != nullptr);
        QVERIFY(font_size->setProperty("value", 21));
        QVERIFY(QMetaObject::invokeMethod(font_size, "valueModified"));
        QCOMPARE(model.value(QStringLiteral("fontSize")).toInt(), 21);
        QObject* copy_on_select = window->findChild<QObject*>(QStringLiteral("copy_on_select_switch"));
        QVERIFY(copy_on_select != nullptr);
        QVERIFY(copy_on_select->property("enabled").toBool());
        QSignalSpy changes(&model, &terminal_app::Terminal_settings_model::changes_requested);
        QVERIFY(copy_on_select->setProperty("checked", true));
        QVERIFY(QMetaObject::invokeMethod(copy_on_select, "toggled"));
        QCOMPARE(model.value(QStringLiteral("copyOnSelect")).toBool(), true);
        QCOMPARE(changes.size(), 1);
        QCOMPARE(changes.at(0).at(0).toMap(), QVariantMap({{QStringLiteral("copy_on_select"), true}}));
        QVERIFY(window->close());
        settings.show_window();
        QCoreApplication::processEvents();
        QVERIFY(window->isVisible());
        QVERIFY(warnings.isEmpty());
    }

    void terminal_search_bar_changes_matching_without_changing_query()
    {
        QQmlEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        QQuickWindow window;
        window.resize(600, 300);
        VNM_TerminalSurface surface(window.contentItem());
        surface.setSize(QSizeF(600, 300));
        terminal_app::Terminal_search_bar search(engine, window, surface);
        QVERIFY2(search.is_valid(), qPrintable(search.error_string()));
        surface.set_search_query(QStringLiteral("project"));
        QVERIFY(surface.search_case_sensitive());
        QQuickItem* button = search.root_item()->findChild<QQuickItem*>(
            QStringLiteral("terminal_search_match_case_button"));
        QVERIFY(button != nullptr);
        QQmlExpression toggle(qmlContext(button), button, QStringLiteral("match_case_mouse.clicked(null)"));
        toggle.evaluate();
        QVERIFY2(!toggle.hasError(), qPrintable(toggle.error().toString()));
        QVERIFY(!surface.search_case_sensitive());
        QCOMPARE(surface.search_query(), QStringLiteral("project"));
        toggle.evaluate();
        QVERIFY(surface.search_case_sensitive());
        QVERIFY(warnings.isEmpty());
    }

    void settings_font_picker_tracks_the_selected_family()
    {
        QQmlEngine engine;
        terminal_app::Terminal_settings_model model;
        terminal_app::Terminal_settings_controller controller;
        terminal_app::Terminal_settings_window settings(engine, model, controller);
        QVERIFY2(settings.is_valid(), qPrintable(settings.error_string()));
        QQuickWindow* const window = settings_window();
        QVERIFY(window != nullptr);
        QObject* const picker = window->findChild<QObject*>(QStringLiteral("font_family_combo"));
        QVERIFY(picker != nullptr);
        QSignalSpy changes(&model, &terminal_app::Terminal_settings_model::changes_requested);
        settings.show_window();
        QCoreApplication::processEvents();
        QCOMPARE(picker->property("currentText").toString(),
            QStringLiteral("Ubuntu Sans Mono derivative vnm"));
        QCOMPARE(picker->property("currentIndex").toInt(), 0);
        QVERIFY(changes.isEmpty());

        const QString alternate = controller.available_font_families().constLast();
        model.set_values({{QStringLiteral("font_family"), alternate}});
        QCoreApplication::processEvents();
        QCOMPARE(picker->property("currentText").toString(), alternate);
        QVERIFY(changes.isEmpty());

        const QString unavailable = QStringLiteral("Missing terminal picker test font");
        model.set_values({{QStringLiteral("font_family"), unavailable}});
        QCoreApplication::processEvents();
        QCOMPARE(picker->property("currentIndex").toInt(), -1);
        QCOMPARE(picker->property("currentText").toString(), QString());
        QCOMPARE(model.value(QStringLiteral("fontFamily")).toString(), unavailable);
        QVERIFY(changes.isEmpty());
    }

    void settings_font_picker_activation_reports_the_effective_fallback()
    {
        QQmlEngine engine;
        terminal_app::Terminal_settings_model model;
        terminal_app::Terminal_settings_controller controller;
        terminal_app::Terminal_settings_window settings(engine, model, controller);
        QVERIFY2(settings.is_valid(), qPrintable(settings.error_string()));
        QQuickWindow* const window = settings_window();
        QVERIFY(window != nullptr);
        QObject* const picker = window->findChild<QObject*>(QStringLiteral("font_family_combo"));
        QVERIFY(picker != nullptr);
        QSignalSpy changes(&model, &terminal_app::Terminal_settings_model::changes_requested);
        QCOMPARE(picker->property("currentIndex").toInt(), 0);
        QVERIFY(changes.isEmpty());
        QVERIFY(QMetaObject::invokeMethod(picker, "activated", Q_ARG(int, 0)));
        QCOMPARE(changes.size(), 1);
        QCOMPARE(changes.at(0).at(0).toMap(), QVariantMap({
            {QStringLiteral("font_family"), vnm_terminal::default_monospace_font_family()},
        }));
    }

    void worker_font_picker_selection_reaches_the_preference_owner()
    {
        vnm_fonts::initialize_resources();
        QFile font_file(QStringLiteral(":/vnm_fonts/UbuntuSansMonoDerivativeVnm-Regular.ttf"));
        QVERIFY(font_file.open(QIODevice::ReadOnly));
        const QString test_family = QStringLiteral("VNM Terminal Worker Font Test");
        const auto marked = vnm_fonts::mark_font_family(font_file.readAll(), {
            {1, test_family}, {4, test_family}, {16, test_family},
        });
        QVERIFY2(marked.is_valid(), qPrintable(marked.error));
        const int font_id = QFontDatabase::addApplicationFontFromData(marked.bytes);
        QVERIFY(font_id >= 0);
        const auto remove_font = qScopeGuard([font_id] { QFontDatabase::removeApplicationFont(font_id); });

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QSettings store(directory.filePath(QStringLiteral("worker-font.ini")), QSettings::IniFormat);
        const QString font_key = QStringLiteral("appearance/font_family");
        store.setValue(font_key, QStringLiteral("VNM Terminal Unavailable Worker Test"));
        terminal_app::Terminal_display_settings preferences(true, &store);
        VNM_TerminalSurface surface;
        terminal_app::apply_terminal_settings_snapshot(
            terminal_app::load_terminal_settings_snapshot(store), surface);
        QList<terminal_app::Terminal_setting_delta> deltas;
        terminal_app::Terminal_settings_reconciler reconciler(
            surface, true, [
                &deltas,
                &preferences
            ](
                const auto& delta)
            {
                deltas.push_back(delta);
                preferences.apply_changes({{delta.key, delta.value}}, delta.dark_mode);
            });
        QQmlEngine engine;
        terminal_app::Terminal_settings_controller controller;
        terminal_app::Terminal_settings_window settings(engine, surface, controller);
        QVERIFY2(settings.is_valid(), qPrintable(settings.error_string()));
        QObject::connect(
            &settings, &terminal_app::Terminal_settings_window::font_family_selected,
            &reconciler, &terminal_app::Terminal_settings_reconciler::notify_font_family_selection);
        QQuickWindow* const window = settings_window();
        QVERIFY(window != nullptr);
        QObject* const picker = window->findChild<QObject*>(QStringLiteral("font_family_combo"));
        QVERIFY(picker != nullptr);
        QVERIFY(deltas.isEmpty());
        QCOMPARE(picker->property("currentIndex").toInt(), 0);
        QVERIFY(QMetaObject::invokeMethod(picker, "activated", Q_ARG(int, 0)));
        QCOMPARE(deltas.size(), 1);
        QCOMPARE(deltas.front().key, QStringLiteral("font_family"));
        QCOMPARE(store.value(font_key).toString(), vnm_terminal::default_monospace_font_family());

        const int alternate = controller.available_font_families().indexOf(marked.family);
        QVERIFY(alternate > 0);
        QVERIFY(picker->setProperty("currentIndex", alternate));
        QVERIFY(QMetaObject::invokeMethod(picker, "activated", Q_ARG(int, alternate)));
        QCOMPARE(deltas.size(), 2);
        QCOMPARE(deltas.back().value.toString(), marked.family);
        QCOMPARE(surface.font_family(), marked.family);
        QCOMPARE(store.value(font_key).toString(), marked.family);
        QCOMPARE(terminal_app::load_terminal_settings_snapshot(store).font_family, marked.family);
    }

    void application_settings_dialog_reuses_refreshes_and_releases_window()
    {
        QQmlApplicationEngine engine;
        engine.loadData(R"qml(
            import QtQuick
            import QtQuick.Window
            Window { visible: true; width: 640; height: 480 }
        )qml");
        QCOMPARE(engine.rootObjects().size(), 1);
        auto* const anchor = qobject_cast<QWindow*>(engine.rootObjects().front());
        QVERIFY(anchor != nullptr);
        terminal_app::Terminal_display_settings preferences(true);
        auto dialog = std::make_unique<terminal_app::Terminal_settings_dialog>(
            &engine, QStringLiteral("Terminal host"));
        QSignalSpy changes(dialog.get(), &terminal_app::Terminal_settings_dialog::changes_requested);
        QObject::connect(
            dialog.get(),
            &terminal_app::Terminal_settings_dialog::changes_requested,
            dialog.get(),
            [
                &preferences,
                &dialog
            ](
                const QVariantMap& delta)
            {
                preferences.apply_changes(delta, true);
                dialog->refresh(preferences.values(), true);
            });
        dialog->refresh(preferences.values(), true);
        QVERIFY2(dialog->show_window(preferences.values(), true), qPrintable(dialog->error_string()));
        QPointer<QQuickWindow> window = settings_window();
        QVERIFY(window != nullptr);
        QVERIFY(window->isVisible());
        QCOMPARE(window->transientParent(), anchor);
        QObject* const font_size = window->findChild<QObject*>(QStringLiteral("font_size_spin"));
        QVERIFY(font_size != nullptr);
        QVERIFY(font_size->setProperty("value", 21));
        QVERIFY(QMetaObject::invokeMethod(font_size, "valueModified"));
        QCOMPARE(changes.size(), 1);
        QCOMPARE(preferences.values().value(QStringLiteral("font_size")).toInt(), 21);

        QVERIFY(preferences.apply_changes({{QStringLiteral("font_size"), 18.0}}, true));
        QVERIFY(preferences.set_dark_mode(false));
        dialog->refresh(preferences.values(), false);
        QCOMPARE(font_size->property("value").toInt(), 18);
        QCOMPARE(window->property("dark_mode").toBool(), false);
        QCOMPARE(changes.size(), 1);
        QVERIFY(QMetaObject::invokeMethod(window, "close_requested"));
        QVERIFY(!window->isVisible());
        QVERIFY(dialog->show_window(preferences.values(), false));
        QCOMPARE(settings_window(), window.data());
        QVERIFY(window->isVisible());
        dialog.reset();
        QVERIFY(window.isNull());
    }

    void application_settings_dialog_releases_window_before_engine_is_gone()
    {
        auto engine = std::make_unique<QQmlApplicationEngine>();
        terminal_app::Terminal_display_settings preferences(true);
        terminal_app::Terminal_settings_dialog dialog(engine.get(), QStringLiteral("Terminal host"));
        QVERIFY2(dialog.show_window(preferences.values(), true), qPrintable(dialog.error_string()));
        QPointer<QQuickWindow> window = settings_window();
        QVERIFY(window != nullptr);
        engine.reset();
        QVERIFY(window.isNull());
        dialog.refresh(preferences.values(), false);
        QVERIFY(!dialog.show_window(preferences.values(), false));
        QVERIFY(!dialog.error_string().isEmpty());
    }

    void settings_model_and_surface_share_palette_previews()
    {
        terminal_app::Terminal_settings_model model;
        VNM_TerminalSurface surface;
        QCOMPARE(model.available_color_schemes(), surface.available_color_schemes());
        for (const QString& scheme : model.available_color_schemes()) {
            const QVariantMap preview = model.color_scheme_preview(scheme);
            QCOMPARE(preview, surface.color_scheme_preview(scheme));
            QCOMPARE(preview.value(QStringLiteral("name")).toString(), scheme);
            QCOMPARE(preview.value(QStringLiteral("ansi")).toList().size(), 16);
        }
    }
};

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    App_support_tests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "app_support_tests.moc"
