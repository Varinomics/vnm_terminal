#Requires -Version 5.1
param([string[]] $CommandPrefix, [string[]] $Arguments = @())

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$codex_command = $CommandPrefix[0]

function quote_native_argument([string] $value) {
    $quoted = [Text.StringBuilder]::new('"')
    $backslashes = 0
    foreach ($character in $value.ToCharArray()) {
        if ($character -eq '\') {
            ++$backslashes
            continue
        }
        if ($character -eq '"') {
            [void] $quoted.Append(('\' * (2 * $backslashes + 1)))
        }
        else {
            [void] $quoted.Append(('\' * $backslashes))
        }
        [void] $quoted.Append($character)
        $backslashes = 0
    }
    [void] $quoted.Append(('\' * (2 * $backslashes)))
    [void] $quoted.Append('"')
    return $quoted.ToString()
}

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
    $resolved = Get-Command -Name ([WildcardPattern]::Escape($codex_command)) `
        -CommandType Application, ExternalScript -ErrorAction Stop | Select-Object -First 1
    # Parent-terminal hints otherwise hide TERM or select an unsupported protocol.
    foreach ($name in $identity_names) {
        [Environment]::SetEnvironmentVariable($name, [NullString]::Value, 'Process')
    }
    $env:TERM = 'vnm-terminal-sixel'
    $forwarded_arguments = @(
        for ($index = 1; $index -lt $CommandPrefix.Length; ++$index) {
            $CommandPrefix[$index]
        }
        '-c'
        "shell_environment_policy.set.TERM='xterm-256color'"
        foreach ($argument in $Arguments) {
            $argument
        }
    )
    if ($resolved.CommandType -eq 'Application') {
        # Fully quoted arguments pass through the Windows PowerShell binder unchanged.
        # Use its string mode in newer hosts too, retaining native IO and console control handling.
        $PSNativeCommandArgumentPassing = 'Legacy'
        $forwarded_arguments = @($forwarded_arguments | ForEach-Object { quote_native_argument $_ })
    }
    # Scripts keep their caller's host and argument array; native commands keep its terminal handles.
    if ($MyInvocation.ExpectingInput) {
        $input | & $resolved.Path @forwarded_arguments
    }
    else {
        & $resolved.Path @forwarded_arguments
    }
    $codex_exit_code = $LASTEXITCODE
}
finally {
    # NullString passes an actual CLR null; PowerShell otherwise binds $null as an empty string.
    foreach ($name in $identity_names) {
        $saved_value = $saved_identity[$name]
        if ($null -eq $saved_value) {
            $saved_value = [NullString]::Value
        }
        [Environment]::SetEnvironmentVariable($name, $saved_value, 'Process')
    }
}

exit $codex_exit_code
