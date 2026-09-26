@"@POWERSHELL@" -NoLogo -NoProfile -File "%~dp0start_codex.ps1" --cmd-path %*
@exit /b %errorlevel%
