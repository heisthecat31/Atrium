//! What VR hardware is here and how it reaches the PC: USB ids (no VR software needed), what
//! Meta's runtime reports (ovrprobe), SteamVR, streaming apps, and the simulated Steam Frame.
use crate::sys;
use serde::Deserialize;

#[derive(Clone, Debug, Default)]
pub struct Headset {
    pub name: String,
    /// picks the line-art icon: rift, rifts, quest, index, vive, frame, wmr, pimax, beyond, generic
    pub family: &'static str,
    pub vendor: String,
    pub model: String,
    pub via: String,
    pub serial: String,
    pub active: bool,
    pub is_virtual: bool,
}

#[derive(Clone, Debug)]
pub struct Accessory {
    pub name: &'static str,
    pub count: usize,
}

#[derive(Clone, Debug)]
pub struct Streamer {
    pub id: &'static str,
    pub name: &'static str,
    pub running: bool,
}

#[derive(Clone, Debug, Default, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct MetaHmd {
    #[serde(default)]
    pub product: String,
    #[serde(default)]
    pub serial: String,
    #[serde(default)]
    pub ltouch: bool,
    #[serde(default)]
    pub rtouch: bool,
    #[serde(default)]
    pub hmd_present: bool,
    #[serde(default)]
    pub width: i32,
    #[serde(default)]
    pub height: i32,
    #[serde(default)]
    pub refresh: f32,
}

#[derive(Clone, Debug, Default)]
pub struct Hardware {
    pub headsets: Vec<Headset>,
    pub controllers: Vec<(String, String)>, // (name, via)
    pub accessories: Vec<Accessory>,
    pub meta_running: bool,
    pub meta: Option<MetaHmd>,
    pub emulation: Option<String>,
    pub steamvr_running: bool,
    pub streaming: Vec<Streamer>,
    pub sim_running: bool,
    pub sim_available: bool,
}

impl Hardware {
    pub fn primary(&self) -> Option<&Headset> {
        self.headsets.iter().find(|h| h.active).or(self.headsets.first())
    }
}

struct Known(&'static str, &'static str, &'static str, &'static str, &'static str, &'static str); // vid pid name family vendor model
const USB_HEADSETS: &[Known] = &[
    Known("2833", "0021", "Oculus Rift DK2", "rift", "Oculus", "DK2"),
    Known("2833", "0031", "Oculus Rift", "rift", "Oculus", "CV1"),
    Known("2833", "0051", "Oculus Rift S", "rifts", "Oculus", ""),
    Known("2833", "0137", "Meta Quest", "quest", "Meta", "Link"),
    Known("2833", "0182", "Meta Quest", "quest", "Meta", "Link"),
    Known("2833", "0183", "Meta Quest", "quest", "Meta", "Link"),
    Known("2833", "0186", "Meta Quest", "quest", "Meta", "Link"),
    Known("0bb4", "2c87", "HTC Vive", "vive", "HTC", ""),
    Known("0bb4", "0309", "HTC Vive Pro", "vive", "HTC", ""),
    Known("0bb4", "0313", "HTC Vive Pro 2", "vive", "HTC", ""),
    Known("0bb4", "0178", "HTC Vive Cosmos", "vive", "HTC", ""),
    Known("28de", "2613", "Valve Index", "index", "Valve", ""),
    Known("0483", "0101", "Pimax", "pimax", "Pimax", ""),
    Known("045e", "0659", "Windows Mixed Reality headset", "wmr", "Microsoft", ""),
    Known("03f0", "0580", "HP Reverb G2", "wmr", "HP", ""),
    Known("35ca", "0101", "Bigscreen Beyond", "beyond", "Bigscreen", ""),
];
const USB_ACCESSORIES: &[(&str, &str, &str)] = &[
    ("2833", "0211", "Rift Sensor"),
    ("28de", "2101", "SteamVR Watchman dongle"),
    ("28de", "2102", "SteamVR Watchman dongle"),
    ("28de", "2000", "Lighthouse receiver"),
];
const STREAMERS: &[(&str, &str, &[&str])] = &[
    ("virtualdesktop", "Virtual Desktop", &["virtualdesktop.streamer.exe", "virtualdesktop.service.exe"]),
    ("alvr", "ALVR", &["alvr dashboard.exe", "alvr_dashboard.exe", "alvr_dashboard"]),
    ("steamlink", "Steam Link", &["steamlinkvr.exe"]),
    ("wivrn", "WiVRn", &["wivrn-server"]),
    ("monado", "Monado", &["monado-service"]),
];

/// (vid, pid, serial) of every USB device, one entry per physical device.
fn usb() -> Vec<(String, String, String)> {
    let mut out = vec![];
    #[cfg(windows)]
    {
        // pnputil lists connected devices only, without PowerShell's start-up cost. Its labels
        // are localised, so match the instance id itself.
        let text = sys::output("pnputil.exe", &["/enum-devices", "/connected"]);
        for line in text.lines() {
            let Some(at) = line.find("USB\\VID_") else { continue };
            let id = line[at..].trim();
            let u = id.to_ascii_uppercase();
            if !u.starts_with("USB\\VID_") || u.contains("&MI_") {
                continue; // count each physical device once (its composite parent)
            }
            let vid = u.get(8..12).unwrap_or_default().to_ascii_lowercase();
            let pid = u.get(17..21).unwrap_or_default().to_ascii_lowercase();
            let serial = id.rsplit('\\').next().unwrap_or_default().to_string();
            out.push((vid, pid, serial));
        }
    }
    #[cfg(target_os = "linux")]
    if let Ok(rd) = std::fs::read_dir("/sys/bus/usb/devices") {
        for e in rd.flatten() {
            let p = e.path();
            let r = |f: &str| std::fs::read_to_string(p.join(f)).map(|s| s.trim().to_lowercase()).ok();
            if let (Some(v), Some(pr)) = (r("idVendor"), r("idProduct")) {
                out.push((v, pr, r("serial").unwrap_or_default()));
            }
        }
    }
    out
}

fn meta_probe() -> Option<MetaHmd> {
    let probe = sys::native_tool("ovrprobe.exe")?;
    let text = sys::output(probe, &["json"]);
    serde_json::from_str(text.lines().last()?.trim()).ok()
}

pub fn detect() -> Hardware {
    let procs = sys::processes();
    let mut hw = Hardware::default();

    for (vid, pid, serial) in usb() {
        if let Some(k) = USB_HEADSETS.iter().find(|k| k.0 == vid && k.1 == pid) {
            hw.headsets.push(Headset { name: k.2.into(), family: k.3, vendor: k.4.into(), model: k.5.into(), via: "USB".into(), serial, ..Default::default() });
        }
        if let Some(a) = USB_ACCESSORIES.iter().find(|a| a.0 == vid && a.1 == pid) {
            match hw.accessories.iter_mut().find(|x| x.name == a.2) {
                Some(x) => x.count += 1,
                None => hw.accessories.push(Accessory { name: a.2, count: 1 }),
            }
        }
    }

    #[cfg(windows)]
    {
        hw.emulation = sys::reg::string("HKCU\\Environment", "OVREmulationPlugin");
        hw.meta_running = procs.contains("ovrserver_x64.exe");
        if hw.meta_running {
            hw.meta = meta_probe();
        }
    }
    hw.sim_running = procs.contains("steamframesim.exe");
    hw.sim_available = sys::native_tool("SteamFrameSim.exe").is_some();
    hw.steamvr_running = procs.contains("vrserver.exe") || procs.contains("vrserver");
    hw.streaming = STREAMERS.iter().map(|(id, name, p)| Streamer { id, name, running: p.iter().any(|x| procs.contains(*x)) }).collect();

    if hw.sim_running {
        hw.headsets.insert(0, Headset { name: "Steam Frame".into(), family: "frame", vendor: "Valve".into(), model: "simulated".into(), via: "Atrium simulator".into(), is_virtual: true, active: true, ..Default::default() });
    }
    if let Some(m) = hw.meta.clone().filter(|m| m.hmd_present) {
        let emulated = m.product.to_lowercase().contains("emulation") || m.serial == "HMD0000000000";
        if emulated && !hw.sim_running {
            hw.headsets.insert(0, Headset { name: "Emulated headset".into(), family: "generic", vendor: "Meta".into(), model: m.product.clone(), via: "Meta runtime emulation".into(), is_virtual: true, active: true, ..Default::default() });
        } else if !emulated {
            if let Some(h) = hw.headsets.iter_mut().find(|h| h.vendor == "Oculus" || h.vendor == "Meta") {
                h.active = true;
            } else {
                let fam = if m.product.to_lowercase().contains("quest") { "quest" } else { "generic" };
                hw.headsets.insert(0, Headset { name: m.product.clone(), family: fam, vendor: "Meta".into(), via: "Link / Air Link".into(), serial: m.serial.clone(), active: true, ..Default::default() });
            }
        }
        if m.ltouch {
            hw.controllers.push(("Left Touch".into(), "Meta runtime".into()));
        }
        if m.rtouch {
            hw.controllers.push(("Right Touch".into(), "Meta runtime".into()));
        }
    }
    hw
}

pub fn start_sim() -> Result<(), String> {
    let exe = sys::native_tool("SteamFrameSim.exe").ok_or("SteamFrameSim.exe isn't built (native\\build.bat)")?;
    std::process::Command::new(&exe).current_dir(exe.parent().unwrap()).spawn().map(|_| ()).map_err(|e| e.to_string())
}

/// Meta's emulated headset hides a real Rift. The variable is read when Meta's service starts.
pub fn set_emulation(on: bool) -> bool {
    #[cfg(windows)]
    return sys::reg::set_user_env("OVREmulationPlugin", if on { Some("3") } else { None });
    #[cfg(not(windows))]
    {
        let _ = on;
        false
    }
}

pub fn restart_meta_service() -> bool {
    #[cfg(windows)]
    {
        let mut cmd = std::process::Command::new("powershell.exe");
        cmd.args(["-NoProfile", "-Command", "Start-Process powershell -Verb RunAs -Wait -WindowStyle Hidden -ArgumentList '-NoProfile -Command Restart-Service OVRService -Force'"]);
        sys::no_window(&mut cmd);
        return cmd.status().map(|s| s.success()).unwrap_or(false);
    }
    #[cfg(not(windows))]
    false
}
