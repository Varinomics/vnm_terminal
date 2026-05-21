# Unicode Width Policy

`vnm_terminal` uses owned terminal-cell width tables generated from Unicode
16.0.0 data. Width is a parser/model contract; Qt shaping and fallback glyph
metrics are render-time data and do not redefine terminal cell occupancy.

## Source Data

The generated tables are derived from these Unicode 16.0.0 files:

- `https://www.unicode.org/Public/16.0.0/ucd/EastAsianWidth.txt`
- `https://www.unicode.org/Public/16.0.0/ucd/UnicodeData.txt`
- `https://www.unicode.org/Public/16.0.0/ucd/PropList.txt`
- `https://www.unicode.org/Public/16.0.0/ucd/emoji/emoji-data.txt`
- `https://www.unicode.org/Public/16.0.0/ucd/emoji/emoji-variation-sequences.txt`

Generated artifacts record the Unicode version, input URLs, input hashes,
generator command, and generator version. The generated table files are:

- `include/vnm_terminal/internal/unicode_width_tables.h`
- `src/unicode_width_tables.cpp`

The generator is `tools/unicode_width/generate_unicode_width_tables.py`.

## Width Rules

- Ambiguous East Asian Width characters are narrow.
- Combining marks and variation selectors are zero-width.
- Default emoji presentation is two cells.
- VS15 requests text presentation.
- VS16 requests emoji presentation when the base scalar is emoji-capable.
- Assigned or unassigned scalar values without a width property are narrow.
- Invalid UTF-8 is replaced with U+FFFD. Each replacement has width 1, and the
  scan reports the replacement count plus the first invalid byte offset.

## Rejected Sources

The production width table is not copied from `wcwidth`, Markus Kuhn-derived
tables, terminal emulator source trees, or GPL-licensed projects.

`utf8proc` is not a dependency. Qt Unicode and shaping APIs are not the
terminal-cell width source of truth because their behavior depends on the Qt
build in use.
