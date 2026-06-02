#include "terminal_title_metadata.h"
#include "helpers/test_check.h"

#include <QByteArray>
#include <QString>

#include <array>
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

bool check_marker_equal(
    const std::optional<QString>& actual,
    const std::optional<QString>& expected,
    const std::string&            message)
{
    if (actual.has_value() != expected.has_value()) {
        std::cerr << "FAIL: " << message
            << " expected-has-marker=" << expected.has_value()
            << " actual-has-marker="   << actual.has_value() << '\n';
        return false;
    }

    if (!actual.has_value()) {
        return true;
    }

    return check_qstring_equal(*actual, *expected, message);
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

        chrome::Leading_activity_marker detected =
            chrome::leading_activity_marker(first_marker + QStringLiteral("work"));
        ok &= check(chrome::has_activity_marker(detected),
            std::string(marker_range.label) + " first codepoint detected");
        ok &= check_qstring_equal(detected.marker, first_marker,
            std::string(marker_range.label) + " first marker text");
        ok &= check(detected.code_unit_count == 1,
            std::string(marker_range.label) + " first marker width");

        detected = chrome::leading_activity_marker(last_marker + QStringLiteral("work"));
        ok &= check(chrome::has_activity_marker(detected),
            std::string(marker_range.label) + " last codepoint detected");
        ok &= check_qstring_equal(detected.marker, last_marker,
            std::string(marker_range.label) + " last marker text");
        ok &= check(detected.code_unit_count == 1,
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

    const chrome::Leading_activity_marker nonleading_marker =
        chrome::leading_activity_marker(
            QStringLiteral("x") + scalar_text(chrome::k_activity_marker_braille_first));
    ok &= check(!chrome::has_activity_marker(nonleading_marker),
        "activity marker is only detected at the leading position");

    const char32_t non_bmp_codepoint = 0x1f600;
    const chrome::Leading_activity_marker non_bmp =
        chrome::leading_activity_marker(
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
        std::optional<QString> expected_marker;
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
            QStringLiteral("shell"),
            QStringLiteral("plain-icon"),
            std::nullopt,
            QStringLiteral("shell"),
            "no marker",
        },
        {
            QString(),
            braille + QStringLiteral("icon-frame"),
            braille,
            QString(),
            "empty title with icon marker",
        },
    };

    bool ok = true;
    for (const title_case_t& test_case : cases) {
        const chrome::Terminal_title_content content =
            chrome::derive_terminal_title_content(
                test_case.terminal_title,
                test_case.terminal_icon_name);

        ok &= check_marker_equal(
            content.activity_marker,
            test_case.expected_marker,
            std::string(test_case.label) + " activity marker");
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

    const chrome::Terminal_title_content content =
        chrome::derive_terminal_title_content(
            raw_title,
            icon_marker + QStringLiteral("icon-frame"));

    bool ok = true;
    ok &= check_marker_equal(content.activity_marker, icon_marker,
        "icon marker remains activity marker");
    ok &= check_qstring_equal(content.display_title, QStringLiteral("compile"),
        "title marker stripped from display title");
    ok &= check_qstring_equal(raw_title, title_marker + QStringLiteral("compile"),
        "raw terminal title value is not modified");
    ok &= check_qstring_equal(chrome::activity_marker_text(content), icon_marker,
        "activity marker helper returns marker text");
    return ok;
}

} // namespace

int main()
{
    bool ok = true;
    ok &= test_activity_marker_detection_table();
    ok &= test_title_content_derivation_table();
    ok &= test_marker_stripping_is_display_only();
    return ok ? 0 : 1;
}
