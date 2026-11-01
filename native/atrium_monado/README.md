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
- `0004`-`0005`: the Atrium streaming driver and target.
- `0006`: the Rift drivers on Windows (`build.ps1 -Rift`): a Windows HID backend
  (`os_hid_win32.c`, the hidraw semantics the drivers expect, no driver change), the prober opens
  HID interfaces through it, a `constellation` vcpkg feature (Ceres), and the Windows config-dir
  file helper the Touch calibration cache uses.
- `0007`: Touch position tracking. The Touch calibration's LED points (relative to the IMU, used
  as is, as in Jan Schmidt's OpenHMD work) become a constellation LED model; each Touch joins the
  HMD's tracker once its calibration is in. Pose = IMU rotation turned onto the cameras' frame
  (slow blend towards each optical rotation) + optical position, held 2 s when the cameras lose it.
  With more than one sensor, `0011` places them.
- `0008`: the sensors over WinUSB on Windows. Atrium moves them onto Windows' own signed WinUSB
  driver (`native/src/sensordriver.cpp`, admin once; `restore` gives them back to Meta's driver).
  Fixes: an upstream heap overflow in `uvc_fs_enumerate_modes` (Windows' heap catches it), and
  isochronous transfers rounded to whole 1 ms frames as WinUSB requires. `sensordriver winusb`
  also turns the power plan's USB 3 link power management off (saved; `restore` puts it back): a
  sensor behind a USB 3 hub can't wake its link, so its first request aborts and the hub resets
  it. With it off both sensors stream (2026-10-23); together they drop <1% of frames, in pairs.
- `0009`: Touch fixes from the first tracked run (2026-10-23, one sensor: left Touch tracked in
  81% of samples, right 63%): skip empty radio messages (read as full trigger/grip), reject
  implausible optical jumps, speed-limited extrapolation.
- `0010`: live view of each sensor for the Atrium app (shared memory `Local\AtriumSensorView<n>`).
- `0011`: room calibration. Upstream put every sensor at one placeholder pose, so with two sensors a
  device seen by both snapped between two places. Now the first sensor is the reference and each
  other one is placed as Meta's runtime does it: from where a device (the headset, a Touch) *is*
  in frames a placed sensor sees it too. Each frame is a point pair (the device in this sensor's
  frame, and in the origin as the placed sensor puts it); the sensor's pose is the rigid fit of the
  two clouds (Horn's method), found among outliers with RANSAC (4 cm) and refined on the points
  that agree. It needs 60 points 3 cm apart spanning 0.5 m; it's re-solved every 150 more (the last
  600 kept), so a moved sensor follows. (A first version chained each sighting's full pose: a few
  degrees of a Touch's turn put the sensor tens of cm off, and 200+ sightings never agreed.)
  Until placed, a sensor's poses aren't used. Saved by sensor serial in
  `%APPDATA%\Atrium\rift_room.json` (Linux: `~/.config/atrium/`), loaded at start; the Atrium
  app's Mix & match tab draws it. A missing reference sensor starts a new room.
- `0012`: the Windows IPC server survives a client that connects and leaves before it's served
  (`ERROR_NO_DATA`): upstream stopped the whole service, so the sensors' streams died. TouchLink's
  SteamVR driver retrying while the service started killed it again and again (2026-11-01).
- `0013`: a position filter for the Touch. Raw camera poses stepped the hands a few dozen times a
  second and passed every bad solve on (stutter, jumps). As Meta's runtime and Jan Schmidt's
  OpenHMD work do, the accelerometer (gravity learned from it) carries the Touch between camera
  poses at IMU rate; each camera pose, compared with where the filter had the Touch at its time,
  corrects position and velocity (alpha-beta, 0.3 / 0.05). One more than 12 cm off is dropped
  unless the next ones agree. 0011 also writes `rift_live.json` (what the sensors see now, ~10 Hz)
  for the room view. The Touch also gives its orientation as the tracker's prior when it has no
  fresh position (for 0014).
- `0014`: every pose the tracker finds for a device is checked against the device's own IMU
  orientation (its tracking source, turned into the world by a rotation learned from the first 20
  poses and kept following); more than 25 degrees off is another device's LEDs and is left out.
  Before, a full search after losing a Touch could take one Touch for the other (they mirror each
  other) or a Touch for the headset: hands jumped, and the room view showed the headset at a Touch.

Meta's OVRService (Automatic) starts OVRServer at boot; it fights Atrium's driver for the Rift
radio (Touches stop answering), so it must not run while Atrium drives the Rift.

## Hardware access

The Rift's HID and camera devices need udev rules so a normal user can open them (Monado ships
`xr-hardware` rules; most distros package them as `xr-hardware`).
