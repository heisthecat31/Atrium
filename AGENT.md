# AGENT.md — Atrium

Notes for any agent (or person) picking up this project. Read this first.

## The goal

**Atrium** is one open-source home for every VR headset and every VR game:

- **One native app** for Windows and Linux: Rust, with the UI in HTML/CSS shown by the system
  webview (wry: WebView2 / WebKitGTK). Looks like the Electron prototype, ships no browser engine.
  Not Electron, not egui (an egui version was tried 2026-10-20; the user preferred the web look).
- **One runtime: the Atrium Runtime.** It's an OpenXR runtime (`atrium_openxr.json` +
  `atrium_xr.dll` / `libatrium_xr.so`) and the *only* runtime Atrium exposes. The Atrium app
  registers it as the system OpenXR runtime. Users never pick SteamVR / Meta / Virtual Desktop.
  - Device backends live inside it. v1 forwards each OpenXR call to the vendor stack that
    drives the connected hardware (Meta's runtime for Rift/Quest Link, SteamVR for Lighthouse,
    Monado/WiVRn on Linux), while the game only ever sees "Atrium". Later versions replace
    vendor stacks with Atrium's own drivers, headset by headset (Rift CV1 first: test hardware).
  - Games that aren't OpenXR are translated into it. Oculus SDK games: **Atrium OVR**
    (`native/atrium_ovr`), Atrium's own layer: ReviveXR (Revive, MIT, pinned upstream ab73167)
    + Atrium's entry point. Not RiftLift (GPL, and the user wants our own). OpenVR games: TBD
    (xrizer / OpenComposite are the references).
- **One library**: Steam, Meta and local games together. The user still signs in to Steam and
  Meta to see what they own; installed games show without signing in.
- Licence: MIT. Ship **no** Meta or Valve binaries, artwork or game files.

## Layout

| Path | What |
| --- | --- |
| `Cargo.toml`, `src/` | the Atrium app (Rust, egui/eframe) |
| `src/sys/` | platform access: registry, processes, USB, VDF, settings, secrets (DPAPI) |
| `src/services/` | headsets, runtime, steam, meta, library, launcher |
| `src/app.rs` | app state + commands from the page; pushes a full JSON snapshot (`atrium.update`) after every change |
| `src/ui.rs` | tao window + wry webview; serves `web/` (compiled in) on the `atrium` protocol (`http://atrium.localhost/` on Windows), Meta store art at `/art?p=` (StoreAssets only) |
| `web/` | the page: `index.html`, `app.css` (palette in `:root`), `app.js` (renders from the snapshot, `ipc.postMessage` for actions), `icons.js` (our line art) |
| `src/login.rs` | sign-in: Steam via system browser + localhost OpenID return; Meta in a native webview (wry) subprocess (`atrium --login-meta`) that catches the `oculus://` callback |
| `runtime/` (top level, gitignored) | Meta DLLs copied earlier for research. Locked by OVRServer; never commit |
| `native/atrium_xr/` | the Atrium Runtime (C++, OpenXR runtime ABI): forwards to the backend in `runtime.json`; trace with `ATRIUM_XR_TRACE=1` -> `logs\xr.log` |
| `native/atrium_client/` | Atrium's headset app (Quest/Pico): Atrium's build of ALVR's client, `build.ps1` -> `native\out\atrium-client.apk`. App side: `src/services/standalone.rs` (adb) |
| `native/atrium_monado/` | the Atrium Runtime on Linux: Atrium's build of Monado (pinned + `patches/`), `build.sh` -> `native/out/linux/` (`libopenxr_atrium.so`, `atrium-service`, `atrium_openxr.json`). App side: `src/services/atrium_service.rs` |
| `native/src/` | `ovrprobe` (Meta runtime probe, `ovrprobe json`), `SteamFrameSim` (simulated Steam Frame) |
| `Dashboard/` | in-VR dashboard, **Unity** (OpenXR plugin only), owned by a second dev. See `Dashboard/README.md`; app↔dashboard protocol not defined yet |
| `native/atrium_ovr/` | Atrium OVR: CMake fetches pinned Revive/OpenXR SDK/Oculus SDK/Detours, builds `LibOVRRT64_1.dll` (static CRT + static OpenXR loader, one DLL). Log: `%APPDATA%\Atrium\logs\ovr.log` |
| `src/sys/inject.rs` | starts a game suspended with Atrium OVR loaded (remote LoadLibraryW), sets `LIBOVR_DLL_DIR` + `XR_RUNTIME_JSON` |
| `native/third_party/LibOVR` | optional local Oculus SDK copy, gitignored; build.bat falls back to the fetched one |
| `assets/` | Atrium icon |

Build: `cargo build --release` (app), `native\build.bat` (MSVC: ovrprobe, SteamFrameSim, runtime).

## History

- 2026-10-20 (17:50): **First Quest 3 connection to Atrium.** Atrium headset app installed on the
  user's Quest 3 (serial 2G0YC5ZG9C01ZN, adb over USB, developer mode). The app starts its OpenXR
  session through the Khronos loader (broker lookup fails, falls back to
  /odm/etc/openxr/1/active_runtime.aarch64.json: fine) and waits in its lobby. atrium-service with
  ATRIUM_STREAM=1 discovers it as `4909.client` (untrusted); after setting `trusted: true` in
  `%APPDATA%\Atrium\stream\session.json` it connects ("Streaming", playspace 5.6 x 5.3 m) and an
  OpenXR test app on the Atrium Runtime gets live head poses (flags 0xf). Headset shows black:
  no encoder yet (expected).
  - Open: pairing/trust UI in the Atrium app (not hand-edited session.json); ALVR core tries to
    "restart SteamVR" after a settings change and the first handshake fails (patch it out);
    controller poses read 0 in a headless test (check once frames flow); ALVR's web API listens
    on 0.0.0.0:8082 (restrict); Windows firewall rule for atrium-service (UDP 9943/9944).
  - Next: the video path: a Monado compositor target that hardware-encodes each frame (Vulkan
    Video / NVENC) and calls alvr_set_video_config_nals + alvr_send_video_nal.
- 2026-10-20 (16:30): **Atrium Runtime for Windows** (streaming, step 1). `native/atrium_xr_linux`
  renamed `native/atrium_monado`: one pinned Monado + patches, `build.sh` (Linux) and `build.ps1`
  (Windows -> `native\out\monado\`: openxr_atrium.dll, atrium-service.exe, atrium_openxr.json,
  cjson.dll, pthreadVCE3.dll). Out-of-process service, socket `atrium_comp_ipc`, no vcpkg default
  features (no SDL GUI, no USB). Tested: service with the simulated HMD + a Khronos-loader app ->
  same results as Linux (runtime "Atrium", OVRPlugin quirk works). Runs on the system Vulkan
  loader (vcpkg's vulkan-1.dll isn't staged).
  - Traps: vcpkg shallow clone lacks Monado's `builtin-baseline` commit (4334d8b4) -> fetch it by
    SHA. VS 2026's bundled vcpkg has no ports catalogue: use a real vcpkg checkout (2026.07.29).
    MSVC needs `/Zc:__cplusplus` or Monado's u_file.cpp falls back to <experimental/filesystem>
    (removed). PowerShell/Python string literals with `\a`, `\b`... in Windows paths mangled two
    docs; use raw strings.
  - Next (step 2): Atrium streaming driver in Monado: a target builder + HMD/controller devices
    fed by ALVR server_core's C API, and a compositor target that hardware-encodes each frame and
    sends NALs. Then the app runs atrium-service on Windows for a connected Quest.
- 2026-10-20 (16:00): **Atrium streaming, step 1: the headset app** (`native/atrium_client`). User
  decisions: headset app = Atrium's build of ALVR's client (MIT; not from scratch, not WiVRn: GPL);
  PC side = the Monado-based Atrium Runtime on Windows too, with an Atrium streaming driver;
  test device = a Quest. Streamed headsets are the open door on Windows: no GPU direct mode, so
  no NVIDIA/Meta wall (wired PC headsets on Windows stay on Meta's OVRServer).
  - `build.ps1`: ALVR v20.14.1 (a9f6542) + patches (0001 Khronos loader only, 0002 name "Atrium",
    package `vr.atrium.client`, Atrium icon) -> `native\out\atrium-client.apk` (20 MB, arm64).
    Only the Khronos OpenXR loader is bundled: no Quest 1 / old Pico / YVR / Lynx vendor loaders.
  - Needs: rustup target aarch64-linux-android, cargo-apk (zarik5 fork, rev 0fd3126dad5a), Android
    platform 32 (installed into J:\AndroidSdk from dl.google.com, sha1 checked), NDK 30, JDK.
    ALVR's `openvr` submodule (BSD headers) is needed at build time only.
  - App side: `src/services/standalone.rs` finds Quest/Pico over adb (developer mode), shows the
    installed Atrium app version on the Devices page, installs/updates (`adb install -r -g`) and
    opens it. adb from next to atrium, PATH, or an Android SDK (bundling: later).
  - Plan for the PC side: ALVR's `server_core` (MIT, Rust) has a C API (alvr_initialize,
    alvr_poll_event, alvr_get_device_motion, alvr_set_video_config_nals, alvr_send_video_nal,
    alvr_report_present...). An Atrium Monado driver links it, feeds tracking into Monado and sends
    the compositor's frames, encoded by a hardware encoder (Vulkan Video / NVENC / AMF / Media
    Foundation), as NALs. Needs the Atrium Runtime (Monado) built for Windows first. Not tested on a
    Quest yet. For a protocol check before that, ALVR's own PC streamer speaks the same protocol.
  - Traps: Windows PowerShell 5.1 + `$ErrorActionPreference='Stop'` treats native stderr as an
    error when redirected; check `$LASTEXITCODE` instead.
- 2026-10-20 (15:30-16:00): **OVRPlugin games** (Unity/Unreal with Meta's OVRPlugin, instance
  name "Oculus VR Plugin (...)"). They need the runtime name "Oculus" and locate poses at XrTime 0
  (only Meta's runtime takes that as "now"). Approach from RiftLift's OpenXR layer (MIT), Atrium's
  own code, built into the Atrium Runtime instead of a separate layer: Windows `runtime.cpp`
  ("OVRPlugin games" section; also enables XR_KHR_win32_convert_performance_counter_time for them;
  locate wrappers only handed to OVRPlugin instances) and Linux patch 0003 (Monado quirk).
  Tested: Windows on Meta's backend (name "Oculus", time conversion available; ordinary apps keep
  "Atrium (Oculus OpenXR)"); Linux headless session (time 0 ok for OVRPlugin, TIME_INVALID for
  others). Not yet tested with a real OVRPlugin game.
- 2026-10-20 (15:00-15:30): **Atrium Runtime for Linux** (`native/atrium_monado`). User chose
  "built on Monado" (not from scratch, not a user-installed Monado) and build-only for now (no
  Linux boot on the test PC; WSL2 can't drive the headset display).
  - Monado main (pinned 5bca1effb, 2026-10-20) already has the CV1 end to end: `rift` (HMD,
    Touch over the radio, Touch calibration), `rift_sensor` (sensor cameras over UVC/libusb) and
    `tracking/constellation` (LED blob tracking, correspondence search, lambdatwist P3P, Ceres
    pose refinement: Jan Schmidt's OpenHMD rift-kalman-filter work, ported). Needs Ceres >= 2.1.
  - Options: runtime suffix `atrium`, socket `atrium_comp_ipc`, service ON (systemd off), no
    SteamVR plugin/lighthouse, no OpenVR target. Patches: 0001 runtimeName "Atrium", 0002 system
    name = headset's own. Release + strip: service 2.3 MB, runtime 1.7 MB (links system Ceres,
    OpenCV, libusb, hidapi: packaging must bring them).
  - Smoke test in WSL (Ubuntu 26.04): service up, a Khronos-loader app gets "Atrium
    'v25.1.0-939-g5bca1effb'" and a system (simulated HMD via `SIMULATED_ENABLE=1`,
    `XRT_COMPOSITOR_NULL=1`). The app's Linux code passes `cargo check` in WSL.
  - Traps: (1) WSL PATH has /mnt/c folders; CMake's package search crawls them for minutes per
    lookup (build.sh strips them). (2) The service won't start with stdin on /dev/null (epoll on
    stdin); the app gives it a pipe. (3) WSL stops its VM when no wsl.exe is attached: run long
    builds from a detached Windows `wsl.exe`, not `setsid` inside WSL. (4) Don't edit build.sh
    while it runs (bash reads scripts as it goes).
  - RiftLift (MIT) on Linux is game-side only (Revive-derived bridges under GE-Proton crossing to
    the host runtime via Wine unixlib, a Platform SDK shim, xrizer); it "doesn't install headset
    drivers or VR runtimes". Atrium copies that approach in its own code for the game side.
  - Next: Linux game side (Atrium OVR under Proton reaches the host Atrium Runtime through GE-Proton's
    wineopenxr, as RiftLift does; signed-shim games have no Meta DLL to load on Linux: open, and
    not via a faked signature), udev rules for the Rift, packaging.
- 2026-10-20 (later): `src/login.rs` written. Meta: parent runs `begin_login`, spawns
  `atrium --login-meta`, sends the auth URL on **stdin**; child (tao + wry, incognito, WebView2
  data in `%APPDATA%\Atrium\webview`) blocks the `oculus://`/`oculus-client://` navigation and
  prints `callback <url>` on stdout; parent finishes `complete_login`, seals the token. Steam:
  OpenID in system browser → `127.0.0.1:<random>/steam?state=<nonce>`, verified with
  `check_authentication`. `services/library.rs` holds only the `Game`/`Art`/`Source` types so far.
  Until the UI exists, `atrium --sign-in steam|meta` runs a sign-in from a terminal and saves it.
  Then: `services/library.rs` (merge installed + owned, caches), `services/launcher.rs` (Steam via
  `steam://`, Meta/local direct or through RiftLiftLauncher), and the webview UI.
  Open: art for owned-but-not-installed Meta games (graph.oculus.com has `cover_square_image`
  fields but they came back empty; store pages render images client-side). The Steam sign-in
  printed success but its save didn't land once; `Settings::save` now logs failures.
- 2026-10-20 (later): Atrium OVR. Key finding: the Oculus SDK 1.x shim in newer games only
  loads a **Meta-signed** LibOVRRT64_1.dll (WinVerifyTrust + cert names), so LIBOVR_DLL_DIR
  alone only works for old games (Echo). Atrium injects Atrium OVR first; the shim then
  sign-checks Meta's real DLL (found via PATH: Meta's oculus-runtime folder) and LoadLibraryW's
  it, which Atrium OVR's hook answers with itself. So for now **Meta's runtime must be
  installed** for signed-shim games; never put Atrium OVR's folder first on PATH (the shim would
  find the unsigned copy and stop). Verified with `atrium --translate native\out\ovrprobe.exe
  json`: Revive's HMD desc + the emulated HMD's 2880x1600 over Meta's OpenXR runtime.
  `XR_RUNTIME_JSON` = Atrium Runtime manifest when built, else the backend Atrium chose.
  Open: no-Meta-installed case. **Not** by faking the signature check (making WinVerifyTrust /
  cert names report Atrium OVR as signed by Oculus VR, LLC): that forges a code-signing identity;
  declined 2026-10-20. Older games without the check load Atrium OVR via LIBOVR_DLL_DIR;
  port EchoXR's generic Revive fixes; LibOVRPlatform stand-in for games that need entitlement.
- 2026-10-20 (11:30-11:53): first real game. Echo VR runs through Atrium OVR (injection works on a
  signed-shim game). Revive changes live as patches in `native/atrium_ovr/patches/` (applied by
  CMake with `git --git-dir=.nogit --work-tree=. apply`, since a plain `git apply` inside Atrium's
  repo skips every path): 0001 trace to ovr.log (failed XR calls, head pose + layers once/s),
  0002 EchoXR's generic session/frame fixes (no frame begun in BeginSession, recenter after the
  first xrWaitFrame, wait for READY in the FOV probe on every runtime, opaque eye layers,
  request OpenXR 1.0), 0003 depth trace + `ATRIUM_OVR_DEPTH=0`.
  The "picture moves with the head" bug on Meta's OpenXR runtime: see the next entry.
- 2026-10-20 (12:00-12:24): **Atrium Runtime v1** (`native/atrium_xr`) and the Meta bug fixed.
  - Runtime: `atrium_xr.dll` + `atrium_openxr.json`, exports `xrNegotiateLoaderRuntimeInterface`,
    loads the backend named in `%APPDATA%\Atrium\runtime.json` (`ATRIUM_XR_BACKEND` overrides),
    negotiates with it and hands out its functions directly; wraps `xrGetInstanceProperties`
    (name "Atrium (<backend>)") and, with `ATRIUM_XR_TRACE=1`, traces sessions, spaces,
    swapchains, image acquire/release counts, xrWaitFrame and xrEndFrame to `logs\xr.log`.
    Verified: Echo VR -> Atrium OVR -> Atrium Runtime -> Meta, and -> SteamVR.
  - Root cause of the Meta bug: the user's **Echo FOV mod** renders a wider FOV (tan L1.35 R1.00
    U1.42 D1.78) than Meta's OpenXR runtime offers for the CV1 (L0.97 R0.72 U0.89 D1.11). Meta's
    native LibOVR path displays such frames fine (and uses the extra as a reprojection margin);
    **Meta's OpenXR runtime shows projection views wider than its max FOV at the wrong scale**
    (world swims with the head, looks magnified); SteamVR's handles them. D3D11 trailer was fine
    only because it renders the default FOV. Fix = Revive patch 0005: crop each eye's rect to
    the runtime's max FOV and report the cropped FOV (exact; `ATRIUM_OVR_CROP_FOV=0` off).
    Ruled out on the way: formats (Meta offers 29/45; Meta's images are TYPELESS 27 with
    SIMULTANEOUS_ACCESS), depth, the VD implicit layer, swapchain copy (tried, reverted: cost GPU).
  - Lobby at 30 fps was **SteamVR left running** from a test (its compositor shared the GPU);
    with it closed Echo holds ~85-90. Killing SteamVR mid-session left the Rift black until
    Echo restarted.
  - Launcher: no SteamVR preference any more. Games Atrium starts use the Atrium Runtime
    (`XR_RUNTIME_JSON`, per launch, no system registration); Auto sends Oculus SDK games through
    Atrium OVR whenever it's built; `runtime.json` is rewritten before every launch.
  - Revive patches now: 0001 trace, 0002 EchoXR session/frame fixes, 0003 depth trace/switch,
    0004 D3D12 negotiated format, 0005 FOV crop, 0006 timing trace, 0007 pixelsPerDisplayPixel.
  - Open: games Steam starts can only reach the Atrium Runtime if it's registered system-wide
    (Runtime page button, UAC); Atrium Runtime is Windows-only so far; no-Meta-installed case for
    signed-shim games; LibOVRPlatform stand-in.
- 2026-10-20 (12:30-12:50): **Atrium's own Rift CV1 access** (`native/src/riftprobe.cpp`,
  packet formats after OpenHMD, BSL-1.0). Goal is no Meta, no SteamVR (user, repeatedly).
  - Read-only (Meta running): config reports (2160x1200 panel, ±4 g / ±2000 °/s), 1 kHz IMU,
    gyro+accel orientation; `touch`: both Touch controllers via the radio (MI_01): buttons in
    the low 4 bits, trigger/grip/stick raw (need per-controller calibration over the radio).
  - Driving it (OVRService stopped, with the user's OK): enable display/audio/LEDs (0x1d),
    keep-alive (0x08, 10 s), sensor config packet interval 1 (headset default 19 = 100 Hz)
    -> 998 samples/s with no Meta software at all.
  - **Display blocker:** on NVIDIA (RTX 5060, driver 610.88) the Rift panel never reaches
    Windows (no OVR monitor device ever; WinRT DisplayManager sees only the 2 desktop monitors)
    and Vulkan has no VK_KHR_display / VK_NV_acquire_winrt_display. NVIDIA keeps known HMDs for
    its VRWorks Direct Mode API, which isn't in the public NVAPI (github NVIDIA/nvapi). Meta's
    log shows it uses that path (NVDirectDisplay). `native/src/riftdisplay.cpp` lists WinRT
    display targets.
  - `native/src/nvdisplays.cpp` (public NVAPI, github NVIDIA/nvapi; headers/lib not committed):
    the Rift IS physically connected: display id 0x80061080, DisplayPort (via the user's
    DP->HDMI adapter), EDID maker OVR product 0x0004, but filtered out of
    GetConnectedDisplayIds. Public "dedicated display" API (R495+) exists
    (NvAPI_DISP_AcquireDedicatedDisplay -> Windows DisplaySource, then
    Windows.Devices.Display.Core), but Acquire on the Rift returns NVAPI_UNREGISTERED_RESOURCE
    and making a display dedicated needs NVIDIA's configureDdisplay, which only supports
    RTX Pro/Quadro on Windows 11 Pro/Enterprise. Not GeForce + Win10 Home (this PC).
    `nvdisplays setedid <id>` (public NvAPI_GPU_SetEDID, maker OVR -> ATR, elevated) returns
    NVAPI_NOT_SUPPORTED on GeForce, as documented (tried 2026-10-20). The Rift's EDID only reads
    while the panel is on (`riftprobe drive`, 15 s). No NVIDIA/Windows registry switch for HMD
    hiding found, and none among the driver's 129 DRS settings (public NvAPI_DRS_*, only old
    3D Vision stereo + VRR). With the panel on, WinRT DisplayManager still lists only the 2
    desktop monitors; Vulkan exposes no VK_KHR_display / VK_EXT_direct_mode_display /
    VK_NV_acquire_winrt_display. That exhausts the public Windows software routes on this PC.
  - **Decision (user):** no hardware adapters (not every user can buy one). For Rift on
    Windows + NVIDIA, Atrium runs Meta's runtime **barebones** as its display backend:
    `src/services/meta_runtime.rs`. Only OVRServer_x64 is kept: it runs as the user without Meta's
    service (OVR Service Launcher + OVR Redir, both SYSTEM), so Atrium starts it directly: as its own child, no console window (it's a console app; run by hand it shows as a separate Task Manager app), in a kill-on-close job so it ends with Atrium (`meta_runtime::start()`, from the hardware loop whenever the Meta backend is chosen). Dash off via
    Meta's own `HKLM\SOFTWARE\Wow6432Node\Oculus VR, LLC\Oculus\Config\PreventDashLaunch=1`
    (one UAC step, once per PC, in `prepare()` on the first Rift-game launch). Client.exe (Meta
    Horizon Link app, relaunched by OVRServer whenever a game connects) and
    oculus-platform-runtime are closed by a 200 ms process-snapshot watcher (`watch()`); the runtime keeps working without them (ovrprobe sees
    CV1 + both Touch). RemoteDesktopCompanion is left alone: OVRServer restarts it within ~1 s
    of every exit (`onCoreTrackerExit::launchRemoteDesktop`). Open: games that check Meta
    entitlements need the platform runtime until Atrium's LibOVRPlatform stand-in exists.
    - Started by Atrium (not the service), OVRServer doesn't launch Client.exe at all; Echo VR
      runs fine with Client + platform runtime closed.
    - Only a **wired Rift** (CV1 / Rift S on USB, `meta_runtime::needed`) starts OVRServer.
      Quest isn't covered (user: only wired headsets get OVRServer).
    - A launch waits until ovrprobe reports `hmdPresent`, then re-detects and picks the backend
      again. **Trap:** OVRServer powers the Rift up and it drops off USB for a moment; a hardware
      pass in that gap picked Virtual Desktop (its service always runs, so `streaming` is true)
      and Echo failed with "Failed to initialize Oculus VR session". The hardware loop also keeps
      Meta chosen for 30 s after the Rift was last seen. One launch at a time; a running game's
      exe isn't started twice (repeat clicks while waiting had started Echo 9 times).
    - Settings page: "Start Atrium when I sign in" (HKCU Run value `Atrium`; XDG autostart on
      Linux), `src/sys/autostart.rs`.
    - Declined: renaming/re-iconing OVRServer as "Atrium-OVR_Server". That means editing Meta's
      binary and presenting it as Atrium's (licence, breaks its signature, updates undo it).
    - OVRServer itself: one 9 MB exe, but it needs most of `Support\oculus-runtime` (386 MB:
      OculusAppFramework.dll, d3dcompiler_47, server-plugins\Rift.dll / RiftS.dll, the OpenXR
      runtime LibOVRRTImpl64_1.dll + oculus_openxr_64.json) and Meta's drivers (Oculus VR
      Headset USB, Rift Sensor, Oculus Virtual Audio). Atrium can't ship or fetch these (no Meta
      binaries); users without Meta installed need Meta's installer until Atrium's own driver.
    Remaining routes: hardware EDID adapter, Linux (VK_KHR_display lists hidden HMDs),
    NVIDIA VRWorks licence. Don't use private/undocumented NVAPI entry points.
- 2026-10-20: started as SteamFrameSim (simulated Steam Frame on Meta's emulated headset).
  Repurposed into Atrium. An Electron prototype was built and verified (detection, library,
  Meta names), then replaced by the native Rust app on the user's request. The detection logic
  was ported 1:1.

## Facts found (2026-10-20)

**Meta Horizon Link app** (`C:\Program Files\Oculus\Support\oculus-client\resources\app.asar`,
Electron): Devices page names the headset in `deviceFamilyNameFromHeadset` by `subtype`
(`cv1`, `km02`, `quest`, `quest2`, `seacliff`, `eureka`, `panther`); anything else is
"Other Devices". The emulated Steam Frame (Emulation.dll mode 3, ovrHmdType 18) shows that way.

**Test PC**
- Real **Rift CV1**: VID 2833 PID 0031 (HMD), 0211 (sensor ×2), 0330 (Rift Audio).
- `HKCU\Environment\OVREmulationPlugin=3` → OVRServer reports "HMD X (Emulation)" instead of the
  CV1. Delete it and restart OVRService to get the CV1 back. Ask the user before changing it.
  (Removed 2026-10-20 11:32 at the user's request; CV1 confirmed working after.)
- **Trap:** `OVRServerPluginDirectory` (HKCU `SOFTWARE\Oculus VR, LLC\Oculus` and
  `SOFTWARE\Oculus`) overrides where OVRServer loads its plugins (`Rift.dll` = CV1 driver,
  `Emulation.dll`, ...). The SteamFrameSim research pointed it at a copy with `Rift.dll` moved to
  `disabled\`; after the folder rename it pointed nowhere, so OVRServer loaded no plugins and the
  CV1 vanished ("No HMD attached", no `USBHID:Detector` lines in `%LOCALAPPDATA%\Oculus\Service_*.txt`).
  Both values deleted 2026-10-20 11:34; Meta's default `oculus-runtime\server-plugins` is intact.
  `runtime/` here still holds that research copy.
- OpenXR runtimes registered: Meta, SteamVR (`H:\Program Files (x86)\Steam\steamapps\common\
  SteamVR\steamxr_win64.json`), Virtual Desktop (currently active).
- Steam at `h:/program files (x86)/steam`; only SteamVR + redist installed.
- Meta libraries: `HKCU\Software\Oculus VR, LLC\Oculus\Libraries\{guid}` → `OriginalPath`
  (prefer) / `Path` (volume GUID). Apps: `<lib>\Manifests\<canonicalName>.json`, `launchFile`
  relative to `<lib>\Software\<canonicalName>\`. Art: `C:\Program Files\Oculus\CoreData\Software\
  StoreAssets\<canonicalName>_assets\cover_square_image.jpg` etc.
- Real app names without sign-in: `https://www.meta.com/experiences/<appId>/` `<title>` =
  "Echo VR on Oculus Rift | ...". The Meta app's own DB (`%APPDATA%\Oculus\Sessions\_oaf\
  data.sqlite`) is locked while it runs.
- Steam store VR categories: 31, 53, 54 (VR only), 401, 402.

**Related projects (reuse, don't rewrite)**
- RiftLift (github.com/Villagers654/RiftLift, MIT): Meta sign-in, owned Rift apps, downloads,
  Revive-derived Oculus→OpenXR/OpenVR runtime (`RiftLiftLauncher.exe /openxr /app <key> /cwd
  <dir> <exe> args`, `RiftLiftOpenXR64.dll`), Linux via GE-Proton + xrizer.
  - Meta sign-in: POST multipart `https://meta.graph.meta.com/webview_tokens_query` (FRL client
    token) → `https://auth.meta.com/native_sso/confirm?...` → `oculus://...?blob=&token=`
    (token = sha256(request_token)[:16]) → `/webview_blobs_decrypt` → GraphQL doc
    `24112177345042346` → Oculus profile token. Owned: `graph.oculus.com/graphql` doc
    `4850747515044496`, platform `PC`. **Never register Atrium as the `oculus://` handler**.
- `J:\EchoVR-Tools-Launcher\EchoVR-Hand-Tracking` (EchoXR): ReviveXR as `LibOVRRT64_1.dll` +
  `LIBOVR_DLL_DIR`, Meta-free `LibOVRPlatform64_1.dll`, Linux script, OpenXR SDK checkout at
  `xr/OpenXR-SDK` (headers for the Atrium Runtime).
- `J:\EchoVR-Tools-Launcher\EchoQuestXR` (EchoFrame): Quest APK VrApi→OpenXR, Steam Frame
  (Lepton) packaging, `cv1bridge`.

## Rules

- Don't change system-wide settings (OpenXR registration, services, env vars, protocol
  handlers) without the user's say-so. Registering the Atrium Runtime is an explicit button.
- Don't commit Meta/Valve binaries or artwork. Headset icons are our own line art.
- Tokens: Windows DPAPI (CurrentUser); Linux: settings dir with 0600 permissions.
- Edit files with the Edit/Write tools or Python with explicit UTF-8. PowerShell
  `Get-Content`/`Set-Content` round-trips mangled UTF-8 once already.
