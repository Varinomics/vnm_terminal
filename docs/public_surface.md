# Public Surface

`VNM_TerminalSurface` is the public Qt Quick terminal item. It is declared in
`include/vnm_terminal/vnm_terminal_surface.h` and implemented by
`src/vnm_terminal_surface.cpp`.

Hosts use the surface as a C++ `QQuickItem`: create it under a
`QQuickWindow` content item or expose it to QML, size it, configure the Qt
properties, connect the signals that matter to the host, then call
`start_process()`. The terminal pipeline itself is C++ code, not a tree of QML
controls.

## Host Contract

The surface owns the Qt-facing boundary for one terminal session:

- maps Qt key, mouse, wheel, hover, focus, IME, geometry, window, screen, and
  scene-graph events into terminal-domain operations;
- creates the native terminal backend used by the session;
- publishes terminal metadata, process state, backend state, grid geometry,
  viewport state, and selection state as read-only Qt properties;
- routes host operations such as paste, selection access, viewport scrolling,
  interrupt, terminate, and OSC 52 clipboard decisions into the same ordered
  session pipeline as backend output;
- renders immutable session snapshots through Qt Scene Graph nodes in
  `updatePaintNode()`.

The GUI thread owns the public surface and the session state behind it. Backend
threads report output, exit, and errors back through queued session callbacks.

## Configuration Properties

Writable Qt properties:

- `fontFamily` and `fontSize` select the terminal font. Positive font sizes are
  rounded to an integer pixel size. Font, device-pixel-ratio, and item geometry
  changes recompute terminal grid metrics.
- `colorTheme` selects the render theme. The renderer treats `light`
  case-insensitively as the light palette; other names use the default palette.
  The example accepts `default` and `light`.
- `cursorStyle` is `BLOCK`, `BAR`, or `UNDERLINE`.
- `cursorBlinkEnabled` enables the render-side blink override.
- `scrollbackLimit` clamps to zero or greater and is applied to a live session.
- `synchronizedOutputStaleTimeoutMs` clamps to at least one millisecond and is
  used to release synchronized-output mode if the application leaves output
  hidden.
- `mouseReportingPolicy` is `APPLICATION_CONTROLLED` or `DISABLED`. With the
  application-controlled policy, mouse-reporting terminal modes receive mouse
  input. Holding Shift forces local selection when no terminal mouse grab is
  active.
- `copyShortcutPolicy` controls plain Ctrl+C:
  `TERMINAL_INPUT` always sends terminal input,
  `COPY_SELECTION_OR_TERMINAL_INPUT` copies a local selection when present and
  otherwise sends terminal input, and `COPY_SELECTION_OR_IGNORE` never sends
  Ctrl+C to the child process.
- `wheelEventPolicy` controls primary-screen wheel routing:
  `APPLICATION_CONTROLLED` tries terminal mouse or alternate-screen behavior
  before local scrollback, `LOCAL_SCROLLBACK_FIRST` prefers local scrollback
  when it can move, and `LOCAL_SCROLLBACK_ONLY` does not route ordinary wheel
  events to the child process.
- `alternateScreenWheelPolicy` controls wheel events in the alternate screen:
  `MOUSE_REPORTING_FIRST` lets mouse-reporting applications consume wheel
  events before key fallback, `CURSOR_KEYS` maps wheel movement to Up and Down
  key input, and `PAGE_KEYS` maps it to PageUp and PageDown key input.
- `bracketedPastePolicy` is `DISABLED`, `APPLICATION_CONTROLLED`, or `ENABLED`.
  Application-controlled paste frames pasted text only when the terminal has
  enabled bracketed paste mode.
- `audibleBellPolicy` and `visualBellPolicy` enable or disable the corresponding
  bell effects.

`backend_output_capture_path()` and `set_backend_output_capture_path()` are C++
diagnostic accessors rather than Qt properties. When set before process start,
the session appends raw backend output bytes to that path.

## Published State

Read-only Qt properties expose the host-visible state:

- `terminalTitle` and `terminalIconName` come from terminal metadata updates.
- `processState` is `NOT_STARTED`, `STARTING`, `RUNNING`, `EXITED`, or
  `FAILED`.
- `backendReady` reports whether the backend accepted the active launch.
- `backendGeometryInSync` reports whether the backend has acknowledged the
  current terminal grid size.
- `rows` and `columns` are the grid size derived from item geometry and font
  metrics.
- `scrollbackRows`, `viewportVisibleRows`, `viewportOffsetFromTail`, and
  `viewportAtTail` describe the published primary-screen viewport.
- `selectionState` is `NONE` or `ACTIVE`.

Every property has a matching notify signal. Grid size changes emit
`grid_geometry_changed()`. Viewport changes emit `viewport_changed()`. Backend
resize synchronization changes emit `geometry_sync_changed()`.

## Process Launch

`start_process(QStringList argv, QString working_directory = {})` starts one
native terminal backend:

- Windows uses ConPTY.
- Linux uses PTY APIs.
- Platforms without a native backend fail with `backend_error()`.

The launch contract is an argument vector. `argv[0]` must name the executable;
the surface does not parse shell command strings. An empty vector, an empty
program name, an unavailable working directory, an invalid initial grid, backend
startup failure, or a second start while a process is live returns `false` and
reports a typed backend error. Starting again after a process has exited resets
the old session before launching the new one.

Initial rows and columns come from the item size, font metrics, and device pixel
ratio. Hosts should size the item before launch. Later geometry, font, screen,
or device-pixel-ratio changes refresh the grid and send ordered resize requests
through the session.

`interrupt_process()` and `terminate_process()` forward lifecycle requests to
the active session. They return `false` and emit a backend error if there is no
active session or if the backend rejects the request. Destroying a surface with a
live process requests termination during teardown.

## Host Operations

Invokable methods:

- `selected_text()` returns the local terminal selection. When synchronized
  output is hiding unpublished model changes, it reads from the visible render
  snapshot so host copy behavior matches what the user can see.
- `clear_selection()` clears local selection and drag state.
- `paste_text(QString text)` writes paste text through the terminal input path.
  Paste text is UTF-8 encoded, CR and CRLF are normalized to LF, C0/C1 controls
  other than LF and TAB are removed, and bracketed-paste framing follows
  `bracketedPastePolicy`.
- `scroll_viewport_lines(int line_delta)` moves the published primary-screen
  scrollback viewport by lines.
- `scroll_to_offset_from_tail(int offset_from_tail)` moves the published
  primary-screen scrollback viewport to an offset from the tail.
- `respond_clipboard_write(quint64 request_id, Clipboard_response_decision
  decision)` answers a pending OSC 52 write request.

Viewport operations return `true` only when the visible viewport actually moves.
They return `false` for no session, zero or no-op movement, alternate-screen
state, hidden synchronized output, or an already-boundary request. Large offset
requests may clamp to the available scrollback and still return `true` if the
viewport moved.

## Clipboard Policy

There are three clipboard paths:

- Local copy uses the host clipboard directly when `copyShortcutPolicy` chooses
  to copy a local selection.
- Host paste calls `paste_text()` directly. The example maps Ctrl+V and
  Ctrl+Shift+V to clipboard paste through this method; the surface also handles
  right-click paste when a session accepts the paste.
- OSC 52 clipboard writes are mediated by the host. The parser decodes the
  payload and the surface emits
  `clipboard_write_requested(request_id, target_selection, payload)`. The host
  must call `respond_clipboard_write()` with the matching request id.

`DENY` consumes the pending request and leaves the clipboard unchanged. `ALLOW`
writes the decoded payload to `QClipboard::Clipboard` only for target `c` or
`clipboard`. Wrong ids, stale ids, duplicate responses, and attempts to allow an
unsupported target return `false` and report `CALLBACK_MISSING`.

## Runtime Signals

Process and runtime signals:

- `process_started()`;
- `process_exited(Exit_reason reason, int exit_code)`;
- `backend_error(Backend_error_code code, QString message)`;
- `output_activity()`;
- `output_backpressure_changed(bool active)`;
- `bell_requested()`;
- `text_area_resize_requested(int rows, int columns)`;
- `clipboard_write_requested(quint64 request_id, QString target_selection,
  QByteArray payload)`.

`Exit_reason` is `EXITED`, `INTERRUPTED`, `TERMINATED`, or
`FAILED_TO_START`. `Backend_error_code` mirrors backend and session failure
classes such as invalid launch config, unavailable working directory, start,
write, resize, interrupt, terminate, output overflow, callback, and read
failures.

`text_area_resize_requested()` is emitted for `CSI 8 ; rows ; columns t`
xterm text-area resize requests. The terminal model applies the requested grid
at the parser sequence point so following output is interpreted against the new
text area. Hosts that choose to honor this signal should resize the surrounding
item/window so the visual shell mirrors the terminal grid.

## Event And Render Overrides

The surface is an item with contents, input-method support, focus, hover, and
all mouse buttons enabled. Hosts normally interact through properties,
invokables, and signals; they do not call the protected event handlers.

At a high level:

- `keyPressEvent()` handles copy policy, page-scroll keys for primary
  scrollback, then terminal key encoding.
- `mousePressEvent()`, `mouseMoveEvent()`, `mouseReleaseEvent()`, and
  `hoverMoveEvent()` route terminal mouse reporting when active and otherwise
  maintain local selection. Shift-drag forces local selection.
- `wheelEvent()` handles Ctrl+wheel font zoom, terminal mouse wheel routing,
  alternate-screen wheel policy, and local scrollback policy.
- `inputMethodEvent()` sends commit text through the terminal input path and
  keeps preedit state render-only.
- `inputMethodQuery()` reports IME enablement and the cursor rectangle.
- `geometryChange()` and relevant `itemChange()` cases refresh metrics, bind
  window and screen signals, report focus changes, and handle scene-graph
  invalidation.
- `updatePaintNode()` builds a render frame from the latest immutable snapshot
  and updates the QSG renderer. `releaseResources()` schedules render-node
  release through the Qt Scene Graph lifecycle.

## Example Terminal App

`examples/terminal_app` is the runnable host. It constructs a `QQuickWindow`,
creates `VNM_TerminalSurface`, attaches `Terminal_scrollbar`, optionally creates
custom window chrome, sizes all three items together, and starts the process on
the next event turn. It updates the window title and custom titlebar from
`terminalTitle` and `terminalIconName`.

The example demonstrates default-shell launch, explicit command launch after
`--`, custom window chrome by default where supported, native-titlebar opt-out,
output capture, software-renderer selection, smoke-test controls, and
host-mediated OSC 52 clipboard policy. It also honors terminal text-area resize
requests by resizing the example window around the requested grid. The
canonical option list lives in [Repository guide](repository_guide.md).

The example clipboard policy allows OSC 52 writes only for target `c` or
`clipboard` and denies other targets. It responds during signal delivery because
the surface stores one pending host clipboard write request at a time.

On Windows the example target is built as a GUI subsystem executable so opening
the terminal app does not allocate a separate console window.
