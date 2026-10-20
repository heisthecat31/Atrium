# Atrium

One open-source home for every VR headset and every VR game.

- **One app.** A native Rust + egui launcher for Windows and Linux. No Electron, no web UI.
- **One runtime.** The Atrium Runtime is an OpenXR runtime and the only one Atrium exposes.
  Under the hood it forwards to the vendor stack that drives the connected headset (Meta, SteamVR,
  Monado/WiVRn), then replaces those with Atrium's own drivers headset by headset.
  LibOVR (Oculus SDK) and OpenVR games are translated into OpenXR.
- **One library.** Steam, Meta and local games together. Sign in to Steam and Meta to see what
  you own; installed games show without signing in.
- **One dashboard.** An in-VR dashboard (Unity, in [`Dashboard/`](Dashboard/)) shared by every
  headset.

Status: early. Headset detection, Steam/Meta libraries and Steam/Meta sign-in work; the UI,
launcher and runtime are in progress.

## Layout

| Path | What |
| --- | --- |
| `src/` | the Atrium app (Rust, egui/eframe) |
| `src/sys/` | platform access: registry, processes, VDF, settings, sealed secrets |
| `src/services/` | headsets, runtime, Steam, Meta, library |
| `src/login.rs` | Steam (OpenID) and Meta (native SSO) sign-in |
| `native/` | C++ tools and the Atrium Runtime (MSVC) |
| `Dashboard/` | the in-VR dashboard (Unity) |
| `assets/` | icon and headset line art |

## Build

App:

```
cargo build --release
```

Native tools (Windows, Visual Studio with C++):

```
native\build.bat
```

`native/third_party/LibOVR` is Meta's Oculus PC SDK (`Include/` and `Shim/`). It isn't in this
repo for licence reasons; download the Oculus PC SDK from Meta and copy those two folders there.

Until the UI lands, sign-ins can be run from a terminal:

```
atrium --sign-in steam
atrium --sign-in meta
```

## Rules

- No Meta or Valve binaries, artwork or game files in the repo.
- Atrium never registers itself as the `oculus://` handler, and never changes system settings
  (OpenXR runtime registration, services, environment) without an explicit user action.
- Tokens are stored sealed: DPAPI on Windows, a 0600 settings file on Linux.

More detail for contributors (and agents) in [AGENT.md](AGENT.md).

## Licence

MIT
