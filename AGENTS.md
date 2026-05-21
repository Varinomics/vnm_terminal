# AGENTS

This file defines working conventions for AI coding agents in this repository.

## Review and plan artifacts

Default rule: we are not staging reviews and plans.

Do not create, stage, or commit review reports, review plans, implementation
plans, analysis notes, working notes, or other transient `.md` files unless the
user explicitly requests a repo-tracked artifact.

These files are scratch work and have no place in the repo by default. Keep
them outside the repo or delete them when done; do not add them to `.gitignore`
as a workaround.

## Coding style

This repository does not maintain its own coding-style document. All code
must follow the Varinomics coding standards, which are the single source of
truth across every Varinomics product. The standards consist of:

- `varinomics_coding_style_guideline.md` - baseline rules for naming,
  structure, formatting, and C++ conventions.
- `varinomics_coding_style_llm_addendum.md` - companion with house-style
  formatting judgment (alignment, wrapping, visual-table patterns) that
  applies on top of the baseline. When the addendum conflicts with the
  guideline, the addendum wins.
- `varinomics_review_scope.md` - what reviewers must and must not flag.
- `varinomics_change_governance.md` - rules for multi-batch work
  (migrations, refactors, multi-step features). Read this BEFORE
  starting any work that will span more than one commit. The rules are
  non-negotiable; if a batch cannot honor them, split it.

Local path: `C:\plms\varinomics\varinomics-standards\`
Canonical repo: `https://github.com/Varinomics/varinomics-standards`

Read all four files before writing or modifying any code.

## Review scope

When reviewing code in this repository, follow `varinomics_review_scope.md`
in the same standards repo. Short form: for **elective features** (i18n,
a11y where not legally baseline, cross-platform support beyond the declared
target, specific architectural patterns, specific compliance regimes), do
not flag the absence of what the project has not adopted. **Baseline
software quality** - correctness, data integrity, security hygiene,
performance sanity, resource hygiene, maintainability - is always in scope
and does not require adoption. The product's domain raises the baseline
(banking implies banking-level security, etc.), never lowers it. Read the
full doc before producing any review report against this repository.

## Local Windows toolchain

On this workstation, initialize native MSVC builds from:
`C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat`

Use the x64 environment for native x64 builds, for example:
`cmd.exe /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 && cmake --build <build-dir>'`

The Windows debuggers are installed at:
`C:\Program Files\Windows Kits\10\Debuggers\x64`

If a Ninja/MSVC build cannot find standard headers such as `stddef.h` or
`optional`, first check that the shell was initialized through `vcvarsall.bat`.

## Codex Claude review helper

Codex agents may invoke Claude review-only sessions through
`C:\plms\invoking_claude_from_codex` when a task calls for Claude review.
This instruction is for Codex only: Claude must not use this helper to invoke
Claude.
