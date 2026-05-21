#!/usr/bin/env python3
"""Unicode width table generator entry point for vnm_terminal.

The generator is intentionally network-free. When a Unicode 16.0.0 source
directory is supplied, it emits compact range tables from those files. When no
source directory is supplied, it emits a deterministic representative fallback
for self-tests and local smoke checks.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any


UNICODE_VERSION = "16.0.0"
GENERATOR_VERSION = "1.6.0"
AUTHORED_ARTIFACT_KIND = "authored_representative"
SOURCE_ARTIFACT_KIND = "unicode_source_generated"
SOURCE_FILES = {
    "EastAsianWidth.txt": "https://www.unicode.org/Public/16.0.0/ucd/EastAsianWidth.txt",
    "UnicodeData.txt": "https://www.unicode.org/Public/16.0.0/ucd/UnicodeData.txt",
    "PropList.txt": "https://www.unicode.org/Public/16.0.0/ucd/PropList.txt",
    "emoji/emoji-data.txt": "https://www.unicode.org/Public/16.0.0/ucd/emoji/emoji-data.txt",
    "emoji/emoji-variation-sequences.txt": (
        "https://www.unicode.org/Public/16.0.0/ucd/emoji/emoji-variation-sequences.txt"
    ),
}


@dataclass(frozen=True, order=True)
class Codepoint_range:
    first: int
    last: int


@dataclass(frozen=True)
class Unicode_width_tables:
    zero_width: tuple[Codepoint_range, ...]
    wide: tuple[Codepoint_range, ...]
    ambiguous: tuple[Codepoint_range, ...]
    emoji_presentation: tuple[Codepoint_range, ...]
    emoji_variation_base: tuple[Codepoint_range, ...]


AUTHORED_TABLES = Unicode_width_tables(
    zero_width=(
        Codepoint_range(0x0300, 0x036F),
        Codepoint_range(0x1AB0, 0x1AFF),
        Codepoint_range(0x1DC0, 0x1DFF),
        Codepoint_range(0x200B, 0x200D),
        Codepoint_range(0x2060, 0x2060),
        Codepoint_range(0x20D0, 0x20FF),
        Codepoint_range(0xFE00, 0xFE0F),
        Codepoint_range(0xFE20, 0xFE2F),
        Codepoint_range(0xFEFF, 0xFEFF),
        Codepoint_range(0xE0001, 0xE0001),
        Codepoint_range(0xE0020, 0xE007F),
        Codepoint_range(0xE0100, 0xE01EF),
    ),
    wide=(
        Codepoint_range(0x3000, 0x3000),
        Codepoint_range(0x3400, 0x4DBF),
        Codepoint_range(0x4E00, 0x9FFF),
        Codepoint_range(0xF900, 0xFAFF),
        Codepoint_range(0x20000, 0x3FFFD),
    ),
    ambiguous=(
        Codepoint_range(0x00A1, 0x00A1),
        Codepoint_range(0x00A4, 0x00A4),
        Codepoint_range(0x00A7, 0x00A8),
        Codepoint_range(0x00AA, 0x00AA),
        Codepoint_range(0x00AD, 0x00AE),
        Codepoint_range(0x00B0, 0x00B1),
        Codepoint_range(0x03A9, 0x03A9),
    ),
    emoji_presentation=(
        Codepoint_range(0x231A, 0x231B),
        Codepoint_range(0x1F600, 0x1F600),
    ),
    emoji_variation_base=(
        Codepoint_range(0x231A, 0x231B),
        Codepoint_range(0x263A, 0x263A),
        Codepoint_range(0x2665, 0x2665),
        Codepoint_range(0x2764, 0x2764),
    ),
)


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while True:
            chunk = handle.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def parse_codepoint_range(text: str) -> Codepoint_range:
    fields = text.strip().split("..")
    first = int(fields[0], 16)
    last = int(fields[-1], 16)
    return Codepoint_range(first, last)


def strip_comment(line: str) -> str:
    return line.split("#", 1)[0].strip()


def merge_ranges(ranges: set[Codepoint_range]) -> tuple[Codepoint_range, ...]:
    if not ranges:
        return ()

    sorted_ranges = sorted(ranges)
    merged: list[Codepoint_range] = []

    current = sorted_ranges[0]
    for item in sorted_ranges[1:]:
        if item.first <= current.last + 1:
            current = Codepoint_range(current.first, max(current.last, item.last))
            continue

        merged.append(current)
        current = item

    merged.append(current)
    return tuple(merged)


def add_range_values(target: set[Codepoint_range], item: Codepoint_range) -> None:
    target.add(item)


def parse_property_file(path: Path, properties: set[str]) -> dict[str, set[Codepoint_range]]:
    out = {property_name: set() for property_name in properties}

    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            body = strip_comment(line)
            if not body:
                continue

            fields = [field.strip() for field in body.split(";")]
            if len(fields) < 2 or fields[1] not in properties:
                continue

            add_range_values(out[fields[1]], parse_codepoint_range(fields[0]))

    return out


def parse_unicode_data_zero_width(path: Path) -> set[Codepoint_range]:
    ranges: set[Codepoint_range] = set()
    pending_first: tuple[int, str] | None = None

    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            fields = line.rstrip("\n").split(";")
            if len(fields) < 3:
                continue

            codepoint = int(fields[0], 16)
            name = fields[1]
            category = fields[2]

            if name.endswith(", First>"):
                pending_first = (codepoint, category)
                continue

            if name.endswith(", Last>") and pending_first is not None:
                first, first_category = pending_first
                if first_category.startswith("M") or first_category == "Cf":
                    ranges.add(Codepoint_range(first, codepoint))
                pending_first = None
                continue

            if category.startswith("M") or category == "Cf":
                ranges.add(Codepoint_range(codepoint, codepoint))

    return ranges


def parse_emoji_variation_bases(path: Path) -> set[Codepoint_range]:
    ranges: set[Codepoint_range] = set()

    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            body = strip_comment(line)
            if not body:
                continue

            sequence = body.split(";", 1)[0].strip()
            parts = sequence.split()
            if len(parts) < 2:
                continue

            variation_selector = int(parts[1], 16)
            if variation_selector in {0xFE0E, 0xFE0F}:
                codepoint = int(parts[0], 16)
                ranges.add(Codepoint_range(codepoint, codepoint))

    return ranges


def load_tables_from_source(source_dir: Path) -> Unicode_width_tables:
    east_asian_width = parse_property_file(
        source_dir / "EastAsianWidth.txt", {"A", "F", "W"})
    prop_list = parse_property_file(source_dir / "PropList.txt", {"Variation_Selector"})
    emoji_data = parse_property_file(
        source_dir / "emoji" / "emoji-data.txt", {"Emoji_Presentation"})

    zero_width = set(parse_unicode_data_zero_width(source_dir / "UnicodeData.txt"))
    zero_width.update(prop_list["Variation_Selector"])

    wide: set[Codepoint_range] = set()
    wide.update(east_asian_width["F"])
    wide.update(east_asian_width["W"])

    emoji_variation_base = parse_emoji_variation_bases(
        source_dir / "emoji" / "emoji-variation-sequences.txt")

    return Unicode_width_tables(
        zero_width=merge_ranges(zero_width),
        wide=merge_ranges(wide),
        ambiguous=merge_ranges(east_asian_width["A"]),
        emoji_presentation=merge_ranges(emoji_data["Emoji_Presentation"]),
        emoji_variation_base=merge_ranges(emoji_variation_base),
    )


def check_source_dir(source_dir: Path) -> list[str]:
    errors: list[str] = []

    for relative in SOURCE_FILES:
        path = source_dir / relative
        if not path.is_file():
            errors.append(f"missing {relative}")

    return errors


def build_manifest(
    source_dir: Path | None,
    generator_command: list[str],
    artifact_kind: str,
) -> dict[str, Any]:
    inputs: list[dict[str, Any]] = []

    for relative, url in SOURCE_FILES.items():
        entry: dict[str, Any] = {
            "path": relative,
            "url": url,
        }

        if source_dir is not None:
            path = source_dir / relative
            entry["present"] = path.is_file()
            if path.is_file():
                entry["sha256"] = file_sha256(path)
        else:
            entry["present"] = False

        inputs.append(entry)

    return {
        "artifact_kind": artifact_kind,
        "generator_command": generator_command,
        "generator_version": GENERATOR_VERSION,
        "unicode_version": UNICODE_VERSION,
        "policy": {
            "east_asian_wide_and_fullwidth": 2,
            "east_asian_ambiguous": 1,
            "combining_marks": 0,
            "format_controls": 0,
            "variation_selectors": 0,
            "emoji_presentation": 2,
            "invalid_utf8_replacement": "U+FFFD width 1",
            "unknown_scalar": 1,
        },
        "inputs": inputs,
    }


def path_text(path: Path) -> str:
    return path.as_posix()


def canonical_generator_command(
    artifact_kind: str,
    source_dir: Path | None,
) -> list[str]:
    command = [
        "tools/unicode_width/generate_unicode_width_tables.py",
    ]

    if artifact_kind == SOURCE_ARTIFACT_KIND and source_dir is not None:
        command.extend(["--source-dir", path_text(source_dir)])

    command.extend([
        "--header",
        "include/vnm_terminal/internal/unicode_width_tables.h",
        "--source",
        "src/unicode_width_tables.cpp",
    ])

    return command


def cpp_hex(value: int) -> str:
    if value <= 0xFFFF:
        return f"0x{value:04X}U"

    return f"0x{value:X}U"


def format_ranges(ranges: tuple[Codepoint_range, ...]) -> str:
    lines = []
    for item in ranges:
        lines.append(f"    {{{cpp_hex(item.first)}, {cpp_hex(item.last)}}},")
    return "\n".join(lines)


def format_manifest_raw_string(manifest: dict[str, Any]) -> str:
    manifest_text = json.dumps(manifest, indent=2, sort_keys=True)
    return f'R"vnmuw(\n{manifest_text}\n)vnmuw"'


def render_header() -> str:
    return """#pragma once

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
"""


def render_source(tables: Unicode_width_tables, manifest: dict[str, Any]) -> str:
    manifest_raw_string = format_manifest_raw_string(manifest)

    return f"""#include "vnm_terminal/internal/unicode_width_tables.h"

#include <cstddef>

namespace vnm_terminal::internal {{
namespace {{

constexpr unicode_width_range_t k_zero_width_ranges[] = {{
{format_ranges(tables.zero_width)}
}};

constexpr unicode_width_range_t k_wide_ranges[] = {{
{format_ranges(tables.wide)}
}};

constexpr unicode_width_range_t k_ambiguous_ranges[] = {{
{format_ranges(tables.ambiguous)}
}};

constexpr unicode_width_range_t k_emoji_presentation_ranges[] = {{
{format_ranges(tables.emoji_presentation)}
}};

constexpr unicode_width_range_t k_emoji_variation_base_ranges[] = {{
{format_ranges(tables.emoji_variation_base)}
}};

template <std::size_t Count>
bool contains_codepoint(
    const unicode_width_range_t (&ranges)[Count],
    char32_t codepoint)
{{
    std::size_t first = 0U;
    std::size_t last  = Count;

    while (first < last) {{
        const std::size_t midpoint = first + (last - first) / 2U;
        const unicode_width_range_t& range = ranges[midpoint];

        if (codepoint < range.first) {{
            last = midpoint;
        }}
        else
        if (codepoint > range.last) {{
            first = midpoint + 1U;
        }}
        else {{
            return true;
        }}
    }}

    return false;
}}

}}

const char* unicode_width_tables_manifest_json()
{{
    return {manifest_raw_string};
}}

bool unicode_width_is_zero_width(char32_t codepoint)
{{
    return contains_codepoint(k_zero_width_ranges, codepoint);
}}

bool unicode_width_is_wide(char32_t codepoint)
{{
    return contains_codepoint(k_wide_ranges, codepoint);
}}

bool unicode_width_is_ambiguous(char32_t codepoint)
{{
    return contains_codepoint(k_ambiguous_ranges, codepoint);
}}

bool unicode_width_is_default_emoji(char32_t codepoint)
{{
    return contains_codepoint(k_emoji_presentation_ranges, codepoint);
}}

bool unicode_width_is_emoji_variation_base(char32_t codepoint)
{{
    return contains_codepoint(k_emoji_variation_base_ranges, codepoint);
}}

}}
"""


def write_or_check_artifact(path: Path | None, text: str, check: bool) -> bool:
    if path is None:
        return True

    if check:
        if not path.is_file():
            print(f"missing generated artifact: {path}")
            return False

        if path.read_text(encoding="utf-8") != text:
            print(f"stale generated artifact: {path}")
            return False

        return True

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return True


def emit_artifacts(
    header: Path | None,
    source: Path | None,
    tables: Unicode_width_tables,
    manifest: dict[str, Any],
    check: bool,
) -> bool:
    ok = True
    ok &= write_or_check_artifact(header, render_header(), check)
    ok &= write_or_check_artifact(source, render_source(tables, manifest), check)
    return ok


def run_self_test() -> int:
    manifest = build_manifest(
        None,
        ["generate_unicode_width_tables.py", "--self-test"],
        AUTHORED_ARTIFACT_KIND)

    for key in [
        "artifact_kind",
        "generator_command",
        "generator_version",
        "inputs",
        "policy",
        "unicode_version",
    ]:
        if key not in manifest:
            print(f"missing manifest key: {key}")
            return 1

    if manifest["generator_version"] != GENERATOR_VERSION:
        print("generator version mismatch")
        return 1

    if manifest["unicode_version"] != UNICODE_VERSION:
        print("Unicode version mismatch")
        return 1

    input_urls = {entry["url"] for entry in manifest["inputs"]}
    if set(SOURCE_FILES.values()) != input_urls:
        print("source URL set mismatch")
        return 1

    policy = manifest["policy"]
    expected_policy = {
        "east_asian_wide_and_fullwidth": 2,
        "east_asian_ambiguous": 1,
        "combining_marks": 0,
        "format_controls": 0,
        "variation_selectors": 0,
        "emoji_presentation": 2,
        "invalid_utf8_replacement": "U+FFFD width 1",
        "unknown_scalar": 1,
    }
    if policy != expected_policy:
        print("policy manifest mismatch")
        return 1

    if AUTHORED_TABLES.wide[0] != Codepoint_range(0x3000, 0x3000):
        print("authored table mismatch")
        return 1

    with tempfile.TemporaryDirectory() as directory:
        missing = check_source_dir(Path(directory))
        if len(missing) != len(SOURCE_FILES):
            print("empty source directory did not fail every required input")
            return 1

    with tempfile.TemporaryDirectory() as directory:
        source_dir = Path(directory)
        (source_dir / "emoji").mkdir()
        (source_dir / "EastAsianWidth.txt").write_text(
            "3000; F\n3400..3401; W\n00A1; A\n", encoding="utf-8")
        (source_dir / "UnicodeData.txt").write_text(
            "0300;COMBINING GRAVE ACCENT;Mn;0;NSM;;;;;N;;;;;\n"
            "200D;ZERO WIDTH JOINER;Cf;0;BN;;;;;N;;;;;\n",
            encoding="utf-8")
        (source_dir / "PropList.txt").write_text(
            "FE00..FE0F ; Variation_Selector\n", encoding="utf-8")
        (source_dir / "emoji" / "emoji-data.txt").write_text(
            "231A..231B ; Emoji_Presentation\n",
            encoding="utf-8")
        (source_dir / "emoji" / "emoji-variation-sequences.txt").write_text(
            "2764 FE0F ; emoji style\n", encoding="utf-8")

        tables = load_tables_from_source(source_dir)
        if Codepoint_range(0x3400, 0x3401) not in tables.wide:
            print("source wide table mismatch")
            return 1
        if Codepoint_range(0xFE00, 0xFE0F) not in tables.zero_width:
            print("source variation selector table mismatch")
            return 1
        if Codepoint_range(0x200D, 0x200D) not in tables.zero_width:
            print("source format control table mismatch")
            return 1
        if Codepoint_range(0x2764, 0x2764) not in tables.emoji_variation_base:
            print("source emoji variation base mismatch")
            return 1
        if Codepoint_range(0x231A, 0x231B) in tables.emoji_variation_base:
            print("source default emoji should not imply variation base")
            return 1

    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-dir", type=Path)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--header", type=Path)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        return run_self_test()

    if args.source_dir is not None:
        errors = check_source_dir(args.source_dir)
        if errors:
            for error in errors:
                print(error)
            return 1

        artifact_kind = SOURCE_ARTIFACT_KIND
        tables = load_tables_from_source(args.source_dir)
    else:
        if args.check:
            parser.error("--check requires --source-dir")

        artifact_kind = AUTHORED_ARTIFACT_KIND
        tables = AUTHORED_TABLES

    manifest = build_manifest(
        args.source_dir,
        canonical_generator_command(artifact_kind, args.source_dir),
        artifact_kind)
    manifest_text = json.dumps(manifest, indent=2, sort_keys=True) + "\n"

    if not emit_artifacts(args.header, args.source, tables, manifest, args.check):
        return 1

    if args.manifest is not None:
        if not write_or_check_artifact(args.manifest, manifest_text, args.check):
            return 1
    elif args.header is None and args.source is None:
        print(manifest_text, end="")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
