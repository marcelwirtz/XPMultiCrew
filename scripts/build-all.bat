@echo off
REM Configure and build the XPMultiCrew X-Plane plugin on Windows (MSVC).
REM
REM Usage:
REM   scripts\build-all.bat [C:\path\to\extracted\XPSDK]
REM
REM If no SDK path is given, the script expects the SDK to already be
REM extracted at third_party\XPSDK (see plugin\CMakeLists.txt).
setlocal

set ROOT_DIR=%~dp0..
set BUILD_DIR=%ROOT_DIR%\build
if "%~1"=="" (
    set XPSDK_ROOT=%ROOT_DIR%\third_party\XPSDK
) else (
    set XPSDK_ROOT=%~1
)

cmake -S "%ROOT_DIR%" -B "%BUILD_DIR%" -A x64 -DXPSDK_ROOT="%XPSDK_ROOT%"
if errorlevel 1 exit /b 1

cmake --build "%BUILD_DIR%" --config RelWithDebInfo
if errorlevel 1 exit /b 1

echo Built plugin: %BUILD_DIR%\XPMultiCrew
