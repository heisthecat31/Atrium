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
| `native/atrium_xr/` | the Atrium Runtime (C++, OpenXR runtime ABI) |
| `native/src/` | `ovrprobe` (Meta runtime probe, `ovrprobe json`), `SteamFrameSim` (simulated Steam Frame) |
| `Dashboard/` | in-VR dashboard, **Unity** (OpenXR plugin only), owned by a second dev. See `Dashboard/README.md`; app↔dashboard protocol not defined yet |
| `native/atrium_ovr/` | Atrium OVR: CMake fetches pinned Revive/OpenXR SDK/Oculus SDK/Detours, builds `LibOVRRT64_1.dll` (static CRT + static OpenXR loader, one DLL). Log: `%APPDATA%\Atrium\logs\ovr.log` |
| `src/sys/inject.rs` | starts a game suspended with Atrium OVR loaded (remote LoadLibraryW), sets `LIBOVR_DLL_DIR` + `XR_RUNTIME_JSON` |
| `native/third_party/LibOVR` | optional local Oculus SDK copy, gitignored; build.bat falls back to the fetched one |
| `assets/` | Atrium icon |

Build: `cargo build --release` (app), `native\build.bat` (MSVC: ovrprobe, SteamFrameSim, runtime).

## History

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
  Open: no-Meta-installed case (hook WinVerifyTrust + cert helpers, or ship a stand-in);
  port EchoXR's generic Revive fixes; LibOVRPlatform stand-in for games that need entitlement.
- 2026-10-20 (11:30-11:53): first real game. Echo VR runs through Atrium OVR (injection works on a
  signed-shim game). Revive changes live as patches in `native/atrium_ovr/patches/` (applied by
  CMake with `git --git-dir=.nogit --work-tree=. apply`, since a plain `git apply` inside Atrium's
  repo skips every path): 0001 trace to ovr.log (failed XR calls, head pose + layers once/s),
  0002 EchoXR's generic session/frame fixes (no frame begun in BeginSession, recenter after the
  first xrWaitFrame, wait for READY in the FOV probe on every runtime, opaque eye layers,
  request OpenXR 1.0), 0003 depth trace + `ATRIUM_OVR_DEPTH=0`.
  **Open bug:** on Meta's OpenXR runtime (Rift CV1) the picture moves with the head, although the
  trace shows good head poses, non-head-locked EyeFovDepth layers at 90 fps, no XR errors and
  sane depth (near 0.05, far 1023); depth off doesn't help. Same build on SteamVR's runtime is
  correct, and Echo direct on Meta is correct. So translated games prefer SteamVR
  (`launcher::xr_runtime`); `ATRIUM_XR=<manifest>` overrides for `--translate`. Next suspects:
  frame/pose timing vs Meta's runtime (absTime -> XrTime), D3D12 image handover on Meta.
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
