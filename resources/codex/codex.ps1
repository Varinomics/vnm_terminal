if ($MyInvocation.ExpectingInput) {
    $input | & "$PSScriptRoot/launch_codex.ps1" -CommandPrefix @('@CODEX_COMMAND@') -Arguments $args
}
else {
    & "$PSScriptRoot/launch_codex.ps1" -CommandPrefix @('@CODEX_COMMAND@') -Arguments $args
}
exit $LASTEXITCODE
