# Runs the Atrium Runtime (Monado, built with build.ps1 -Rift, or a dev build dir) on the Rift CV1
# with constellation tracking and verbose logs, for testing Atrium's own Rift drivers.
#
#   native\atrium_monado\test_rift.ps1                # service only, logs to native\atrium_monado\build\rift_test\
#   native\atrium_monado\test_rift.ps1 -HelloXr       # plus Khronos hello_xr (cubes at the Touches)
#
# Meta's OVRServer must not be running (it holds the Rift). The sensors need UsbDk.
# The system OpenXR runtime isn't changed: only hello_xr is pointed at this build (XR_RUNTIME_JSON).
param(
    [string]$Bin = (Join-Path $PSScriptRoot 'build\win'),
    [string]$HelloXrExe = (Join-Path $PSScriptRoot 'build\helloxr\src\tests\hello_xr\hello_xr.exe'),
    [switch]$HelloXr
)
$ErrorActionPreference = 'Stop'

if (Get-Process OVRServer_x64 -ErrorAction SilentlyContinue) {
    throw "Meta's OVRServer is running and holds the Rift. Close it first (Atrium or the Oculus app started it)."
}

$Service = Join-Path $Bin 'src\xrt\targets\service\monado-service.exe'
$Manifest = Join-Path $Bin 'openxr_atrium-dev.json'
if (-not (Test-Path $Service)) { throw "No service at ${Service}: build first" }

# The service links the streaming driver, so ALVR's core DLL has to sit next to it.
$Alvr = Join-Path (Split-Path $Service) 'alvr_server_core.dll'
if (-not (Test-Path $Alvr)) {
    $AlvrSrc = Join-Path $PSScriptRoot 'build\alvr_core\alvr_server_core.dll'
    if (-not (Test-Path $AlvrSrc)) { throw "No ${AlvrSrc}: run build.ps1 (it builds ALVR's core) or copy the DLL beside the service" }
    Copy-Item $AlvrSrc $Alvr
}

$Logs = Join-Path $PSScriptRoot 'build\rift_test'
New-Item -ItemType Directory -Force $Logs | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'

$env:RIFT_PROBER_CONSTELLATION_TRACKING = '1'
$env:RIFT_LOG = 'debug'
$env:RIFT_SENSOR_LOG = 'debug'
$env:CONSTELLATION_TRACKER_LOG = 'info'
$env:XRT_LOG = 'info'

$svcOut = Join-Path $Logs "service-$stamp.log"
$svcErr = Join-Path $Logs "service-$stamp.err.log"
$svc = Start-Process $Service -PassThru -NoNewWindow -RedirectStandardOutput $svcOut -RedirectStandardError $svcErr
Start-Sleep -Seconds 2
if ($svc.HasExited) { throw "The service exited at once (code $($svc.ExitCode)), see $svcErr" }
Write-Host "Service running (pid $($svc.Id)). Logs: $svcOut"

try {
    if ($HelloXr) {
        Start-Sleep -Seconds 8   # Rift startup wait + Touch calibration
        $env:XR_RUNTIME_JSON = $Manifest
        $hxOut = Join-Path $Logs "hello_xr-$stamp.log"
        Push-Location (Split-Path $HelloXrExe)
        # Through cmd so hello_xr's stderr goes to the log instead of stopping the script.
        try { cmd /c "`"$HelloXrExe`" -g D3D11 -v > `"$hxOut`" 2>&1" } finally { Pop-Location }
        Write-Host "hello_xr exited. Log: $hxOut"
    } else {
        Write-Host 'Press Enter to stop the service.'
        [void][Console]::ReadLine()
    }
} finally {
    if (-not $svc.HasExited) { Stop-Process -Id $svc.Id -Force }
}
