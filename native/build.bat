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
set "OVR=third_party\LibOVR"

rem ovrprobe: a plain LibOVR app, to see the headset the way games do
cl.exe /nologo /MT /O2 /EHsc /W3 /I%OVR%\Include /Foobj\ /Fe"out\ovrprobe.exe" src\ovrprobe.cpp %OVR%\Shim\OVR_CAPIShim.c
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%

rem SteamFrameSim.exe: the simulated headset, driving Meta's emulator through OculusDebugToolCLI
rem (src\unsigned_plugin is the dropped plugin route: OVRServer only loads Meta-signed plugins)
cl.exe /nologo /MT /O2 /EHsc /W3 /Foobj\ /Fe"out\SteamFrameSim.exe" src\steamframe_sim.cpp user32.lib gdi32.lib dwmapi.lib /link /SUBSYSTEM:WINDOWS
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%
del /q out\*.exp out\*.lib 2>nul
echo Built.
