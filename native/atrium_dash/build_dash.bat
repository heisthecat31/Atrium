@echo off
call "J:\vs2026\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d J:\EchoVR-Tools-Launcher\Atrium\native\atrium_dash
cmake -S . -B build -A x64 >build_configure.log 2>&1
if errorlevel 1 (type build_configure.log & exit /b 1)
cmake --build build --config Release --parallel
