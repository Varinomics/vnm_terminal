@echo off
setlocal
REM ========================================================================
REM build_windows_packages.bat - Build the portable ZIP and per-user MSI
REM ========================================================================

cd /d "%~dp0"

call "%~dp0build_portable.bat"
if errorlevel 1 exit /b 1

call "%~dp0build_config.bat"

if "%CMAKE%"=="" set CMAKE=cmake
if "%CPACK%"=="" (
    for %%I in ("%CMAKE%") do set CPACK=%%~dpIcpack.exe
)
if not exist "%CPACK%" set CPACK=cpack

set CONFIG=Release
set BUILD_DIR=%~dp0build_portable

echo.
echo Configuring Windows installer packaging ...
"%CMAKE%" -S "%~dp0." -B "%BUILD_DIR%" ^
    -DVNM_TERMINAL_BUILD_PACKAGES=ON
if errorlevel 1 (
    echo ERROR: Installer packaging configuration failed.
    exit /b 1
)

"%CMAKE%" --build "%BUILD_DIR%" --config "%CONFIG%" --target vnm_terminal --parallel
if errorlevel 1 (
    echo ERROR: Installer packaging build failed.
    exit /b 1
)

echo.
echo Creating MSI installer ...
"%CPACK%" -G WIX -C "%CONFIG%" --config "%BUILD_DIR%\CPackConfig.cmake"
if errorlevel 1 (
    echo ERROR: MSI creation failed. WiX Toolset 3 is required.
    exit /b 1
)

for %%F in ("%~dp0dist\vnm_terminal_v*_windows_x64.msi") do (
    if exist "%%~fF" set MSI_PATH=%%~fF
)
if "%MSI_PATH%"=="" (
    echo ERROR: CPack completed without creating the expected MSI.
    exit /b 1
)

echo.
echo ========================================================================
echo Windows packages ready:
echo   %~dp0dist\vnm_terminal_v*_w64.zip
echo   %MSI_PATH%
echo ========================================================================
