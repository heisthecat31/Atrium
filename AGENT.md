# AGENT.md — Atrium

Notes for any agent (or person) picking up this project. Read this first.

## The goal

**Atrium** is one open-source home for every VR headset and every VR game:

- **One native app** (Rust + egui, our own UI design, no Electron, no web UI) for Windows and Linux.
- **One runtime: the Atrium Runtime.** It's an OpenXR runtime (`atrium_openxr.json` +
  `atrium_xr.dll` / `libatrium_xr.so`) and the *only* runtime Atrium exposes. The Atrium app
  registers it as the system OpenXR runtime. Users never pick SteamVR / Meta / Virtual Desktop.
  - Device backends live inside it. v1 forwards each OpenXR call to the vendor stack that
    drives the connected hardware (Meta's runtime for Rift/Quest Link, SteamVR for Lighthouse,
    Monado/WiVRn on Linux), while the game only ever sees "Atrium". Later versions replace
    vendor stacks with Atrium's own drivers, headset by headset (Rift CV1 first: test hardware).
  - Games that aren't OpenXR are translated into it: LibOVR (Oculus SDK) games through the
    Revive-derived layer (RiftLift / EchoXR, MIT), OpenVR games through xrizer / OpenComposite.
- **One library**: Steam, Meta and local games together. The user still signs in to Steam and
  Meta to see what they own; installed games show without signing in.
- Licence: MIT. Ship **no** Meta or Valve binaries, artwork or game files.

## Layout

| Path | What |
| --- | --- |
| `Cargo.toml`, `src/` | the Atrium app (Rust, egui/eframe) |
| `src/sys/` | platform access: registry, processes, USB, VDF, settings, secrets (DPAPI) |
| `src/services/` | headsets, runtime, steam, meta, library, launcher |
| `src/ui/` | theme, widgets, pages. All drawing is ours (`ui/theme.rs` holds the palette) |
| `src/login.rs` | sign-in: Steam via system browser + localhost OpenID return; Meta in a native webview (wry) subprocess (`atrium --login-meta`) that catches the `oculus://` callback |
| `runtime/` (top level, gitignored) | Meta DLLs copied earlier for research. Locked by OVRServer; never commit |
| `native/atrium_xr/` | the Atrium Runtime (C++, OpenXR runtime ABI) |
| `native/src/` | `ovrprobe` (Meta runtime probe, `ovrprobe json`), `SteamFrameSim` (simulated Steam Frame) |
| `Dashboard/` | in-VR dashboard, **Unity** (OpenXR plugin only), owned by a second dev. See `Dashboard/README.md`; app↔dashboard protocol not defined yet |
| `native/third_party/LibOVR` | Meta Oculus SDK headers/shim: gitignored (not MIT) |
| `assets/` | Atrium icon; `icons-reference.js` holds the headset line-art SVGs |

Build: `cargo build --release` (app), `native\build.bat` (MSVC: ovrprobe, SteamFrameSim, runtime).

## History

- 2026-10-20 (later): `src/login.rs` written. Meta: parent runs `begin_login`, spawns
  `atrium --login-meta`, sends the auth URL on **stdin**; child (tao + wry, incognito, WebView2
  data in `%APPDATA%\Atrium\webview`) blocks the `oculus://`/`oculus-client://` navigation and
  prints `callback <url>` on stdout; parent finishes `complete_login`, seals the token. Steam:
  OpenID in system browser → `127.0.0.1:<random>/steam?state=<nonce>`, verified with
  `check_authentication`. `services/library.rs` holds only the `Game`/`Art`/`Source` types so far.
  Until the UI exists, `atrium --sign-in steam|meta` runs a sign-in from a terminal and saves it.
  Next: library merge, launcher, egui UI.
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
