# The Atrium Runtime for Windows: Atrium's build of Monado (BSL-1.0), the same pinned commit and
# patches as build.sh (Linux). Used for headsets Atrium drives itself on Windows: streamed
# standalone headsets (Quest, Pico). Wired PC headsets on Windows stay on the forwarding runtime
# (native/atrium_xr) and Meta's OVRServer.
#
# Output (native\out\monado\):
#   openxr_atrium.dll    the OpenXR runtime games load
#   atrium-service.exe   the service (compositor + drivers); the Atrium app runs it
#   atrium_openxr.json   the runtime manifest
#   alvr_server_core.dll  ALVR's streaming core (MIT), used by the Atrium streaming driver
#   cjson.dll, pthreadVCE3.dll
#
# Needs: Visual Studio (C++), the Vulkan SDK, git, Python. vcpkg is fetched into build\vcpkg.
#
# -Rift adds Atrium's own Rift CV1 drivers (rift, rift_sensor, the constellation tracker: patches
# 0006-0007). Opt-in while they're tested: the service would otherwise open a Rift that Meta's
# OVRServer is using. The sensors need UsbDk installed.
param([string]$VsDir = 'J:\vs2026', [switch]$Rift)
# Native tools print progress on stderr; exit codes are checked instead (Windows PowerShell 5.1).
$ErrorActionPreference = 'Continue'
function Check([string]$what) { if ($LASTEXITCODE) { throw "$what failed ($LASTEXITCODE)" } }

$MonadoCommit = '5bca1effb30aed2ce68d805cddff1bc87b67005e'   # main, 2026-10-20 (same as build.sh)
$VcpkgTag = '2026.07.29'
$VcpkgBaseline = '4334d8b4c8916018600212ab4dd4bbdc343065d1' # Monado's vcpkg.json builtin-baseline

$Here = $PSScriptRoot
$Build = Join-Path $Here 'build'
$Src = Join-Path $Build 'monado'
$Vcpkg = Join-Path $Build 'vcpkg'
$Bin = Join-Path $Build 'win'
$Out = Join-Path $Here '..\out\monado'
New-Item -ItemType Directory -Force $Build, $Out | Out-Null

# Pinned Monado, patched from a clean tree each run.
if (-not (Test-Path (Join-Path $Src '.git'))) {
    git clone --quiet https://gitlab.freedesktop.org/monado/monado.git $Src; Check 'git clone monado'
}
git -C $Src fetch --quiet origin $MonadoCommit 2>$null
git -C $Src checkout --quiet --force $MonadoCommit; Check 'git checkout monado'
git -C $Src clean --quiet -fdx; Check 'git clean'
foreach ($p in Get-ChildItem (Join-Path $Here 'patches\*.patch') | Sort-Object Name) {
    git -C $Src apply --whitespace=nowarn $p.FullName; Check "patch $($p.Name)"
}

# Atrium's streaming driver goes into the Monado tree (patch 0004 registers it).
Copy-Item (Join-Path $Here 'atrium_stream') (Join-Path $Src 'src\xrt\drivers\atrium_stream') -Recurse -Force

# ALVR's server_core (MIT): the streaming protocol the driver speaks to the Atrium headset app.
# Built from the same pinned ALVR checkout as the headset app (native\atrium_client).
$AlvrSrc = Join-Path $Here '..\atrium_client\build\alvr'
if (-not (Test-Path (Join-Path $AlvrSrc 'Cargo.toml'))) { throw 'Run native\atrium_client\build.ps1 first (it fetches ALVR)' }
$AlvrOut = Join-Path $Build 'alvr_core'
New-Item -ItemType Directory -Force $AlvrOut | Out-Null
Push-Location $AlvrSrc
try {
    cargo build -p alvr_server_core --release; Check 'ALVR server_core build'
    # cbindgen writes ALVR's C API header; one union member is named "float", a C keyword.
    $hdr = Join-Path $AlvrOut 'alvr_server_core.h'
    Push-Location 'alvr\server_core'
    cbindgen --config cbindgen.toml --crate alvr_server_core -o $hdr . 2>$null; Check 'cbindgen'
    Pop-Location
    (Get-Content $hdr -Raw).Replace('float float;', 'float float_;') | Set-Content $hdr -Encoding ascii -NoNewline
} finally { Pop-Location }
Copy-Item (Join-Path $AlvrSrc 'target\release\alvr_server_core.dll'), (Join-Path $AlvrSrc 'target\release\alvr_server_core.dll.lib') $AlvrOut -Force
$AlvrOutCmake = $AlvrOut.Replace('\', '/')

# vcpkg, plus the baseline commit Monado's manifest pins (a shallow clone doesn't have it).
if (-not (Test-Path (Join-Path $Vcpkg 'vcpkg.exe'))) {
    git clone --quiet --depth 1 --branch $VcpkgTag https://github.com/microsoft/vcpkg.git $Vcpkg; Check 'git clone vcpkg'
    cmd /c "`"$Vcpkg\bootstrap-vcpkg.bat`" -disableMetrics"; Check 'vcpkg bootstrap'
}
git -C $Vcpkg cat-file -e "$VcpkgBaseline^{commit}" 2>$null
if ($LASTEXITCODE) { git -C $Vcpkg fetch --quiet --depth 1 origin $VcpkgBaseline; Check 'vcpkg baseline fetch' }

# Configure + build in a Visual Studio x64 environment. /Zc:__cplusplus: MSVC otherwise reports
# C++98 and Monado falls back to <experimental/filesystem>, which newer toolsets removed.
$ninja = Join-Path $VsDir 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
# PS Sense is off: its LED tables don't compile with MSVC, and it's no use here.
$RiftArgs = if ($Rift) {
    '"-DVCPKG_MANIFEST_FEATURES=usb;constellation" -DXRT_BUILD_DRIVER_RIFT=ON -DXRT_BUILD_DRIVER_RIFT_SENSOR=ON -DXRT_BUILD_DRIVER_PSSENSE=OFF'
} else { '' }
$bat = Join-Path $Build 'configure.bat'
Set-Content $bat -Encoding ascii -Value @"
@echo off
call "$VsDir\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set PATH=$ninja;%PATH%
set VCPKG_ROOT=$Vcpkg
cmake -S "$Src" -B "$Bin" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  "-DCMAKE_TOOLCHAIN_FILE=$Vcpkg\scripts\buildsystems\vcpkg.cmake" ^
  -DVCPKG_MANIFEST_NO_DEFAULT_FEATURES=ON ^
  "-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /EHsc /GR /Zc:__cplusplus" ^
  -DBUILD_TESTING=OFF -DBUILD_DOC=OFF ^
  -DXRT_OXR_RUNTIME_SUFFIX=atrium -DXRT_IPC_MSG_SOCK_FILENAME=atrium_comp_ipc ^
  -DXRT_FEATURE_SERVICE=ON -DXRT_FEATURE_STEAMVR_PLUGIN=OFF -DXRT_FEATURE_OPENVR=OFF ^
  -DXRT_BUILD_DRIVER_ATRIUM_STREAM=ON "-DATRIUM_ALVR_DIR=$AlvrOutCmake" $RiftArgs
if errorlevel 1 exit /b 1
cmake --build "$Bin"
"@
cmd /c "`"$bat`""; Check 'build'

# Stage under Atrium's names. vulkan-1.dll is left out: games use the GPU driver's loader.
$t = Join-Path $Bin 'src\xrt\targets'
Copy-Item "$t\openxr\openxr_atrium.dll", "$t\openxr\cjson.dll", "$t\openxr\pthreadVCE3.dll" $Out -Force
Copy-Item "$t\service\monado-service.exe" (Join-Path $Out 'atrium-service.exe') -Force
Copy-Item (Join-Path $AlvrOut 'alvr_server_core.dll') $Out -Force
Set-Content (Join-Path $Out 'atrium_openxr.json') -Encoding ascii -Value @'
{
    "file_format_version": "1.0.0",
    "runtime": {
        "name": "Atrium",
        "library_path": "./openxr_atrium.dll"
    }
}
'@
Write-Host "Built: $(Resolve-Path $Out)"
