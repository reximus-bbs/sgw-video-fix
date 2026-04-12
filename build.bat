@echo off
rem =====================================================================
rem  SGW Video Fix - build script (x86 wmvcore.dll proxy)
rem
rem  Produces  build\wmvcore.dll  from src\sgw_video_fix.cpp.
rem
rem  Requirements:
rem    - Visual Studio 2022 (or newer) with the x86 C++ toolchain.
rem    - Run from the **x86 Native Tools Command Prompt for VS** so
rem      cl.exe and link.exe are on PATH and target Win32.
rem
rem  Optional environment overrides:
rem    set WMVPROXY_EXTRA_DEFS=/D WMVPROXY_VERBOSE=1
rem        Builds a verbose diagnostic DLL that writes wmvproxy.log
rem        next to itself.  Default build is silent.
rem
rem  Output:  build\wmvcore.dll  (drop next to Sniper_x86.exe)
rem =====================================================================

setlocal

if not exist build mkdir build
del /q src\sgw_video_fix.obj 2>nul

echo Building SGW Video Fix (wmvcore.dll, x86)...
echo.

cl.exe /nologo /O2 /W3 /EHsc /MT ^
    /D "WIN32" /D "NDEBUG" /D "_WINDOWS" ^
    %WMVPROXY_EXTRA_DEFS% ^
    /Fo"src\\" ^
    src\sgw_video_fix.cpp ^
    /link /DLL /NOLOGO ^
    /MACHINE:X86 ^
    /DEF:src\sgw_video_fix.def ^
    /OUT:"build\wmvcore.dll" ^
    kernel32.lib ole32.lib oleaut32.lib user32.lib strmiids.lib

if %ERRORLEVEL% neq 0 (
    echo.
    echo BUILD FAILED
    exit /b %ERRORLEVEL%
)

del /q src\sgw_video_fix.obj 2>nul
del /q build\wmvcore.exp build\wmvcore.lib 2>nul

echo.
echo SUCCESS: build\wmvcore.dll
echo.
endlocal
