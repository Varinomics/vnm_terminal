# vnm_terminal debugging knowledge

This file records durable facts learned during debugging sessions. It is not a
scratch report. Update it when a fact would otherwise be lost across context
compaction or future agent sessions.

## Repository boundaries

- `C:\plms\varinomics\vnm_terminal` is the Qt host application.
- `C:\plms\varinomics\vnm_terminal_surface` is the terminal engine/library used
  by the app.
- `C:\plms\bsd_licensed\vnm_terminal` is not the active application repo for
  Varinomics debugging unless the user explicitly asks to inspect or port from
  it.
- Do not assume fixes made in one of these repositories exist in the others.
  Check the intended repo before editing, building, or launching.

## Current blank-line / scrollback findings

- Plain CRLF blank rows are not currently proven to be the failing mechanism.
  Deterministic tests were added during the investigation to assert that
  explicit CRLF blank rows survive model scrollback movement and public viewport
  projection with retained-row provenance.
- A plain long-output child process creates normal terminal scrollback and mouse
  wheel scrollback works.
- A Codex TUI session on the current post-cleanup baseline did not create normal
  terminal scrollback: transcript diagnostics showed `scrollback_rows=0` for the
  Codex screen while a plain line-output repro did create scrollback.
- Therefore, Codex history scrolling is not the same mechanism as ordinary
  terminal scrollback. Codex mostly repaints a primary-buffer TUI screen.
- The old `recover_scrollback_from_primary_repaints` path synthesized scrollback
  from repaint patterns. It was rejected as heuristic/data-driven and removed
  from `vnm_terminal_surface` in commit `d1d0637`.
- Do not reintroduce repaint-pattern inference, row-content matching, action
  budgets, or Codex/Markdown-specific branches to reconstruct scrollback.
- If Codex-style repaint history is required, design it as an explicit,
  non-heuristic TUI snapshot/history mechanism with structural blank-row
  preservation. Do not pretend it is protocol scrollback unless the terminal
  stream actually scrolls.

## Capture and replay facts

- The normal app build at `build\vnm_terminal.exe` may be transcript-disabled.
  Check `--help` before relying on transcript flags.
- The transcript-enabled build used for diagnostic sessions is:
  `C:\plms\varinomics\vnm_terminal\build_codex_transcript_on\vnm_terminal.exe`.
- Rebuild `build_codex_transcript_on` after changing `vnm_terminal_surface`;
  stale app binaries can contain removed fields such as
  `recover_scrollback_from_primary_repaints` in captured transcripts.
- Use the full Codex command path when launching through `vnm_terminal`:
  `C:\Users\imak\AppData\Roaming\npm\codex.cmd`.
  Relying on `codex` lookup may fail with `CreateProcessW: The system cannot
  find the file specified`.
- Useful capture flags:
  - `--capture-output <path>`
  - `--capture-transcript <path>`
  - `--transcript-snapshot-diagnostics`
- `vnm_terminal_transcript_replay.exe` replays transcript NDJSON, not raw
  backend bytes alone.
- A raw-only artifact is useful evidence, but it is not sufficient for the
  existing transcript replay tool.
- Transcript replay currently checks model/snapshot consistency. It does not by
  itself prove final QSG/window geometry unless additional render-frame or GUI
  diagnostics are present.
- A replay divergence must first be checked against binary/source mismatch
  before being interpreted as a terminal bug.

## Software renderer / custom chrome issue

- The transcript-enabled diagnostic build showed corrupted custom chrome when
  launched with `--software-renderer`.
- The same minimal launch without `--software-renderer` had correct custom
  chrome.
- Do not silently work around this as if it were irrelevant. It is a separate
  concrete bug to investigate later.
- For blank-line capture sessions, avoid `--software-renderer` unless the
  software-renderer bug is the target of the run.

## Debugging policy reminders

- For concrete deterministic failures, first reproduce the exact behavior.
- If the cause is unclear, improve instrumentation before production fixes.
- Do not add pattern-matching, data-driven, or test-tailored fixes.
- A passing similar test is only narrowing evidence; it is not proof that the
  reported bug is fixed or absent.

## Rejected experiment: visible nested ConPTY tee for Codex

A standalone visible ConPTY tee was built and tested as a possible reference path for Codex interaction behavior. It passed a simple `cmd.exe` smoke test, but failed for the intended Codex use case: the session did not behave like normal Codex, right-click paste did not work, and mouse-wheel scrolling did not work.

The reason is architectural: the visible tee creates a nested ConPTY topology, so terminal modes and host UI policies for bracketed paste, mouse tracking, wheel events, focus, alternate screen, synchronized output, scrollback, and selection are not equivalent to either native Windows Terminal or `vnm_terminal`.

Do not use a visible nested ConPTY tee as evidence for Codex mouse, wheel, paste, selection, scrollback, or visual redraw behavior. Debug those issues inside `vnm_terminal`, at the real UI and ConPTY boundaries.
