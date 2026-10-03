if ($MyInvocation.ExpectingInput) {
    $input | & "$PSScriptRoot/launch_codex.ps1" @CODEX_COMMAND@ @args
}
else {
    & "$PSScriptRoot/launch_codex.ps1" @CODEX_COMMAND@ @args
}
exit $LASTEXITCODE
