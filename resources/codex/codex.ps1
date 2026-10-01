if ($PSVersionTable.PSVersion -lt [version]'7.3') {
    # Windows PowerShell drops empty native arguments and embedded quotes.
    # Carry the invocation as encoded script text into the prepared pwsh host.
    $forwarded_command = "& '" + $PSScriptRoot.Replace("'", "''") + "/start_codex.ps1' codex"
    foreach ($argument in $args) {
        $forwarded_command += " '" + ([string]$argument).Replace("'", "''") + "'"
    }
    $forwarded_command += '; exit $LASTEXITCODE'
    $encoded_command = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($forwarded_command))
    if ($MyInvocation.ExpectingInput) {
        $input | & '@POWERSHELL@' -NoLogo -NoProfile -OutputFormat Text -EncodedCommand $encoded_command
    }
    else {
        & '@POWERSHELL@' -NoLogo -NoProfile -OutputFormat Text -EncodedCommand $encoded_command
    }
    exit $LASTEXITCODE
}

if ($MyInvocation.ExpectingInput) {
    $input | & "$PSScriptRoot/launch_codex.ps1" codex @args
}
else {
    & "$PSScriptRoot/launch_codex.ps1" codex @args
}
exit $LASTEXITCODE
