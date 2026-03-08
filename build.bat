@echo off
REM BALANCER_META:
REM   meta_version: 1
REM   component: fatp-balancer
REM   file_role: build_script
REM   path: build.bat
REM   layer: Testing
REM   summary: Windows batch build script for fatp-balancer — configure, build, test.
REM   api_stability: in_work

setlocal

REM %~dp0 ends with a backslash. Strip it so quoted paths don't get their
REM closing quote escaped: "C:\path with spaces\" is seen as unclosed.
set SCRIPT_DIR=%~dp0
set SCRIPT_DIR=%SCRIPT_DIR:~0,-1%

set BUILD_DIR=%SCRIPT_DIR%\build
set BUILD_TYPE=Debug

if defined FATP_INCLUDE_DIR (
    set FATP_DIR=%FATP_INCLUDE_DIR%
) else (
    set FATP_DIR=%SCRIPT_DIR%\..\FatP\include\fat_p
)

echo === fatp-balancer build ===
echo   Build type : %BUILD_TYPE%
echo   FAT-P dir  : %FATP_DIR%

cmake -S "%SCRIPT_DIR%" -B "%BUILD_DIR%" -DCMAKE_BUILD_TYPE=%BUILD_TYPE% "-DFATP_INCLUDE_DIR=%FATP_DIR%"
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%

cmake --build "%BUILD_DIR%" --config %BUILD_TYPE%
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%

echo.
echo === Running tests ===
cd /d "%BUILD_DIR%"
ctest --output-on-failure --config %BUILD_TYPE%
if %ERRORLEVEL% neq 0 (
    echo === Tests FAILED ===
    exit /b %ERRORLEVEL%
)

echo.
echo === All tests passed ===
endlocal
