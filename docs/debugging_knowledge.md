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
  from repaint patterns. It was removed from `vnm_terminal_surface` in commit
  `d1d0637`, then intentionally restored in the primary-backing Phase R work as
  an optional recovery policy on top of the canonical backing model.
- Repaint recovery is not canonical protocol scrollback and must not be used as
  storage evidence for core backing, viewport, resize, selection, or publication
  correctness. Ordinary scrollback must remain feasible with recovery disabled.
- The app flag `--disable-primary-repaint-recovery` disables this optional
  policy for the launched session. New recovery changes need explicit provenance,
  regression tests, and review against the Phase R plan; do not add ad hoc
  Codex/Markdown-specific branches.

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

## Renderer cutover note

- The app no longer exposes a Qt software scene graph launch flag. Renderer
  debugging should use the canonical atlas path and explicit Qt/RHI environment
  controls when hardware backend selection needs to be constrained.

## Clipboard / right-click paste hang

- A `vnm_terminal` window can appear hung after right-click paste when
  `VNM_TerminalSurface::mousePressEvent` synchronously calls
  `QGuiApplication::clipboard()->text()` and the Windows clipboard owner is
  another process that is not servicing OLE clipboard requests.
- One observed case: the requesting packaged app was blocked in
  `QClipboard::text -> ole32!CClipDataObject::GetData` while the clipboard owner
  was another `vnm_terminal.exe` benchmark process stuck during shutdown, with
  the GUI thread joining ConPTY backend worker threads and another thread inside
  `ClosePseudoConsole`.
- In a 2026-06-17 stale packaged-app capture, the outer launcher process was
  only waiting for its runtime child. The runtime had no visible top-level
  windows, no hosted shell/Codex descendants, and was stuck in Qt window
  destruction while joining ConPTY backend threads: one worker was blocked in a
  synchronous `ReadFile`, another waited for reader completion, and a detached
  thread was blocked in `ClosePseudoConsole`. Fixes for this class belong in
  `vnm_terminal_surface/src/windows_conpty_backend.cpp`; do not rely only on
  `std::thread::native_handle()` for MinGW packaged builds when cancelling
  blocking Windows I/O.
- For this class of hang, inspect the clipboard owner with `GetClipboardOwner`
  / `GetWindowThreadProcessId` before assuming the requesting terminal's backend
  or hosted shell is the root cause. Replacing the clipboard contents or ending
  the stale owner can unblock the waiting paste, but do not do that without user
  intent because it changes external state.

## Debugging policy reminders

- For concrete deterministic failures, first reproduce the exact behavior.
- If the cause is unclear, improve instrumentation before production fixes.
- Do not add pattern-matching, data-driven, or test-tailored fixes.
- A passing similar test is only narrowing evidence; it is not proof that the
  reported bug is fixed or absent.
- For live Windows hang captures, prefer non-invasive `cdb` attach with an
  explicit detach:
  `cdb.exe -pv -p <pid> -y "<symbol path>" -c ".reload; ~* kP; qd"`.
  Do not put an unquoted semicolon-separated symbol path at the start of `-c`;
  `.sympath` treats semicolons as symbol path separators and can swallow the
  intended commands. Exiting an invasive attach without `qd` can terminate the
  debuggee instead of leaving the hung process available for another capture.

## Rejected experiment: visible nested ConPTY tee for Codex

A standalone visible ConPTY tee was built and tested as a possible reference path for Codex interaction behavior. It passed a simple `cmd.exe` smoke test, but failed for the intended Codex use case: the session did not behave like normal Codex, right-click paste did not work, and mouse-wheel scrolling did not work.

The reason is architectural: the visible tee creates a nested ConPTY topology, so terminal modes and host UI policies for bracketed paste, mouse tracking, wheel events, focus, alternate screen, synchronized output, scrollback, and selection are not equivalent to either native Windows Terminal or `vnm_terminal`.

Do not use a visible nested ConPTY tee as evidence for Codex mouse, wheel, paste, selection, scrollback, or visual redraw behavior. Debug those issues inside `vnm_terminal`, at the real UI and ConPTY boundaries.
