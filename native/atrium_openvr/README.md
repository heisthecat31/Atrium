# Atrium OpenVR (SteamVR games without SteamVR)

Atrium's build of [OpenComposite](https://gitlab.com/znixian/OpenOVR) (GPL-3.0), shipped as a
**separate GPL component**: the game loads its `vrclient_x64.dll`; Atrium (MIT) only points the
game at it (`VR_OVERRIDE`). xrizer (GPL-3.0) is Linux-only today and is kept for Atrium's Linux side.

Status (2026-10-20): builds (pin `openxr` branch cff07db). Patch 0001 adds a missing include for
VS 2026. Needs `libs/vulkan/{Include,Lib/vulkan-1.lib}` copied from the Vulkan SDK, and only the
`OCOVR` target (the C# RuntimeSwitcher doesn't build and isn't needed). Staged at
`native/out/openvr/bin/vrclient_x64.dll`.

Tested: an OpenVR test app with VR_OVERRIDE gets an OpenXR session on the "Oculus Rift CV1"
through the Atrium Runtime (state READY with the headset worn), but **VR_Init hangs right after
IVRSystem_026 is created**. Not the Virtual Desktop implicit layer, not initUsingVulkan (already
off on Windows). Next: find where IVRSystem's setup blocks; then a build script and launcher
wiring (VR_OVERRIDE needs an existing openvrpaths.vrpath).
