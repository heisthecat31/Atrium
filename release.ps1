# release.ps1: builds Atrium and packs dist\Atrium-<version>-windows-x64.zip.
#
#   .\release.ps1 [-Monado <Monado build folder>] [-SkipBuild]
#
# - atrium.exe is built with the C runtime linked in (no Visual C++ redistributable needed).
# - native\build.bat builds the runtime, Atrium OVR, the dash, the tools and TouchLink.
# - The tracking service comes from a Monado build made with native\atrium_monado\build.ps1 -Rift
#   (-Monado, default native\atrium_monado\build\win). It must have the Rift drivers (ceres.dll):
#   Mix & match and TouchLink need them. Its Visual C++ runtime DLLs ship next to it.
#
# Layout (sys::native_tool looks next to atrium.exe):
#   atrium.exe  atrium_xr.dll  atrium_openxr.json  atrium-dash.exe  ovrprobe.exe  sensordriver.exe
#   SteamFrameSim.exe  atrium-client.apk  ovr\LibOVRRT64_1.dll  monado\  touchlink\
# Nothing of Meta's or Valve's is shipped. OpenComposite (GPL, unfinished) is left out.
param(
    [string]$Monado = (Join-Path $PSScriptRoot 'native\atrium_monado\build\win'),
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$Root = $PSScriptRoot
$Native = Join-Path $Root 'native\out'

$Version = (Select-String -Path (Join-Path $Root 'Cargo.toml') -Pattern '^version\s*=\s*"([^"]+)"' | Select-Object -First 1).Matches[0].Groups[1].Value
$Name = "Atrium-$Version-windows-x64"
$Dist = Join-Path $Root 'dist'
$Stage = Join-Path $Dist $Name
$TargetDir = Join-Path $Root 'target\dist'

function Check($what) { if ($LASTEXITCODE -ne 0) { throw "$what failed (exit $LASTEXITCODE)" } }

$Svc = Join-Path $Monado 'src\xrt\targets\service'
$Xr = Join-Path $Monado 'src\xrt\targets\openxr'
if (-not (Test-Path (Join-Path $Svc 'monado-service.exe'))) { throw "No Monado build in $Monado (native\atrium_monado\build.ps1 -Rift)" }
if (-not (Test-Path (Join-Path $Svc 'ceres.dll'))) { throw "$Monado has no Rift drivers: build it with native\atrium_monado\build.ps1 -Rift" }

if (-not $SkipBuild) {
    Write-Host '== atrium.exe'
    $env:RUSTFLAGS = '-C target-feature=+crt-static'
    cargo build --release --target-dir $TargetDir; Check 'cargo build'
    Remove-Item Env:RUSTFLAGS
    Write-Host '== native parts'
    cmd /c "`"$(Join-Path $Root 'native\build.bat')`""; Check 'native\build.bat'
}
$Exe = Join-Path $TargetDir 'release\atrium.exe'
if (-not (Test-Path $Exe)) { throw "No $Exe (run without -SkipBuild)" }

Write-Host "== staging $Stage"
if (Test-Path $Stage) { Remove-Item $Stage -Recurse -Force }
New-Item -ItemType Directory -Force $Stage, "$Stage\ovr", "$Stage\monado" | Out-Null

Copy-Item $Exe $Stage
foreach ($f in 'atrium_xr.dll', 'atrium_openxr.json', 'atrium-dash.exe', 'ovrprobe.exe', 'sensordriver.exe', 'SteamFrameSim.exe', 'atrium-client.apk') {
    $p = Join-Path $Native $f
    if (-not (Test-Path $p)) { throw "Missing native\out\$f" }
    Copy-Item $p $Stage
}
Copy-Item (Join-Path $Native 'ovr\LibOVRRT64_1.dll') "$Stage\ovr"
Copy-Item (Join-Path $Native 'touchlink') $Stage -Recurse

# The tracking service under Atrium's names (as build.ps1 stages it), with its libraries.
Copy-Item (Join-Path $Svc 'monado-service.exe') "$Stage\monado\atrium-service.exe"
Get-ChildItem "$Svc\*.dll" | Where-Object Name -ne 'vulkan-1.dll' | Copy-Item -Destination "$Stage\monado"
Copy-Item (Join-Path $Xr 'openxr_atrium.dll') "$Stage\monado"
Set-Content "$Stage\monado\atrium_openxr.json" -Encoding ascii -Value @'
{
    "file_format_version": "1.0.0",
    "runtime": {
        "name": "Atrium",
        "library_path": "./openxr_atrium.dll"
    }
}
'@
# Monado is built against the DLL C runtime: ship Microsoft's redistributable DLLs next to it.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = if (Test-Path $vswhere) { & $vswhere -latest -products * -property installationPath } else { 'J:\vs2026' }
$crt = Get-ChildItem (Join-Path $vs 'VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT') -Directory | Sort-Object FullName | Select-Object -Last 1
if (-not $crt) { throw "No Visual C++ redistributable folder under $vs" }
Copy-Item "$($crt.FullName)\*.dll" "$Stage\monado"

Copy-Item (Join-Path $Root 'README.md') $Stage
Set-Content "$Stage\LICENSE.txt" -Encoding utf8 -Value @'
MIT License

Copyright (c) 2026 Atrium contributors

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
associated documentation files (the "Software"), to deal in the Software without restriction,
including without limitation the rights to use, copy, modify, merge, publish, distribute,
sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or
substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES
OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
'@
Set-Content "$Stage\THIRD-PARTY.txt" -Encoding utf8 -Value @'
Atrium includes builds of these projects (each under its own licence):

- Monado (monado\: the tracking service and OpenXR runtime), Boost Software License 1.0, with
  Atrium's patches. Its libraries: Ceres Solver (BSD-3), glog (BSD-3), gflags (BSD-3), METIS
  (Apache-2.0), libusb (LGPL-2.1, as a separate DLL), hidapi (BSD-3), cJSON (MIT), pthreads4w
  (Apache-2.0).
- ALVR server core and client (monado\alvr_server_core.dll, atrium-client.apk), MIT.
- Revive (ovr\LibOVRRT64_1.dll, Atrium OVR), MIT. Microsoft Detours, MIT.
- Khronos OpenXR SDK loader and headers, Apache-2.0.
- OpenVR SDK (TouchLink): the driver header, and openvr_api.dll for touchlink camera, BSD-3-Clause.
- Oculus PC SDK headers and loader shim (TouchLink's optional Meta source, built in; it loads Meta's installed runtime), Oculus SDK License.
- Microsoft Visual C++ runtime (monado\vcruntime*.dll, msvcp*.dll, concrt*.dll), redistributed
  under the Visual Studio licence terms.
- Rust crates used by atrium.exe (wry, tao, serde, ureq, image, ...), MIT and/or Apache-2.0.

No Meta or Valve binaries, artwork or game files are included. TouchLink refers to SteamVR's own
Touch profile and render models by path.
'@

$Zip = Join-Path $Dist "$Name.zip"
if (Test-Path $Zip) { Remove-Item $Zip -Force }
Write-Host "== $Zip"
# Windows' own bsdtar: a standard zip. (PowerShell 5.1's Compress-Archive writes backslash paths
# that other unzippers turn into files, not folders.)
& "$env:SystemRoot\System32\tar.exe" -a -c -f $Zip -C $Dist $Name; Check 'tar'
$mb = [math]::Round((Get-Item $Zip).Length / 1MB, 1)
Write-Host "Done: $Zip ($mb MB)"
