# Terminal Sequence Matrix

This matrix records supported, ignored, rejected, and unsupported terminal
sequence behavior. It is a policy artifact, not an implementation. Tests and
fixtures may cover narrower cases, but they must not silently change the
behavior recorded here.

Each sequence record uses these exact fields:

- `id`
- `family`
- `sequence`
- `feature`
- `status`
- `action_category`
- `behavior`
- `host_policy`
- `payload_limit`
- `recovery`
- `reply`
- `diagnostic`
- `oracle`

Valid status values are `supported`, `ignored`, `rejected`, and
`unsupported-discard`.

## osc-payload-limit

id: osc-payload-limit
family: OSC
sequence: OSC string payload
feature: string payload hard limit
status: supported
action_category: payload-limit
behavior: accepts payloads up to 1048576 raw bytes
host_policy: none
payload_limit: 1048576 raw bytes
recovery: discard over-limit payload until ST or recovery boundary
reply: no-reply
diagnostic: over-limit diagnostic
oracle: product-decision-vnm-terminal

## dcs-payload-limit

id: dcs-payload-limit
family: DCS
sequence: DCS string payload
feature: string payload hard limit
status: supported
action_category: payload-limit
behavior: accepts payloads up to 1048576 raw bytes before unsupported discard
host_policy: none
payload_limit: 1048576 raw bytes
recovery: discard over-limit payload until ST or recovery boundary
reply: no-reply
diagnostic: over-limit diagnostic
oracle: product-decision-vnm-terminal

## apc-payload-limit

id: apc-payload-limit
family: APC
sequence: APC string payload
feature: string payload hard limit
status: supported
action_category: payload-limit
behavior: accepts payloads up to 1048576 raw bytes before unsupported discard
host_policy: none
payload_limit: 1048576 raw bytes
recovery: discard over-limit payload until ST or recovery boundary
reply: no-reply
diagnostic: over-limit diagnostic
oracle: product-decision-vnm-terminal

## pm-payload-limit

id: pm-payload-limit
family: PM
sequence: PM string payload
feature: string payload hard limit
status: supported
action_category: payload-limit
behavior: accepts payloads up to 1048576 raw bytes before unsupported discard
host_policy: none
payload_limit: 1048576 raw bytes
recovery: discard over-limit payload until ST or recovery boundary
reply: no-reply
diagnostic: over-limit diagnostic
oracle: product-decision-vnm-terminal

## sos-payload-limit

id: sos-payload-limit
family: SOS
sequence: SOS string payload
feature: string payload hard limit
status: supported
action_category: payload-limit
behavior: accepts payloads up to 1048576 raw bytes before unsupported discard
host_policy: none
payload_limit: 1048576 raw bytes
recovery: discard over-limit payload until ST or recovery boundary
reply: no-reply
diagnostic: over-limit diagnostic
oracle: product-decision-vnm-terminal

## dcs-unsupported-discard

id: dcs-unsupported-discard
family: DCS
sequence: unsupported DCS
feature: unsupported string recovery
status: unsupported-discard
action_category: unsupported-discard
behavior: discards payload and mutates no screen state
host_policy: none
payload_limit: 1048576 raw bytes
recovery: recover at ST or recovery boundary
reply: no-reply
diagnostic: unsupported DCS diagnostic
oracle: product-decision-vnm-terminal

## apc-unsupported-discard

id: apc-unsupported-discard
family: APC
sequence: unsupported APC
feature: unsupported string recovery
status: unsupported-discard
action_category: unsupported-discard
behavior: discards payload and mutates no screen state
host_policy: none
payload_limit: 1048576 raw bytes
recovery: recover at ST or recovery boundary
reply: no-reply
diagnostic: unsupported APC diagnostic
oracle: product-decision-vnm-terminal

## pm-unsupported-discard

id: pm-unsupported-discard
family: PM
sequence: unsupported PM
feature: unsupported string recovery
status: unsupported-discard
action_category: unsupported-discard
behavior: discards payload and mutates no screen state
host_policy: none
payload_limit: 1048576 raw bytes
recovery: recover at ST or recovery boundary
reply: no-reply
diagnostic: unsupported PM diagnostic
oracle: product-decision-vnm-terminal

## sos-unsupported-discard

id: sos-unsupported-discard
family: SOS
sequence: unsupported SOS
feature: unsupported string recovery
status: unsupported-discard
action_category: unsupported-discard
behavior: discards payload and mutates no screen state
host_policy: none
payload_limit: 1048576 raw bytes
recovery: recover at ST or recovery boundary
reply: no-reply
diagnostic: unsupported SOS diagnostic
oracle: product-decision-vnm-terminal

## osc-0-title

id: osc-0-title
family: OSC
sequence: OSC 0
feature: icon and window title set
status: supported
action_category: notification
behavior: updates terminal icon name and terminal title, emitting both notifications
host_policy: host observes terminal icon name and title signals
payload_limit: 4096 decoded Unicode scalars
recovery: reject over-limit title and preserve previous icon name and title
reply: no-reply
diagnostic: title overflow diagnostic
oracle: xterm-409-reference

## osc-1-icon-name

id: osc-1-icon-name
family: OSC
sequence: OSC 1
feature: icon name set
status: supported
action_category: notification
behavior: updates terminal icon name and emits icon name notification without changing terminal title
host_policy: host observes terminal icon name signal
payload_limit: 4096 decoded Unicode scalars
recovery: reject over-limit icon name and preserve previous icon name
reply: no-reply
diagnostic: title overflow diagnostic
oracle: xterm-409-reference

## osc-2-title

id: osc-2-title
family: OSC
sequence: OSC 2
feature: window title set
status: supported
action_category: notification
behavior: updates terminal title and emits title notification without changing terminal icon name
host_policy: host observes terminal title signal
payload_limit: 4096 decoded Unicode scalars
recovery: reject over-limit title and preserve previous title
reply: no-reply
diagnostic: title overflow diagnostic
oracle: xterm-409-reference

## osc-title-overflow

id: osc-title-overflow
family: OSC
sequence: OSC 0, OSC 1, or OSC 2 over 4096 scalars
feature: title or icon-name overflow
status: rejected
action_category: rejected-with-recovery
behavior: rejects whole title or icon name and preserves previous affected state
host_policy: no title or icon name notification
payload_limit: 4096 decoded Unicode scalars
recovery: recover at ST and keep previous title or icon name
reply: no-reply
diagnostic: title overflow diagnostic
oracle: product-decision-vnm-terminal

## osc-8-open

id: osc-8-open
family: OSC
sequence: OSC 8 open
feature: hyperlink open
status: supported
action_category: screen-mutation
behavior: begins hyperlink metadata for subsequent cells
host_policy: activation remains host UI policy
payload_limit: 1048576 raw bytes
recovery: malformed parameters ignored with diagnostic
reply: no-reply
diagnostic: malformed OSC 8 diagnostic
oracle: xterm-409-reference

## osc-8-close

id: osc-8-close
family: OSC
sequence: OSC 8 close
feature: hyperlink close
status: supported
action_category: screen-mutation
behavior: ends active hyperlink metadata
host_policy: activation remains host UI policy
payload_limit: 1048576 raw bytes
recovery: malformed close ignored with diagnostic
reply: no-reply
diagnostic: malformed OSC 8 diagnostic
oracle: xterm-409-reference

## osc-52-write-default-deny

id: osc-52-write-default-deny
family: OSC
sequence: OSC 52 write
feature: clipboard write default denial
status: supported
action_category: host-policy-request
behavior: recognizes write request but mutates no clipboard by default
host_policy: deny by default; missing late or duplicate responses deny
payload_limit: 1048576 raw bytes
recovery: malformed or oversized request denied with diagnostic
reply: no-reply
diagnostic: OSC 52 denied diagnostic
oracle: product-decision-vnm-terminal

## osc-52-write-host-request

id: osc-52-write-host-request
family: OSC
sequence: OSC 52 write
feature: clipboard write host opt-in
status: supported
action_category: host-policy-request
behavior: emits host request carrying id, selection, decoded payload, and raw size
host_policy: explicit host opt-in required before clipboard mutation
payload_limit: 1048576 raw bytes
recovery: malformed or oversized request denied with diagnostic
reply: no-reply
diagnostic: OSC 52 request diagnostic on failure
oracle: product-decision-vnm-terminal

## osc-52-read-deny

id: osc-52-read-deny
family: OSC
sequence: OSC 52 read
feature: clipboard read denial
status: supported
action_category: host-policy-request
behavior: read requests are denied and no wire reply is sent
host_policy: reads disabled
payload_limit: 1048576 raw bytes
recovery: recover at ST after denial
reply: no-reply
diagnostic: OSC 52 read denied diagnostic
oracle: product-decision-vnm-terminal

## dec-private-1

id: dec-private-1
family: CSI
sequence: DECSET/DECRST ?1
feature: application cursor keys
status: supported
action_category: input-mode-mutation
behavior: toggles application cursor key encoding
host_policy: keyboard input encoder observes mode
payload_limit: none
recovery: malformed mode ignored
reply: DECRQM private mode reply
diagnostic: malformed mode diagnostic
oracle: xterm-409-reference

## dec-private-3

id: dec-private-3
family: CSI
sequence: DECSET/DECRST ?3
feature: 132-column mode
status: ignored
action_category: ignored-with-diagnostic
behavior: no column count change; geometry remains host controlled
host_policy: resize follows item geometry only
payload_limit: none
recovery: mode ignored and parser continues
reply: DECRQM private mode reply
diagnostic: ignored DEC private mode diagnostic
oracle: product-decision-vnm-terminal

## dec-private-5

id: dec-private-5
family: CSI
sequence: DECSET/DECRST ?5
feature: reverse video
status: supported
action_category: mode-mutation
behavior: toggles reverse-video render metadata
host_policy: renderer applies snapshot mode
payload_limit: none
recovery: malformed mode ignored
reply: DECRQM private mode reply
diagnostic: malformed mode diagnostic
oracle: xterm-409-reference

## dec-private-6

id: dec-private-6
family: CSI
sequence: DECSET/DECRST ?6
feature: origin mode
status: supported
action_category: mode-mutation
behavior: toggles cursor addressing relative to scroll region
host_policy: none
payload_limit: none
recovery: malformed mode ignored
reply: DECRQM private mode reply
diagnostic: malformed mode diagnostic
oracle: xterm-409-reference

## dec-private-7

id: dec-private-7
family: CSI
sequence: DECSET/DECRST ?7
feature: autowrap
status: supported
action_category: mode-mutation
behavior: toggles wrap at right margin
host_policy: none
payload_limit: none
recovery: malformed mode ignored
reply: DECRQM private mode reply
diagnostic: malformed mode diagnostic
oracle: xterm-409-reference

## dec-private-25

id: dec-private-25
family: CSI
sequence: DECSET/DECRST ?25
feature: cursor visibility
status: supported
action_category: mode-mutation
behavior: toggles cursor visibility in snapshots
host_policy: renderer observes cursor visibility
payload_limit: none
recovery: malformed mode ignored
reply: DECRQM private mode reply
diagnostic: malformed mode diagnostic
oracle: xterm-409-reference

## dec-private-47

id: dec-private-47
family: CSI
sequence: DECSET/DECRST ?47
feature: alternate screen
status: supported
action_category: mode-mutation
behavior: switches active screen without primary scrollback mutation
host_policy: alternate-screen wheel policy applies
payload_limit: none
recovery: malformed mode ignored
reply: DECRQM private mode reply
diagnostic: malformed mode diagnostic
oracle: xterm-409-reference

## dec-private-1000

id: dec-private-1000
family: CSI
sequence: DECSET/DECRST ?1000
feature: mouse button reporting
status: supported
action_category: input-mode-mutation
behavior: enables button press and release reporting
host_policy: mouse reporting policy may disable reports
payload_limit: none
recovery: malformed mode ignored
reply: DECRQM private mode reply
diagnostic: malformed mode diagnostic
oracle: xterm-409-reference

## dec-private-1002

id: dec-private-1002
family: CSI
sequence: DECSET/DECRST ?1002
feature: mouse drag reporting
status: supported
action_category: input-mode-mutation
behavior: enables button and drag reporting
host_policy: mouse reporting policy may disable reports
payload_limit: none
recovery: malformed mode ignored
reply: DECRQM private mode reply
diagnostic: malformed mode diagnostic
oracle: xterm-409-reference

## dec-private-1003

id: dec-private-1003
family: CSI
sequence: DECSET/DECRST ?1003
feature: all-motion mouse reporting
status: supported
action_category: input-mode-mutation
behavior: enables all-motion mouse reporting
host_policy: mouse reporting policy may disable reports
payload_limit: none
recovery: malformed mode ignored
reply: DECRQM private mode reply
diagnostic: malformed mode diagnostic
oracle: xterm-409-reference

## dec-private-1004

id: dec-private-1004
family: CSI
sequence: DECSET/DECRST ?1004
feature: focus reporting
status: supported
action_category: input-mode-mutation
behavior: enables focus-in and focus-out reports
host_policy: focus reports sent only while mode is active
payload_limit: none
recovery: malformed mode ignored
reply: DECRQM private mode reply
diagnostic: malformed mode diagnostic
oracle: xterm-409-reference

## dec-private-1005

id: dec-private-1005
family: CSI
sequence: DECSET/DECRST ?1005
feature: legacy UTF-8 mouse protocol
status: ignored
action_category: ignored-with-diagnostic
behavior: parsed but does not enable UTF-8 mouse reporting
host_policy: SGR 1006 remains the supported coordinate protocol
payload_limit: none
recovery: mode ignored and parser continues
reply: DECRQM private mode reply
diagnostic: ignored mouse protocol diagnostic
oracle: product-decision-vnm-terminal

## dec-private-1006

id: dec-private-1006
family: CSI
sequence: DECSET/DECRST ?1006
feature: SGR mouse protocol
status: supported
action_category: input-mode-mutation
behavior: enables SGR mouse coordinate encoding
host_policy: mouse reporting policy may disable reports
payload_limit: none
recovery: malformed mode ignored
reply: DECRQM private mode reply
diagnostic: malformed mode diagnostic
oracle: xterm-409-reference

## dec-private-1007

id: dec-private-1007
family: CSI
sequence: DECSET/DECRST ?1007
feature: alternate-scroll mode
status: supported
action_category: input-mode-mutation
behavior: tracks alternate-scroll mode for DECRQM and input-mode snapshots
host_policy: surface alternate-screen wheel policy may still translate wheel events to cursor or page keys after DECRST ?1007
payload_limit: none
recovery: malformed mode ignored
reply: DECRQM private mode reply
diagnostic: malformed mode diagnostic
oracle: xterm-409-reference

## dec-private-1015

id: dec-private-1015
family: CSI
sequence: DECSET/DECRST ?1015
feature: urxvt mouse protocol
status: ignored
action_category: ignored-with-diagnostic
behavior: parsed but does not enable urxvt mouse reporting
host_policy: SGR 1006 remains the supported coordinate protocol
payload_limit: none
recovery: mode ignored and parser continues
reply: DECRQM private mode reply
diagnostic: ignored mouse protocol diagnostic
oracle: product-decision-vnm-terminal

## dec-private-1047

id: dec-private-1047
family: CSI
sequence: DECSET/DECRST ?1047
feature: alternate screen
status: supported
action_category: mode-mutation
behavior: enters alternate buffer and clears alternate contents
host_policy: alternate-screen scrollback policy applies
payload_limit: none
recovery: malformed mode ignored
reply: DECRQM private mode reply
diagnostic: malformed mode diagnostic
oracle: xterm-409-reference

## dec-private-1048

id: dec-private-1048
family: CSI
sequence: DECSET/DECRST ?1048
feature: save and restore cursor
status: supported
action_category: mode-mutation
behavior: saves or restores cursor state
host_policy: none
payload_limit: none
recovery: malformed mode ignored
reply: DECRQM private mode reply
diagnostic: malformed mode diagnostic
oracle: xterm-409-reference

## dec-private-1049

id: dec-private-1049
family: CSI
sequence: DECSET/DECRST ?1049
feature: alternate screen with cursor save
status: supported
action_category: mode-mutation
behavior: combines cursor save with alternate-screen switch
host_policy: alternate-screen scrollback policy applies
payload_limit: none
recovery: malformed mode ignored
reply: DECRQM private mode reply
diagnostic: malformed mode diagnostic
oracle: xterm-409-reference

## dec-private-2004

id: dec-private-2004
family: CSI
sequence: DECSET/DECRST ?2004
feature: bracketed paste mode
status: supported
action_category: input-mode-mutation
behavior: toggles bracketed paste framing for accepted paste input
host_policy: paste policy and write queue capacity apply
payload_limit: none
recovery: malformed mode ignored
reply: DECRQM private mode reply
diagnostic: malformed mode diagnostic
oracle: xterm-409-reference

## dec-private-2026

id: dec-private-2026
family: CSI
sequence: DECSET/DECRST ?2026
feature: synchronized output
status: supported
action_category: mode-mutation
behavior: coalesces snapshot publication only; parser and screen still mutate
host_policy: render invalidation throttling policy applies
payload_limit: none
recovery: timeout or DECRST exits synchronized output with diagnostic if stale
reply: DECRQM private mode reply
diagnostic: stale synchronized-output diagnostic
oracle: product-decision-vnm-terminal

## dec-private-2027

id: dec-private-2027
family: CSI
sequence: DECSET/DECRST ?2027
feature: grapheme-cluster mode
status: rejected
action_category: rejected-with-recovery
behavior: no runtime grapheme-cluster mode is stored or toggled
host_policy: Unicode width policy remains pinned and mode-independent
payload_limit: none
recovery: command rejected and parser continues
reply: no-reply
diagnostic: rejected grapheme-cluster mode diagnostic
oracle: product-decision-vnm-terminal

## c0-bel

id: c0-bel
family: C0
sequence: BEL
feature: bell
status: supported
action_category: notification
behavior: emits bell request without mutating screen cells
host_policy: audible and visual bell policies apply
payload_limit: none
recovery: none
reply: no-reply
diagnostic: no diagnostic
oracle: product-decision-vnm-terminal

## esc-index

id: esc-index
family: ESC
sequence: ESC D
feature: index
status: supported
action_category: screen-mutation
behavior: moves the cursor down one row, scrolling the active region at the bottom margin
host_policy: top-anchored primary-region scrolling appends host scrollback
payload_limit: none
recovery: unsupported ESC controls continue through normal recovery
reply: no-reply
diagnostic: no diagnostic
oracle: xterm-409-reference

## esc-next-line

id: esc-next-line
family: ESC
sequence: ESC E
feature: next line
status: supported
action_category: screen-mutation
behavior: moves the cursor to column zero of the next row, scrolling the active region at the bottom margin
host_policy: top-anchored primary-region scrolling appends host scrollback
payload_limit: none
recovery: unsupported ESC controls continue through normal recovery
reply: no-reply
diagnostic: no diagnostic
oracle: xterm-409-reference

## esc-reverse-index

id: esc-reverse-index
family: ESC
sequence: ESC M
feature: reverse index
status: supported
action_category: screen-mutation
behavior: moves the cursor up one row or scrolls the active region down at the top margin
host_policy: reverse scrolling does not append host scrollback
payload_limit: none
recovery: unsupported ESC controls continue through normal recovery
reply: no-reply
diagnostic: no diagnostic
oracle: xterm-409-reference

## csi-scroll-up

id: csi-scroll-up
family: CSI
sequence: SU / CSI Ps S
feature: scroll region up
status: supported
action_category: screen-mutation
behavior: scrolls the active scroll region up and blanks vacated bottom rows
host_policy: top-anchored primary regions append scrolled rows to host scrollback
payload_limit: none
recovery: malformed sequence ignored with diagnostic
reply: no-reply
diagnostic: malformed sequence diagnostic
oracle: xterm-409-reference

## csi-scroll-down

id: csi-scroll-down
family: CSI
sequence: SD / CSI Ps T single-parameter form; XTHIMOUSE-shaped multi-parameter CSI T
feature: scroll region down
status: supported
action_category: screen-mutation
behavior: single-parameter CSI Ps T scrolls the active scroll region down and blanks vacated top rows; XTHIMOUSE-shaped multi-parameter CSI T mutates no screen state
host_policy: does not append host scrollback
payload_limit: none
recovery: malformed or unsupported CSI T form ignored with diagnostic
reply: no-reply
diagnostic: malformed sequence diagnostic; unsupported diagnostic for XTHIMOUSE-shaped multi-parameter CSI T
oracle: xterm-409-reference

## csi-decsca

id: csi-decsca
family: CSI
sequence: DECSCA
feature: protected cell attribute
status: ignored
action_category: ignored-with-diagnostic
behavior: no protected-cell model state is stored
host_policy: none
payload_limit: none
recovery: command ignored and parser continues
reply: no-reply
diagnostic: ignored DECSCA diagnostic
oracle: product-decision-vnm-terminal

## bracketed-paste-generated-input

id: bracketed-paste-generated-input
family: generated-input
sequence: bracketed paste wrappers
feature: generated paste input
status: supported
action_category: input-mode-mutation
behavior: accepted paste is wrapped only when bracketed paste mode is active
host_policy: paste capacity reservation must include wrappers and payload
payload_limit: write queue capacity
recovery: reject whole paste if full frame cannot be enqueued
reply: backend write bytes
diagnostic: paste rejection diagnostic
oracle: product-decision-vnm-terminal

## mouse-sgr-1006-generated-input

id: mouse-sgr-1006-generated-input
family: generated-input
sequence: SGR 1006 mouse report
feature: generated mouse input
status: supported
action_category: input-mode-mutation
behavior: sends SGR mouse reports when mode and host policy allow
host_policy: mouse reporting policy may suppress terminal reports
payload_limit: write queue capacity
recovery: out-of-bounds events ignored with diagnostic
reply: backend write bytes
diagnostic: mouse report suppression diagnostic
oracle: product-decision-vnm-terminal

## focus-generated-input

id: focus-generated-input
family: generated-input
sequence: focus in and focus out reports
feature: generated focus input
status: supported
action_category: input-mode-mutation
behavior: sends focus reports only while focus reporting mode is active
host_policy: item focus state controls report source
payload_limit: write queue capacity
recovery: disabled mode suppresses reports without backend write
reply: backend write bytes
diagnostic: no diagnostic
oracle: product-decision-vnm-terminal

## reply-da1

id: reply-da1
family: CSI
sequence: DA1
feature: terminal identity reply
status: supported
action_category: terminal-reply
behavior: emits typed DA1 reply action
host_policy: backend write queue capacity applies
payload_limit: none
recovery: malformed query ignored with diagnostic
reply: DA1 reply through same backend write path
diagnostic: malformed query diagnostic
oracle: product-decision-vnm-terminal

## reply-da2

id: reply-da2
family: CSI
sequence: secondary DA
feature: terminal identity reply
status: supported
action_category: terminal-reply
behavior: emits typed DA2 reply action
host_policy: backend write queue capacity applies
payload_limit: none
recovery: malformed query ignored with diagnostic
reply: DA2 reply through same backend write path
diagnostic: malformed query diagnostic
oracle: product-decision-vnm-terminal

## reply-dsr-cursor-position

id: reply-dsr-cursor-position
family: CSI
sequence: DSR 6
feature: cursor position report
status: supported
action_category: terminal-reply
behavior: emits cursor position report for active buffer cursor
host_policy: backend write queue capacity applies
payload_limit: none
recovery: malformed query ignored with diagnostic
reply: DSR cursor reply through same backend write path
diagnostic: malformed query diagnostic
oracle: product-decision-vnm-terminal

## reply-decrqm-private-mode

id: reply-decrqm-private-mode
family: CSI
sequence: DECRQM private mode query
feature: mode status report
status: supported
action_category: terminal-reply
behavior: emits DECRQM reply for known private modes
host_policy: backend write queue capacity applies
payload_limit: none
recovery: unsupported query gets explicit unsupported reply or diagnostic
reply: DECRQM reply through same backend write path
diagnostic: unsupported query diagnostic
oracle: product-decision-vnm-terminal

## reply-osc-color-query

id: reply-osc-color-query
family: OSC
sequence: OSC color query
feature: color state reply
status: supported
action_category: terminal-reply
behavior: emits typed OSC color reply action for supported color queries
host_policy: backend write queue capacity applies
payload_limit: 1048576 raw bytes
recovery: unsupported color query ignored with diagnostic
reply: OSC color reply through same backend write path
diagnostic: unsupported color query diagnostic
oracle: xterm-409-reference

## reply-unsupported-query-no-reply

id: reply-unsupported-query-no-reply
family: CSI
sequence: unsupported terminal query
feature: unsupported query behavior
status: ignored
action_category: ignored-with-diagnostic
behavior: unsupported query mutates no state and emits no wire reply
host_policy: backend write queue receives nothing
payload_limit: none
recovery: query ignored and parser continues
reply: no-reply
diagnostic: unsupported query diagnostic
oracle: product-decision-vnm-terminal
