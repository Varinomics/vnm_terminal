# Conformance Test Setup

This directory contains terminal conformance inputs and harnesses that are
enabled from CMake cache variables in `tests/CMakeLists.txt`. Set variables at
configure time with `-D<name>=<value>`, build the target that CMake creates, and
then run the matching CTest name unless the area is explicitly a manual target.

Examples below use `<build-dir>` and `<config>` placeholders. Omit
`--config <config>` and `-C <config>` for single-config generators.

## CMake Cache Variables

| Variable | Default | Effect |
| --- | --- | --- |
| `VNM_TERMINAL_CAPTURE_REPLAY_DIR` | `tests/conformance/captures` when present | Adds `vnm_terminal_capture_replay_conformance`. |
| `VNM_TERMINAL_FUZZ_SMOKE_CORPUS_DIR` | `tests/conformance/captures` when present | Adds `vnm_terminal_parser_fuzz_smoke`. |
| `VNM_TERMINAL_GRAPHICS_CAPTURE_REPLAY_DIR` | empty | Adds `vnm_terminal_graphics_capture_replay_conformance`. |
| `VNM_TERMINAL_UNICODE_CONFORMANCE_DATA_DIR` | empty | Adds `vnm_terminal_unicode_data_conformance`. |
| `VNM_TERMINAL_UNICODE_ASPIRATIONAL_CONFORMANCE` | `OFF` | Adds `vnm_terminal_unicode_data_aspirational_conformance`. |
| `VNM_TERMINAL_LIBVTERM_INCLUDE_DIR` | empty | Include directory for linked libvterm differential. |
| `VNM_TERMINAL_LIBVTERM_LIBRARY` | empty | Library path for linked libvterm differential. |
| `VNM_TERMINAL_LIBVTERM_CORPUS_DIR` | empty | Corpus directory for linked libvterm differential. |
| `VNM_TERMINAL_LIBVTERM_DIFF_COMMAND` | empty | Adds external command test `vnm_terminal_external_libvterm_diff`. |
| `VNM_TERMINAL_GRAPHICS_CONFORMANCE_COMMAND` | empty | Adds external command test `vnm_terminal_external_graphics_conformance`. |
| `VNM_TERMINAL_EXTERNAL_CONFORMANCE_TIMEOUT` | `300` | Timeout, in seconds, for external command tests. |
| `VNM_TERMINAL_VTTEST_COMMAND` | empty | Adds manual target `vnm_terminal_external_vttest`. |
| `VNM_TERMINAL_BUILD_LIBFUZZER` | `OFF` | Builds `vnm_terminal_parser_libfuzzer` with Clang libFuzzer flags. |

Semicolon-separated command variables are passed to CMake as lists, not through
a shell. Put the executable first and each argument in its own list element.

## Built-In Capture Replay and Fuzz Smoke

The default conformance configuration uses `tests/conformance/captures` for both
capture replay and parser fuzz smoke when that directory exists:

```sh
cmake -S . -B <build-dir>
cmake --build <build-dir> --target vnm_terminal_capture_replay_conformance vnm_terminal_parser_fuzz_smoke --config <config>
ctest --test-dir <build-dir> -C <config> -R "^(vnm_terminal_capture_replay_conformance|vnm_terminal_parser_fuzz_smoke)$" --output-on-failure
```

Use `VNM_TERMINAL_CAPTURE_REPLAY_DIR` and
`VNM_TERMINAL_FUZZ_SMOKE_CORPUS_DIR` to point those tests at another local
corpus.

Parser fuzz smoke scans every regular file in its corpus directory. It decodes
authored `.vnm_capture` and `.vnm_seed` files, and treats files with other
extensions as raw bytes. Because `.raw.expect` sidecars are regular files, keep
them out of a fuzz-smoke corpus unless those expectation bytes are intentionally
part of the fuzz input. Use a separate fuzz corpus when a capture replay corpus
contains raw expectation sidecars.

## Unicode Data Conformance

Configure with a Unicode data directory:

```sh
cmake -S . -B <build-dir> -DVNM_TERMINAL_UNICODE_CONFORMANCE_DATA_DIR=<unicode-data-dir>
cmake --build <build-dir> --target vnm_terminal_unicode_data_conformance --config <config>
ctest --test-dir <build-dir> -C <config> -R "^vnm_terminal_unicode_data_conformance$" --output-on-failure
```

Enable the aspirational pass with:

```sh
cmake -S . -B <build-dir> -DVNM_TERMINAL_UNICODE_CONFORMANCE_DATA_DIR=<unicode-data-dir> -DVNM_TERMINAL_UNICODE_ASPIRATIONAL_CONFORMANCE=ON
ctest --test-dir <build-dir> -C <config> -R "^vnm_terminal_unicode_data(_aspirational)?_conformance$" --output-on-failure
```

The data directory must contain Unicode 16.0.0 and emoji 16.0 files in this
layout:

```text
<unicode-data-dir>/
  vnm_terminal_unicode_data_manifest.json
  EastAsianWidth.txt
  UnicodeData.txt
  PropList.txt
  auxiliary/GraphemeBreakTest.txt
  emoji/emoji-data.txt
  emoji/emoji-variation-sequences.txt
  emoji/emoji-zwj-sequences.txt
```

The manifest must be JSON with `artifact_kind` set to
`vnm_terminal_unicode_conformance_data`, `unicode_version` set to `16.0.0`,
`emoji_version` set to `16.0`, and a `files` array. Each file entry records
the exact `relative_path`, source `url`, and `sha256` for the file bytes.

## Linked libvterm Differential

The linked libvterm harness is enabled only when all three libvterm cache
variables are set:

```sh
cmake -S . -B <build-dir> -DVNM_TERMINAL_LIBVTERM_INCLUDE_DIR=<libvterm-include-dir> -DVNM_TERMINAL_LIBVTERM_LIBRARY=<libvterm-library> -DVNM_TERMINAL_LIBVTERM_CORPUS_DIR=<corpus-dir>
cmake --build <build-dir> --target vnm_terminal_libvterm_differential --config <config>
ctest --test-dir <build-dir> -C <config> -R "^vnm_terminal_libvterm_differential$" --output-on-failure
```

The linked corpus accepts `.raw` and `.vnm_capture` files.

## External libvterm Command Hook

Use the external hook when the differential runner lives outside this build:

```sh
cmake -S . -B <build-dir> -DVNM_TERMINAL_LIBVTERM_DIFF_COMMAND="<command>;<arg1>;<arg2>"
ctest --test-dir <build-dir> -C <config> -R "^vnm_terminal_external_libvterm_diff$" --output-on-failure
```

The command owns its own setup, inputs, and output policy. CTest only executes
it and applies `VNM_TERMINAL_EXTERNAL_CONFORMANCE_TIMEOUT`.

## vttest Manual Target

`vttest` is an interactive/manual target, not a CTest test:

```sh
cmake -S . -B <build-dir> -DVNM_TERMINAL_VTTEST_COMMAND="<vttest-command>"
cmake --build <build-dir> --target vnm_terminal_external_vttest --config <config>
```

The target launches `vnm_terminal_example_terminal` with the configured command
inside it.

## Graphics Conformance

Graphics capture replay uses the same replay harness as text captures, under a
separate CTest name:

```sh
cmake -S . -B <build-dir> -DVNM_TERMINAL_GRAPHICS_CAPTURE_REPLAY_DIR=<graphics-capture-dir>
cmake --build <build-dir> --target vnm_terminal_capture_replay_conformance --config <config>
ctest --test-dir <build-dir> -C <config> -R "^vnm_terminal_graphics_capture_replay_conformance$" --output-on-failure
```

For a fully external graphics runner:

```sh
cmake -S . -B <build-dir> -DVNM_TERMINAL_GRAPHICS_CONFORMANCE_COMMAND="<command>;<arg1>;<arg2>"
ctest --test-dir <build-dir> -C <config> -R "^vnm_terminal_external_graphics_conformance$" --output-on-failure
```

## libFuzzer Mode

The libFuzzer target requires a Clang C++ compiler. Configure and build it with:

```sh
cmake -S . -B <build-dir> -DVNM_TERMINAL_BUILD_LIBFUZZER=ON
cmake --build <build-dir> --target vnm_terminal_parser_libfuzzer --config <config>
```

Run the produced `vnm_terminal_parser_libfuzzer` executable with a local corpus,
and normal libFuzzer options such as `-runs=0` for a corpus-only check. The
libFuzzer executable passes each input file's raw bytes directly to the parser;
it does not decode `.vnm_capture` or `.vnm_seed` fixture syntax. Use a
libFuzzer corpus made of raw terminal byte streams or decoded files generated
from authored fixtures. Authored fixture decoding is part of parser fuzz smoke
mode.

## Capture File Formats

`.raw` files are byte-for-byte terminal input streams. Capture replay can pair a
raw stream with a sidecar expectation file named `<capture>.raw.expect`.

`.vnm_fixture`, `.vnm_capture`, and `.vnm_seed` files are text fixtures for
independently authored byte streams. They carry provenance headers,
`key = value` fields, and expectations in the same file. The parser ignores
blank lines and `#` comments outside quoted values. Quoted values are accepted
by removing the surrounding quotes.

Parser fuzz smoke also accepts `.vnm_seed` files with the same authored
byte-stream format. `.vnm_fixture` files are used for parser/action fixture
checks. Capture replay and linked libvterm discovery are limited to `.raw` and
`.vnm_capture`.

Authored byte-stream fixtures require this header:

```text
# vnm-fixture-format: 1
# vnm-provenance-origin: independently-authored
# vnm-provenance-license: BSD-3-Clause
# vnm-provenance-generator: <non-empty value>
# vnm-provenance-independent: true
# vnm-provenance-reviewer: <non-empty value>
```

Each required provenance key appears once. `.vnm_capture` files must include
`byte_stream_hex`, whose hex digits are converted to the replayed byte stream.

`byte_stream_hex` is space-separated byte text. Parser/action fixtures may also
use these fields:

| Key | Meaning |
| --- | --- |
| `expected_screen_cells` | Semicolon-separated `row,column,text` records. |
| `terminal_replies` | Semicolon-separated terminal reply records. |
| `diagnostics` | Semicolon-separated parser diagnostic records. |

Empty parser/action fixture lists are encoded as an empty quoted string.

Supported expectation keys are:

| Key | Meaning |
| --- | --- |
| `max_diagnostics` | Maximum parser diagnostics; defaults to `0`. |
| `min_scrollback_rows` | Minimum scrollback rows; disabled when unset. |
| `visible_contains` | Literal text expected in the visible screen. |
| `visible_excludes` | Literal text forbidden from the visible screen. |
| `title_contains` | Literal text expected in the terminal title. |
| `visible_contains_utf8_hex` | UTF-8 byte form of `visible_contains`. |
| `visible_excludes_utf8_hex` | UTF-8 byte form of `visible_excludes`. |
| `title_contains_utf8_hex` | UTF-8 byte form of `title_contains`. |
| `check_dirty_text_coverage` | When `true`, chunked replay checks that visible text row changes publish dirty rows. |
| `check_dirty_cell_coverage` | When `true`, chunked replay checks that visible cell, style, and color changes publish dirty rows. |

The UTF-8 hex expectation keys are preferred for non-ASCII text. The replay
harness runs every capture as one complete byte stream and as deterministic
chunks, then requires equivalent visible text, title, icon name, cursor, and
scrollback state.

## External Tool Policy

Strong-copyleft conformance tools may be installed and run locally when a
developer needs them. Optional strong-copyleft runners belong under ignored
`more_tests/`, which is included by CMake only when a local
`more_tests/CMakeLists.txt` exists. Do not check in their source, imported
external test names, output, transcripts, byte streams, or golden files. Tool
names such as `esctest`, `vttest`, and `libvterm` may be used when documenting
or configuring local runners. Checked-in fixtures must remain independently
authored and carry the provenance header above.
