#!/usr/bin/env python3
"""Continuously write numbered lines with random text and an exact width.

Usage:
    python tools/emit_counted_lines.py COLUMNS [--count COUNT]

Without --count, the script continues until interrupted with Ctrl+C.
"""

from __future__ import annotations

import argparse
import random
import string
import sys


RANDOM_TEXT_ALPHABET = string.ascii_letters + string.digits + " .,:;!?-_+/"
RANDOM = random.Random()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Write counted ASCII lines filled with random text to an exact width."
    )
    parser.add_argument(
        "columns",
        type=int,
        help="number of columns in every emitted line",
    )
    parser.add_argument(
        "--count",
        type=int,
        default=0,
        help="emit this many lines; zero means continue until Ctrl+C",
    )
    args = parser.parse_args()
    if args.columns < 1:
        parser.error("columns must be at least 1")
    if args.count < 0:
        parser.error("count must not be negative")
    return args


def main() -> int:
    args = parse_args()
    line_number = 1

    try:
        while args.count == 0 or line_number <= args.count:
            label = str(line_number)
            if len(label) > args.columns:
                label = label[-args.columns :]
            random_text_length = max(0, args.columns - len(label) - 1)
            random_text = "".join(
                RANDOM.choices(RANDOM_TEXT_ALPHABET, k=random_text_length)
            )
            separator = " " if random_text_length > 0 else ""
            line = label + separator + random_text
            sys.stdout.write(line + "\n")

            # Flush regularly so the terminal sees progress without imposing a
            # flush syscall on every line.
            if line_number % 256 == 0:
                sys.stdout.flush()
            line_number += 1
    except KeyboardInterrupt:
        pass

    sys.stdout.flush()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
