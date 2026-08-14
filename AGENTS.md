# AGENTS

## Common Varinomics Rules

This repository follows the shared Varinomics standards. Do not duplicate,
reinterpret, or weaken those rules locally.

Before modifying code, read the coding guideline and LLM addendum:

- `varinomics_coding_style_guideline.md`
- `varinomics_coding_style_llm_addendum.md`

Before producing a code review, read:

- `varinomics_review_scope.md`

Before work that will span more than one commit, migration, refactor, or
multi-step feature, read:

- `varinomics_change_governance.md`

If the addendum conflicts with the guideline, the addendum wins. If you are
unsure which standard applies, read all four before proceeding.

Local path: `C:\plms\varinomics\varinomics-standards\`
Canonical repo: `https://github.com/Varinomics/varinomics-standards`

## Review and Plan Artifacts

Default rule: do not stage or commit review reports, review plans,
implementation plans, analysis notes, working notes, investigation reports,
phase plans, or other transient documentation unless the user explicitly
requests a repo-tracked artifact.

These files are scratch work. Keep them outside the repository or delete them
when done; do not add them to `.gitignore` as a workaround.

## Local Windows Toolchain

Follow `varinomics_llm_tooling_windows.md` in the Varinomics standards repo and
the user-wide runtime policy it identifies.

The supported `build_portable.bat` and `build_windows_packages.bat` workflows
are project-specific MinGW/Ninja packaging exceptions. FASTBuild flags do not
apply to them, but the compiler-invoking outer script still runs through the
build queue: use `queued-build --slots all -- cmd.exe /c build_portable.bat` or
`queued-build --slots all -- cmd.exe /c build_windows_packages.bat`.

## Codex Claude Review Helper

Codex agents may invoke Claude review-only sessions through
`C:\plms\invoking_claude_from_codex` when a task calls for Claude review.
This instruction is for Codex only: Claude must not use this helper to invoke
Claude.
