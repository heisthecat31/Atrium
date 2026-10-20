@echo off
setlocal
set "VARS_BAT="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VARS_BAT=%%i\VC\Auxiliary\Build\vcvars64.bat"
    )
)
if not defined VARS_BAT (
    echo [ERROR] MSVC not found.
    exit /b 1
)
if not defined VSCMD_ARG_TGT_ARCH call "%VARS_BAT%" >nul
cd /d "%~dp0"
if not exist out mkdir out
if not exist obj mkdir obj
rem Atrium OVR (atrium_ovr\): the translation layer for Oculus SDK games. CMake fetches its pinned
rem sources (Revive, OpenXR SDK, Oculus SDK headers) on the first run.
cmake -S atrium_ovr -B atrium_ovr\build -A x64 >nul
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%
cmake --build atrium_ovr\build --config Release --target atrium_ovr --parallel
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%
if not exist out\ovr mkdir out\ovr
copy /y atrium_ovr\build\out\ovr\LibOVRRT64_1.dll out\ovr\ >nul

rem The Atrium Runtime (atrium_xr\): Atrium's OpenXR runtime, atrium_xr.dll + atrium_openxr.json.
cmake -S atrium_xr -B atrium_xr\build -A x64 >nul
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%
cmake --build atrium_xr\build --config Release --parallel
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%
copy /y atrium_xr\build\out\atrium_xr.dll out\ >nul
copy /y atrium_xr\build\out\atrium_openxr.json out\ >nul

rem Oculus SDK headers: a local copy in third_party\LibOVR, else the one CMake fetched above.
set "OVR=third_party\LibOVR"
if not exist "%OVR%\Include\OVR_CAPI.h" set "OVR=atrium_ovr\build\_deps\ovr_sdk-src\LibOVR"

rem ovrprobe: a plain LibOVR app, to see the headset the way games do
cl.exe /nologo /MT /O2 /EHsc /W3 /I%OVR%\Include /Foobj\ /Fe"out\ovrprobe.exe" src\ovrprobe.cpp %OVR%\Shim\OVR_CAPIShim.c
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%

rem SteamFrameSim.exe: the simulated headset, driving Meta's emulator through OculusDebugToolCLI
rem (src\unsigned_plugin is the dropped plugin route: OVRServer only loads Meta-signed plugins)
cl.exe /nologo /MT /O2 /EHsc /W3 /Foobj\ /Fe"out\SteamFrameSim.exe" src\steamframe_sim.cpp user32.lib gdi32.lib dwmapi.lib /link /SUBSYSTEM:WINDOWS
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%
del /q out\*.exp out\*.lib 2>nul
echo Built.
