#include "vnm_terminal/internal/unicode_width.h"
#include "helpers/test_check.h"

#include <QByteArray>
#include <QString>
#include <iostream>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

QByteArray bytes_from_hex(const char* hex)
{
    return QByteArray::fromHex(hex);
}

bool test_table_version()
{
    bool ok = true;

    ok &= check(term::k_unicode_width_version_major == 16, "Unicode width major version");
    ok &= check(term::k_unicode_width_version_minor == 0, "Unicode width minor version");
    ok &= check(term::k_unicode_width_version_patch == 0, "Unicode width patch version");
    ok &= check(term::unicode_width_table_version() == QStringLiteral("16.0.0"),
        "Unicode width table version string");

    return ok;
}

bool test_ascii_and_combining_width()
{
    bool ok = true;

    const term::Terminal_utf8_width_result ascii =
        term::measure_utf8_width(QByteArrayLiteral("abc"));
    ok &= check(ascii.status == term::Terminal_unicode_width_status::OK, "ASCII status");
    ok &= check(ascii.cells == 3, "ASCII cell width");
    ok &= check(ascii.codepoints.size() == 3U, "ASCII scalar count");
    ok &= check(ascii.codepoints.front().width_class ==
        term::Terminal_unicode_width_class::NARROW,
        "ASCII width class");

    const term::Terminal_utf8_width_result combining = term::measure_utf8_width(
        QByteArrayLiteral("a") +
        bytes_from_hex("cc81"));
    ok &= check(combining.status == term::Terminal_unicode_width_status::OK,
        "combining status");
    ok &= check(combining.cells == 1, "combining width");
    ok &= check(combining.codepoints.size() == 2U, "combining scalar count");
    ok &= check(combining.codepoints[1].width_class ==
        term::Terminal_unicode_width_class::ZERO,
        "combining mark width class");

    const term::Terminal_utf8_width_result zwj =
        term::measure_utf8_width(bytes_from_hex("e2808d"));
    ok &= check(zwj.status == term::Terminal_unicode_width_status::OK,
        "ZWJ status");
    ok &= check(zwj.cells == 0, "ZWJ cell width");
    ok &= check(zwj.codepoints.front().width_class == term::Terminal_unicode_width_class::ZERO,
        "ZWJ width class");

    return ok;
}

bool test_wide_ambiguous_and_unknown_width()
{
    bool ok = true;

    const term::Terminal_utf8_width_result cjk =
        term::measure_utf8_width(bytes_from_hex("e4b880"));
    ok &= check(cjk.status == term::Terminal_unicode_width_status::OK, "CJK status");
    ok &= check(cjk.cells == 2, "CJK wide width");
    ok &= check(cjk.codepoints.size() == 1U, "CJK scalar count");
    ok &= check(cjk.codepoints.front().width_class == term::Terminal_unicode_width_class::WIDE,
        "CJK width class");

    const term::Terminal_utf8_width_result ambiguous =
        term::measure_utf8_width(bytes_from_hex("c2a1"));
    ok &= check(ambiguous.status == term::Terminal_unicode_width_status::OK,
        "ambiguous status");
    ok &= check(ambiguous.cells == 1, "ambiguous narrow width");
    ok &= check(ambiguous.codepoints.front().width_class ==
        term::Terminal_unicode_width_class::AMBIGUOUS_NARROW,
        "ambiguous width class");

    const term::Terminal_utf8_width_result omega =
        term::measure_utf8_width(bytes_from_hex("cea9"));
    ok &= check(omega.status == term::Terminal_unicode_width_status::OK,
        "omega ambiguous status");
    ok &= check(omega.cells == 1, "omega ambiguous narrow width");
    ok &= check(omega.codepoints.front().width_class ==
        term::Terminal_unicode_width_class::AMBIGUOUS_NARROW,
        "omega ambiguous width class");

    const term::Terminal_utf8_width_result ideographic_space =
        term::measure_utf8_width(bytes_from_hex("e38080"));
    ok &= check(ideographic_space.status == term::Terminal_unicode_width_status::OK,
        "ideographic space status");
    ok &= check(ideographic_space.cells == 2, "ideographic space width");
    ok &= check(ideographic_space.codepoints.front().width_class ==
        term::Terminal_unicode_width_class::WIDE,
        "ideographic space width class");

    const term::Terminal_utf8_width_result non_wide_punctuation =
        term::measure_utf8_width(bytes_from_hex("e380bf"));
    ok &= check(non_wide_punctuation.status == term::Terminal_unicode_width_status::OK,
        "U+303F status");
    ok &= check(non_wide_punctuation.cells == 1, "U+303F narrow width");
    ok &= check(non_wide_punctuation.codepoints.front().width_class ==
        term::Terminal_unicode_width_class::NARROW,
        "U+303F width class");

    const term::Terminal_utf8_width_result unassigned =
        term::measure_utf8_width(bytes_from_hex("cdb8"));
    ok &= check(unassigned.status == term::Terminal_unicode_width_status::OK,
        "unassigned scalar status");
    ok &= check(unassigned.cells == 1, "unassigned scalar narrow width");
    ok &= check(unassigned.codepoints.front().width_class ==
        term::Terminal_unicode_width_class::NARROW,
        "unassigned scalar width class");

    return ok;
}

bool test_emoji_and_variation_width()
{
    bool ok = true;

    const term::Terminal_utf8_width_result emoji =
        term::measure_utf8_width(bytes_from_hex("f09f9880"));
    ok &= check(emoji.status == term::Terminal_unicode_width_status::OK, "emoji status");
    ok &= check(emoji.cells == 2, "emoji presentation width");
    ok &= check(emoji.codepoints.front().width_class ==
        term::Terminal_unicode_width_class::EMOJI_PRESENTATION,
        "emoji width class");

    const term::Terminal_utf8_width_result heart_text =
        term::measure_utf8_width(bytes_from_hex("e29da4efb88e"));
    ok &= check(heart_text.status == term::Terminal_unicode_width_status::OK,
        "VS15 status");
    ok &= check(heart_text.cells == 1, "VS15 text width");
    ok &= check(heart_text.codepoints.front().presentation ==
        term::Terminal_unicode_presentation::TEXT,
        "VS15 base presentation");
    ok &= check(heart_text.codepoints[1].width_class ==
        term::Terminal_unicode_width_class::ZERO,
        "VS15 selector width");

    const term::Terminal_utf8_width_result emoji_text =
        term::measure_utf8_width(bytes_from_hex("f09f9880efb88e"));
    ok &= check(emoji_text.status == term::Terminal_unicode_width_status::OK,
        "emoji VS15 status");
    ok &= check(emoji_text.cells == 2, "emoji VS15 keeps default emoji width");
    ok &= check(emoji_text.codepoints.front().presentation ==
        term::Terminal_unicode_presentation::DEFAULT,
        "emoji VS15 does not retag non-standardized base");
    ok &= check(emoji_text.codepoints.front().width_class ==
        term::Terminal_unicode_width_class::EMOJI_PRESENTATION,
        "emoji VS15 keeps default emoji width class");

    const term::Terminal_utf8_width_result heart_emoji =
        term::measure_utf8_width(bytes_from_hex("e29da4efb88f"));
    ok &= check(heart_emoji.status == term::Terminal_unicode_width_status::OK,
        "VS16 status");
    ok &= check(heart_emoji.cells == 2, "VS16 emoji width");
    ok &= check(heart_emoji.codepoints.front().presentation ==
        term::Terminal_unicode_presentation::EMOJI,
        "VS16 base presentation");
    ok &= check(heart_emoji.codepoints.front().width_class ==
        term::Terminal_unicode_width_class::EMOJI_PRESENTATION,
        "VS16 base width class");

    const term::Terminal_utf8_width_result bare_heart =
        term::measure_utf8_width(bytes_from_hex("e29da4"));
    ok &= check(bare_heart.status == term::Terminal_unicode_width_status::OK,
        "bare heart status");
    ok &= check(bare_heart.cells == 1, "bare heart text width");
    ok &= check(bare_heart.codepoints.front().width_class ==
        term::Terminal_unicode_width_class::NARROW,
        "bare heart width class");

    const term::Terminal_utf8_width_result ascii_with_vs16 = term::measure_utf8_width(
        QByteArrayLiteral("A") +
        bytes_from_hex("efb88f"));
    ok &= check(ascii_with_vs16.status == term::Terminal_unicode_width_status::OK,
        "ASCII VS16 status");
    ok &= check(ascii_with_vs16.cells == 1, "ASCII VS16 width");
    ok &= check(ascii_with_vs16.codepoints.front().presentation ==
        term::Terminal_unicode_presentation::DEFAULT,
        "ASCII VS16 does not retag base");
    ok &= check(ascii_with_vs16.codepoints[1].width_class ==
        term::Terminal_unicode_width_class::ZERO,
        "ASCII VS16 selector width");

    const term::Terminal_utf8_width_result ascii_with_vs15 = term::measure_utf8_width(
        QByteArrayLiteral("A") +
        bytes_from_hex("efb88e"));
    ok &= check(ascii_with_vs15.status == term::Terminal_unicode_width_status::OK,
        "ASCII VS15 status");
    ok &= check(ascii_with_vs15.cells == 1, "ASCII VS15 width");
    ok &= check(ascii_with_vs15.codepoints.front().presentation ==
        term::Terminal_unicode_presentation::DEFAULT,
        "ASCII VS15 does not retag base");
    ok &= check(ascii_with_vs15.codepoints[1].width_class ==
        term::Terminal_unicode_width_class::ZERO,
        "ASCII VS15 selector width");

    const term::Terminal_utf8_width_result leading_vs16 =
        term::measure_utf8_width(bytes_from_hex("efb88f"));
    ok &= check(leading_vs16.status == term::Terminal_unicode_width_status::OK,
        "leading VS16 status");
    ok &= check(leading_vs16.cells == 0, "leading VS16 width");

    return ok;
}

bool test_invalid_utf8_width()
{
    bool ok = true;

    const term::Terminal_utf8_width_result lone_continuation =
        term::measure_utf8_width(bytes_from_hex("80"));
    ok &= check(lone_continuation.status ==
        term::Terminal_unicode_width_status::OK_WITH_REPLACEMENT,
        "lone continuation status");
    ok &= check(lone_continuation.cells == 1, "lone continuation width");
    ok &= check(lone_continuation.invalid_offset == 0, "lone continuation offset");
    ok &= check(lone_continuation.replacement_count == 1, "lone continuation replacement count");
    ok &= check(lone_continuation.codepoints.size() == 1U, "lone continuation replacement cell");
    ok &= check(lone_continuation.codepoints.front().codepoint == 0xfffdU,
        "lone continuation replacement scalar");

    const term::Terminal_utf8_width_result overlong =
        term::measure_utf8_width(bytes_from_hex("c0af"));
    ok &= check(overlong.status == term::Terminal_unicode_width_status::OK_WITH_REPLACEMENT,
        "overlong UTF-8 status");
    ok &= check(overlong.invalid_offset == 0, "overlong UTF-8 offset");
    ok &= check(overlong.replacement_count == 2, "overlong UTF-8 replacement count");
    ok &= check(overlong.cells == 2, "overlong UTF-8 width");

    const term::Terminal_utf8_width_result surrogate =
        term::measure_utf8_width(bytes_from_hex("eda080"));
    ok &= check(surrogate.status == term::Terminal_unicode_width_status::OK_WITH_REPLACEMENT,
        "surrogate UTF-8 status");
    ok &= check(surrogate.invalid_offset == 0, "surrogate UTF-8 offset");
    ok &= check(surrogate.replacement_count == 3, "surrogate UTF-8 replacement count");
    ok &= check(surrogate.cells == 3, "surrogate UTF-8 width");

    const term::Terminal_utf8_width_result replacement_between_ascii = term::measure_utf8_width(
        QByteArrayLiteral("A") +
        bytes_from_hex("ff")   +
        QByteArrayLiteral("B"));
    ok &= check(replacement_between_ascii.status ==
        term::Terminal_unicode_width_status::OK_WITH_REPLACEMENT,
        "invalid byte between ASCII status");
    ok &= check(replacement_between_ascii.cells == 3, "invalid byte between ASCII width");
    ok &= check(replacement_between_ascii.replacement_count == 1,
        "invalid byte between ASCII replacement count");
    ok &= check(replacement_between_ascii.invalid_offset == 1,
        "invalid byte between ASCII offset");

    return ok;
}

}

int main()
{
    bool ok = true;
    ok &= test_table_version();
    ok &= test_ascii_and_combining_width();
    ok &= test_wide_ambiguous_and_unknown_width();
    ok &= test_emoji_and_variation_width();
    ok &= test_invalid_utf8_width();
    return ok ? 0 : 1;
}
