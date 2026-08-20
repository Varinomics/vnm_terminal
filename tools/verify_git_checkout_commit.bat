@echo off
setlocal EnableExtensions DisableDelayedExpansion

set "VNM_VERIFY_SOURCE_DIR=%~1"
set "VNM_VERIFY_EXPECTED_COMMIT=%~2"
set "VNM_VERIFY_DEPENDENCY_NAME=%~3"

if "%VNM_VERIFY_SOURCE_DIR%"=="" (
    echo ERROR: Git commit verification requires a source directory.
    exit /b 2
)
if "%VNM_VERIFY_EXPECTED_COMMIT%"=="" (
    echo ERROR: Git commit verification requires an expected commit.
    exit /b 2
)
if "%VNM_VERIFY_DEPENDENCY_NAME%"=="" (
    echo ERROR: Git commit verification requires a dependency name.
    exit /b 2
)

set "VNM_VERIFY_OUTPUT=%TEMP%\vnm-terminal-git-commit-%RANDOM%-%RANDOM%.txt"
git -C "%VNM_VERIFY_SOURCE_DIR%" rev-parse --verify HEAD > "%VNM_VERIFY_OUTPUT%" 2>nul
set "VNM_VERIFY_GIT_EXIT_CODE=%ERRORLEVEL%"
if not "%VNM_VERIFY_GIT_EXIT_CODE%"=="0" (
    del /q "%VNM_VERIFY_OUTPUT%" >nul 2>nul
    echo ERROR: %VNM_VERIFY_DEPENDENCY_NAME% did not resolve to locked commit %VNM_VERIFY_EXPECTED_COMMIT%.
    exit /b 1
)

set "VNM_VERIFY_ACTUAL_COMMIT="
set "VNM_VERIFY_OUTPUT_RECORDS=0"
setlocal EnableDelayedExpansion
for /f "usebackq delims=" %%I in ("!VNM_VERIFY_OUTPUT!") do (
    set /a VNM_VERIFY_OUTPUT_RECORDS+=1 >nul
    set "VNM_VERIFY_ACTUAL_COMMIT=%%I"
)
del /q "!VNM_VERIFY_OUTPUT!" >nul 2>nul

if not "!VNM_VERIFY_OUTPUT_RECORDS!"=="1" (
    echo ERROR: !VNM_VERIFY_DEPENDENCY_NAME! did not resolve to locked commit !VNM_VERIFY_EXPECTED_COMMIT!.
    exit /b 1
)
if /i not "!VNM_VERIFY_ACTUAL_COMMIT!"=="!VNM_VERIFY_EXPECTED_COMMIT!" (
    echo ERROR: !VNM_VERIFY_DEPENDENCY_NAME! did not resolve to locked commit !VNM_VERIFY_EXPECTED_COMMIT!.
    exit /b 1
)

exit /b 0
