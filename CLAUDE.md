# CLAUDE

## Coding standards

This repository does not maintain its own coding-style document. All code
must follow the Varinomics coding standards, which are the single source
of truth across every Varinomics product. The standards consist of:

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

Read all four files before writing or modifying any code. Project-level
policy and review-and-plan-artifact rules live in `AGENTS.md`.

## Local Windows toolchain

On this workstation, initialize native MSVC builds from:
`C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat`

Use the x64 environment for native x64 builds, for example:
`cmd.exe /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 && cmake --build <build-dir>'`

The Windows debuggers are installed at:
`C:\Program Files\Windows Kits\10\Debuggers\x64`

If a Ninja/MSVC build cannot find standard headers such as `stddef.h` or
`optional`, first check that the shell was initialized through `vcvarsall.bat`.
