# Atrium's headset app (Quest, Pico): Atrium's build of ALVR's OpenXR client (MIT), pinned
# upstream plus the patches in patches/, the same pattern as Atrium OVR on Revive.
#
# The app receives the picture Atrium streams from the PC and sends back head/controller poses.
# Only the Khronos OpenXR loader is bundled (Apache-2.0); no vendor binaries.
#
# Needs: Rust (rustup) with aarch64-linux-android, the Android SDK + NDK, a JDK (keytool), and
# ALVR's cargo-apk fork:
#   rustup target add aarch64-linux-android
#   cargo install --locked --git https://github.com/zarik5/cargo-apk --rev 0fd3126dad5a cargo-apk
# ANDROID_HOME / ANDROID_NDK_ROOT default to J:\AndroidSdk and its newest NDK when unset.
#
# Output: native\out\atrium-client.apk
param([string]$Sdk = $(if ($env:ANDROID_HOME) { $env:ANDROID_HOME } else { 'J:\AndroidSdk' }))
# Native tools (git, cargo) print progress on stderr; with 'Stop' Windows PowerShell 5.1 would
# treat that as a failure. Exit codes are checked instead.
$ErrorActionPreference = 'Continue'
function Check([string]$what) { if ($LASTEXITCODE) { throw "$what failed ($LASTEXITCODE)" } }

$AlvrTag = 'v20.14.1'
$AlvrCommit = 'a9f6542fa507a841f40ab4f3fcb531427cd02550'
$LoaderUrl = 'https://github.com/KhronosGroup/OpenXR-SDK-Source/releases/download/release-1.0.34/openxr_loader_for_android-1.0.34.aar'
$LoaderSha256 = 'c8b50603ad3b81756c6bd203ab9c9d19341326e14f6cbae87ee894137ee5e256'

$Here = $PSScriptRoot
$Src = Join-Path $Here 'build\alvr'
$Out = Join-Path $Here '..\out'

# Pinned source, patched. Reset to a clean tree each run so edits to patches\ take effect.
if (-not (Test-Path (Join-Path $Src '.git'))) {
    git clone --quiet --depth 1 --branch $AlvrTag https://github.com/alvr-org/ALVR.git $Src; Check 'git clone'
}
git -C $Src checkout --quiet --force $AlvrCommit; Check 'git checkout'
git -C $Src clean --quiet -fdx -e deps -e target -e build; Check 'git clean'
# Valve's OpenVR headers (BSD-3): ALVR's settings crate reads property names from them at build
# time only; nothing of them ships.
git -C $Src submodule update --quiet --init --depth 1; Check 'git submodule'
foreach ($p in Get-ChildItem (Join-Path $Here 'patches\*.patch') | Sort-Object Name) {
    git -C $Src apply --whitespace=nowarn $p.FullName; Check "patch $($p.Name)"
}

# The Khronos OpenXR loader for Android (checked against its known hash).
$loaderDir = Join-Path $Src 'deps\android_openxr\arm64-v8a'
$loader = Join-Path $loaderDir 'libopenxr_loader.so'
if (-not (Test-Path $loader)) {
    New-Item -ItemType Directory -Force $loaderDir | Out-Null
    $aar = Join-Path $env:TEMP 'atrium_openxr_loader.aar'
    Invoke-WebRequest $LoaderUrl -OutFile $aar -ErrorAction Stop
    if ((Get-FileHash $aar -Algorithm SHA256).Hash -ne $LoaderSha256) { throw 'OpenXR loader download has the wrong hash' }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($aar)
    try {
        $entry = $zip.GetEntry('prefab/modules/openxr_loader/libs/android.arm64-v8a/libopenxr_loader.so')
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $loader, $true)
    } finally { $zip.Dispose() }
}

# Android toolchain.
$env:ANDROID_HOME = $Sdk
$env:ANDROID_SDK_ROOT = $Sdk
if (-not $env:ANDROID_NDK_ROOT) {
    $ndk = Get-ChildItem (Join-Path $Sdk 'ndk') -Directory | Sort-Object Name -Descending | Select-Object -First 1
    $env:ANDROID_NDK_ROOT = $ndk.FullName
}
$env:ANDROID_NDK_HOME = $env:ANDROID_NDK_ROOT

Push-Location $Src
try {
    cargo xtask build-client --release; Check 'APK build'
} finally { Pop-Location }

New-Item -ItemType Directory -Force $Out | Out-Null
Copy-Item (Join-Path $Src 'build\alvr_client_android\alvr_client_android.apk') (Join-Path $Out 'atrium-client.apk') -Force
Write-Host "Built: $(Resolve-Path (Join-Path $Out 'atrium-client.apk'))"
