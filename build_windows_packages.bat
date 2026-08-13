@echo off
setlocal
REM ========================================================================
REM build_windows_packages.bat - Build the portable ZIP and Qt IFW EXE
REM ========================================================================

cd /d "%~dp0"

call "%~dp0build_portable.bat"
if errorlevel 1 exit /b 1

echo.
echo Creating unsigned Qt IFW installer for validation ...
powershell.exe -NoProfile -ExecutionPolicy Bypass ^
    -File "%~dp0tools\build_windows_ifw_installer.ps1" ^
    -PayloadPath "%~dp0dist\portable_candidate"
if errorlevel 1 (
    echo ERROR: Qt IFW installer creation failed.
    exit /b 1
)

echo.
echo ========================================================================
echo Windows packages ready:
echo   %~dp0dist\vnm_terminal_v*_w64.zip
echo   %~dp0dist\vnm_terminal_v*_windows_x64_unsigned.exe
echo ========================================================================
