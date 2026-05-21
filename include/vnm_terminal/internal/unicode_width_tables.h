#pragma once

#include <cstdint>

namespace vnm_terminal::internal {

struct unicode_width_range_t
{
    char32_t first;
    char32_t last;
};

const char* unicode_width_tables_manifest_json();

bool unicode_width_is_zero_width(char32_t codepoint);
bool unicode_width_is_wide(char32_t codepoint);
bool unicode_width_is_ambiguous(char32_t codepoint);
bool unicode_width_is_default_emoji(char32_t codepoint);
bool unicode_width_is_emoji_variation_base(char32_t codepoint);

}
