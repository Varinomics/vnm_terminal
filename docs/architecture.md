# Architecture

`vnm_terminal` is a Qt Quick terminal component built around one public Qt item,
one ordered session, a platform terminal backend, parser/model state, and an
immutable render snapshot path.

The runtime shape is:

```text
host / QQuickWindow
    -> VNM_TerminalSurface
    -> Terminal_session
    -> Terminal_backend
    -> child process attached to ConPTY or PTY

backend output bytes
    -> Terminal_session
    -> Terminal_byte_stream_parser
    -> Terminal_screen_model
    -> Terminal_render_snapshot
    -> Terminal_render_frame
    -> Qsg_terminal_renderer
    -> Qt Scene Graph nodes

Qt input / paste / IME / focus
    -> VNM_TerminalSurface
    -> Terminal_input_encoder
    -> Terminal_session write command
    -> Terminal_backend::write
```

## Public Surface Boundary

`VNM_TerminalSurface` is the host-facing API. It is declared in
`include/vnm_terminal/vnm_terminal_surface.h` and implemented in
`src/vnm_terminal_surface.cpp`.

Hosts configure the item through Qt properties, call invokable methods for
terminal operations, and observe Qt signals for runtime state. The detailed host
contract is described in [Public surface](public_surface.md). The public launch
API accepts an argument vector and working directory; shell string parsing is
not part of the surface contract.

The surface owns Qt integration. It handles `QQuickItem` event overrides,
geometry and screen changes, focus, IME, window binding, scene-graph invalidation,
and `updatePaintNode`. It does not expose parser, model, backend, or renderer
objects to hosts.

`include/vnm_terminal/internal` contains implementation contracts and test
contracts. `VNM_TerminalSurface_render_bridge` is an internal render/test
handoff, not a public host extension point.

Look at:

- `include/vnm_terminal/vnm_terminal_surface.h` for the public API.
- `src/vnm_terminal_surface.cpp` for Qt event handling, backend selection, and
  session/signal bridging.
- `examples/terminal_app` for a host that embeds the item.

## Session Ordering Boundary

`Terminal_session` is the central ordering boundary. It is declared in
`include/vnm_terminal/internal/terminal_session.h`; command, result,
notification, queue, resize, and bell contracts live in
`include/vnm_terminal/internal/session_contract.h`; implementation is in
`src/terminal_session.cpp`.

The session owns the backend pointer and terminal-domain state behind the
surface: screen model, viewport controller, selection state, IME preedit state,
queue state, render snapshot handles, process state, backend sync state, and
diagnostic traces. Public session operations drain backend callback ingress,
assign a sequence, enqueue a `Terminal_session_command`, process pending
commands, and capture the result for that sequence.

The command stream covers start, interrupt, terminate, force-release of
synchronized output, backend output, backend exit, backend errors, resize, user
writes, paste writes, and parser-generated terminal replies. Parser replies use
the same write command path as user input, so the child process observes replies
and input in session order.

Backend callbacks initially enter a lifetime-owned callback queue. The surface
configures `Terminal_session_config::backend_event_notifier` so backend worker
threads only queue work and wake the GUI thread. Tests may omit the notifier,
which makes callback delivery synchronous through the same session methods.

Look at:

- `include/vnm_terminal/internal/terminal_session.h` for the session API.
- `include/vnm_terminal/internal/session_contract.h` for command, queue, resize,
  result, and notification contracts.
- `src/terminal_session.cpp` for command processing, backend callback ingress,
  parser action handling, and snapshot publication.

## Backend Boundary

`Terminal_backend` is the process-hosting abstraction in
`include/vnm_terminal/internal/backend_contract.h`. A backend accepts `start`,
`write`, `resize`, `set_output_paused`, `interrupt`, and `terminate` requests,
and reports output bytes, process exit, and errors through callbacks.

The contract also owns launch validation, environment construction, process
state, exit reason, backend error codes, termination policy, and effective
terminal identity (`TERM` and `COLORTERM`). Backends return explicit
`Terminal_backend_result` values; failures do not fall back to a pipe-like
transport.

Native backend selection happens in `make_native_backend()` in
`src/vnm_terminal_surface.cpp`.

- Windows uses ConPTY in `src/windows_conpty_backend.cpp`.
- Linux uses PTY APIs in `src/linux_pty_backend.cpp`.
- Shared native-backend mechanics live in `src/native_backend_io_core.*`.

The supported native product targets are Windows x64 and Linux x86_64. A build
on a platform without a native backend may still compile library code, but
process launch reports an unavailable-backend error instead of claiming terminal
support.

Platform backends own OS handles, process lifetime, worker threads, write
queues, resize calls, output pause/resume behavior, interrupt delivery, and
termination escalation. They do not parse terminal bytes, mutate screen state,
or touch Qt Scene Graph state.

`src/native_backend_io_core.*` contains platform-neutral helper code used by
both native backends. This includes write-queue byte accounting, shared IO
limits, callback delivery, paused-output accumulation, start precondition and
effective-launch preparation, start-rejection reporting, and join-or-detach
worker-thread cleanup. It deliberately does not own child-process handles,
platform IO calls, signal delivery, process waiting, or termination policy
execution.

Look at:

- `include/vnm_terminal/internal/backend_contract.h` for the backend API.
- `src/native_backend_io_core.*` for shared native-backend mechanics.
- `src/windows_conpty_backend.cpp` for ConPTY process hosting.
- `src/linux_pty_backend.cpp` for PTY process hosting.

## Parser And Model Boundary

The parser/model layer is centered on
`include/vnm_terminal/internal/terminal_screen_model.h`,
`include/vnm_terminal/internal/parser_action.h`, and
`src/terminal_screen_model.cpp`.

The byte-stream parser is owned in-tree. `Terminal_byte_stream_parser` converts
backend byte streams into `Parser_action` IR. The IR separates printable text,
screen mutations, SGR/style mutations, control sequences, terminal replies,
terminal queries, diagnostics, notifications, and host requests. Parser limits
for OSC, DCS, APC, PM, SOS, CSI pending data, and title payloads are declared
with the parser action contract.

The screen-model ingestion path runs the parser, applies actions to the model,
collects dirty rows and viewport changes, and returns a
`Terminal_screen_model_result` to the session. The model owns primary and
alternate buffers, scrollback, cursor state, tab stops, modes, SGR styles,
colors, title/icon state, hyperlinks, wide and combining cell state,
synchronized output state, and render snapshot production.

Terminal cell widths come from generated Unicode 16.0.0 tables in
`unicode_width_tables`. Ambiguous East Asian Width characters are narrow,
combining marks and variation selectors are zero-width, default emoji
presentation is two cells, and invalid UTF-8 is replaced with U+FFFD at width 1.
The full width policy is in [Unicode width policy](unicode_width_policy.md).

The parser does not render. The renderer does not mutate parser or model state.
Terminal replies and host requests produced by parsing are surfaced back to the
session through parser actions.

Look at:

- `include/vnm_terminal/internal/parser_action.h` for parser action IR and
  payload limits.
- `include/vnm_terminal/internal/terminal_screen_model.h` for parser/model
  types and screen-model API.
- `src/terminal_screen_model.cpp` for byte ingestion, action application, and
  render snapshot production.

## Input Path

Qt input starts at `VNM_TerminalSurface` event handlers in
`src/vnm_terminal_surface.cpp`. The surface handles host policy first: copy
shortcuts, selection gestures, right-click paste, wheel zoom, local scrollback,
alternate-screen wheel routing, focus reports, and IME preedit tracking.

Terminal bytes are encoded by
`include/vnm_terminal/internal/terminal_input_encoder.h` and
`src/terminal_input_encoder.cpp`. The encoder maps key, mouse, paste, and focus
behavior from Qt events plus `Terminal_input_mode_state`. That mode state comes
from the screen model and includes application cursor keys, keypad mode,
bracketed paste, focus reporting, mouse tracking, SGR mouse encoding, and
alternate-scroll mode.

IME preedit is surface/session state used for rendering the composition overlay.
IME commit text is UTF-8 encoded and written through the same session write path
as ordinary user input. Public paste uses the session paste-write path with the
surface bracketed-paste policy.

Look at:

- `src/vnm_terminal_surface.cpp` for Qt key, mouse, wheel, paste, focus, and IME
  event handling.
- `include/vnm_terminal/internal/terminal_input_encoder.h` for input event
  contracts.
- `src/terminal_input_encoder.cpp` for key, mouse, paste, and focus encoding.
- `src/terminal_session.cpp` for the ordered write path into the backend.

## Render Snapshot And QSG Path

Rendering is snapshot based and has one public renderer contract: immutable
snapshots flow through render frames into the QSG renderer. Internal packing and
cache choices do not create alternate renderer APIs.

The model creates `Terminal_render_snapshot` objects defined in
`include/vnm_terminal/internal/render_snapshot.h`. A snapshot owns grid size,
viewport, color metadata, style table, cells, dirty row ranges, hyperlink
metadata, cursor, IME preedit, selection spans, metadata, and terminal modes.
Snapshot cell vector entries are positioned. The producer currently emits them
in row-major order, but row-major production order is not the architecture
contract.

The session stores the latest snapshot as a
`std::shared_ptr<const Terminal_render_snapshot>` and increments the snapshot
generation. The surface sync path copies the shared handle into the surface
through `VNM_TerminalSurface_render_bridge` and schedules a Qt Quick update.

The surface's `updatePaintNode` override builds a `Terminal_render_frame` with
the frame-building path in `src/qsg_terminal_renderer.cpp`. The frame owns
per-frame vectors of rects, arcs, text runs, cursor primitives, decorations,
overlays, dirty row ranges, and packed row/text/graphic sidecars. The QSG
renderer turns that frame into reusable Qt Scene Graph nodes.

`Terminal_render_frame::text_runs` is the canonical renderer input for terminal
text. `Terminal_render_frame::cursor_text_runs` carries cursor inverse-text
overlay input. Packed text sidecars are auxiliary diagnostics, accounting,
classification, and cache data; they do not own text and do not replace
`frame.text_runs`. Packed row and graphic sidecars may support graphics-route
grouping and row identity.

`Terminal_scene_node` owns renderer-local reusable row caches keyed by active
buffer, logical row, and exact content or layer descriptors. Clean-row reuse is
an internal optimization and must respect dirty rows, viewport identity,
geometry, style/color state, and descriptor equality.

Text rendering uses public Qt APIs, including `QTextLayout` and `QSGTextNode`,
through the allowed Qt Core, Gui, and Quick modules. The production renderer
does not paint terminal rows or frames into `QImage` with `QPainter`, and it
does not allocate one child `QQuickItem` per cell. The detailed constraints are
in [Qt rendering policy](qt_rendering_policy.md).

Cell ownership is model-owned. Qt owns glyph shaping, fallback, text-node,
material, and render-thread details behind public APIs. Those details do not
redefine terminal cell identity.

Look at:

- `include/vnm_terminal/internal/render_snapshot.h` for snapshot invariants.
- `include/vnm_terminal/internal/qsg_terminal_render_frame.h` for render-frame
  primitives.
- `src/qsg_terminal_renderer.cpp` for frame building and QSG node updates.
- `src/vnm_terminal_surface.cpp` for Qt scene graph lifecycle integration.

## Threading And Ownership

The surface asserts GUI-thread access for public item operations. In normal
surface use, the GUI thread owns `VNM_TerminalSurface`, `Terminal_session`,
parser/model state, viewport state, selection state, input state, IME preedit
state, and snapshot publication.

Backend worker threads own platform IO and process waiting. They enqueue output,
exit, and error commands through the session callback lifetime and wake the
surface through a queued Qt invocation. They do not mutate parser, model,
viewport, selection, or render state directly.

Qt calls `updatePaintNode` on the scene graph update path. That path consumes
immutable snapshot handles and renderer-local resources. Renderer diagnostics and
lifecycle recorders use small mutex-protected publishers for cross-thread test
and diagnostic reads.

Look at:

- `src/vnm_terminal_surface.cpp` for GUI-thread assertions and scene graph
  integration.
- `src/terminal_session.cpp` for backend callback lifetime and session ingress.
- `src/windows_conpty_backend.cpp` and `src/linux_pty_backend.cpp` for backend
  worker-thread setup.
- `src/qsg_terminal_renderer.cpp` for renderer diagnostics and lifecycle
  publishing.

## Resize, Backpressure, And Error Policy

Resize starts at the surface. Font, device-pixel-ratio, geometry, window, and
screen changes call `refresh_grid_metrics`. `Qt_grid_metrics_provider` computes
cell metrics and grid size from the current font and item geometry.
`Terminal_resize_controller` turns geometry into the initial launch grid or a
session resize command.

A session resize validates the target grid against model limits, resizes the
model, forwards the resize to the backend, records a
`Terminal_resize_transaction`, updates `backend_geometry_in_sync`, and publishes
a snapshot when visible state or geometry metadata changed. While synchronized
output blocks content publication, the session can publish a geometry-only
snapshot so the surface stays sized to the backend geometry.

Output and write queues are bounded by `Terminal_queue_limits`. Output high
water activates backpressure through `Terminal_backend::set_output_paused` and
emits `output_backpressure_changed`. Output hard-limit overflow records an
`OUTPUT_OVERFLOW` error, stops further backend output delivery, and requests
backend termination. Write hard limits reject the write or paste with an
explicit error.

Error handling is explicit. Invalid launch configuration, invalid metrics,
missing native backend support, backend start/write/resize/interrupt/terminate
failures, callback contract failures, read failures, and overflow produce
`Terminal_backend_error` values. The session records them in results or
notifications, and the surface emits `backend_error`.

Look at:

- `src/qt_grid_metrics_provider.cpp` and
  `src/terminal_resize_controller.cpp` for grid and resize calculation.
- `src/terminal_session.cpp` for resize transaction handling, backpressure,
  overflow, and session error propagation.
- `src/vnm_terminal_surface.cpp` for host-visible backend error reporting.
