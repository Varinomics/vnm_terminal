#Requires -Version 7.3
<#
.SYNOPSIS
Runs Codex with SIXEL detection enabled inside vnm_terminal.

.DESCRIPTION
Passes all arguments to Codex. The terminal identity override lasts only for
this invocation. Codex shell commands receive xterm-256color through a
command-line configuration override; no configuration file is changed.

.EXAMPLE
pwsh -NoProfile -File tools/codex_sixel.ps1 resume
#>

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
# Standard native argument passing preserves empty arguments and embedded quotes.
$PSNativeCommandArgumentPassing = 'Standard'

$identity_names = @(
    'TERM',
    'TERM_PROGRAM',
    'TERM_PROGRAM_VERSION',
    'GHOSTTY_RESOURCES_DIR',
    'WEZTERM_VERSION',
    'WEZTERM_EXECUTABLE',
    'ITERM_SESSION_ID',
    'ITERM_PROFILE',
    'ITERM_PROFILE_NAME',
    'TERM_SESSION_ID',
    'KITTY_WINDOW_ID',
    'ALACRITTY_SOCKET',
    'KONSOLE_VERSION',
    'GNOME_TERMINAL_SCREEN',
    'VTE_VERSION',
    'WT_SESSION'
)
$saved_identity = @{}
foreach ($name in $identity_names) {
    $saved_identity[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}

try {
    # Parent-terminal hints otherwise hide TERM or select an unsupported protocol.
    foreach ($name in $identity_names) {
        [Environment]::SetEnvironmentVariable($name, [NullString]::Value, 'Process')
    }
    $env:TERM = 'vnm-terminal-sixel'
    & codex -c 'shell_environment_policy.set.TERM="xterm-256color"' @args
    $codex_exit_code = $LASTEXITCODE
}
finally {
    foreach ($name in $identity_names) {
        [Environment]::SetEnvironmentVariable(
            $name, ($saved_identity[$name] ?? [NullString]::Value), 'Process')
    }
}

exit $codex_exit_code
