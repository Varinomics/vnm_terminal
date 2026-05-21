# LLM Orientation

This document is the shortest stable map for a first-time LLM or engineer.
Read it first when entering the repository.

## Level 0: What This Is

`vnm_terminal` is a BSD-licensed Qt Quick terminal component. It embeds
interactive terminal-canvas applications inside Varinomics products.

The public UI component is `VNM_TerminalSurface`, a C++ `QQuickItem`. The
surface starts a platform terminal backend, receives PTY or ConPTY output,
parses terminal byte streams, updates a terminal screen model, and renders the
visible terminal with Qt Scene Graph nodes.

The target behavior is a real interactive terminal, not a log viewer. Alternate
screen, cursor addressing, scrollback, resize propagation, keyboard modes, mouse
reporting, paste modes, selection, clipboard policy, and terminal replies are
part of the product shape.

## Level 1: Where Things Are

- `include/vnm_terminal/vnm_terminal_surface.h` is the public component API.
- `src/vnm_terminal_surface.cpp` bridges Qt events, process lifecycle, session
  notifications, and rendering.
- `include/vnm_terminal/internal/terminal_session.h` and
  `src/terminal_session.cpp` own the ordered terminal session pipeline.
- Backend selection happens in `src/vnm_terminal_surface.cpp`; the selected
  backend implementation then hosts the child process.
- `src/windows_conpty_backend.cpp` implements Windows ConPTY hosting.
- `src/linux_pty_backend.cpp` implements Linux PTY hosting.
- `src/native_backend_io_core.*` holds platform-neutral native-backend helpers
  such as start preparation, write-queue accounting, callback forwarding,
  paused-output buffering, and worker-thread join behavior.
- `src/terminal_screen_model.cpp` owns terminal cell state, scrollback,
  alternate screen, modes, and escape-sequence effects.
- `src/terminal_input_encoder.cpp` maps Qt input events to terminal byte
  sequences.
- `src/qsg_terminal_renderer.cpp` converts render snapshots into Qt Scene Graph
  nodes.
- `examples/terminal_app` is the runnable terminal host used for manual checks
  and smoke tests.
- `tools/terminal_canvas_fixture` builds `vnm_terminal_canvas_fixture`, a small
  scripted child process used by automated terminal behavior tests.
- `tests` contains contract, model, backend, renderer, host, conformance,
  randomization, and lifecycle tests.

The root `CMakeLists.txt` is also a useful map: it lists the core library
headers and sources together, then conditionally adds the Windows ConPTY backend,
Linux PTY backend, example app, tests, and benchmarks.

## Level 2: How The Runtime Fits Together

The normal path is:

```text
QQuickWindow / host app
    -> VNM_TerminalSurface
    -> Terminal_session
    -> Terminal_backend
       -> child process attached to ConPTY or PTY

backend output bytes
    -> Terminal_session
    -> parser action IR
    -> Terminal_screen_model and viewport state
    -> immutable render snapshot
    -> QSG renderer
    -> visible terminal

Qt key / mouse / paste / IME events
    -> VNM_TerminalSurface
    -> Terminal_input_encoder
    -> Terminal_session write queue
    -> same backend write path as terminal replies
```

`Terminal_session` is the ordering boundary. Lifecycle commands, backend output,
terminal replies, input writes, resize commands, backend errors, and process
exit notifications pass through one ordered session pipeline.

## Level 3: Ownership Rules

- GUI-thread state is owned by `VNM_TerminalSurface` and `Terminal_session`.
- Backend worker threads may perform platform IO but do not mutate parser,
  screen, viewport, selection, input, or render state directly.
- The renderer consumes immutable snapshots. It does not reach back into mutable
  model state.
- Parser-originated terminal replies use the same backend write queue as user
  input.
- Resize is a transaction: the surface resolves grid metrics, the session
  updates model geometry, and the backend receives the resize in command order.
- Output and write paths are bounded. Overflow is reported as an explicit error.

## Level 4: Task Entry Points

- To understand how hosts use the component, start with
  `include/vnm_terminal/vnm_terminal_surface.h`, then read
  `examples/terminal_app/main.cpp`.
- To change process hosting, start with `include/vnm_terminal/internal/backend_contract.h`,
  then `src/native_backend_io_core.*` for shared backend mechanics, and finally
  the platform backend in `src/windows_conpty_backend.cpp` or
  `src/linux_pty_backend.cpp`.
- To change terminal parsing or screen behavior, start with
  `include/vnm_terminal/internal/parser_action.h`,
  `include/vnm_terminal/internal/terminal_screen_model.h`, and
  `src/terminal_screen_model.cpp`.
- To change input behavior, start with
  `include/vnm_terminal/internal/terminal_input_encoder.h` and
  `src/terminal_input_encoder.cpp`.
- To change rendering, start with
  `include/vnm_terminal/internal/render_snapshot.h`,
  `include/vnm_terminal/internal/qsg_terminal_render_frame.h`, and
  `src/qsg_terminal_renderer.cpp`.
- To add or update fixtures, start with
  `tools/terminal_canvas_fixture/terminal_canvas_fixture.cpp` and
  the relevant tests under `tests`. The canvas fixture is an executable child
  process, not just fixture data.

## Level 5: What To Read Next

- Use [Architecture](architecture.md) for the runtime model and component
  boundaries.
- Use [Public surface](public_surface.md) when integrating or changing
  `VNM_TerminalSurface`.
- Use [Repository guide](repository_guide.md) when building, testing, or looking
  for files.
- Use [Terminal sequence matrix](terminal_sequence_matrix.md) for terminal
  escape-sequence behavior.
- Use [Conformance oracles](terminal_conformance_oracles.md) and
  [reference inventory](terminal_reference_inventory.md) for licensing and
  reference-source boundaries.
- Use [Unicode width policy](unicode_width_policy.md) for terminal cell-width
  behavior.
- Use [Qt rendering policy](qt_rendering_policy.md) for Qt dependency and
  renderer API constraints.
