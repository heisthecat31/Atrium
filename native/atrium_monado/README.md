# Atrium Runtime (Monado-based), Linux and Windows

Atrium's own OpenXR runtime: Atrium's build of [Monado](https://gitlab.freedesktop.org/monado/monado)
(BSL-1.0) at a pinned commit plus the patches in `patches/`. Same pattern as Atrium OVR on Revive.
One source, two builds:

- **Linux** (`build.sh`): the runtime for every headset Atrium drives on Linux.
  - **Rift CV1**: Monado's `rift` driver (headset IMU, display, distortion, Touch over the radio),
    `rift_sensor` (the sensor cameras) and the constellation tracker (LED tracking, full 6DoF).
  - **Rift S** (`rift_s`), and the other headsets Monado drives.
  - **Display**: the compositor takes the headset display directly (DRM lease /
    `VK_KHR_display`), which on Linux needs no NVIDIA permission.
- **Windows** (`build.ps1`): the runtime for headsets Atrium streams to (Quest, Pico), with no
  Meta or SteamVR. Wired PC headsets on Windows stay on `native/atrium_xr` + Meta's OVRServer,
  because the GPU driver keeps their panels for vendor direct mode.

No SteamVR pieces are built. The Atrium app runs `atrium-service` and points games at
`atrium_openxr.json`.

## Build

Linux:

```bash
native/atrium_monado/build.sh            # build dir defaults to ~/.cache/atrium-build
```

Output in `native/out/linux/`: `libopenxr_atrium.so`, `atrium-service`, `atrium_openxr.json`.

Windows (Visual Studio, Vulkan SDK, git, Python; vcpkg is fetched into `build\vcpkg`):

```powershell
native\atrium_monado\build.ps1
```

Output in `native\out\monado\`: `openxr_atrium.dll`, `atrium-service.exe`, `atrium_openxr.json`,
`cjson.dll`, `pthreadVCE3.dll`.

Ubuntu/Debian packages for the Linux build:

```bash
sudo apt install build-essential git cmake ninja-build pkg-config python3 libeigen3-dev \
  libvulkan-dev glslang-tools glslc libusb-1.0-0-dev libudev-dev libhidapi-dev libx11-dev \
  libx11-xcb-dev libxxf86vm-dev libxrandr-dev libxcb-randr0-dev libwayland-dev wayland-protocols \
  libdrm-dev libgl1-mesa-dev libegl1-mesa-dev libglvnd-dev libjpeg-dev libbsd-dev libcjson-dev \
  libsystemd-dev libv4l-dev libuvc-dev libceres-dev libopencv-dev
```

## Patches

Edit Monado in a checkout of the pinned commit, then regenerate with `git diff` / `git format-patch`.

- `0001`: report the runtime as "Atrium" in `xrGetInstanceProperties`.
- `0002`: the system name is the headset's own ("Oculus Rift CV1"), not "Monado: ...".
- `0003`: OVRPlugin quirk: apps named "Oculus VR Plugin ..." (Meta's Unity/Unreal plugin) see the
  runtime name "Oculus" and may locate poses at XrTime 0 (= now). Same as the Windows runtime.

## Hardware access

The Rift's HID and camera devices need udev rules so a normal user can open them (Monado ships
`xr-hardware` rules; most distros package them as `xr-hardware`).
