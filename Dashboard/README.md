# Atrium Dashboard

The in-VR dashboard every headset uses under Atrium: the overlay you open inside VR to see your
library, launch or switch games, and check headset/controller state. Built in **Unity**.

This folder is owned by the dashboard developer. The rest of the repo (Rust app, native runtime)
is worked on separately; coordinate changes to anything outside `Dashboard/`.

## Setting up

1. Create the Unity project **in this folder** (`Dashboard/` is the project root, so
   `Dashboard/Assets`, `Dashboard/Packages`, `Dashboard/ProjectSettings` live here).
2. Use the **OpenXR** plugin (XR Plug-in Management → OpenXR). Atrium only exposes OpenXR, so
   the dashboard must not depend on the Oculus/Meta XR or SteamVR plugins.
3. Commit `Assets/`, `Packages/` and `ProjectSettings/` only. `.gitignore` here already
   excludes `Library/`, `Temp/`, `Logs/`, builds, etc.
4. Editor settings: Version Control → Visible Meta Files; Asset Serialization → Force Text.

Record the Unity version you pick here once chosen: **Unity version: _TBD_**

## Talking to Atrium

Not defined yet. The intended shape:

- The dashboard runs as its own OpenXR app/overlay on the Atrium Runtime.
- It gets the library, headset state and launch actions from the Atrium app over a local
  channel (likely a localhost WebSocket or named pipe with JSON messages).

Agree the message format with the app side before building against it, and write it down in
this folder (e.g. `Dashboard/PROTOCOL.md`).

## Rules (same as the rest of Atrium)

- MIT licence. No Meta or Valve binaries, artwork, fonts or store images committed.
- Headset/controller visuals are our own art.
