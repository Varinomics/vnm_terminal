#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$codex_command = $args[0]
$codex_arguments = @($args | Select-Object -Skip 1)
$saved_path = $env:PATH

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
    # Resolve the user's original command without rediscovering this PATH shim.
    $shim_directory = $PSScriptRoot.Replace('/', '\').TrimEnd('\')
    $env:PATH = (($saved_path -split ';' | Where-Object {
        !$_.Replace('/', '\').TrimEnd('\').Equals($shim_directory, [StringComparison]::OrdinalIgnoreCase)
    }) -join ';')
    $resolved = Get-Command -Name ([WildcardPattern]::Escape($codex_command)) `
        -CommandType Application, ExternalScript -ErrorAction Stop | Select-Object -First 1
    # Parent-terminal hints otherwise hide TERM or select an unsupported protocol.
    foreach ($name in $identity_names) {
        [Environment]::SetEnvironmentVariable($name, $null, 'Process')
    }
    $env:TERM = 'vnm-terminal-sixel'
    $forwarded_arguments = @('-c', "shell_environment_policy.set.TERM='xterm-256color'") + $codex_arguments
    if ($resolved.CommandType -eq 'ExternalScript') {
        # Keep the user's script in its caller's host; its internal forwarding follows that host's rules.
        if ($MyInvocation.ExpectingInput) {
            $input | & $resolved.Path @forwarded_arguments
        }
        else {
            & $resolved.Path @forwarded_arguments
        }
        $codex_exit_code = $LASTEXITCODE
    }
    else {
        # A native child preserves terminal handles and PowerShell pipeline behavior without its argv rewriter.
        $native_arguments = ($forwarded_arguments | ForEach-Object { quote_native_argument $_ }) -join ' '
        $launcher = "$PSScriptRoot/codex_launcher.exe"
        # The file transport leaves the Windows command-line limit available to the original arguments.
        $argument_file = [IO.Path]::Combine($PSScriptRoot, [IO.Path]::GetRandomFileName())
        $stream = [IO.File]::Open($argument_file, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
        try {
            try {
                $bytes = [Text.Encoding]::Unicode.GetBytes($native_arguments)
                $stream.Write($bytes, 0, $bytes.Length)
            }
            finally {
                $stream.Dispose()
            }
            if ($MyInvocation.ExpectingInput) {
                $input | & $launcher --arguments-file $resolved.Path $argument_file
            }
            else {
                & $launcher --arguments-file $resolved.Path $argument_file
            }
            $codex_exit_code = $LASTEXITCODE
        }
        finally {
            [IO.File]::Delete($argument_file)
        }
    }
}
finally {
    [Environment]::SetEnvironmentVariable('PATH', $saved_path, 'Process')
    foreach ($name in $identity_names) {
        [Environment]::SetEnvironmentVariable(
            $name, $saved_identity[$name], 'Process')
    }
}

exit $codex_exit_code
