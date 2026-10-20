# Atrium headset app (Quest, Pico)

The app that runs on standalone headsets: it receives the picture Atrium streams from the PC and
sends back head and controller poses. Atrium's build of [ALVR](https://github.com/alvr-org/ALVR)'s
OpenXR client (MIT) at a pinned release plus the patches in `patches/`, the same pattern as
Atrium OVR on Revive. The PC side is Atrium's own streaming server (in progress), built on ALVR's
`server_core`, so no SteamVR is involved.

Supported: Quest 2, 3, 3S, Pro and current Pico firmware (all load their runtime through the
Khronos OpenXR loader). Quest 1 and old Pico firmware need vendor loader binaries, which Atrium
doesn't ship.

## Build

```powershell
native\atrium_client\build.ps1
```

Output: `native\out\atrium-client.apk` (package `vr.atrium.client`, installs next to ALVR).

Needs Rust with the Android target, the Android SDK (platform 32) + NDK, a JDK, and ALVR's
`cargo-apk` fork:

```powershell
rustup target add aarch64-linux-android
cargo install --locked --git https://github.com/zarik5/cargo-apk --rev 0fd3126dad5a cargo-apk
```

Install on a headset in developer mode: `adb install -r native\out\atrium-client.apk`.

## Patches

Edit ALVR in `build\alvr` (a checkout of the pinned tag), then regenerate with `git diff`.

- `0001`: load only the Khronos OpenXR loader (no vendor binaries).
- `0002`: name "Atrium", package `vr.atrium.client`, Atrium's icon.
