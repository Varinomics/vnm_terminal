#include "vnm_terminal/internal/unicode_width.h"
#include "vnm_terminal/internal/unicode_width_tables.h"
#include "helpers/test_check.h"

#include <QString>

#include <iostream>
#include <string_view>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

bool contains(std::string_view text, std::string_view needle)
{
    return text.find(needle) != std::string_view::npos;
}

int count_occurrences(std::string_view text, std::string_view needle)
{
    int count = 0;
    std::string_view rest = text;

    while (true) {
        const std::size_t offset = rest.find(needle);
        if (offset == std::string_view::npos) {
            return count;
        }

        ++count;
        rest.remove_prefix(offset + needle.size());
    }
}

bool test_table_manifest()
{
    bool ok = true;

    const std::string_view manifest(term::unicode_width_tables_manifest_json());

    ok &= check(term::unicode_width_table_version() == QStringLiteral("16.0.0"),
        "public Unicode table version");
    ok &= check(contains(manifest, "\"unicode_version\": \"16.0.0\""),
        "manifest Unicode version");
    ok &= check(contains(manifest, "\"generator_version\": \"1.6.0\""),
        "manifest generator version");
    ok &= check(contains(manifest, "\"artifact_kind\": \"unicode_source_generated\""),
        "manifest source-generated artifact kind");
    ok &= check(contains(manifest, "\"format_controls\": 0"),
        "manifest format-control width policy");
    ok &= check(contains(manifest, "https://www.unicode.org/Public/16.0.0/ucd/EastAsianWidth.txt"),
        "manifest EastAsianWidth provenance");
    ok &= check(contains(manifest, "https://www.unicode.org/Public/16.0.0/ucd/UnicodeData.txt"),
        "manifest UnicodeData provenance");
    ok &= check(contains(manifest, "https://www.unicode.org/Public/16.0.0/ucd/PropList.txt"),
        "manifest PropList provenance");
    ok &= check(contains(manifest, "https://www.unicode.org/Public/16.0.0/ucd/emoji/emoji-data.txt"),
        "manifest emoji-data provenance");
    ok &= check(contains(manifest,
        "https://www.unicode.org/Public/16.0.0/ucd/emoji/emoji-variation-sequences.txt"),
        "manifest emoji variation provenance");
    ok &= check(count_occurrences(manifest, "\"present\": true") == 5,
        "source-generated artifact records present source files");
    ok &= check(count_occurrences(manifest, "\"sha256\":") == 5,
        "source-generated artifact records source checksums");

    return ok;
}

bool test_generated_lookup_cases()
{
    bool ok = true;

    ok &= check(term::unicode_width_is_zero_width(0x0301U), "combining acute is zero width");
    ok &= check(term::unicode_width_is_zero_width(0x0903U), "spacing combining mark is zero width");
    ok &= check(term::unicode_width_is_zero_width(0x200bU), "zero width space is zero width");
    ok &= check(term::unicode_width_is_zero_width(0x200dU), "ZWJ is zero width");
    ok &= check(term::unicode_width_is_zero_width(0x2060U), "word joiner is zero width");
    ok &= check(term::unicode_width_is_zero_width(0xfe0fU), "VS16 is zero width");
    ok &= check(term::unicode_width_is_zero_width(0xfeffU), "zero width no-break space is zero width");
    ok &= check(term::unicode_width_is_zero_width(0xe0020U), "tag characters are zero width");
    ok &= check(term::unicode_width_is_zero_width(0xe0100U),
        "supplementary variation selector is zero width");
    ok &= check(!term::unicode_width_is_zero_width(U'A'), "ASCII is not zero width");

    ok &= check(term::unicode_width_is_wide(0x3000U), "ideographic space is wide");
    ok &= check(term::unicode_width_is_wide(0x4e00U), "CJK unified ideograph is wide");
    ok &= check(term::unicode_width_is_wide(0x20000U), "supplementary CJK range is wide");
    ok &= check(term::unicode_width_is_wide(0xfe10U), "vertical presentation form is wide");
    ok &= check(term::unicode_width_is_wide(0xff21U), "fullwidth Latin capital A is wide");
    ok &= check(!term::unicode_width_is_wide(0x303fU), "ideographic half fill space is not wide");

    ok &= check(term::unicode_width_is_ambiguous(0x00a1U), "inverted exclamation is ambiguous");
    ok &= check(term::unicode_width_is_ambiguous(0x03a9U), "omega is ambiguous");
    ok &= check(!term::unicode_width_is_ambiguous(U'A'), "ASCII is not ambiguous");

    ok &= check(term::unicode_width_is_default_emoji(0x231aU), "watch has default emoji width");
    ok &= check(term::unicode_width_is_default_emoji(0x1f600U), "grinning face has default emoji width");
    ok &= check(!term::unicode_width_is_default_emoji(0x2764U), "heart defaults to text width");

    ok &= check(term::unicode_width_is_emoji_variation_base(0x2764U),
        "heart accepts emoji variation");
    ok &= check(!term::unicode_width_is_emoji_variation_base(0x1f600U),
        "default emoji alone does not imply standardized text variation");
    ok &= check(!term::unicode_width_is_emoji_variation_base(U'A'),
        "ASCII is not an emoji variation base");

    return ok;
}

bool test_public_width_uses_generated_tables()
{
    bool ok = true;
    const term::Terminal_codepoint_width combining = term::width_for_codepoint(0x0301U);
    ok &= check(combining.cells == 0, "generated combining lookup public width");
    ok &= check(combining.width_class == term::Terminal_unicode_width_class::ZERO,
        "generated combining lookup public class");

    const term::Terminal_codepoint_width wide = term::width_for_codepoint(0x4e00U);
    ok &= check(wide.cells == 2, "generated wide lookup public width");
    ok &= check(wide.width_class == term::Terminal_unicode_width_class::WIDE,
        "generated wide lookup public class");

    const term::Terminal_codepoint_width ambiguous = term::width_for_codepoint(0x03a9U);
    ok &= check(ambiguous.cells == 1, "generated ambiguous lookup public width");
    ok &= check(ambiguous.width_class == term::Terminal_unicode_width_class::AMBIGUOUS_NARROW,
        "generated ambiguous lookup public class");

    const term::Terminal_codepoint_width emoji = term::width_for_codepoint(0x1f600U);
    ok &= check(emoji.cells == 2, "generated emoji lookup public width");
    ok &= check(emoji.width_class == term::Terminal_unicode_width_class::EMOJI_PRESENTATION,
        "generated emoji lookup public class");

    const term::Terminal_codepoint_width fe10 = term::width_for_codepoint(0xfe10U);
    ok &= check(fe10.cells == 2, "source-generated table applies U+FE10 width");
    ok &= check(fe10.width_class == term::Terminal_unicode_width_class::WIDE,
        "source-generated table applies U+FE10 class");

    return ok;
}

}

int main()
{
    bool ok = true;
    ok &= test_table_manifest();
    ok &= test_generated_lookup_cases();
    ok &= test_public_width_uses_generated_tables();
    return ok ? 0 : 1;
}
