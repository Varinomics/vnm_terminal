#Requires -Version 7.3
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
# Standard native argument passing preserves empty arguments and embedded quotes.
$PSNativeCommandArgumentPassing = 'Standard'
$codex_command = $args[0]
$codex_arguments = @($args | Select-Object -Skip 1)
$saved_path = $env:PATH

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
    # Resolve the user's original command without rediscovering this PATH shim.
    $shim_directory = $PSScriptRoot.Replace('/', '\').TrimEnd('\')
    $env:PATH = (($saved_path -split ';' | Where-Object {
        !$_.Replace('/', '\').TrimEnd('\').Equals($shim_directory, [StringComparison]::OrdinalIgnoreCase)
    }) -join ';')
    if ($codex_command -eq '--cmd-path') {
        # Keep cmd's current-directory/PATHEXT precedence, including site wrappers.
        $command_extensions = $env:PATHEXT -split ';'
        $codex_command = & "$env:SystemRoot/System32/where.exe" codex 2>$null |
            Where-Object { $command_extensions -contains [IO.Path]::GetExtension($_) } |
            Select-Object -First 1
        if (!$codex_command) {
            throw 'codex was not found on the original terminal PATH.'
        }
    }
    # Parent-terminal hints otherwise hide TERM or select an unsupported protocol.
    foreach ($name in $identity_names) {
        [Environment]::SetEnvironmentVariable($name, [NullString]::Value, 'Process')
    }
    $env:TERM = 'vnm-terminal-sixel'
    if ($MyInvocation.ExpectingInput) {
        $input | & $codex_command -c "shell_environment_policy.set.TERM='xterm-256color'" @codex_arguments
    }
    else {
        & $codex_command -c "shell_environment_policy.set.TERM='xterm-256color'" @codex_arguments
    }
    $codex_exit_code = $LASTEXITCODE
}
finally {
    [Environment]::SetEnvironmentVariable('PATH', ($saved_path ?? [NullString]::Value), 'Process')
    foreach ($name in $identity_names) {
        [Environment]::SetEnvironmentVariable(
            $name, ($saved_identity[$name] ?? [NullString]::Value), 'Process')
    }
}

exit $codex_exit_code
