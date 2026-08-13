@echo off
setlocal
REM ========================================================================
REM build_windows_packages.bat - Build the portable ZIP and Qt IFW EXE
REM ========================================================================

cd /d "%~dp0"

if not exist "%~dp0build_config.bat" (
    echo ERROR: build_config.bat not found.
    echo Copy build_config.bat.example to build_config.bat and set IFW_ROOT.
    exit /b 1
)
call "%~dp0build_config.bat"

if "%IFW_ROOT%"=="" (
    echo ERROR: IFW_ROOT is not set in build_config.bat.
    echo Run tools\provision_windows_ifw.ps1, then set IFW_ROOT to its destination.
    exit /b 1
)
if not exist "%IFW_ROOT%\bin\binarycreator.exe" (
    echo ERROR: Qt IFW binarycreator not found under IFW_ROOT: %IFW_ROOT%
    exit /b 1
)

call "%~dp0build_portable.bat"
if errorlevel 1 exit /b 1

echo.
echo Creating unsigned Qt IFW installer for validation ...
powershell.exe -NoProfile -ExecutionPolicy Bypass ^
    -File "%~dp0tools\build_windows_ifw_installer.ps1" ^
    -PayloadPath "%~dp0dist\portable_candidate" ^
    -IfwRoot "%IFW_ROOT%"
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
