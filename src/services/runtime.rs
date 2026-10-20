//! The Atrium Runtime: Atrium's own OpenXR runtime, and the only one Atrium exposes.
//!
//! The runtime (native/atrium_xr) is registered as the system OpenXR runtime. Inside, it drives
//! the headset through a backend. Until Atrium's own drivers cover a headset, the backend is the
//! vendor stack that already talks to that hardware; the app picks it from what's connected and
//! writes it to `%APPDATA%\Atrium\runtime.json`, which the runtime reads when a game starts.
use crate::services::headsets::Hardware;
use crate::sys;
use serde::Serialize;
use std::path::{Path, PathBuf};

#[derive(Clone, Debug, Serialize)]
pub struct Backend {
    pub id: &'static str,
    pub label: &'static str,
    pub manifest: PathBuf,
}

#[derive(Clone, Debug, Default, Serialize)]
pub struct RuntimeState {
    /// atrium_openxr.json, if the runtime is built/installed
    pub manifest: Option<PathBuf>,
    /// the system's active OpenXR runtime manifest
    pub system_active: Option<PathBuf>,
    pub registered: bool,
    pub backends: Vec<Backend>,
    pub chosen: Option<Backend>,
    pub reason: String,
}

const KHRONOS: &str = "HKLM\\SOFTWARE\\Khronos\\OpenXR\\1";

fn classify(file: &Path) -> Option<(&'static str, &'static str)> {
    let s = file.to_string_lossy().to_lowercase();
    let name = sys::read_json(file).and_then(|j| j["runtime"]["name"].as_str().map(str::to_lowercase)).unwrap_or_default();
    let t = format!("{s} {name}");
    Some(if t.contains("atrium") {
        return None;
    } else if t.contains("steamxr") || t.contains("steamvr") {
        ("steamvr", "SteamVR")
    } else if t.contains("oculus") || t.contains("meta") {
        ("meta", "Meta")
    } else if t.contains("virtualdesktop") || t.contains("virtual desktop") {
        ("virtualdesktop", "Virtual Desktop")
    } else if t.contains("mixedreality") {
        ("wmr", "Windows Mixed Reality")
    } else if t.contains("wivrn") {
        ("wivrn", "WiVRn")
    } else if t.contains("monado") {
        ("monado", "Monado")
    } else if t.contains("pimax") {
        ("pimax", "Pimax")
    } else if t.contains("varjo") {
        ("varjo", "Varjo")
    } else {
        ("other", "Other")
    })
}

/// SteamVR's folder, from openvrpaths.vrpath.
pub fn steamvr_dir() -> Option<PathBuf> {
    let f = if cfg!(windows) {
        PathBuf::from(std::env::var("LOCALAPPDATA").unwrap_or_default()).join("openvr").join("openvrpaths.vrpath")
    } else {
        sys::home().join(".config/openvr/openvrpaths.vrpath")
    };
    let j = sys::read_json(&f)?;
    j["runtime"].as_array()?.iter().filter_map(|v| v.as_str()).map(PathBuf::from).find(|p| p.exists())
}

fn vendor_manifests() -> (Vec<PathBuf>, Option<PathBuf>) {
    let mut files: Vec<PathBuf> = vec![];
    let mut active = None;
    #[cfg(windows)]
    {
        active = sys::reg::string(KHRONOS, "ActiveRuntime").map(PathBuf::from);
        files.extend(sys::reg::value_names(&format!("{KHRONOS}\\AvailableRuntimes")).into_iter().map(PathBuf::from));
    }
    #[cfg(not(windows))]
    {
        let cfg = std::env::var("XDG_CONFIG_HOME").map(PathBuf::from).unwrap_or_else(|_| sys::home().join(".config"));
        for f in [cfg.join("openxr/1/active_runtime.json"), "/etc/xdg/openxr/1/active_runtime.json".into(), "/etc/openxr/1/active_runtime.json".into()] {
            if f.exists() {
                active = std::fs::canonicalize(&f).ok().or(Some(f));
                break;
            }
        }
        for dir in ["/usr/share/openxr/1", "/usr/local/share/openxr/1", "/etc/xdg/openxr/1"] {
            if let Ok(rd) = std::fs::read_dir(dir) {
                files.extend(rd.flatten().map(|e| e.path()).filter(|p| p.extension().is_some_and(|e| e == "json") && !p.ends_with("active_runtime.json")));
            }
        }
    }
    if let Some(sv) = steamvr_dir() {
        let f = sv.join(if cfg!(windows) { "steamxr_win64.json" } else { "steamxr_linux64.json" });
        if f.exists() {
            files.push(f);
        }
    }
    if let Some(a) = &active {
        files.push(a.clone());
    }
    let mut seen = std::collections::HashSet::new();
    files.retain(|f| f.exists() && seen.insert(f.to_string_lossy().to_lowercase()));
    (files, active)
}

/// The Atrium Runtime manifest: next to atrium(.exe), or native/out in a source checkout.
pub fn manifest() -> Option<PathBuf> {
    sys::native_tool("atrium_openxr.json")
}

fn same(a: &Path, b: &Path) -> bool {
    let n = |p: &Path| std::fs::canonicalize(p).unwrap_or_else(|_| p.to_path_buf()).to_string_lossy().to_lowercase();
    n(a) == n(b)
}

/// Pick the backend for what's connected. Order matters: a Rift on USB → Meta, a Lighthouse
/// headset → SteamVR, a running streamer → that streamer, then whatever is left.
pub fn state(hw: &Hardware) -> RuntimeState {
    let (files, active) = vendor_manifests();
    let backends: Vec<Backend> = files.iter().filter_map(|f| classify(f).map(|(id, label)| Backend { id, label, manifest: f.clone() })).collect();
    let find = |id: &str| backends.iter().find(|b| b.id == id).cloned();
    let usb = |vendor: &str| hw.headsets.iter().any(|h| h.via == "USB" && h.vendor == vendor);
    let streaming = |id: &str| hw.streaming.iter().any(|s| s.id == id && s.running);
    let (chosen, reason) = if (usb("Oculus") || usb("Meta") || hw.meta.as_ref().is_some_and(|m| m.hmd_present)) && find("meta").is_some() {
        (find("meta"), "Meta headset connected".to_string())
    } else if (usb("Valve") || usb("HTC") || usb("Bigscreen") || usb("Pimax")) && find("steamvr").is_some() {
        (find("steamvr"), "Lighthouse headset connected".to_string())
    } else if streaming("virtualdesktop") && find("virtualdesktop").is_some() {
        (find("virtualdesktop"), "Virtual Desktop is streaming".to_string())
    } else if (streaming("wivrn") || streaming("monado")) && (find("wivrn").is_some() || find("monado").is_some()) {
        (find("wivrn").or_else(|| find("monado")), "Linux XR service running".to_string())
    } else if usb("Microsoft") || usb("HP") {
        (find("wmr").or_else(|| find("steamvr")), "Mixed Reality headset connected".to_string())
    } else if let Some(b) = find("steamvr") {
        (Some(b), "default (SteamVR is installed)".to_string())
    } else {
        (backends.first().cloned(), "only backend available".to_string())
    };
    let manifest = manifest();
    let registered = matches!((&manifest, &active), (Some(m), Some(a)) if same(m, a));
    RuntimeState { manifest, system_active: active, registered, backends, chosen, reason }
}

/// Tell the runtime which backend to use for the next session.
pub fn write_backend(s: &RuntimeState) {
    let j = serde_json::json!({
        "backend": s.chosen.as_ref().map(|b| b.manifest.to_string_lossy().into_owned()),
        "backendId": s.chosen.as_ref().map(|b| b.id),
        "reason": s.reason,
    });
    let p = sys::data_dir().join("runtime.json");
    let old = std::fs::read_to_string(&p).unwrap_or_default();
    let new = serde_json::to_string_pretty(&j).unwrap_or_default();
    if old != new {
        let _ = std::fs::write(p, new);
    }
}

/// Make the Atrium Runtime the system's OpenXR runtime. Windows asks for admin (UAC) once.
pub fn register() -> Result<(), String> {
    let m = manifest().ok_or("The Atrium Runtime isn't built yet (native\\build.bat)")?;
    #[cfg(windows)]
    {
        let args = format!("add \"{KHRONOS}\" /v ActiveRuntime /t REG_SZ /d \"{}\" /f", m.display());
        let ps = format!("Start-Process reg.exe -Verb RunAs -Wait -WindowStyle Hidden -ArgumentList '{}'", args.replace('\'', "''"));
        let mut cmd = std::process::Command::new("powershell.exe");
        cmd.args(["-NoProfile", "-Command", &ps]);
        sys::no_window(&mut cmd);
        let st = cmd.status().map_err(|e| e.to_string())?;
        if !st.success() {
            return Err("Windows didn't allow the change".into());
        }
        Ok(())
    }
    #[cfg(not(windows))]
    {
        let dir = std::env::var("XDG_CONFIG_HOME").map(PathBuf::from).unwrap_or_else(|_| sys::home().join(".config")).join("openxr/1");
        std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
        let link = dir.join("active_runtime.json");
        let _ = std::fs::remove_file(&link);
        std::os::unix::fs::symlink(&m, &link).map_err(|e| e.to_string())
    }
}
