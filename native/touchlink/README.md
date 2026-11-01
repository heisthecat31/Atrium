# TouchLink: Rift CV1 Touches in SteamVR, with any headset

A SteamVR driver that lets you use Rift CV1 Touch controllers with a Valve Index (or any other
SteamVR headset) in **every SteamVR game**. TouchLink is a separate tool. Atrium can use it, but
TouchLink doesn't need the Atrium app, and Atrium doesn't need TouchLink.

## How it works

Base stations can't track a Touch. The Touch has IR LEDs for cameras and no photodiodes for the
lasers. So TouchLink uses two tracking systems and lines them up:

1. **Atrium's tracking service** (`native/atrium_monado`, Monado with the Rift drivers, no
   compositor) tracks the Touches with the **Rift sensors**. The Rift headset only needs USB,
   because its radio talks to the Touches. No Meta software is involved.
2. **One calibration** maps the sensors' space onto SteamVR's. No buttons and no timing: while
   there isn't one, hold either Touch against the side of the headset and look around (left,
   right, up, down, tilt) until both Touches buzz. The driver solves the last 8 seconds of each
   Touch every second and takes the first window where a Touch moved rigidly with the headset; a
   Touch that's only in a hand never fits. X+Y+A+B (or `touchlink calibrate`) starts looking
   again. Delays from -120 to +120 ms between the two trackers are searched for during the solve.
3. Each Touch shows up as an **`oculus_touch` controller** that uses Valve's own Touch input
   profile and CV1 render model (both already in SteamVR). Games' Touch bindings work as they do
   with a real Rift. A hand priority of 100 means the Touches take the hands over Index
   controllers.

## Use

```
native\atrium_monado\build.ps1 -Rift     (the tracking service, once)
native\build.bat                         (-> native\out\touchlink\)
```

Plug the Rift into USB (only its radio is used), place the sensor(s) facing your play space, and
run `touchlink.exe` (double-click it, or `touchlink setup`). It does everything in order and
the Atrium app isn't needed:

1. registers the driver with SteamVR;
2. turns Meta's service off and sets it to Manual (so it stays off after a restart), and puts
   the sensors on Windows' WinUSB driver with USB 3 power saving off (`sensordriver.exe`, shipped
   next to it). This is one UAC prompt;
3. starts SteamVR, or restarts it if anything changed;
4. waits until the tracking service runs, the headset is tracked and a Touch is seen, and says
   what's missing. Switched sensors are replugged in software (a USB port cycle); if the Touches
   are on but no sensor sees them for 20 s, it replugs them again and restarts SteamVR;
5. shows the calibration's progress on one line (how far the head has turned, or what's off)
   until it's done.

The calibration lasts until the sensors or base stations move. The Atrium app, if it runs with
"Headset's own controllers" picked in Mix & match, starts Meta's runtime for a plugged-in Rift,
so close it (setup warns you).

| Command | |
| --- | --- |
| `touchlink [setup] [--service DIR]` | everything above |
| `touchlink install [--service DIR]` | registers the driver (SteamVR's `vrpathreg`) and stores where the tracking service is |
| `touchlink uninstall` | unregisters it, and gives the Rift back to Meta (service Automatic, sensors on Meta's driver) if setup took it |
| `touchlink calibrate` | starts a calibration and follows it |
| `touchlink status [--json]` | the driver's state; `--json` gives `status.json` for other tools |
| `touchlink source atrium\|meta` | where the Touches come from: Atrium's tracking service, or Meta's runtime |
| `touchlink camera`, `touchlink meta` | the helpers the driver starts (see above) |

The driver connects to a tracking service that is already running (for example, the Atrium
app's). If none is running, it starts its own, and that one stops when SteamVR does
(`start_service` in config.json).

## The headset's cameras keep the calibration right

With a headset that has cameras SteamVR can use (Valve Index), the driver also starts
`touchlink.exe camera` (src/camera.cpp). Every camera frame comes with the headset's pose at its
exposure, in SteamVR's raw space. The Touches' poses at that moment are projected into each camera
with the current calibration, and the glow of their IR LEDs is looked for around there. Every 150
sightings the calibration (6 numbers) and where the glow sits on each Touch (3 each) are fitted to
what the cameras saw (Levenberg-Marquardt, outliers dropped); a better fit is written to
calibration.json, which the driver reloads. Like EchoQuestXR's cv1bridge aligning with the Quest's
hand tracking, but continuous. It needs a first calibration to know where to look. SteamVR's camera
must be on (Settings > Camera); its state is in camera.json.

## Optional: Meta's runtime tracks the Touches

`touchlink source meta` (or Atrium's Mix & match, "Touch tracking: Meta's runtime") makes the
driver start `touchlink.exe meta` (src/meta.cpp) and read the Touches from it instead of Atrium's
tracking service: a Meta runtime session as cv1bridge had (a tiny black picture each frame keeps the
Rift awake and the session in focus for rumble), poses and buttons in shared memory
(`Local\TouchLinkMeta`, meta_shared.h). Meta's runtime only tracks the Touches while the Rift is
"worn": instead of tape on the proximity sensor, OVRServer's `EnableProximitySensor` setting and
`prox` command are sent through Meta's Debug Tool CLI. It needs the Oculus app installed, the
sensors on Meta's driver (`touchlink setup` gives them back), and the Rift fully connected: Meta's
runtime won't open a Rift whose display (HDMI) isn't connected, so with the Rift on USB only (the
usual Mix & match setup) it can't work; Atrium's own tracking only needs USB. `touchlink source
atrium` goes back.

## In Atrium

With Atrium, nothing here has to be run by hand: in Mix & match, picking **Rift CV1 Touches**
with a wired SteamVR headset (an Index, ...) turns Meta's software off and sets up the sensors
(buttons), runs the tracking service, registers TouchLink with SteamVR (`touchlink install`,
automatically), and shows the room (where each sensor was placed) and the Touch calibration live,
with a Calibrate button.

## For Atrium (or any other tool)

- `%APPDATA%\TouchLink\status.json` is rewritten every second while SteamVR runs. It holds
  connected, hmd_tracked, each Touch's active/tracked state, calibrated, error_mm, calibrating, and messages.
- To start a calibration, create `%APPDATA%\TouchLink\calibrate`, or call
  `IVRSystem::DriverDebugRequest(touch, "calibrate")`.
- `config.json`: `service_dir`, `start_service`, `hand_priority`, and
  `grip_offset: [x, y, z, pitch, yaw, roll]` (metres and degrees, in the grip frame) to fine-tune
  where the Touch sits in your hand.

## Files

| | |
| --- | --- |
| `src/driver.cpp` | the SteamVR driver: devices, inputs, haptics, poses, calibration flow, service start |
| `src/source.cpp` | the headless OpenXR session on the tracking service (as `atrium_xr/src/touch.cpp`) |
| `src/calibration.cpp` | the solve: rotation (Horn), offset (least squares), delay search, checks |
| `src/touchlink.cpp` | `touchlink.exe` |
| `src/calib_check.cpp` | the solve on synthetic tracks (`build\check\calib_check.exe`) |

Status (2026-11-01): builds, and the calibration solve passes on synthetic tracks (0.17 mm,
delay exact). **Not run in SteamVR yet.** Things to check on the hardware: SteamVR loads it next
to the Index (`alwaysActivate`), poses land in the right space, the controller sits right in the
hand (grip offset), and haptics. No finger skeleton yet. Games that animate hands from
`/input/skeleton` fall back to their own hand poses.

Licence: MIT. Ships no Valve or Meta files. The profile, render models and bindings are SteamVR's
own, referenced by `{oculus}` path.
