# Terminal Conformance Oracles

This file defines which sources may establish terminal behavior for
`vnm_terminal`. Nothing in this file imports external source, comments, test
names, transcripts, byte streams, captured output, fixture bytes, or goldens.

Checked-in expected behavior must cite an approved `oracle_id`. Strong-copyleft
and reference-only material may inform local questions, but never checked-in
source, fixtures, expected output, or golden data.

Each oracle record uses:

- `oracle_id`
- `oracle_type`
- `status`
- `license_posture`
- `pin_or_version`
- `checked_in_output_allowed`
- `allowed_use`
- `forbidden_use`
- `inventory_ref`
- `notes`

## product-decision-vnm-terminal

oracle_id: product-decision-vnm-terminal
oracle_type: product-decision
status: approved
license_posture: project-owned
pin_or_version: current repository commit
checked_in_output_allowed: yes
allowed_use: document vnm_terminal behavior where terminal references diverge
forbidden_use: no silent undocumented behavior change
inventory_ref: none
notes: product decisions must be reviewed before fixtures cite them

## product-platform-matrix

oracle_id: product-platform-matrix
oracle_type: product-decision
status: approved
license_posture: project-owned
pin_or_version: current repository commit
checked_in_output_allowed: yes
allowed_use: platform support and exclusion decisions for vnm_terminal
forbidden_use: no implicit support claims outside the listed matrix
inventory_ref: none
notes: Windows x64 and Linux x86_64 are supported native targets. Other
platforms have no native backend support claim.

## independent-vnm-fixture

oracle_id: independent-vnm-fixture
oracle_type: independently-authored-fixture
status: approved
license_posture: project-owned
pin_or_version: fixture provenance headers
checked_in_output_allowed: yes
allowed_use: authored regression fixtures and generators with provenance headers
forbidden_use: no copied external output, names, comments, byte streams, or goldens
inventory_ref: tests/conformance/README.md
notes: fixture authors must avoid GPL-derived material

## xterm-409-reference

oracle_id: xterm-409-reference
oracle_type: reference-only
status: approved-reference
license_posture: permissive reference; no import
pin_or_version: xterm patch 409, 2026-04-13
checked_in_output_allowed: no
allowed_use: public docs, patch notes, and local manual comparison
forbidden_use: no source, transcripts, runtime output, fixture bytes, or goldens
inventory_ref: docs/terminal_reference_inventory.md#xterm-409
notes: authoritative xterm pin for behavior questions

## vttest-reference

oracle_id: vttest-reference
oracle_type: reference-only
status: approved-reference
license_posture: permissive reference; no import
pin_or_version: vttest-20251205.tgz
checked_in_output_allowed: no
allowed_use: local manual behavior exploration and checklist inspiration
forbidden_use: no captured screens, transcripts, byte streams, or goldens
inventory_ref: docs/terminal_reference_inventory.md#vttest
notes: menu output never becomes a repo oracle

## contour-candidate

oracle_id: contour-candidate
oracle_type: permissive-import-candidate
status: candidate
license_posture: Apache-2.0 candidate; import requires provenance gate
pin_or_version: record exact commit before import
checked_in_output_allowed: no
allowed_use: behavior questions; material import requires provenance approval
forbidden_use: no material import before provenance approval
inventory_ref: docs/terminal_reference_inventory.md#contour
notes: candidate status is not approval

## libvterm-candidate

oracle_id: libvterm-candidate
oracle_type: permissive-import-candidate
status: candidate
license_posture: MIT candidate; import requires provenance gate
pin_or_version: libvterm-0.3.3.tar.gz
checked_in_output_allowed: no
allowed_use: behavior questions; material import requires provenance approval
forbidden_use: no material import before provenance approval
inventory_ref: docs/terminal_reference_inventory.md#libvterm
notes: candidate status is not approval

## wezterm-candidate

oracle_id: wezterm-candidate
oracle_type: permissive-import-candidate
status: candidate
license_posture: MIT candidate; bundled materials need separate audit
pin_or_version: record exact commit before import
checked_in_output_allowed: no
allowed_use: behavior questions; material import requires provenance approval
forbidden_use: no material import before provenance approval
inventory_ref: docs/terminal_reference_inventory.md#wezterm
notes: candidate status is not approval

## microsoft-terminal-candidate

oracle_id: microsoft-terminal-candidate
oracle_type: permissive-import-candidate
status: candidate
license_posture: MIT candidate; bundled materials need separate audit
pin_or_version: record exact commit before import
checked_in_output_allowed: no
allowed_use: Windows and ConPTY behavior questions
forbidden_use: no material import before provenance approval
inventory_ref: docs/terminal_reference_inventory.md#microsoft-terminal
notes: candidate status is not approval

## iterm2-esctest-reference-only

oracle_id: iterm2-esctest-reference-only
oracle_type: reference-only
status: strong-copyleft-reference
license_posture: GPL-family reference-only
pin_or_version: record commit only in local reference logs
checked_in_output_allowed: no
allowed_use: local exploratory behavior questions only
forbidden_use: no GPL-derived source, names, comments, output, streams, or goldens
inventory_ref: docs/terminal_reference_inventory.md#iterm2-esctest
notes: clean-room authored fixtures only

## strong-copyleft-terminal-rejected

oracle_id: strong-copyleft-terminal-rejected
oracle_type: rejected-reference
status: rejected
license_posture: GPL-family; not a dependency or oracle source
pin_or_version: not adopted
checked_in_output_allowed: no
allowed_use: local behavior questions only if needed
forbidden_use: no source, test names, output, byte streams, fixtures, or goldens
inventory_ref: none
notes: explicitly out of scope as a dependency and oracle source
