@echo off
setlocal
REM ========================================================================
REM build_portable.bat - Build portable distribution of vnm_terminal
REM ========================================================================
REM
REM Creates a self-contained portable directory under dist\ with a visible
REM vnm_terminal.exe GUI launcher and a vnm_terminal_runtime\ directory
REM containing the real Qt application and Qt/MinGW runtime dependencies.
REM
REM Requires a build_config.bat file with local tool paths.
REM See build_config.bat.example for a template.
REM ========================================================================

cd /d "%~dp0"

if not exist "%~dp0build_config.bat" (
    echo ERROR: build_config.bat not found.
    echo.
    echo Copy build_config.bat.example to build_config.bat and set the paths
    echo for your local Qt / MinGW installation. For example:
    echo.
    echo   set QT_PREFIX=C:\Qt\6.10.1\mingw_64
    echo   set MINGW_BIN=C:\Qt\Tools\mingw1310_64\bin
    echo   set CMAKE=C:\Qt\Tools\CMake_64\bin\cmake.exe
    echo   set NINJA=C:\Qt\Tools\Ninja\ninja.exe
    echo   set VNM_TERMINAL_SURFACE_SOURCE_DIR=C:\plms\varinomics\vnm_terminal_surface
    echo.
    exit /b 1
)
call "%~dp0build_config.bat"

if "%QT_PREFIX%"=="" (
    echo ERROR: QT_PREFIX is not set.
    exit /b 1
)
if "%MINGW_BIN%"=="" (
    echo ERROR: MINGW_BIN is not set.
    exit /b 1
)
if "%CMAKE%"=="" set CMAKE=cmake
if "%NINJA%"=="" set NINJA=ninja
if "%VNM_TERMINAL_SURFACE_SOURCE_DIR%"=="" set VNM_TERMINAL_SURFACE_SOURCE_DIR=%~dp0..\vnm_terminal_surface

for %%I in ("%VNM_TERMINAL_SURFACE_SOURCE_DIR%") do set VNM_TERMINAL_SURFACE_SOURCE_DIR=%%~fI

set CONFIG=Release
set WINDEPLOYQT=%QT_PREFIX%\bin\windeployqt.exe
set BUILD_DIR=%~dp0build_portable
set DIST_DIR=%~dp0dist
set PORTABLE_DIR=%DIST_DIR%\portable
set RUNTIME_DIR=%PORTABLE_DIR%\vnm_terminal_runtime
set REAL_EXE=%BUILD_DIR%\vnm_terminal.exe
set LAUNCHER_EXE=%BUILD_DIR%\vnm_terminal_portable_launcher.exe
REM PACKAGE_VERSION is derived from the configured CMake project version after
REM the configure step below (single source of truth: project() in CMakeLists.txt).

if not exist "%CMAKE%" (
    echo ERROR: CMake not found at %CMAKE%
    exit /b 1
)
if not exist "%NINJA%" (
    echo ERROR: Ninja not found at %NINJA%
    exit /b 1
)
if not exist "%MINGW_BIN%\g++.exe" (
    echo ERROR: MinGW g++ not found at %MINGW_BIN%\g++.exe
    exit /b 1
)
if not exist "%MINGW_BIN%\gcc.exe" (
    echo ERROR: MinGW gcc not found at %MINGW_BIN%\gcc.exe
    exit /b 1
)
if not exist "%QT_PREFIX%\bin\qmake.exe" (
    echo ERROR: Qt kit not found at %QT_PREFIX%
    exit /b 1
)
if not exist "%WINDEPLOYQT%" (
    echo ERROR: windeployqt not found at %WINDEPLOYQT%
    exit /b 1
)
if not exist "%VNM_TERMINAL_SURFACE_SOURCE_DIR%\CMakeLists.txt" (
    echo ERROR: vnm_terminal_surface not found at %VNM_TERMINAL_SURFACE_SOURCE_DIR%
    exit /b 1
)

echo.
echo [1/5] Configuring CMake ...
if exist "%BUILD_DIR%\CMakeCache.txt" (
    findstr /c:"CMAKE_GENERATOR:INTERNAL=Ninja" "%BUILD_DIR%\CMakeCache.txt" >nul
    if errorlevel 1 (
        echo Removing stale portable build configured with another CMake generator ...
        rmdir /s /q "%BUILD_DIR%"
    )
)
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

"%CMAKE%" -G Ninja ^
    -DCMAKE_BUILD_TYPE=%CONFIG% ^
    -DCMAKE_PREFIX_PATH="%QT_PREFIX%" ^
    -DQt6_DIR="%QT_PREFIX%\lib\cmake\Qt6" ^
    -DCMAKE_CXX_COMPILER="%MINGW_BIN%\g++.exe" ^
    -DCMAKE_C_COMPILER="%MINGW_BIN%\gcc.exe" ^
    -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
    -DVNM_TERMINAL_SURFACE_SOURCE_DIR="%VNM_TERMINAL_SURFACE_SOURCE_DIR%" ^
    -DVNM_TERMINAL_ENABLE_PROFILING=OFF ^
    -DVNM_TERMINAL_ENABLE_TRANSCRIPT_CAPTURE_REPLAY=OFF ^
    -DVNM_TERMINAL_DISTRIBUTION_BUILD=ON ^
    -DBUILD_TESTING=OFF ^
    -S "%~dp0." ^
    -B "%BUILD_DIR%"
if errorlevel 1 (
    echo ERROR: CMake configuration failed.
    exit /b 1
)

echo.
echo Deriving package version from the configured CMake project version ...
for /f "tokens=2 delims==" %%V in ('findstr /b /c:"CMAKE_PROJECT_VERSION:STATIC=" "%BUILD_DIR%\CMakeCache.txt"') do set PACKAGE_VERSION=%%V
if "%PACKAGE_VERSION%"=="" (
    echo ERROR: Could not derive PACKAGE_VERSION from "%BUILD_DIR%\CMakeCache.txt".
    exit /b 1
)
echo Package version: %PACKAGE_VERSION%

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

for %%F in (
    libgcc_s_seh-1.dll
    libstdc++-6.dll
    libwinpthread-1.dll
) do (
    if not exist "%RUNTIME_DIR%\%%F" (
        if exist "%MINGW_BIN%\%%F" copy /y "%MINGW_BIN%\%%F" "%RUNTIME_DIR%\%%F" >nul
    )
    if not exist "%RUNTIME_DIR%\%%F" (
        echo ERROR: Required MinGW runtime DLL %%F is missing.
        exit /b 1
    )
)

if exist "%PORTABLE_DIR%\vc_redist.x64.exe" (
    echo ERROR: MSVC redistributable unexpectedly present in portable root.
    exit /b 1
)
if exist "%RUNTIME_DIR%\vc_redist.x64.exe" (
    echo ERROR: MSVC redistributable unexpectedly present in runtime directory.
    exit /b 1
)

REM Strip the QML debug-tooling plugins (qmltooling\): they are only used by the
REM QML debugger/profiler and are not needed in a distribution build.
if exist "%RUNTIME_DIR%\qmltooling" rmdir /s /q "%RUNTIME_DIR%\qmltooling"

REM Deploy the QtQml and QtQuick QML module trees.
REM windeployqt cannot discover the chrome's QML imports: the chrome QML is a
REM C++ raw-string literal (see k_terminal_chrome_qml in src\qml_chrome.cpp),
REM loaded via QQmlComponent with a synthetic qrc: URL, so there is no .qml file
REM for windeployqt's import scanner to read, and it leaves qml\ empty. The chrome
REM imports QtQuick and QtQuick.Window (QtQuick.Window lives under the QtQuick
REM module tree). Mirror the regular build's post-build step
REM (vnm_terminal_copy_qt_qml_module for QtQml and QtQuick in CMakeLists.txt) by
REM copying those module trees from the Qt kit. Without them the portable app
REM exits immediately with: module "QtQuick.Window" is not installed
for %%M in (
    QtQml
    QtQuick
) do (
    if not exist "%QT_PREFIX%\qml\%%M" (
        echo ERROR: Qt QML module %%M not found at "%QT_PREFIX%\qml\%%M".
        exit /b 1
    )
    "%CMAKE%" -E copy_directory "%QT_PREFIX%\qml\%%M" "%RUNTIME_DIR%\qml\%%M"
    if errorlevel 1 (
        echo ERROR: Failed to deploy Qt QML module %%M into the runtime.
        exit /b 1
    )
)
if not exist "%RUNTIME_DIR%\qml\QtQuick\Window" (
    echo ERROR: QtQuick.Window QML module missing after deployment.
    exit /b 1
)

REM Deploy the QtQuick.Controls / Templates / Layouts runtime DLLs.
REM The chrome QML and the VNM_Chrome module it loads import QtQuick.Controls,
REM QtQuick.Controls.Basic and QtQuick.Layouts. windeployqt cannot see these
REM imports (the chrome QML is a C++ string and the VNM_Chrome QML is qrc-embedded
REM in the vnm_qml_chrome library), so it does not deploy the C++ libraries that
REM the QtQuick.Controls QML plugins load dynamically. A local dev build resolves
REM them from the Qt bin on PATH; a portable build has no such fallback. Mirror the
REM regular build's explicit list (vnm_terminal_copy_qt_runtime_library for the
REM QuickControls2 libraries in CMakeLists.txt) and add the QuickControls2 /
REM QuickTemplates2 base libraries and QuickLayouts that those plugins depend on.
REM Without them the portable app exits immediately with:
REM   Cannot load library ...qtquickcontrols2plugin.dll
REM The chrome forces the Basic style (QT_QUICK_CONTROLS_STYLE=Basic in the
REM vnm_qml_chrome build); the wider style set matches the regular build.
for %%F in (
    Qt6QuickControls2.dll
    Qt6QuickControls2Impl.dll
    Qt6QuickControls2Basic.dll
    Qt6QuickControls2BasicStyleImpl.dll
    Qt6QuickControls2Fusion.dll
    Qt6QuickControls2FusionStyleImpl.dll
    Qt6QuickControls2WindowsStyleImpl.dll
    Qt6QuickTemplates2.dll
    Qt6QuickLayouts.dll
) do (
    if not exist "%QT_PREFIX%\bin\%%F" (
        echo ERROR: Required Qt runtime DLL %%F not found at "%QT_PREFIX%\bin\%%F".
        exit /b 1
    )
    copy /y "%QT_PREFIX%\bin\%%F" "%RUNTIME_DIR%\%%F" >nul
    if errorlevel 1 (
        echo ERROR: Failed to copy Qt runtime DLL %%F into the runtime.
        exit /b 1
    )
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
    echo Distribution:    ON
    echo Profiling:       OFF
    echo Transcript:      OFF
    echo Toolchain:       MinGW ^(GCC^)
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
