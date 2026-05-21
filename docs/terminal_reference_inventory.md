# Terminal Reference Inventory

This inventory records which external terminal references may inform behavior
questions and which materials may become repo fixtures. Nothing in this file
imports external source, comments, code structure, test names, expected or
captured output, transcripts, byte streams, fixture bytes, or golden data.

`reference-only` records may be studied or run locally for behavior questions,
but they do not authorize copied repo material. `permissive-import-candidate`
records are only candidates; any material import requires the provenance gate
and a separate review.

## vttest

reference_id: vttest
upstream_name: VTTEST
source_url: https://www.invisible-island.net/vttest/
version_pin: vttest-20251205.tgz, sha256 cd6886f9aefe6a3f6c566fa61271a55710901a71849c630bf5376aa984bf77cc
license: X11 distribution-modification variant and BSD-3-Clause, per upstream COPYING
scope: VT100, VT220, ISO 6429, color, keyboard, mouse, and xterm feature exploration
import_decision: reference-only
allowed_use: local interactive/manual behavior questions and checklist inspiration
forbidden_use: no source, comments, code structure, test names, expected output, captured output, transcripts, byte streams, fixture bytes, or goldens
notes: bundled build-helper files require file-level audit before import approval

## xterm-409

reference_id: xterm-409
upstream_name: xterm
source_url: https://invisible-island.net/archives/xterm/xterm-409.tgz
patch_url: https://invisible-island.net/archives/xterm/patches/xterm-409.patch.gz
version_pin: Patch 409, 2026-04-13
source_sha256: 349dc755c49299ca4b8e3a90f7201dff41877a1e6ac16129e439d76493246c40
patch_sha256: 11c3edb8d0d073f4aa6518ce638f9dfc3bb90a2eeda59f984d39a4d96a3d05c1
license: MIT-X11 and DEC-X11, per upstream xterm-409 COPYING
license_evidence: xterm-409/COPYING and xterm-409/package/debian/copyright
scope: ctlseqs behavior questions, DEC/xterm mode names, and high-risk sequence comparison
import_decision: reference-only
allowed_use: manually read public docs and patch notes; run xterm locally for questions
forbidden_use: no source, comments, code structure, test names, expected output, captured output, transcripts, byte streams, fixture bytes, or goldens
notes: patch 409 is the xterm pin until a reviewed update changes it

## iterm2-esctest

reference_id: iterm2-esctest
upstream_name: iTerm2 esctest
source_url: https://github.com/gnachman/iTerm2
feature_report_url: https://iterm2.com/feature-reporting/
version_pin: repository head not pinned for import; record commit per local reference run
license: GPL-2.0 or GPL-3.0 family depending on mirrored iTerm2 source metadata
scope: modern compatibility questions for OSC, window operations, SGR, focus, and reports
import_decision: reference-only
allowed_use: local exploratory runs and public feature matrix questions only
forbidden_use: no source, comments, code structure, test names, expected output, captured output, transcripts, byte streams, fixture bytes, or goldens
notes: strong-copyleft material is never imported; in-repo fixtures require independent authorship

## contour

reference_id: contour
upstream_name: Contour Terminal Emulator
source_url: https://github.com/contour-terminal/contour
version_pin: no import pin; record commit before candidate import
license: Apache-2.0
scope: VT parser tests, conformance corpora, Unicode, mouse, synchronized output, OSC 8/52
import_decision: permissive-import-candidate
allowed_use: inspect behavior questions; material import only through provenance gate
forbidden_use: no source, comments, code structure, test names, expected output, captured output, transcripts, byte streams, fixture bytes, or goldens before import approval
notes: candidate status is not approval; provenance and inventory gates still apply

## libvterm

reference_id: libvterm
upstream_name: libvterm
source_url: https://www.leonerd.org.uk/code/libvterm/
version_pin: libvterm-0.3.3.tar.gz, sha256 09156f43dd2128bd347cbeebe50d9a571d32c64e0cf18d211197946aff7226e0
license: MIT
scope: parser-state behavior questions and small ECMA-48/VT220 edge-case comparisons
import_decision: permissive-import-candidate
allowed_use: inspect behavior questions; material import only through provenance gate
forbidden_use: no source, comments, code structure, test names, expected output, captured output, transcripts, byte streams, fixture bytes, or goldens before import approval
notes: candidate status requires a separate import review before any material enters the repo

## wezterm

reference_id: wezterm
upstream_name: WezTerm
source_url: https://github.com/wezterm/wezterm
version_pin: no import pin; record exact commit before candidate import
license: MIT, with bundled third-party materials to audit before import
scope: escape parser behavior questions, terminal compatibility tests, and real-tool scenarios
import_decision: permissive-import-candidate
allowed_use: inspect behavior questions; material import only through provenance gate
forbidden_use: no source, comments, code structure, test names, expected output, captured output, transcripts, byte streams, fixture bytes, or goldens before import approval
notes: candidate status excludes bundled third-party data until separately inventoried

## microsoft-terminal

reference_id: microsoft-terminal
upstream_name: Windows Terminal and OpenConsole
source_url: https://github.com/microsoft/terminal
version_pin: no import pin; record exact commit before candidate import
license: MIT
scope: ConPTY expectations, Windows terminal modes, input, resize, and pseudo-console behavior
import_decision: permissive-import-candidate
allowed_use: inspect behavior questions; material import only through provenance gate
forbidden_use: no source, comments, code structure, test names, expected output, captured output, transcripts, byte streams, fixture bytes, or goldens before import approval
notes: useful for Windows-specific behavior; bundled third-party materials need separate inventory
