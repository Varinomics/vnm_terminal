@echo off
setlocal
REM ========================================================================
REM build_portable.bat - Build portable distribution of vnm_terminal
REM ========================================================================
REM
REM Creates a self-contained portable directory under dist\ with a visible
REM vnm_terminal.exe GUI launcher and a vnm_terminal_runtime\ directory
REM containing the real Qt application and runtime dependencies.
REM
REM Requires a build_config.bat file with local tool paths.
REM See build_config.bat.example for a template.
REM ========================================================================

cd /d "%~dp0"

if not exist "%~dp0build_config.bat" (
    echo ERROR: build_config.bat not found.
    echo.
    echo Copy build_config.bat.example to build_config.bat and set the paths
    echo for your local Qt / Visual Studio installation. For example:
    echo.
    echo   set QT_PREFIX=C:\Qt\6.10.1\msvc2022_64
    echo   set CMAKE=C:\Program Files\CMake\bin\cmake.exe
    echo   set VCVARSALL=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat
    echo   set VNM_TERMINAL_SURFACE_SOURCE_DIR=C:\plms\varinomics\vnm_terminal_surface
    echo.
    exit /b 1
)
call "%~dp0build_config.bat"

if "%QT_PREFIX%"=="" (
    echo ERROR: QT_PREFIX is not set.
    exit /b 1
)
if "%CMAKE%"=="" set CMAKE=cmake
if "%VCVARSALL%"=="" set VCVARSALL=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat
if "%VS_ARCH%"=="" set VS_ARCH=x64
if "%CMAKE_GENERATOR%"=="" set CMAKE_GENERATOR=Visual Studio 17 2022
if "%CMAKE_ARCH%"=="" set CMAKE_ARCH=x64
if "%VNM_TERMINAL_SURFACE_SOURCE_DIR%"=="" set VNM_TERMINAL_SURFACE_SOURCE_DIR=%~dp0..\vnm_terminal_surface

for %%I in ("%VNM_TERMINAL_SURFACE_SOURCE_DIR%") do set VNM_TERMINAL_SURFACE_SOURCE_DIR=%%~fI

set CONFIG=Release
set WINDEPLOYQT=%QT_PREFIX%\bin\windeployqt.exe
set BUILD_DIR=%~dp0build_portable
set DIST_DIR=%~dp0dist
set PORTABLE_DIR=%DIST_DIR%\portable
set RUNTIME_DIR=%PORTABLE_DIR%\vnm_terminal_runtime
set REAL_EXE=%BUILD_DIR%\%CONFIG%\vnm_terminal.exe
set LAUNCHER_EXE=%BUILD_DIR%\%CONFIG%\vnm_terminal_portable_launcher.exe
set PACKAGE_VERSION=1.0

if not exist "%QT_PREFIX%\bin\qmake.exe" (
    echo ERROR: Qt kit not found at %QT_PREFIX%
    exit /b 1
)
if not exist "%WINDEPLOYQT%" (
    echo ERROR: windeployqt not found at %WINDEPLOYQT%
    exit /b 1
)
if not exist "%VCVARSALL%" (
    echo ERROR: vcvarsall.bat not found at %VCVARSALL%
    exit /b 1
)
if not exist "%VNM_TERMINAL_SURFACE_SOURCE_DIR%\CMakeLists.txt" (
    echo ERROR: vnm_terminal_surface not found at %VNM_TERMINAL_SURFACE_SOURCE_DIR%
    exit /b 1
)

call "%VCVARSALL%" %VS_ARCH%
if errorlevel 1 (
    echo ERROR: Visual Studio environment initialization failed.
    exit /b 1
)

echo.
echo [1/5] Configuring CMake ...
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

"%CMAKE%" -G "%CMAKE_GENERATOR%" ^
    -A "%CMAKE_ARCH%" ^
    -DCMAKE_PREFIX_PATH="%QT_PREFIX%" ^
    -DQt6_DIR="%QT_PREFIX%\lib\cmake\Qt6" ^
    -DVNM_TERMINAL_SURFACE_SOURCE_DIR="%VNM_TERMINAL_SURFACE_SOURCE_DIR%" ^
    -DVNM_TERMINAL_ENABLE_PROFILING=OFF ^
    -DBUILD_TESTING=OFF ^
    -S "%~dp0." ^
    -B "%BUILD_DIR%"
if errorlevel 1 (
    echo ERROR: CMake configuration failed.
    exit /b 1
)

echo.
echo [2/5] Building ...
"%CMAKE%" --build "%BUILD_DIR%" --config "%CONFIG%" --target vnm_terminal vnm_terminal_portable_launcher --parallel
if errorlevel 1 (
    echo ERROR: Build failed.
    exit /b 1
)

echo.
echo [3/5] Assembling portable distribution ...

if exist "%PORTABLE_DIR%" rmdir /s /q "%PORTABLE_DIR%"
mkdir "%PORTABLE_DIR%"
mkdir "%RUNTIME_DIR%"

copy /y "%LAUNCHER_EXE%" "%PORTABLE_DIR%\vnm_terminal.exe" >nul
if errorlevel 1 (
    echo ERROR: vnm_terminal_portable_launcher.exe not found in build output.
    exit /b 1
)

copy /y "%REAL_EXE%" "%RUNTIME_DIR%\vnm_terminal.exe" >nul
if errorlevel 1 (
    echo ERROR: vnm_terminal.exe not found in build output.
    exit /b 1
)

copy /y "%~dp0LICENSE" "%PORTABLE_DIR%\LICENSE" >nul
copy /y "%~dp0THIRD_PARTY_NOTICES.md" "%PORTABLE_DIR%\THIRD_PARTY_NOTICES.md" >nul

"%WINDEPLOYQT%" --release --no-translations --dir "%RUNTIME_DIR%" "%RUNTIME_DIR%\vnm_terminal.exe"
if errorlevel 1 (
    echo ERROR: windeployqt failed.
    exit /b 1
)

if not exist "%RUNTIME_DIR%\platforms\qwindows.dll" (
    echo ERROR: Critical plugin platforms\qwindows.dll is missing.
    exit /b 1
)

for %%D in (
    qmltooling
    qml
) do (
    if exist "%RUNTIME_DIR%\%%D" rmdir /s /q "%RUNTIME_DIR%\%%D"
)

echo.
echo [4/5] Writing build info ...

set GIT_COMMIT=unknown
set GIT_BRANCH=unknown
set SURFACE_GIT_COMMIT=unknown
set SURFACE_GIT_BRANCH=unknown
for /f %%i in ('git rev-parse --short HEAD 2^>nul') do set GIT_COMMIT=%%i
for /f %%i in ('git rev-parse --abbrev-ref HEAD 2^>nul') do set GIT_BRANCH=%%i
for /f %%i in ('git -C "%VNM_TERMINAL_SURFACE_SOURCE_DIR%" rev-parse --short HEAD 2^>nul') do set SURFACE_GIT_COMMIT=%%i
for /f %%i in ('git -C "%VNM_TERMINAL_SURFACE_SOURCE_DIR%" rev-parse --abbrev-ref HEAD 2^>nul') do set SURFACE_GIT_BRANCH=%%i
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyy-MM-dd_HH:mm"') do set TIMESTAMP=%%i

(
    echo Build timestamp: %TIMESTAMP%
    echo Git branch:      %GIT_BRANCH%
    echo Git commit:      %GIT_COMMIT%
    echo Surface branch:  %SURFACE_GIT_BRANCH%
    echo Surface commit:  %SURFACE_GIT_COMMIT%
    echo Version:         %PACKAGE_VERSION%
    echo Configuration:   %CONFIG%
    echo Profiling:       OFF
    echo Toolchain:       MSVC
    echo Qt:              %QT_PREFIX%
) > "%RUNTIME_DIR%\vnm_terminal_build_info.txt"

echo.
echo [5/5] Creating ZIP archive ...

set ZIP_NAME=vnm_terminal_v%PACKAGE_VERSION%_w64.zip
set ZIP_PATH=%DIST_DIR%\%ZIP_NAME%
if exist "%ZIP_PATH%" del /q "%ZIP_PATH%"

powershell -NoProfile -Command "Compress-Archive -Path '%PORTABLE_DIR%\*' -DestinationPath '%ZIP_PATH%' -Force"
if errorlevel 1 (
    echo WARNING: ZIP creation failed. Portable directory is still available.
) else (
    echo Created: %ZIP_PATH%
)

echo.
echo ========================================================================
echo Portable distribution ready at:
echo   %PORTABLE_DIR%
echo   %ZIP_PATH%
echo ========================================================================
