//! Mix & match: any headset with any controllers the user has. For now the controllers are Rift
//! CV1 Touches, used with a headset that streams (Virtual Desktop, ...):
//!   - Atrium's tracking service (native/atrium_monado: Monado with the rift driver, no compositor)
//!     drives the Rift's radio and sensors and tracks the Touches. The Rift headset only needs USB.
//!   - The Atrium Runtime swaps the game's controllers for the Touches (native/atrium_xr touch.cpp,
//!     told through runtime.json's "touch").
//!   - The sensors run on Windows' WinUSB driver (native/src/sensordriver.cpp, admin once), and
//!     Meta's service must be off: it fights Atrium for the Rift's radio.
//!   - A one-time calibration lines the Touches up with the headset's space (touch.cpp).
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
    let (exe, _) = service_files().ok_or("Atrium's tracking service isn't built (native\\atrium_monado\\build.ps1 -Rift)")?;
    let log = std::fs::File::create(sys::data_dir().join("tracking-service.log")).map_err(|e| e.to_string())?;
    let mut cmd = std::process::Command::new(&exe);
    cmd.current_dir(exe.parent().unwrap_or(std::path::Path::new(".")))
        .env("RIFT_PROBER_CONSTELLATION_TRACKING", "1")
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

/// Everything the tab shows. Runs sensordriver (no admin), so call it from a worker thread.
pub fn state(rift: bool) -> MixState {
    let mut s = MixState {
        available: service_files().is_some() && sensordriver().is_some(),
        service_running: service_running(),
        rift,
        meta_running: sys::processes().contains("ovrserver_x64.exe"),
        calibration: calibration(),
        calibrating: sys::data_dir().join("touch_calibrate").exists(),
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
