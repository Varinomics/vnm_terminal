# CLAUDE

Read `AGENTS.md` first. It contains the repository-specific AI-agent rules,
including the no-transient-artifacts rule.

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

## Local Windows Toolchain

On this workstation, initialize native MSVC builds from:
`C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat`

Use the x64 environment for native x64 builds, for example:
`cmd.exe /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 && cmake --build <build-dir>'`

The Windows debuggers are installed at:
`C:\Program Files\Windows Kits\10\Debuggers\x64`

If a Ninja/MSVC build cannot find standard headers such as `stddef.h` or
`optional`, first check that the shell was initialized through `vcvarsall.bat`.
