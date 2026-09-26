# Keep process stdin inherited; only the shell PATH shim forwards PowerShell pipeline objects.
& "$PSScriptRoot/launch_codex.ps1" @args
exit $LASTEXITCODE
