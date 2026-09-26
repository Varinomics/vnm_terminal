#Requires -Version 7.3
if ($MyInvocation.ExpectingInput) {
    $input | & "$PSScriptRoot/launch_codex.ps1" codex @args
}
else {
    & "$PSScriptRoot/launch_codex.ps1" codex @args
}
exit $LASTEXITCODE
