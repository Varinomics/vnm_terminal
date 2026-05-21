#include "window_chrome_model.h"
#include "helpers/test_check.h"

#include <QByteArray>
#include <QRectF>
#include <QSizeF>
#include <QString>

#include <array>
#include <cmath>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace chrome = vnm_terminal::terminal_app;

namespace {

QString scalar_text(char32_t codepoint)
{
    return QString::fromUcs4(&codepoint, 1);
}

QString ellipsis_text()
{
    return QString(QChar(0x2026));
}

std::string utf8_text(const QString& text)
{
    const QByteArray bytes = text.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

using vnm_terminal::test_helpers::check;

bool check_qstring_equal(
    const QString&     actual,
    const QString&     expected,
    const std::string& message)
{
    if (actual == expected) {
        return true;
    }

    std::cerr << "FAIL: " << message
        << " expected=[" << utf8_text(expected)
        << "] actual=[" << utf8_text(actual) << "]\n";
    return false;
}

bool nearly_equal(qreal actual, qreal expected)
{
    return std::abs(actual - expected) <= 0.000001;
}

bool check_nearly_equal(qreal actual, qreal expected, const std::string& message)
{
    if (nearly_equal(actual, expected)) {
        return true;
    }

    std::cerr << "FAIL: " << message
        << " expected=" << expected
        << " actual="   << actual << '\n';
    return false;
}

bool check_rect_equal(
    const QRectF&                          actual,
    const QRectF&                          expected,
    const std::string&                     message)
{
    if (nearly_equal(actual.x(),      expected.x())     &&
        nearly_equal(actual.y(),      expected.y())     &&
        nearly_equal(actual.width(),  expected.width()) &&
        nearly_equal(actual.height(), expected.height()))
    {
        return true;
    }

    std::cerr << "FAIL: " << message
        << " expected=(" << expected.x() << ", " << expected.y()
        << ", " << expected.width() << ", " << expected.height()
        << ") actual=(" << actual.x() << ", " << actual.y()
        << ", " << actual.width() << ", " << actual.height() << ")\n";
    return false;
}

bool check_spinner_equal(
    const std::optional<QString>&          actual,
    const std::optional<QString>&          expected,
    const std::string&                     message)
{
    if (actual.has_value() != expected.has_value()) {
        std::cerr << "FAIL: " << message
            << " expected-has-spinner=" << expected.has_value()
            << " actual-has-spinner="   << actual.has_value() << '\n';
        return false;
    }

    if (!actual.has_value()) {
        return true;
    }

    return check_qstring_equal(*actual, *expected, message);
}

bool check_button_role_equal(
    chrome::Window_chrome_button_role      actual,
    chrome::Window_chrome_button_role      expected,
    const std::string&                     message)
{
    if (actual == expected) {
        return true;
    }

    std::cerr << "FAIL: " << message
        << " expected=" << static_cast<int>(expected)
        << " actual="   << static_cast<int>(actual) << '\n';
    return false;
}

bool check_button_state_equal(
    chrome::Window_chrome_button_state     actual,
    chrome::Window_chrome_button_state     expected,
    const std::string&                     message)
{
    if (actual == expected) {
        return true;
    }

    std::cerr << "FAIL: " << message
        << " expected=" << static_cast<int>(expected)
        << " actual="   << static_cast<int>(actual) << '\n';
    return false;
}

bool check_button_rects_equal(
    const chrome::Window_chrome_layout&    actual,
    const chrome::Window_chrome_layout&    expected,
    const std::string&                     message)
{
    bool ok = true;
    for (std::size_t index = 0; index < actual.buttons.size(); ++index) {
        ok &= check_rect_equal(
            actual.buttons[index].rect,
            expected.buttons[index].rect,
            message + " button " + std::to_string(index));
    }
    return ok;
}

bool test_activity_marker_detection_table()
{
    struct marker_range_case_t
    {
        char32_t       first;
        char32_t       last;
        const char*    label = "";
    };

    static constexpr std::array<marker_range_case_t, 3> k_marker_ranges = {
        marker_range_case_t{
            chrome::k_activity_marker_braille_first,
            chrome::k_activity_marker_braille_last,
            "braille",
        },
        marker_range_case_t{
            chrome::k_activity_marker_dingbat_first,
            chrome::k_activity_marker_dingbat_last,
            "dingbat",
        },
        marker_range_case_t{
            chrome::k_activity_marker_block_first,
            chrome::k_activity_marker_block_last,
            "block",
        },
    };

    bool ok = true;
    for (const marker_range_case_t& marker_range : k_marker_ranges) {
        const QString first_marker = scalar_text(marker_range.first);
        const QString last_marker  = scalar_text(marker_range.last);

        chrome::Leading_activity_marker detected = chrome::leading_activity_marker(
            first_marker +
            QStringLiteral("work"));
        ok &= check(chrome::has_activity_marker(detected),
            std::string(marker_range.label) + " first codepoint detected");
        ok &= check_qstring_equal(detected.marker, first_marker,
            std::string(marker_range.label) + " first marker text");
        ok &= check(detected.code_unit_count == 1,
            std::string(marker_range.label) + " first marker width");

        detected  = chrome::leading_activity_marker(last_marker + QStringLiteral("work"));
        ok       &= check(chrome::has_activity_marker(detected),
            std::string(marker_range.label) + " last codepoint detected");
        ok       &= check_qstring_equal(detected.marker, last_marker,
            std::string(marker_range.label) + " last marker text");
        ok       &= check(detected.code_unit_count == 1,
            std::string(marker_range.label) + " last marker width");

        detected = chrome::leading_activity_marker(
            scalar_text(marker_range.first - 1) + QStringLiteral("work"));
        ok &= check(!chrome::has_activity_marker(detected),
            std::string(marker_range.label) + " before range rejected");

        detected = chrome::leading_activity_marker(
            scalar_text(marker_range.last + 1) + QStringLiteral("work"));
        ok &= check(!chrome::has_activity_marker(detected),
            std::string(marker_range.label) + " after range rejected");
    }

    const chrome::Leading_activity_marker nonleading_marker = chrome::leading_activity_marker(
        QStringLiteral("x") + scalar_text(chrome::k_activity_marker_braille_first));
    ok &= check(!chrome::has_activity_marker(nonleading_marker),
        "activity marker is only detected at the leading position");

    const char32_t non_bmp_codepoint = 0x1f600;
    const chrome::Leading_activity_marker non_bmp = chrome::leading_activity_marker(
        scalar_text(non_bmp_codepoint) + QStringLiteral("work"));
    ok &= check(!chrome::has_activity_marker(non_bmp),
        "non-BMP leading character is not detected as an activity marker");

    return ok;
}

bool test_title_content_derivation_table()
{
    const QString braille = scalar_text(chrome::k_activity_marker_braille_first + 1);
    const QString dingbat = scalar_text(chrome::k_activity_marker_dingbat_first);
    const QString block   = scalar_text(chrome::k_activity_marker_block_last);

    struct title_case_t
    {
        QString                terminal_title;
        QString                terminal_icon_name;
        std::optional<QString> expected_spinner;
        QString                expected_display_title;
        const char*            label = "";
    };

    const std::vector<title_case_t> cases = {
        {
            dingbat + QStringLiteral("build"),
            braille + QStringLiteral("icon-frame"),
            braille,
            QStringLiteral("build"),
            "icon marker priority",
        },
        {
            block + QStringLiteral("copy"),
            QStringLiteral("plain-icon"),
            block,
            QStringLiteral("copy"),
            "title marker fallback",
        },
        {
            dingbat + QStringLiteral(" build"),
            QStringLiteral("plain-icon"),
            dingbat,
            QStringLiteral("build"),
            "title marker strips one following space",
        },
        {
            dingbat + QStringLiteral("  build"),
            QStringLiteral("plain-icon"),
            dingbat,
            QStringLiteral(" build"),
            "title marker strips only one following space",
        },
        {
            dingbat + QStringLiteral("\tbuild"),
            QStringLiteral("plain-icon"),
            dingbat,
            QStringLiteral("build"),
            "title marker strips one following tab",
        },
        {
            dingbat + QStringLiteral("build"),
            QStringLiteral("plain-icon"),
            dingbat,
            QStringLiteral("build"),
            "title marker without separator",
        },
        {
            QStringLiteral("shell"),
            QStringLiteral("plain-icon"),
            std::nullopt,
            QStringLiteral("shell"),
            "no marker",
        },
        {
            QStringLiteral(" build"),
            QStringLiteral("plain-icon"),
            std::nullopt,
            QStringLiteral(" build"),
            "non-marker leading space is preserved",
        },
        {
            QString(),
            QString(),
            std::nullopt,
            QString(),
            "empty title and icon name",
        },
        {
            QString(),
            braille + QStringLiteral("icon-frame"),
            braille,
            QString(),
            "empty title with icon marker",
        },
        {
            dingbat,
            QString(),
            dingbat,
            QString(),
            "title contains only marker",
        },
    };

    bool ok = true;
    for (const title_case_t& test_case : cases) {
        const chrome::Window_chrome_title_content content = chrome::derive_window_chrome_title_content(
            test_case.terminal_title,
            test_case.terminal_icon_name);

        ok &= check_spinner_equal(
            content.spinner,
            test_case.expected_spinner,
            std::string(test_case.label) + " spinner");
        ok &= check_qstring_equal(
            content.display_title,
            test_case.expected_display_title,
            std::string(test_case.label) + " display title");
    }

    return ok;
}

bool test_marker_stripping_is_display_only()
{
    const QString title_marker = scalar_text(chrome::k_activity_marker_dingbat_last);
    const QString icon_marker  = scalar_text(chrome::k_activity_marker_braille_last);
    const QString raw_title    = title_marker + QStringLiteral("compile");

    const chrome::Window_chrome_title_content content = chrome::derive_window_chrome_title_content(
        raw_title,
        icon_marker + QStringLiteral("icon-frame"));

    bool ok = true;
    ok &= check_spinner_equal(content.spinner, icon_marker, "icon marker remains spinner");
    ok &= check_qstring_equal(content.display_title, QStringLiteral("compile"),
        "title marker stripped from display title");
    ok &= check_qstring_equal(raw_title, title_marker + QStringLiteral("compile"),
        "raw terminal title value is not modified");
    return ok;
}

bool test_layout_rectangles_are_stable()
{
    const QSizeF narrow_size(360.0, 32.0);
    const QSizeF wide_size(520.0, 32.0);

    const chrome::Window_chrome_layout narrow =
        chrome::calculate_window_chrome_layout(narrow_size);
    const chrome::Window_chrome_layout wide =
        chrome::calculate_window_chrome_layout(wide_size);

    bool ok = true;
    ok &= check_rect_equal(narrow.icon_rect, QRectF(8.0, 8.0, 16.0, 16.0),
        "narrow icon rect");
    ok &= check_rect_equal(narrow.spinner_slot_rect, QRectF(30.0, 0.0, 18.0, 32.0),
        "narrow spinner slot rect");
    ok &= check_rect_equal(narrow.title_text_rect, QRectF(56.0, 0.0, 158.0, 32.0),
        "narrow title text rect");
    ok &= check_rect_equal(narrow.buttons[0].rect, QRectF(222.0, 0.0, 46.0, 32.0),
        "narrow minimize button rect");
    ok &= check_rect_equal(narrow.buttons[1].rect, QRectF(268.0, 0.0, 46.0, 32.0),
        "narrow maximize button rect");
    ok &= check_rect_equal(narrow.buttons[2].rect, QRectF(314.0, 0.0, 46.0, 32.0),
        "narrow close button rect");
    ok &= check_button_role_equal(
        narrow.buttons[0].role,
        chrome::Window_chrome_button_role::MINIMIZE,
        "default minimize button role");
    ok &= check_button_role_equal(
        narrow.buttons[1].role,
        chrome::Window_chrome_button_role::MAXIMIZE_RESTORE,
        "default maximize/restore button role");
    ok &= check_button_role_equal(
        narrow.buttons[2].role,
        chrome::Window_chrome_button_role::CLOSE,
        "default close button role");
    ok &= check_button_state_equal(
        narrow.buttons[0].state,
        chrome::Window_chrome_button_state::NORMAL,
        "default minimize button state");
    ok &= check_button_state_equal(
        narrow.buttons[1].state,
        chrome::Window_chrome_button_state::NORMAL,
        "default maximize/restore button state");
    ok &= check_button_state_equal(
        narrow.buttons[2].state,
        chrome::Window_chrome_button_state::NORMAL,
        "default close button state");
    ok &= check(!narrow.window_maximized, "default layout is not maximized");

    ok &= check_rect_equal(wide.icon_rect, narrow.icon_rect,
        "wide icon rect stays left anchored");
    ok &= check_rect_equal(wide.spinner_slot_rect, narrow.spinner_slot_rect,
        "wide spinner slot stays left anchored");
    ok &= check_nearly_equal(wide.title_text_rect.x(), narrow.title_text_rect.x(),
        "title x position is stable across widths");
    ok &= check_nearly_equal(
        wide.title_text_rect.width(),
        narrow.title_text_rect.width() + 160.0,
        "title width absorbs window width delta");
    ok &= check_nearly_equal(wide.buttons[0].rect.x(), narrow.buttons[0].rect.x() + 160.0,
        "button group stays right anchored across widths");

    chrome::Window_chrome_button_states active_states;
    active_states.minimize         = chrome::Window_chrome_button_state::HOVERED;
    active_states.maximize_restore = chrome::Window_chrome_button_state::PRESSED;
    active_states.close            = chrome::Window_chrome_button_state::DISABLED;
    active_states.window_maximized = true;

    const chrome::Window_chrome_layout active =
        chrome::calculate_window_chrome_layout(narrow_size, active_states);
    ok &= check_rect_equal(active.icon_rect, narrow.icon_rect,
        "button states do not move icon rect");
    ok &= check_rect_equal(active.spinner_slot_rect, narrow.spinner_slot_rect,
        "button states do not move spinner slot");
    ok &= check_rect_equal(active.title_text_rect, narrow.title_text_rect,
        "button states do not move title text rect");
    ok &= check_button_rects_equal(active, narrow, "button states preserve button rectangles");
    ok &= check_button_state_equal(
        active.buttons[0].state,
        chrome::Window_chrome_button_state::HOVERED,
        "active minimize button state");
    ok &= check_button_state_equal(
        active.buttons[1].state,
        chrome::Window_chrome_button_state::PRESSED,
        "active maximize/restore button state");
    ok &= check_button_state_equal(
        active.buttons[2].state,
        chrome::Window_chrome_button_state::DISABLED,
        "active close button state");
    ok &= check(active.window_maximized,
        "layout records maximized state without geometry change");

    return ok;
}

bool test_layout_metrics_and_narrow_sizes()
{
    chrome::Window_chrome_layout_metrics metrics;
    metrics.horizontal_padding = 5.0;
    metrics.icon_size          = 20.0;
    metrics.icon_spinner_gap   = 4.0;
    metrics.spinner_slot_width = 24.0;
    metrics.title_gap          = 3.0;
    metrics.title_button_gap   = 7.0;
    metrics.button_width       = 40.0;

    const chrome::Window_chrome_layout custom =
        chrome::calculate_window_chrome_layout(QSizeF(300.0, 30.0), {}, metrics);

    bool ok = true;
    ok &= check_rect_equal(custom.icon_rect, QRectF(5.0, 5.0, 20.0, 20.0),
        "custom metrics icon rect");
    ok &= check_rect_equal(custom.spinner_slot_rect, QRectF(29.0, 0.0, 24.0, 30.0),
        "custom metrics spinner slot rect");
    ok &= check_rect_equal(custom.title_text_rect, QRectF(56.0, 0.0, 117.0, 30.0),
        "custom metrics title rect");
    ok &= check_rect_equal(custom.buttons[0].rect, QRectF(180.0, 0.0, 40.0, 30.0),
        "custom metrics minimize rect");

    const chrome::Window_chrome_layout short_titlebar =
        chrome::calculate_window_chrome_layout(QSizeF(360.0, 12.0));
    ok &= check_rect_equal(short_titlebar.icon_rect, QRectF(8.0, 0.0, 12.0, 12.0),
        "short titlebar clamps icon size to height");
    ok &= check_rect_equal(
        short_titlebar.spinner_slot_rect,
        QRectF(26.0, 0.0, 18.0, 12.0),
        "short titlebar preserves spinner slot height");
    ok &= check_rect_equal(
        short_titlebar.title_text_rect,
        QRectF(52.0, 0.0, 162.0, 12.0),
        "short titlebar title rect follows clamped icon width");

    const chrome::Window_chrome_layout zero =
        chrome::calculate_window_chrome_layout(QSizeF(0.0, 0.0));
    ok &= check_rect_equal(zero.icon_rect, QRectF(8.0, 0.0, 0.0, 0.0),
        "zero titlebar icon rect");
    ok &= check_rect_equal(zero.title_text_rect, QRectF(40.0, 0.0, 0.0, 0.0),
        "zero titlebar title rect");
    ok &= check_nearly_equal(zero.buttons[0].rect.x(), 0.0,
        "zero titlebar clamps button group start");

    const chrome::Window_chrome_layout narrow =
        chrome::calculate_window_chrome_layout(QSizeF(100.0, 32.0));
    ok &= check_rect_equal(narrow.buttons[0].rect, QRectF(0.0, 0.0, 46.0, 32.0),
        "narrow titlebar clamps button group start");
    ok &= check_rect_equal(narrow.buttons[1].rect, QRectF(46.0, 0.0, 46.0, 32.0),
        "narrow titlebar keeps maximize button after minimize button");
    ok &= check_rect_equal(narrow.buttons[2].rect, QRectF(92.0, 0.0, 46.0, 32.0),
        "narrow titlebar keeps close button after maximize button");
    ok &= check_nearly_equal(narrow.title_text_rect.x(), 56.0,
        "narrow titlebar keeps normal title x position");
    ok &= check_nearly_equal(narrow.title_text_rect.width(), 0.0,
        "narrow titlebar clamps title width");

    return ok;
}

bool test_make_model_forwards_layout_inputs()
{
    chrome::Window_chrome_button_states states;
    states.close            = chrome::Window_chrome_button_state::PRESSED;
    states.window_maximized = true;

    chrome::Window_chrome_layout_metrics metrics;
    metrics.spinner_slot_width = 30.0;
    metrics.button_width       = 42.0;

    const chrome::Window_chrome_model model = chrome::make_window_chrome_model(
        QStringLiteral("compile"),
        QString(),
        QSizeF(320.0, 32.0),
        states,
        metrics);

    bool ok = true;
    ok &= check_nearly_equal(model.layout.spinner_slot_rect.width(), 30.0,
        "make model forwards layout metrics");
    ok &= check_button_state_equal(
        model.layout.buttons[2].state,
        chrome::Window_chrome_button_state::PRESSED,
        "make model forwards button state");
    ok &= check(model.layout.window_maximized,
        "make model forwards maximized state");
    return ok;
}

bool test_title_x_position_ignores_activity_marker_glyph()
{
    const QSizeF titlebar_size(360.0, 32.0);
    const chrome::Window_chrome_model plain =
        chrome::make_window_chrome_model(QStringLiteral("compile"), {}, titlebar_size);
    const chrome::Window_chrome_model braille = chrome::make_window_chrome_model(
        scalar_text(chrome::k_activity_marker_braille_last) + QStringLiteral("compile"),
        {},
        titlebar_size);
    const chrome::Window_chrome_model dingbat = chrome::make_window_chrome_model(
        scalar_text(chrome::k_activity_marker_dingbat_first) + QStringLiteral("compile"),
        {},
        titlebar_size);
    const chrome::Window_chrome_model block = chrome::make_window_chrome_model(
        scalar_text(chrome::k_activity_marker_block_first) + QStringLiteral("compile"),
        {},
        titlebar_size);

    bool ok = true;
    ok &= check_rect_equal(braille.layout.title_text_rect, plain.layout.title_text_rect,
        "braille title marker does not change title rect");
    ok &= check_rect_equal(dingbat.layout.title_text_rect, plain.layout.title_text_rect,
        "dingbat title marker does not change title rect");
    ok &= check_rect_equal(block.layout.title_text_rect, plain.layout.title_text_rect,
        "block title marker does not change title rect");
    ok &= check_qstring_equal(braille.title.display_title, plain.title.display_title,
        "braille marker stripped before display");
    ok &= check_qstring_equal(dingbat.title.display_title, plain.title.display_title,
        "dingbat marker stripped before display");
    ok &= check_qstring_equal(block.title.display_title, plain.title.display_title,
        "block marker stripped before display");
    ok &= check_spinner_equal(plain.title.spinner, std::nullopt,
        "plain title has no spinner");
    ok &= check_spinner_equal(
        braille.title.spinner,
        scalar_text(chrome::k_activity_marker_braille_last),
        "braille spinner detected");
    ok &= check_spinner_equal(
        dingbat.title.spinner,
        scalar_text(chrome::k_activity_marker_dingbat_first),
        "dingbat spinner detected");
    ok &= check_spinner_equal(
        block.title.spinner,
        scalar_text(chrome::k_activity_marker_block_first),
        "block spinner detected");
    if (braille.title.spinner.has_value() &&
        dingbat.title.spinner.has_value() &&
        block.title.spinner.has_value())
    {
        ok &= check(*braille.title.spinner != *dingbat.title.spinner,
            "braille and dingbat spinner frames differ");
        ok &= check(*dingbat.title.spinner != *block.title.spinner,
            "dingbat and block spinner frames differ");
    }
    return ok;
}

bool test_elision_uses_display_title_and_layout()
{
    const QString marker = scalar_text(chrome::k_activity_marker_block_first);
    const chrome::Window_chrome_model fitted = chrome::make_window_chrome_model(
        marker + QStringLiteral("Busy"),
        {},
        QSizeF(244.0, 32.0));

    bool ok = true;
    ok &= check(chrome::title_capacity_code_units(fitted.layout, 10.0) == 4,
        "test layout exposes four title code units");
    ok &= check_qstring_equal(
        chrome::elide_window_chrome_title(fitted.title, fitted.layout, 10.0),
        QStringLiteral("Busy"),
        "elision fits display title after marker stripping");

    const chrome::Window_chrome_model elided = chrome::make_window_chrome_model(
        marker + QStringLiteral("Building"),
        {},
        QSizeF(244.0, 32.0));
    ok &= check_qstring_equal(
        chrome::elide_window_chrome_title(elided.title, elided.layout, 10.0),
        QStringLiteral("Bui") + ellipsis_text(),
        "elision truncates display title rather than raw title");

    return ok;
}

bool test_code_unit_elision_edge_cases()
{
    const QString smile               = scalar_text(0x1f600);
    const QString text_with_surrogate = QStringLiteral("A") + smile + QStringLiteral("Z");

    const chrome::Window_chrome_layout layout =
        chrome::calculate_window_chrome_layout(QSizeF(244.0, 32.0));

    bool ok = true;
    ok &= check(chrome::title_capacity_code_units(layout, 0.0) == 0,
        "zero code-unit width returns zero title capacity");
    ok &= check(chrome::title_capacity_code_units(layout, -1.0) == 0,
        "negative code-unit width returns zero title capacity");
    ok &= check_qstring_equal(
        chrome::elide_display_title_to_code_units(QStringLiteral("Busy"), 0),
        QString(),
        "zero capacity returns empty elided title");
    ok &= check_qstring_equal(
        chrome::elide_display_title_to_code_units(QStringLiteral("Busy"), 1),
        ellipsis_text(),
        "single code-unit capacity returns ellipsis");
    ok &= check_qstring_equal(
        chrome::elide_display_title_to_code_units(QString(), 3),
        QString(),
        "empty display title remains empty");
    ok &= check_qstring_equal(
        chrome::elide_display_title_to_code_units(text_with_surrogate, 3),
        QStringLiteral("A") + ellipsis_text(),
        "elision does not leave a dangling high surrogate");
    ok &= check_qstring_equal(
        chrome::elide_display_title_to_code_units(smile + QStringLiteral("ZZ"), 3),
        smile + ellipsis_text(),
        "elision preserves a complete leading surrogate pair when it fits");

    return ok;
}

}

int main()
{
    bool ok = true;
    ok &= test_activity_marker_detection_table();
    ok &= test_title_content_derivation_table();
    ok &= test_marker_stripping_is_display_only();
    ok &= test_layout_rectangles_are_stable();
    ok &= test_layout_metrics_and_narrow_sizes();
    ok &= test_make_model_forwards_layout_inputs();
    ok &= test_title_x_position_ignores_activity_marker_glyph();
    ok &= test_elision_uses_display_title_and_layout();
    ok &= test_code_unit_elision_edge_cases();
    return ok ? 0 : 1;
}
