@echo off
call "J:\vs2026\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d J:\EchoVR-Tools-Launcher\Atrium\native\atrium_xr
cmake -S . -B build -A x64 >nul
cmake --build build --config Release --parallel
