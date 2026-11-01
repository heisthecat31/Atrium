//! Mix & match: any headset with any controllers the user has. For now the controllers are Rift
//! CV1 Touches, used with a headset that streams (Virtual Desktop, ...):
//!   - Atrium's tracking service (native/atrium_monado: Monado with the rift driver, no compositor)
//!     drives the Rift's radio and sensors and tracks the Touches. The Rift headset only needs USB.
//!   - The Atrium Runtime swaps the game's controllers for the Touches (native/atrium_xr touch.cpp,
//!     told through runtime.json's "touch").
//!   - The sensors run on Windows' WinUSB driver (native/src/sensordriver.cpp, admin once), and
//!     Meta's service must be off: it fights Atrium for the Rift's radio.
//!   - A one-time calibration lines the Touches up with the headset's space (touch.cpp).
//!   - With a wired SteamVR headset (an Index, ...), SteamVR games get the Touches through TouchLink
//!     (native/touchlink, shipped in touchlink\): Atrium registers its driver and shows its status
//!     and calibration (%APPDATA%\TouchLink\status.json).
//!   - With more than one sensor the service places them (the room, rift_room.json; patch 0011).
//!
//! The service also publishes what each sensor sees (shared memory Local\AtriumSensorView<n>),
//! which the page shows live so users can place the sensors.
use crate::sys;
use serde::Serialize;
use std::path::PathBuf;

/// What the page shows on the Mix & match tab.
#[derive(Serialize, Clone, Default)]
pub struct MixState {
    /// Atrium's tracking service and sensordriver are built.
    pub available: bool,
    pub service_running: bool,
    /// The Rift CV1 headset is on USB (its radio talks to the Touches).
    pub rift: bool,
    /// Sensors: instance id and driver ("winusb", "winusb-noguid", "meta", ...).
    pub sensors: Vec<(String, String)>,
    /// USB 3 link power management is off (sensors behind a hub need it off).
    pub lpm_off: bool,
    /// Meta's OVRServer is running: it fights Atrium for the Rift.
    pub meta_running: bool,
    pub calibration: Option<Calibration>,
    /// A calibration was asked for and hasn't finished yet.
    pub calibrating: bool,
    /// Where the tracking service has placed each sensor (rift_room.json).
    pub room: Vec<RoomSensor>,
    /// The Touches in SteamVR (TouchLink, next to atrium.exe in touchlink\), None if not shipped.
    pub touchlink: Option<TouchLink>,
}

/// One Rift sensor in the room the tracking service solved (native/atrium_monado patch 0011).
#[derive(Serialize, Clone, Default, PartialEq)]
pub struct RoomSensor {
    pub serial: String,
    /// Everything is measured from this one.
    pub reference: bool,
    pub placed: bool,
    /// Moments it saw a Touch or the headset together with a placed sensor.
    pub sightings: u32,
    /// Position (m) and orientation (quaternion x, y, z, w) in the tracking space.
    pub pose: [f64; 7],
}

/// TouchLink's driver in SteamVR, from %APPDATA%\TouchLink\status.json (written every second).
#[derive(Serialize, Clone, Default, PartialEq)]
pub struct TouchLink {
    /// Registered with SteamVR.
    pub registered: bool,
    /// SteamVR is running with the driver loaded.
    pub running: bool,
    pub connected: bool,
    pub hmd_tracked: bool,
    /// [left, right]: switched on, seen by a sensor.
    pub active: [bool; 2],
    pub tracked: [bool; 2],
    pub calibrated: bool,
    pub error_mm: f64,
    /// Looking for a Touch held to the headset.
    pub calibrating: bool,
    pub calibration_message: String,
    pub message: String,
    /// The headset's cameras keeping the calibration right (touchlink camera, camera.json): what it's
    /// doing, its fit (mm), and how often it has corrected the calibration. Empty without it.
    pub camera: String,
    pub camera_mm: f64,
    pub camera_updates: u32,
}

#[derive(Serialize, Clone)]
pub struct Calibration {
    pub error_mm: f64,
    pub reference: String,
    /// When it was saved (ms since 1970).
    pub when: u64,
}

/// Atrium's tracking service and the runtime manifest games' Touch swap loads: native\out\monado
/// (build.ps1 -Rift), or ATRIUM_MONADO_DIR (a Monado build folder, for development).
fn service_files() -> Option<(PathBuf, PathBuf)> {
    if let Ok(d) = std::env::var("ATRIUM_MONADO_DIR") {
        let d = PathBuf::from(d);
        let dev = (d.join("src/xrt/targets/service/monado-service.exe"), d.join("openxr_atrium-dev.json"));
        if dev.0.exists() && dev.1.exists() {
            return Some(dev);
        }
        let staged = (d.join("atrium-service.exe"), d.join("atrium_openxr.json"));
        return (staged.0.exists() && staged.1.exists()).then_some(staged);
    }
    let d = sys::native_tool("monado")?;
    let staged = (d.join("atrium-service.exe"), d.join("atrium_openxr.json"));
    (staged.0.exists() && staged.1.exists()).then_some(staged)
}

fn sensordriver() -> Option<PathBuf> {
    sys::native_tool(if cfg!(windows) { "sensordriver.exe" } else { "sensordriver" })
}

/// The runtime manifest the Atrium Runtime loads for the Touch swap (runtime.json "touch").
pub fn touch_manifest() -> Option<PathBuf> {
    service_files().map(|(_, m)| m)
}

/// The Touch swap's manifest when the user turned Mix & match on, else None.
pub fn touch_for(settings: &crate::sys::settings::Settings) -> Option<PathBuf> {
    if settings.mix.enabled { touch_manifest() } else { None }
}

static SERVICE: std::sync::Mutex<Option<std::process::Child>> = std::sync::Mutex::new(None);

pub fn service_running() -> bool {
    let mut c = SERVICE.lock().unwrap_or_else(|e| e.into_inner());
    match c.as_mut().map(|ch| ch.try_wait()) {
        Some(Ok(None)) => true,
        Some(_) => {
            *c = None;
            false
        }
        None => false,
    }
}

/// Start Atrium's tracking service: the Rift drivers with the sensors, no compositor (the headset
/// is someone else's), closed with Atrium.
pub fn start_service() -> Result<(), String> {
    if service_running() {
        return Ok(());
    }
    // Only one tracking service can hold the Rift: TouchLink's driver may have started one in
    // SteamVR (it doesn't while Atrium runs, but SteamVR may have been up first). Use that one.
    if sys::processes().contains("atrium-service.exe") {
        return Ok(());
    }
    let (exe, _) = service_files().ok_or("Atrium's tracking service isn't built (native\\atrium_monado\\build.ps1 -Rift)")?;
    let log = std::fs::File::create(sys::data_dir().join("tracking-service.log")).map_err(|e| e.to_string())?;
    let mut cmd = std::process::Command::new(&exe);
    cmd.current_dir(exe.parent().unwrap_or(std::path::Path::new(".")))
        .env("RIFT_PROBER_CONSTELLATION_TRACKING", "1")
        // The Rift is only the Touches' radio here (out of view): don't search for it (patch 0015).
        .env("ATRIUM_RIFT_HMD_UNTRACKED", "1")
        .env("XRT_COMPOSITOR_NULL", "1")
        .env("RIFT_LOG", "info")
        .env("XRT_LOG", "info")
        .stdin(std::process::Stdio::null())
        .stdout(log.try_clone().map_err(|e| e.to_string())?)
        .stderr(log);
    sys::no_window(&mut cmd);
    let child = cmd.spawn().map_err(|e| format!("Couldn't start Atrium's tracking service: {e}"))?;
    #[cfg(windows)]
    sys::end_with_atrium(&child);
    sys::log(&format!("mix: tracking service started (pid {})", child.id()));
    *SERVICE.lock().unwrap_or_else(|e| e.into_inner()) = Some(child);
    Ok(())
}

pub fn stop_service() {
    if let Some(mut c) = SERVICE.lock().unwrap_or_else(|e| e.into_inner()).take() {
        let _ = c.kill();
        let _ = c.wait();
        sys::log("mix: tracking service stopped");
    }
}

/// Put the sensors on WinUSB (and USB 3 link power management off): one admin prompt.
pub fn setup_sensors() -> Result<(), String> {
    let tool = sensordriver().ok_or("sensordriver isn't built (native\\build.bat)")?;
    #[cfg(windows)]
    return sys::run_elevated(&tool, "winusb");
    #[cfg(not(windows))]
    Err(format!("{} is Windows only", tool.display()))
}

/// Give the sensors back to Meta's driver (and the old power setting): one admin prompt.
pub fn restore_sensors() -> Result<(), String> {
    let tool = sensordriver().ok_or("sensordriver isn't built (native\\build.bat)")?;
    #[cfg(windows)]
    return sys::run_elevated(&tool, "restore");
    #[cfg(not(windows))]
    Err(format!("{} is Windows only", tool.display()))
}

/// Stop Meta's service and runtime until the next restart: one admin prompt.
pub fn stop_meta() -> Result<(), String> {
    #[cfg(windows)]
    {
        let ps = std::path::Path::new("powershell.exe");
        return sys::run_elevated(
            ps,
            "-NoProfile -Command Stop-Service OVRService -Force; Get-Process OVRServer_x64,OVRRedir,OVRServiceLauncher -ErrorAction SilentlyContinue | Stop-Process -Force",
        );
    }
    #[cfg(not(windows))]
    Err("Meta's service is Windows only".into())
}

/// Ask the game's Touch swap to calibrate (it watches for this file; touch.cpp).
pub fn calibrate() -> Result<(), String> {
    std::fs::write(sys::data_dir().join("touch_calibrate"), b"").map_err(|e| e.to_string())
}

fn calibration() -> Option<Calibration> {
    let p = sys::data_dir().join("touch_calibration.json");
    let j: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(&p).ok()?).ok()?;
    let when = std::fs::metadata(&p).ok()?.modified().ok()?.duration_since(std::time::UNIX_EPOCH).ok()?.as_millis() as u64;
    Some(Calibration { error_mm: j["error_mm"].as_f64().unwrap_or(0.0), reference: j["reference"].as_str().unwrap_or("").to_string(), when })
}

/// The room the tracking service solved: each sensor's pose, kept by serial.
pub fn room() -> Vec<RoomSensor> {
    let Ok(text) = std::fs::read_to_string(sys::data_dir().join("rift_room.json")) else { return vec![] };
    let Ok(j) = serde_json::from_str::<serde_json::Value>(&text) else { return vec![] };
    j["sensors"]
        .as_array()
        .map(|a| {
            a.iter()
                .map(|s| {
                    let mut pose = [0.0; 7];
                    for (i, v) in s["pose"].as_array().into_iter().flatten().take(7).enumerate() {
                        pose[i] = v.as_f64().unwrap_or(0.0);
                    }
                    RoomSensor {
                        serial: s["serial"].as_str().unwrap_or("").into(),
                        reference: s["reference"].as_bool().unwrap_or(false),
                        placed: s["placed"].as_bool().unwrap_or(false),
                        sightings: s["sightings"].as_u64().unwrap_or(0) as u32,
                        pose,
                    }
                })
                .collect()
        })
        .unwrap_or_default()
}

/// What the sensors see right now (rift_live.json, rewritten ~10 times a second by the service):
/// the headset (id 0) and the Touches, positioned in the room.
pub fn room_live() -> Option<Vec<u8>> {
    let p = sys::data_dir().join("rift_live.json");
    let age = std::fs::metadata(&p).ok()?.modified().ok()?.elapsed().ok()?;
    if age > std::time::Duration::from_secs(2) {
        return None;
    }
    std::fs::read(p).ok()
}

/// Forget the room: every sensor but the first is placed again (the service restarts).
pub fn reset_room() -> Result<(), String> {
    let p = sys::data_dir().join("rift_room.json");
    if p.exists() {
        std::fs::remove_file(&p).map_err(|e| e.to_string())?;
    }
    if service_running() {
        stop_service();
        start_service()?;
    }
    Ok(())
}

/// TouchLink (the Touches as SteamVR controllers): touchlink\touchlink.exe next to atrium.exe.
fn touchlink_exe() -> Option<PathBuf> {
    let d = sys::native_tool("touchlink")?;
    let exe = d.join("touchlink.exe");
    exe.exists().then_some(exe)
}

fn touchlink_dir() -> PathBuf {
    dirs::config_dir().unwrap_or_default().join("TouchLink")
}

/// TouchLink's driver folder is in SteamVR's driver list.
fn touchlink_registered() -> bool {
    let Some(exe) = touchlink_exe() else { return false };
    let driver = exe.parent().map(|d| d.join("touchlink")).unwrap_or_default();
    let paths = dirs::data_local_dir().unwrap_or_default().join("openvr").join("openvrpaths.vrpath");
    let Ok(j) = std::fs::read_to_string(paths).map(|t| serde_json::from_str::<serde_json::Value>(&t).unwrap_or_default()) else { return false };
    let want = driver.to_string_lossy().to_lowercase();
    j["external_drivers"].as_array().into_iter().flatten().any(|d| d.as_str().is_some_and(|d| d.to_lowercase() == want))
}

/// Register TouchLink's driver with SteamVR (no admin; SteamVR loads it at its next start).
pub fn install_touchlink() -> Result<(), String> {
    let exe = touchlink_exe().ok_or("TouchLink isn't next to atrium.exe (native\\build.bat)")?;
    let mut cmd = std::process::Command::new(&exe);
    cmd.arg("install");
    sys::no_window(&mut cmd);
    let out = cmd.output().map_err(|e| e.to_string())?;
    if out.status.success() {
        sys::log("mix: TouchLink registered with SteamVR");
        Ok(())
    } else {
        Err(format!("TouchLink couldn't register with SteamVR: {}", String::from_utf8_lossy(&out.stderr).trim()))
    }
}

/// Where TouchLink takes the Touches from: Atrium's tracking service, or Meta's runtime
/// (touchlink.exe source meta|atrium; SteamVR uses it from its next start).
pub fn set_touchlink_source(meta: bool) -> Result<(), String> {
    let Some(exe) = touchlink_exe() else { return Ok(()) };
    let mut cmd = std::process::Command::new(&exe);
    cmd.args(["source", if meta { "meta" } else { "atrium" }]);
    sys::no_window(&mut cmd);
    let out = cmd.output().map_err(|e| e.to_string())?;
    if out.status.success() { Ok(()) } else { Err("TouchLink couldn't change where the Touches come from".into()) }
}

/// Hand the Rift to Meta's runtime, for its tracking: Meta's service back on (Automatic) and the
/// sensors back on Meta's driver, in one admin prompt (touchlink.exe admin on).
pub fn give_rift_to_meta() -> Result<(), String> {
    let exe = touchlink_exe().ok_or("TouchLink isn't next to atrium.exe (native\\build.bat)")?;
    #[cfg(windows)]
    return sys::run_elevated(&exe, "admin on");
    #[cfg(not(windows))]
    Err(format!("{} is Windows only", exe.display()))
}

/// Where the Touches sit in the hand (Settings): TouchLink's grip_offset in config.json, metres and
/// degrees, which its driver applies at once. The rest of config.json (TouchLink's) stays.
pub fn set_touch_offset(o: &crate::sys::settings::TouchOffset) -> Result<(), String> {
    let d = touchlink_dir();
    std::fs::create_dir_all(&d).map_err(|e| e.to_string())?;
    let p = d.join("config.json");
    let mut j = std::fs::read_to_string(&p).ok().and_then(|t| serde_json::from_str::<serde_json::Value>(&t).ok()).unwrap_or_else(|| serde_json::json!({}));
    if !j.is_object() {
        j = serde_json::json!({});
    }
    let m = |cm: f32| (cm as f64 / 100.0 * 1e4).round() / 1e4;
    j["grip_offset"] = serde_json::json!([m(o.x), m(o.y), m(o.z), o.pitch, o.yaw, o.roll]);
    std::fs::write(&p, serde_json::to_string_pretty(&j).unwrap_or_default()).map_err(|e| e.to_string())
}

/// TouchLink's Meta helper (touchlink.exe meta), started by Atrium.
static META_HELPER: std::sync::Mutex<Option<std::process::Child>> = std::sync::Mutex::new(None);

/// With Meta's runtime tracking the Touches: TouchLink's helper runs from as soon as OVRServer does.
/// Meta's runtime gives the Rift's display, and only with it the Touches' buttons and rumble, to
/// the app that takes it first; started with SteamVR the helper lost it to SteamVR's compositor and
/// a player's Touches tracked with no buttons (2026-11-01). TouchLink's driver uses this one.
pub fn keep_meta_helper() {
    let mut guard = META_HELPER.lock().unwrap_or_else(|e| e.into_inner());
    if let Some(c) = guard.as_mut() {
        if c.try_wait().ok().flatten().is_none() {
            return;
        }
        *guard = None;
    }
    let Some(exe) = touchlink_exe() else { return };
    // One already runs (TouchLink's driver started it): leave it, and its log.
    #[cfg(windows)]
    unsafe {
        use windows_sys::Win32::Foundation::CloseHandle;
        use windows_sys::Win32::System::Threading::OpenMutexW;
        let name: Vec<u16> = "Local\\TouchLinkMetaHelper".encode_utf16().chain(Some(0)).collect();
        let h = OpenMutexW(0x0010_0000 /* SYNCHRONIZE */, 0, name.as_ptr());
        if !h.is_null() {
            CloseHandle(h);
            return;
        }
    }
    let d = touchlink_dir();
    let _ = std::fs::create_dir_all(&d);
    let mut cmd = std::process::Command::new(&exe);
    cmd.arg("meta").current_dir(exe.parent().unwrap_or(std::path::Path::new("."))).stdin(std::process::Stdio::null());
    if let Ok(log) = std::fs::File::create(d.join("meta.log")) {
        if let Ok(err) = log.try_clone() {
            cmd.stderr(err);
        }
        cmd.stdout(log);
    }
    sys::no_window(&mut cmd);
    match cmd.spawn() {
        Ok(c) => {
            sys::end_with_atrium(&c);
            sys::log(&format!("mix: started TouchLink's Meta helper (pid {})", c.id()));
            *guard = Some(c);
        }
        Err(e) => sys::log(&format!("mix: couldn't start TouchLink's Meta helper: {e}")),
    }
}

/// The helper Atrium started, closed (Touch tracking back to Atrium's service, or Mix & match off).
pub fn stop_meta_helper() {
    if let Some(mut c) = META_HELPER.lock().unwrap_or_else(|e| e.into_inner()).take() {
        let _ = c.kill();
        let _ = c.wait();
    }
}

/// Start a Touch calibration in SteamVR (TouchLink watches for this file).
pub fn calibrate_touchlink() -> Result<(), String> {
    let d = touchlink_dir();
    std::fs::create_dir_all(&d).map_err(|e| e.to_string())?;
    std::fs::write(d.join("calibrate"), b"").map_err(|e| e.to_string())
}

pub fn touchlink() -> Option<TouchLink> {
    touchlink_exe()?;
    let mut t = TouchLink { registered: touchlink_registered(), ..Default::default() };
    let Ok(j) = std::fs::read_to_string(touchlink_dir().join("status.json")).map(|s| serde_json::from_str::<serde_json::Value>(&s).unwrap_or_default()) else { return Some(t) };
    let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
    t.running = j["updated"].as_u64().is_some_and(|u| now.saturating_sub(u) < 5);
    if !t.running {
        return Some(t);
    }
    let b = |k: &str| j[k].as_bool().unwrap_or(false);
    t.connected = b("connected");
    t.hmd_tracked = b("hmd_tracked");
    for (i, hand) in ["left", "right"].iter().enumerate() {
        t.active[i] = j[hand]["active"].as_bool().unwrap_or(false);
        t.tracked[i] = j[hand]["tracked"].as_bool().unwrap_or(false);
    }
    t.calibrated = b("calibrated") && !b("other_universe");
    t.error_mm = j["error_mm"].as_f64().unwrap_or(0.0);
    t.calibrating = !j["calibrating"].as_str().unwrap_or("").is_empty();
    t.calibration_message = j["calibration_message"].as_str().unwrap_or("").into();
    t.message = j["message"].as_str().unwrap_or("").into();
    if let Ok(c) = std::fs::read_to_string(touchlink_dir().join("camera.json")).map(|s| serde_json::from_str::<serde_json::Value>(&s).unwrap_or_default()) {
        if c["updated"].as_u64().is_some_and(|u| now.saturating_sub(u) < 5) {
            t.camera = c["state"].as_str().unwrap_or("").into();
            t.camera_mm = c["error_mm"].as_f64().unwrap_or(0.0);
            t.camera_updates = c["updates"].as_u64().unwrap_or(0) as u32;
        }
    }
    Some(t)
}

/// Everything the tab shows. Runs sensordriver (no admin), so call it from a worker thread.
pub fn state(rift: bool) -> MixState {
    let mut s = MixState {
        available: service_files().is_some() && sensordriver().is_some(),
        service_running: service_running(),
        rift,
        meta_running: sys::processes().contains("ovrserver_x64.exe"),
        calibration: calibration(),
        calibrating: sys::data_dir().join("touch_calibrate").exists(),
        room: room(),
        touchlink: touchlink(),
        ..Default::default()
    };
    if let Some(tool) = sensordriver() {
        for line in sys::output(&tool, &["status"]).lines() {
            let parts: Vec<&str> = line.split_whitespace().collect();
            match parts.as_slice() {
                ["usb3-lpm", v] => s.lpm_off = *v == "off",
                [id, _inf, kind] => s.sensors.push((id.to_string(), kind.to_string())),
                _ => {}
            }
        }
    }
    s
}

/// The latest picture sensor `index` sent: [width u32 LE][height u32 LE][serial, 32 bytes]
/// [width*height grey pixels]. None if that sensor isn't streaming.
#[cfg(windows)]
pub fn view(index: u32) -> Option<Vec<u8>> {
    use windows_sys::Win32::System::Memory::{MapViewOfFile, OpenFileMappingW, UnmapViewOfFile, FILE_MAP_READ, MEMORY_MAPPED_VIEW_ADDRESS};
    const HEADER: usize = 64;
    const MAX: usize = 640 * 480;
    let name: Vec<u16> = format!("Local\\AtriumSensorView{index}").encode_utf16().chain(Some(0)).collect();
    unsafe {
        let mapping = OpenFileMappingW(FILE_MAP_READ, 0, name.as_ptr());
        if mapping.is_null() {
            return None;
        }
        let view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, HEADER + MAX);
        let out = (|| {
            let base = view.Value as *const u8;
            if base.is_null() || std::ptr::read_volatile(base as *const u32) != 0x5653_5441 {
                return None;
            }
            let seq = base.add(16) as *const u64;
            // The service bumps the sequence before and after each frame: copy between two equal
            // even values, so the frame isn't half-written.
            for _ in 0..4 {
                let a = std::ptr::read_volatile(seq);
                if a % 2 == 1 {
                    std::thread::sleep(std::time::Duration::from_millis(2));
                    continue;
                }
                let w = std::ptr::read_volatile(base.add(8) as *const u32) as usize;
                let h = std::ptr::read_volatile(base.add(12) as *const u32) as usize;
                if w == 0 || h == 0 || w * h > MAX {
                    return None;
                }
                let mut out = Vec::with_capacity(40 + w * h);
                out.extend_from_slice(&(w as u32).to_le_bytes());
                out.extend_from_slice(&(h as u32).to_le_bytes());
                out.extend_from_slice(std::slice::from_raw_parts(base.add(32), 32));
                out.extend_from_slice(std::slice::from_raw_parts(base.add(HEADER), w * h));
                if std::ptr::read_volatile(seq) == a {
                    return Some(out);
                }
            }
            None
        })();
        if !view.Value.is_null() {
            UnmapViewOfFile(MEMORY_MAPPED_VIEW_ADDRESS { Value: view.Value });
        }
        windows_sys::Win32::Foundation::CloseHandle(mapping);
        out
    }
}

#[cfg(not(windows))]
pub fn view(_index: u32) -> Option<Vec<u8>> {
    None
}
